#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Architecture: HalfKAv2 with horizontally-mirrored king buckets
//   Input  : 768 × 32 king buckets = 24,576 features per perspective
//   Hidden : 512 neurons (SCReLU activation)
//   Output : 8 material-count buckets (one selected per position)
//
// Mirroring rule (mirrors bullet's ChessBucketsMirrored):
//   When the king is on files e–h (kingside), all piece squares are
//   horizontally flipped so the king always lands on files a–d.
//   Kings on files a–d are left as-is. This halves the king-square
//   dimension from 64 to 32 while retaining full king-perspective
//   expressiveness.
//
// Output bucket formula (mirrors bullet's MaterialCount<8>):
//   bucket = (popcount(occupied) - 2) / 4
//   Pass occupied = allWhite | allBlack to nnueForward().
// ---------------------------------------------------------------------------

static constexpr int NNUE_INPUT          = 24576; // 768 * 32
static constexpr int NNUE_HIDDEN         = 512;
static constexpr int NNUE_OUTPUT_BUCKETS = 8;
static constexpr int NNUE_QA             = 255;
static constexpr int NNUE_QB             = 64;
static constexpr int NNUE_SCALE          = 400;

struct NNUEWeights {
    int16_t l0w[NNUE_INPUT][NNUE_HIDDEN];          // 24576 × 512 feature weights
    int16_t l0b[NNUE_HIDDEN];                       // hidden bias
    int16_t l1w[2 * NNUE_HIDDEN][NNUE_OUTPUT_BUCKETS]; // 1024 × 8, saved transposed
    int16_t l1b[NNUE_OUTPUT_BUCKETS];               // 8 output biases
};

static NNUEWeights nnueWeights;
static bool nnueLoaded = false;

inline bool loadNNUE(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(nnueWeights.l0w), sizeof(nnueWeights.l0w));
    f.read(reinterpret_cast<char*>(nnueWeights.l0b), sizeof(nnueWeights.l0b));
    f.read(reinterpret_cast<char*>(nnueWeights.l1w), sizeof(nnueWeights.l1w));
    f.read(reinterpret_cast<char*>(nnueWeights.l1b), sizeof(nnueWeights.l1b));
    nnueLoaded = f.good();
    return nnueLoaded;
}

// ---------------------------------------------------------------------------
// King bucket helpers
//
// FILE_MIRROR maps each file (0-7) to its mirrored index (0-3):
//   file 0(a)→0, 1(b)→1, 2(c)→2, 3(d)→3, 4(e)→3, 5(f)→2, 6(g)→1, 7(h)→0
//
// kingBucket(sq): maps the king's actual board square to bucket index 0-31.
//   bucket = rank * 4 + FILE_MIRROR[file]
//   Matches bullet's ChessBucketsMirrored expansion with identity bucket array.
//
// kingFlip(sq): XOR mask applied to the low 3 bits of a feature index,
//   which flips the piece square's file.
//   flip = 7 when king is on files e-h (kingside) → mirror piece squares
//   flip = 0 when king is on files a-d (queenside) → no mirror
// ---------------------------------------------------------------------------
static constexpr int FILE_MIRROR[8] = {0, 1, 2, 3, 3, 2, 1, 0};

inline int kingBucket(int kingSq) {
    return (kingSq / 8) * 4 + FILE_MIRROR[kingSq % 8];
}

inline int kingFlip(int kingSq) {
    return (kingSq % 8 > 3) ? 7 : 0;
}

inline int pieceTypeIndex(char c) {
    switch (c | 32) {
        case 'p': return 0;
        case 'n': return 1;
        case 'b': return 2;
        case 'r': return 3;
        case 'q': return 4;
        case 'k': return 5;
        default:  return -1;
    }
}

// ---------------------------------------------------------------------------
// Feature indices — king-aware HalfKAv2
//
// White accumulator (white's perspective) uses wKingSq.
// Black accumulator (black's perspective) uses bKingSq.
//
// Base feature (Chess768):
//   own piece  → offset 0,   opponent piece → offset 384   (white perspective)
//   own piece  → offset 0,   opponent piece → offset 384   (black perspective,
//       using sq^56 to flip rank so black's rank-1 maps to white's rank-1)
//
// Final index = 768 * kingBucket(kingSq) + (base ^ kingFlip(kingSq))
//   The XOR flips the file bits of the piece square when the king is on kingside.
// ---------------------------------------------------------------------------
inline void getFeatureIndices(char piece, int sq, int wKingSq, int bKingSq,
                              int& whiteIdx, int& blackIdx) {
    int pt = pieceTypeIndex(piece);
    bool isWhite = (piece >= 'A' && piece <= 'Z');

    int wBase = (isWhite ? 0 : 384) + pt * 64 + sq;
    int bBase = (isWhite ? 384 : 0) + pt * 64 + (sq ^ 56);

    whiteIdx = 768 * kingBucket(wKingSq) + (wBase ^ kingFlip(wKingSq));
    blackIdx = 768 * kingBucket(bKingSq) + (bBase ^ kingFlip(bKingSq));
}

inline void accAdd(int16_t* acc, int featureIdx) {
    const int16_t* row = nnueWeights.l0w[featureIdx];
    for (int i = 0; i < NNUE_HIDDEN; i++)
        acc[i] += row[i];
}

inline void accSub(int16_t* acc, int featureIdx) {
    const int16_t* row = nnueWeights.l0w[featureIdx];
    for (int i = 0; i < NNUE_HIDDEN; i++)
        acc[i] -= row[i];
}

// Runs the output layer. bucket = (popcount(occupied) - 2) / 4.
// l1w is stored transposed as [2*HIDDEN][BUCKETS], so l1w[neuron][bucket].
// Loop written to be auto-vectorizable with NEON/AVX2.
inline int nnueForward(const int16_t* __restrict__ stm_acc,
                       const int16_t* __restrict__ ntm_acc,
                       int bucket) {
    // 512 neurons × 255² × max_l1w can exceed INT32_MAX.
    // Inner 64-element blocks stay within int32; only the 8 block sums use int64.
    int64_t output1 = 0, output2 = 0;

    for (int i = 0; i < NNUE_HIDDEN; i += 64) {
        int32_t block = 0;
        for (int j = i; j < i + 64; j++) {
            int32_t v = stm_acc[j];
            if (v < 0) v = 0;
            if (v > NNUE_QA) v = NNUE_QA;
            block += v * v * nnueWeights.l1w[j][bucket];
        }
        output1 += block;
    }
    for (int i = 0; i < NNUE_HIDDEN; i += 64) {
        int32_t block = 0;
        for (int j = i; j < i + 64; j++) {
            int32_t v = ntm_acc[j];
            if (v < 0) v = 0;
            if (v > NNUE_QA) v = NNUE_QA;
            block += v * v * nnueWeights.l1w[NNUE_HIDDEN + j][bucket];
        }
        output2 += block;
    }

    int32_t output = output1 / NNUE_QA + output2 / NNUE_QA;
    output += nnueWeights.l1b[bucket];
    output = output * NNUE_SCALE / (NNUE_QA * NNUE_QB);
    return (int)output;
}
