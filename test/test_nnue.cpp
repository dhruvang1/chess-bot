#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "../nnue.hpp"

struct PieceOnSquare { char piece; int sq; };

struct Position {
    std::vector<PieceOnSquare> pieces;
    int wKingSq = -1, bKingSq = -1;
    bool whiteToMove = true;
};

Position parseFEN(const std::string& fen) {
    Position pos;
    int rank = 7, file = 0;
    size_t i = 0;
    for (; i < fen.size() && fen[i] != ' '; i++) {
        if (fen[i] == '/') { rank--; file = 0; }
        else if (fen[i] >= '1' && fen[i] <= '8') file += fen[i] - '0';
        else {
            int sq = rank * 8 + file;
            pos.pieces.push_back({fen[i], sq});
            if (fen[i] == 'K') pos.wKingSq = sq;
            if (fen[i] == 'k') pos.bKingSq = sq;
            file++;
        }
    }
    if (i + 1 < fen.size()) pos.whiteToMove = (fen[i + 1] == 'w');
    return pos;
}

void runDiagnostics(const std::string& fen, const std::string& label) {
    Position pos = parseFEN(fen);
    int pieceCount = (int)pos.pieces.size();

    std::cout << "=== " << label << " ===" << std::endl;
    std::cout << "FEN: " << fen << std::endl;
    std::cout << "wKingSq=" << pos.wKingSq << " bKingSq=" << pos.bKingSq
              << " pieces=" << pieceCount
              << " stm=" << (pos.whiteToMove ? "white" : "black") << std::endl;

    // --- Stage A: Raw Weights ---
    std::cout << "\n--- Stage A: Raw Weights ---" << std::endl;
    std::cout << "l0w[0][0..3]:";
    for (int i = 0; i < 4; i++) std::cout << " " << nnueWeights.l0w[0][i];
    std::cout << std::endl;
    std::cout << "l0b[0..3]:";
    for (int i = 0; i < 4; i++) std::cout << " " << nnueWeights.l0b[i];
    std::cout << std::endl;

    const int16_t* l1w_flat = reinterpret_cast<const int16_t*>(nnueWeights.l1w);
    std::cout << "l1w_flat[0..7]:";
    for (int i = 0; i < 8; i++) std::cout << " " << l1w_flat[i];
    std::cout << std::endl;
    std::cout << "l1b[0..7]:";
    for (int i = 0; i < NNUE_OUTPUT_BUCKETS; i++) std::cout << " " << nnueWeights.l1b[i];
    std::cout << std::endl;

    int64_t l0w_sum = 0, l0b_sum = 0, l1w_sum = 0, l1b_sum = 0;
    for (int i = 0; i < NNUE_INPUT; i++)
        for (int j = 0; j < NNUE_HIDDEN; j++)
            l0w_sum += nnueWeights.l0w[i][j];
    for (int i = 0; i < NNUE_HIDDEN; i++) l0b_sum += nnueWeights.l0b[i];
    for (int i = 0; i < NNUE_OUTPUT_BUCKETS * 2 * NNUE_HIDDEN; i++) l1w_sum += l1w_flat[i];
    for (int i = 0; i < NNUE_OUTPUT_BUCKETS; i++) l1b_sum += nnueWeights.l1b[i];
    std::cout << "checksum: l0w=" << l0w_sum << " l0b=" << l0b_sum
              << " l1w=" << l1w_sum << " l1b=" << l1b_sum << std::endl;

    // --- Stage B: Feature Indices ---
    std::cout << "\n--- Stage B: Feature Indices ---" << std::endl;
    std::cout << "kingBucket(wK)=" << kingBucket(pos.wKingSq)
              << " kingFlip(wK)=" << kingFlip(pos.wKingSq) << std::endl;
    int bkFlipped = pos.bKingSq ^ 56;
    std::cout << "kingBucket(bK)=" << kingBucket(bkFlipped)
              << " kingFlip(bK)=" << kingFlip(bkFlipped)
              << " (bKingSq^56=" << bkFlipped << ")" << std::endl;

    auto sorted = pos.pieces;
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.sq < b.sq; });
    for (auto& p : sorted) {
        int wi, bi;
        getFeatureIndices(p.piece, p.sq, pos.wKingSq, pos.bKingSq, wi, bi);
        std::cout << "  " << p.piece << " sq=" << p.sq
                  << " wIdx=" << wi << " bIdx=" << bi << std::endl;
    }

    // --- Stage C: Accumulator ---
    std::cout << "\n--- Stage C: Accumulator ---" << std::endl;
    int16_t wAcc[NNUE_HIDDEN], bAcc[NNUE_HIDDEN];
    memcpy(wAcc, nnueWeights.l0b, sizeof(wAcc));
    memcpy(bAcc, nnueWeights.l0b, sizeof(bAcc));
    for (auto& p : sorted) {
        int wi, bi;
        getFeatureIndices(p.piece, p.sq, pos.wKingSq, pos.bKingSq, wi, bi);
        accAdd(wAcc, wi);
        accAdd(bAcc, bi);
    }

    std::cout << "wAcc[0..7]:";
    for (int i = 0; i < 8; i++) std::cout << " " << wAcc[i];
    std::cout << std::endl;
    std::cout << "bAcc[0..7]:";
    for (int i = 0; i < 8; i++) std::cout << " " << bAcc[i];
    std::cout << std::endl;
    int64_t ws = 0, bs = 0;
    for (int i = 0; i < NNUE_HIDDEN; i++) { ws += wAcc[i]; bs += bAcc[i]; }
    std::cout << "sum(wAcc)=" << ws << " sum(bAcc)=" << bs << std::endl;

    // --- Stage D: Forward Pass ---
    std::cout << "\n--- Stage D: Forward Pass ---" << std::endl;
    int bucket = (pieceCount - 2) / 4;
    std::cout << "bucket=" << bucket << std::endl;

    const int16_t* stm = pos.whiteToMove ? wAcc : bAcc;
    const int16_t* ntm = pos.whiteToMove ? bAcc : wAcc;

    // Forward pass — mirrors nnueForward() exactly
    int64_t dot1 = 0, dot2 = 0;
    const int16_t* sw = nnueWeights.l1w[bucket];
    const int16_t* nw = nnueWeights.l1w[bucket] + NNUE_HIDDEN;
    for (int j = 0; j < NNUE_HIDDEN; j++) {
        int32_t v = stm[j];
        v = v < 0 ? 0 : (v > NNUE_QA ? NNUE_QA : v);
        dot1 += (int64_t)v * v * sw[j];
    }
    for (int j = 0; j < NNUE_HIDDEN; j++) {
        int32_t v = ntm[j];
        v = v < 0 ? 0 : (v > NNUE_QA ? NNUE_QA : v);
        dot2 += (int64_t)v * v * nw[j];
    }

    std::cout << "stm_dot=" << dot1 << " ntm_dot=" << dot2 << std::endl;
    int32_t out = (int32_t)(dot1 / NNUE_QA) + (int32_t)(dot2 / NNUE_QA);
    std::cout << "after_div_QA=" << out << std::endl;
    out += nnueWeights.l1b[bucket];
    std::cout << "after_bias=" << out
              << " (l1b[" << bucket << "]=" << nnueWeights.l1b[bucket] << ")" << std::endl;
    int eval = out * NNUE_SCALE / (NNUE_QA * NNUE_QB);
    std::cout << "eval_cp=" << eval << std::endl;
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test_nnue <path_to_nnue_binary>" << std::endl;
        return 1;
    }
    if (!loadNNUE(argv[1])) {
        std::cerr << "Failed to load: " << argv[1] << std::endl;
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    std::cout << "Loaded: " << argv[1] << std::endl;
    std::cout << "file_size=" << f.tellg() << " expected=" << sizeof(NNUEWeights) << std::endl;
    std::cout << std::endl;

    // Stockfish-evaluated positions
    runDiagnostics(
        "r1bqkb1r/pppp1ppp/5n2/4p3/2BnP3/5N2/PPPP1PPP/RNBQ1RK1 w kq - 0 5",
        "SF: white +1.1 (110 cp)");

    runDiagnostics(
        "r7/1r1k2p1/n2P1p1p/p2pp3/P5P1/BPR5/2K2P1P/2R5 b - - 3 38",
        "SF: black +2.6 (-260 cp from STM=black)");

    runDiagnostics(
        "rr6/3k1ppp/n1pP4/p3pb2/2R5/1P2B3/P2KBPPP/R7 b - - 2 21",
        "SF: eval 0 (0 cp)");

    runDiagnostics(
        "r1bqr1k1/p4pp1/n2b1n1p/2pp4/3P3B/P2BP2P/2Q1NPP1/R2N1RK1 b - - 1 15",
        "SF: white +0.4 (-40 cp from STM=black)");

    runDiagnostics(
        "4qk2/4r3/4pQ2/2bP4/8/p1p1N1P1/5PK1/1R6 b - - 10 65",
        "SF: white +5.7 (-570 cp from STM=black)");

    runDiagnostics(
        "r1r3k1/5ppp/2q1p3/p3B3/1ppPn2b/2P1P2P/P1Q1NPP1/2R2RK1 b - - 5 23",
        "SF: black +2.7 (270 cp from STM=black)");

    return 0;
}
