#pragma once
// K+B+N vs K evaluation: a tiny network distilled from the exact mate distance.
//
// Why a network: the win is long (up to 33 moves) and needs a precise piece "dance" (the W-manoeuvre
// when the defender sits in the wrong-colour corner). Hand-written terms -- distance to the right corner,
// king proximity, piece tropism, per-piece square tables conditioned on the defender king -- measured at
// 45-55% optimal-move accuracy and converted only ~half of random positions inside the 50-move rule
// against best defence. Mate distance has no compact closed form, so we learn it instead.
//
// What it is: eval = BASE + (70 - predictedMateDistance) * SCALE, where predictedMateDistance (plies) comes
// from a one-hidden-layer ReLU net over five one-hot features -- defender-king square, the three attackers'
// squares, side to move -- after folding the position by the board's symmetries. It is an evaluation, not a
// table: it is a few dozen KB of weights, never probed for exact results, and the search still has to find
// the mate. The weights are produced by test/kbn_train.cpp (which solves K+B+N vs K by retrograde analysis
// offline as its teacher) and checked end to end by test/kbn_verify.cpp.
//
// Everything here is standalone (no board dependency) so the trainer and the engine share one definition of
// the features and the integer inference.
#include <cstdint>
#include <array>

#ifndef KBN_TRAINING
#include "kbnNetWeights.hpp"
#endif

namespace KbnEval {

constexpr int WEDGE_SQUARES = 20;                       // defender-king squares left after folding the symmetries
constexpr int ROWS_BK       = WEDGE_SQUARES;
constexpr int ROWS_PIECE    = 64;
constexpr int ROWS_BISHOP   = 32;                       // the bishop always lands on a dark square after folding
constexpr int ROW_WK        = ROWS_BK;
constexpr int ROW_KNIGHT    = ROW_WK + ROWS_PIECE;
constexpr int ROW_BISHOP    = ROW_KNIGHT + ROWS_PIECE;
constexpr int ROW_STM       = ROW_BISHOP + ROWS_BISHOP;
constexpr int NUM_ROWS      = ROW_STM + 2;
constexpr int ACTIVE_ROWS   = 5;

constexpr int BASE  = 1500;   // centipawns for "won, mate distance 70 plies"
constexpr int SCALE = 20;     // centipawns per ply of mate distance (tuned: pruning margins treat smaller steps as noise)
constexpr int PRED_ONE = 1024; // fixed-point unit of a predicted ply

// Defender-king squares inside the fundamental wedge {file <= rank, file + rank <= 7}, numbered 0..19.
constexpr std::array<int8_t, 64> WEDGE = [] {
    std::array<int8_t, 64> w{};
    int k = 0;
    for (int s = 0; s < 64; s++) w[s] = -1;
    for (int f = 0; f < 8; f++)
        for (int r = 0; r < 8; r++)
            if (f <= r && f + r <= 7) w[r * 8 + f] = (int8_t)k++;
    return w;
}();

inline bool isDark(int sq) { return (((sq & 7) + (sq >> 3)) & 1) == 0; }
inline int  transposeSq(int sq) { return (sq & 7) * 8 + (sq >> 3); }

// Fold a position (attacker's king, defender's king, knight, bishop; a1 = 0) by the board symmetries so that
// the bishop is on a dark square and the defender king is in the wedge. 8 equivalent positions -> 1.
inline void fold(int& wk, int& bk, int& n, int& b) {
    if (!isDark(b)) { wk ^= 56; bk ^= 56; n ^= 56; b ^= 56; }                         // mirror ranks: light bishop -> dark
    if ((bk & 7) + (bk >> 3) > 7) { wk ^= 63; bk ^= 63; n ^= 63; b ^= 63; }           // rotate 180 degrees
    if ((bk & 7) > (bk >> 3)) { wk = transposeSq(wk); bk = transposeSq(bk); n = transposeSq(n); b = transposeSq(b); }
}

// Active input rows for a position; `rows` receives ACTIVE_ROWS indices in [0, NUM_ROWS).
inline void featureRows(int wk, int bk, int n, int b, bool attackerToMove, int* rows) {
    fold(wk, bk, n, b);
    rows[0] = WEDGE[bk];
    rows[1] = ROW_WK + wk;
    rows[2] = ROW_KNIGHT + n;
    rows[3] = ROW_BISHOP + (b >> 3) * 4 + ((b & 7) >> 1);
    rows[4] = ROW_STM + (attackerToMove ? 0 : 1);
}

// Integer inference. Weights: rowWeights[NUM_ROWS][hidden] and hiddenBias[hidden] scaled by eScale, outWeights
// by wScale, outBias in 1/PRED_ONE plies. Returns the predicted mate distance in 1/PRED_ONE plies.
inline int64_t predictFixed(const int16_t* rowWeights, const int16_t* hiddenBias, const int16_t* outWeights,
                            int32_t outBias, int hidden, int eScale, int wScale, const int* rows) {
    // int32 is safe: the trainer picks eScale/wScale so that max|hidden| * sum|outWeights| < 2^31.
    int32_t acc = 0;
    for (int j = 0; j < hidden; j++) {
        int32_t h = hiddenBias[j];
        for (int k = 0; k < ACTIVE_ROWS; k++) h += rowWeights[(size_t)rows[k] * hidden + j];
        if (h > 0) acc += h * (int32_t)outWeights[j];
    }
    return (int64_t)acc * PRED_ONE / ((int64_t)eScale * wScale) + outBias;
}

#ifndef KBN_TRAINING
static_assert(sizeof(ROW_WEIGHTS) == sizeof(int16_t) * NUM_ROWS * HIDDEN,
              "kbnNetWeights.hpp does not match the feature layout in kbnEval.hpp -- regenerate it with build/kbn_train");

// Evaluation in centipawns from the side to move's point of view, for a position with K+B+N (`winnerIsWhite`)
// against a bare king.
inline int score(bool winnerIsWhite, bool sideToMoveIsWhite, uint64_t whiteKing, uint64_t blackKing,
                 uint64_t whiteBishops, uint64_t blackBishops, uint64_t whiteKnights, uint64_t blackKnights) {
    int wk, bk, n, b;
    if (winnerIsWhite) {
        wk = __builtin_ctzll(whiteKing); bk = __builtin_ctzll(blackKing);
        n = __builtin_ctzll(whiteKnights); b = __builtin_ctzll(whiteBishops);
    } else {  // look from Black's side: mirror ranks so the attacker is "White"
        wk = __builtin_ctzll(blackKing) ^ 56; bk = __builtin_ctzll(whiteKing) ^ 56;
        n = __builtin_ctzll(blackKnights) ^ 56; b = __builtin_ctzll(blackBishops) ^ 56;
    }
    const bool attackerToMove = winnerIsWhite == sideToMoveIsWhite;
    int rows[ACTIVE_ROWS];
    featureRows(wk, bk, n, b, attackerToMove, rows);
    int64_t pred = predictFixed(&ROW_WEIGHTS[0][0], HIDDEN_BIAS, OUT_WEIGHTS, OUT_BIAS, HIDDEN, E_SCALE, W_SCALE, rows);
    if (pred < -5 * PRED_ONE) pred = -5 * PRED_ONE;
    if (pred > 80 * PRED_ONE) pred = 80 * PRED_ONE;
    const int bonus = BASE + (int)(((int64_t)(70 * PRED_ONE) - pred) * SCALE / PRED_ONE);
    return attackerToMove ? bonus : -bonus;
}
#endif

}  // namespace KbnEval
