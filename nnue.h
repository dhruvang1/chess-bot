#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <algorithm>

static constexpr int NNUE_HIDDEN = 512;
static constexpr int NNUE_QA    = 255;
static constexpr int NNUE_QB    = 64;
static constexpr int NNUE_SCALE = 400;

struct NNUEWeights {
    int16_t l0w[768][NNUE_HIDDEN]; // feature weights: [feature][hidden]
    int16_t l0b[NNUE_HIDDEN];      // hidden bias
    int16_t l1w[2 * NNUE_HIDDEN];  // output weights
    int16_t l1b;                   // output bias
};

static NNUEWeights nnueWeights;
static bool nnueLoaded = false;

inline bool loadNNUE(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
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

// Feature indices for white accumulator (white=STM) and black accumulator (black=STM).
// White piece: whiteIdx = 0+pt*64+sq,      blackIdx = 384+pt*64+(sq^56)
// Black piece: whiteIdx = 384+pt*64+sq,    blackIdx = 0+pt*64+(sq^56)
inline void getFeatureIndices(char piece, int sq, int& whiteIdx, int& blackIdx) {
    int pt = pieceTypeIndex(piece);
    bool isWhite = (piece >= 'A' && piece <= 'Z');
    whiteIdx = (isWhite ? 0 : 384) + pt * 64 + sq;
    blackIdx = (isWhite ? 384 : 0) + pt * 64 + (sq ^ 56);
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

// Runs output layer from pre-built accumulators. Returns eval from STM's perspective.
// Loop is written to be auto-vectorizable with NEON/AVX2.
inline int nnueForward(const int16_t* __restrict__ stm_acc, const int16_t* __restrict__ ntm_acc) {
    // 512 neurons × 255² × max_l1w can exceed INT32_MAX.
    // Inner 64-element blocks stay within int32; only the 8 block sums use int64.
    int64_t output1 = 0, output2 = 0;
    const int16_t* w = nnueWeights.l1w;

    for (int i = 0; i < NNUE_HIDDEN; i += 64) {
        int32_t block = 0;
        for (int j = i; j < i + 64; j++) {
            int32_t v = stm_acc[j];
            if (v < 0) v = 0;
            if (v > NNUE_QA) v = NNUE_QA;
            block += v * v * w[j];
        }
        output1 += block;
    }
    for (int i = 0; i < NNUE_HIDDEN; i += 64) {
        int32_t block = 0;
        for (int j = i; j < i + 64; j++) {
            int32_t v = ntm_acc[j];
            if (v < 0) v = 0;
            if (v > NNUE_QA) v = NNUE_QA;
            block += v * v * w[NNUE_HIDDEN + j];
        }
        output2 += block;
    }

    int32_t output = output1 / NNUE_QA + output2 / NNUE_QA;
    output += nnueWeights.l1b;
    output = output * NNUE_SCALE / (NNUE_QA * NNUE_QB);
    return (int)output;
}
