#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <algorithm>

static constexpr int NNUE_HIDDEN = 256;
static constexpr int NNUE_INPUT = 49152; // 768 * 64 king buckets (ChessBuckets HalfKP)
static constexpr int NNUE_QA    = 255;
static constexpr int NNUE_QB    = 64;
static constexpr int NNUE_SCALE = 400;

struct NNUEWeights {
    int16_t l0w[NNUE_INPUT][NNUE_HIDDEN]; // feature weights: [feature][hidden]
    int16_t l0b[NNUE_HIDDEN];             // hidden bias
    int16_t l1w[2 * NNUE_HIDDEN];         // output weights
    int16_t l1b;                          // output bias
};

static NNUEWeights nnueWeights;
static bool nnueLoaded = false;

inline bool loadNNUE(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::invalid_argument("NNUE path " + path + " not found");
    }
    f.read(reinterpret_cast<char*>(nnueWeights.l0w), sizeof(nnueWeights.l0w));
    f.read(reinterpret_cast<char*>(nnueWeights.l0b), sizeof(nnueWeights.l0b));
    f.read(reinterpret_cast<char*>(nnueWeights.l1w), sizeof(nnueWeights.l1w));
    f.read(reinterpret_cast<char*>(&nnueWeights.l1b), sizeof(nnueWeights.l1b));
    nnueLoaded = f.good();
    return nnueLoaded;
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

// Feature indices for ChessBuckets (64 king buckets = HalfKP equivalent).
// Bucket = king square from own perspective. Black king square is flipped (^56).
// White piece: whiteIdx = 768*wksq + pt*64+sq,           blackIdx = 768*(bksq^56) + 384+pt*64+(sq^56)
// Black piece: whiteIdx = 768*wksq + 384+pt*64+sq,       blackIdx = 768*(bksq^56) + pt*64+(sq^56)
inline void getFeatureIndices(char piece, int sq, int wksq, int bksq, int& whiteIdx, int& blackIdx) {
    int pt = pieceTypeIndex(piece);
    bool isWhite = (piece >= 'A' && piece <= 'Z');
    int whiteBucket = 768 * wksq;
    int blackBucket = 768 * (bksq ^ 56);
    whiteIdx = whiteBucket + (isWhite ? 0 : 384) + pt * 64 + sq;
    blackIdx = blackBucket + (isWhite ? 384 : 0) + pt * 64 + (sq ^ 56);
}

inline void accAdd(int16_t* acc, int featureIdx) {
    const int16_t* row = nnueWeights.l0w[featureIdx];
    for (int i = 0; i < NNUE_HIDDEN; i++)
        acc[i] += row[i];}

inline void accSub(int16_t* acc, int featureIdx) {
    const int16_t* row = nnueWeights.l0w[featureIdx];
    for (int i = 0; i < NNUE_HIDDEN; i++)
        acc[i] -= row[i];
}

// Runs output layer from pre-built accumulators. Returns eval from STM's perspective.
// Loop is written to be auto-vectorizable with NEON/AVX2.
inline int nnueForward(const int16_t* __restrict__ stm_acc, const int16_t* __restrict__ ntm_acc) {
    int64_t output = 0;
    const int16_t* w = nnueWeights.l1w;

    for (int i = 0; i < NNUE_HIDDEN; i++) {
        int32_t v = stm_acc[i];
        if (v < 0) v = 0;
        if (v > NNUE_QA) v = NNUE_QA;
        output += (int64_t)(v * v) * w[i];
    }
    for (int i = 0; i < NNUE_HIDDEN; i++) {
        int32_t v = ntm_acc[i];
        if (v < 0) v = 0;
        if (v > NNUE_QA) v = NNUE_QA;
        output += (int64_t)(v * v) * w[NNUE_HIDDEN + i];
    }

    output /= NNUE_QA;
    output += nnueWeights.l1b;
    output = output * NNUE_SCALE / (NNUE_QA * NNUE_QB);
    return (int)output;
}
