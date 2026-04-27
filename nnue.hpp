#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Architecture: HalfKAv2-HM with horizontally-mirrored king buckets
//   Input  : 768 × 16 king buckets = 12,288 features per perspective
//   Hidden : 1024 neurons (SCReLU activation)
//   Output : 8 material-count buckets (one selected per position)
//
// Mirroring rule (mirrors bullet's ChessBucketsMirrored):
//   When the king is on files e–h (kingside), all piece squares are
//   horizontally flipped so the king always lands on files a–d.
//   Kings on files a–d are left as-is. This halves the king-square
//   dimension from 64 to 32 post-mirror squares.
//
// King bucket layout (32 post-mirror squares → 16 buckets):
//   Back ranks get full file granularity;
//   middle ranks use file pairs; advanced ranks share bucket pairs
//   across 2 ranks. Halving 32→16 doubles training examples per L0
//   row with equal parameter count to a 32-bucket/512-hidden net.
//     rank 1: 4 individual buckets (castled king)
//     rank 2: 4 individual buckets (luft positions)
//     rank 3–4: file pairs (2 buckets each rank)
//     rank 5–6: file pairs, rank-paired (share buckets)
//     rank 7–8: file pairs, rank-paired (share buckets)
//
// Output bucket formula (mirrors bullet's MaterialCount<8>):
//   bucket = (popcount(occupied) - 2) / 4
//   Pass occupied = allWhite | allBlack to nnueForward().
// ---------------------------------------------------------------------------

static constexpr int NNUE_INPUT          = 12288; // 768 * 16
static constexpr int NNUE_HIDDEN         = 1024;
static constexpr int NNUE_OUTPUT_BUCKETS = 8;
static constexpr int NNUE_QA             = 255;
static constexpr int NNUE_QB             = 64;
static constexpr int NNUE_SCALE          = 400;

struct alignas(64) NNUEWeights {
    int16_t l0w[NNUE_INPUT][NNUE_HIDDEN];          // 12288 × 1024 feature weights
    int16_t l0b[NNUE_HIDDEN];                       // hidden bias
    int16_t l1w[NNUE_OUTPUT_BUCKETS][2 * NNUE_HIDDEN];  // 8 × 2048, saved as [buckets][neurons]
    int16_t l1b[NNUE_OUTPUT_BUCKETS];               // 8 output biases
};

static NNUEWeights nnueWeights;
static bool nnueLoaded = false;

inline bool loadNNUE(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(nnueWeights.l0w), sizeof(nnueWeights.l0w));
    f.read(reinterpret_cast<char*>(nnueWeights.l0b), sizeof(nnueWeights.l0b));
    // .transpose() in bullet converts column-major to row-major = [BUCKETS][2*HIDDEN]
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
// BUCKET_LAYOUT maps 32 post-mirror squares (rank*4 + mirrored_file) to
//   16 buckets. Mirrors bullet's BUCKET_LAYOUT in halfkav2-1024.rs exactly.
//
// kingBucket(sq): maps the king's actual board square to bucket index 0-15.
//   Step 1: post-mirror square = rank * 4 + FILE_MIRROR[file]  (0-31)
//   Step 2: BUCKET_LAYOUT[post-mirror square]                   (0-15)
//
// kingFlip(sq): XOR mask applied to the low 3 bits of a feature index,
//   which flips the piece square's file.
//   flip = 7 when king is on files e-h (kingside) → mirror piece squares
//   flip = 0 when king is on files a-d (queenside) → no mirror
// ---------------------------------------------------------------------------
static constexpr int FILE_MIRROR[8] = {0, 1, 2, 3, 3, 2, 1, 0};

// clang-format off
static constexpr int BUCKET_LAYOUT[32] = {
     0,  1,  2,  3,  // rank 1: 4 individual (castled king, file critical)
     4,  5,  6,  7,  // rank 2: 4 individual (luft positions, still file-sensitive)
     8,  8,  9,  9,  // rank 3: file pairs
    10, 10, 11, 11,  // rank 4: file pairs
    12, 12, 13, 13,  // rank 5: file pairs } rank pair (ranks 5-6 share)
    12, 12, 13, 13,  // rank 6:           }
    14, 14, 15, 15,  // rank 7: file pairs } rank pair (ranks 7-8 share)
    14, 14, 15, 15,  // rank 8:           }
};
// clang-format on

inline int kingBucket(int kingSq) {
    int postMirrorSq = (kingSq / 8) * 4 + FILE_MIRROR[kingSq % 8];
    return BUCKET_LAYOUT[postMirrorSq];
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
    // Black perspective: flip bKingSq into black's coordinate system (back rank = rank 0)
    int bkFlipped = bKingSq ^ 56;
    blackIdx = 768 * kingBucket(bkFlipped) + (bBase ^ kingFlip(bkFlipped));
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

// Fused quiet update: acc += w[add1] - w[sub1]  (one combined pass, half the bandwidth)
inline void accFusedQuiet(int16_t* acc, int sub1, int add1) {
    const int16_t* rowSub = nnueWeights.l0w[sub1];
    const int16_t* rowAdd = nnueWeights.l0w[add1];
    for (int i = 0; i < NNUE_HIDDEN; i++)
        acc[i] += static_cast<int16_t>(rowAdd[i] - rowSub[i]);
}

// Fused capture update: acc += w[add1] - w[sub1] - w[sub2]  (one pass, two-thirds the bandwidth)
inline void accFusedCapture(int16_t* acc, int sub1, int sub2, int add1) {
    const int16_t* rowSub1 = nnueWeights.l0w[sub1];
    const int16_t* rowSub2 = nnueWeights.l0w[sub2];
    const int16_t* rowAdd  = nnueWeights.l0w[add1];
    for (int i = 0; i < NNUE_HIDDEN; i++)
        acc[i] += static_cast<int16_t>(rowAdd[i] - rowSub1[i] - rowSub2[i]);
}

// Runs the output layer. bucket = (popcount(occupied) - 2) / 4.
// stm and ntm loops merged into one pass for better cache utilisation.
inline int nnueForward(const int16_t* __restrict__ stm_acc,
                       const int16_t* __restrict__ ntm_acc,
                       int bucket) {
    // 1024 neurons × 255² × max_l1w can exceed INT32_MAX.
    // Inner 64-element blocks stay within int32; block sums widen to int64.
    int64_t output1 = 0, output2 = 0;

    const int16_t* stm_w = nnueWeights.l1w[bucket];
    const int16_t* ntm_w = stm_w + NNUE_HIDDEN;

    for (int i = 0; i < NNUE_HIDDEN; i += 64) {
        int32_t sb = 0, nb = 0;
        for (int j = i; j < i + 64; j++) {
            int32_t vs = std::clamp((int32_t)stm_acc[j], 0, (int32_t)NNUE_QA);
            int32_t vn = std::clamp((int32_t)ntm_acc[j], 0, (int32_t)NNUE_QA);
            sb += vs * vs * stm_w[j];
            nb += vn * vn * ntm_w[j];
        }
        output1 += sb;
        output2 += nb;
    }

    int32_t output = (int32_t)(output1 / NNUE_QA) + (int32_t)(output2 / NNUE_QA);
    output += nnueWeights.l1b[bucket];
    return (int)(output * NNUE_SCALE / (NNUE_QA * NNUE_QB));
}
