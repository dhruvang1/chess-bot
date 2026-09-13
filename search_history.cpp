#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>
using namespace std;

// Ply cap for per-ply search-stack arrays (PV table, killers, move/piece stacks, eval stack).
static constexpr int MAX_PLY = 128;

// Move-ordering history tables (index convention throughout: pieceIdx() 0-11, P/N/B/R/Q/K
// white then black, x to-square) plus the two eval-correction tables, and the small helpers
// (pieceIdx, updateHist) that index/update them. All reset once per search via reset() --
// aged toward zero rather than zeroed outright, so relative ordering survives across
// searches while fresh updates still dominate.
struct HistoryTables {
    uint16_t killers[2 * MAX_PLY] = {};  // 2 killer slots per ply; indexed killers[2*ply(+1)]
    // History heuristic: history[pieceChar][toSquare] tracks how often a quiet move causes beta cutoffs.
    // Quiet moves that frequently cause cutoffs get ordered earlier, making LMR more effective
    // since the truly bad moves end up at high indices where they get aggressively reduced.
    // Indexed by pieceIdx() 0-11 (P/N/B/R/Q/K white, p/n/b/r/q/k black) × to-square.
    // int16_t: gravity in updateHist() keeps every entry in [-MAX_HISTORY, MAX_HISTORY].
    int16_t history[12][64] = {};
    uint16_t countermoves[12][64] = {};

    // Continuation history: contHist[prevPieceIdx][prevToSq][curPieceIdx][curToSq]
    // 1-ply: opponent's last move as context. 2-ply: our own last move as context.
    // Uses compact piece indices 0-11; int16_t entries keep each table at ~1.1MB.
    int16_t contHist[12][64][12][64] = {};
    int16_t contHist2[12][64][12][64] = {};

    // Capture history: captHist[movingPieceIdx][toSq][capturedPieceType]
    // Separates capture ordering from quiet ordering. capturedPieceType = pieceIdx(cap)/2
    // so color is ignored (white queen captured == black queen captured). ~9KB.
    int16_t captHist[12][64][6] = {};

    // Cap history entries to [-MAX_HISTORY, MAX_HISTORY] via gravity. With |bonus|
    // also capped at MAX_HISTORY (see the cutoff handler), the fixed point of this
    // recurrence stays within ±MAX_HISTORY, so int16_t storage is exact.
    static constexpr int MAX_HISTORY = 16384;
    static inline void updateHist(int16_t& entry, int bonus) {
        int v = entry + bonus - entry * std::abs(bonus) / MAX_HISTORY;
        entry = static_cast<int16_t>(v);
    }

    static constexpr int CORR_HIST_SIZE    = 16384;
    static constexpr int NONPAWN_CORR_SIZE = 65536;
    static constexpr int CORR_HIST_INERTIA = 143;
    static constexpr int CORR_HIST_CAP    = 9710;
    int32_t pawnCorrHist[2][CORR_HIST_SIZE] = {};
    int32_t nonPawnCorrHist[2][2][NONPAWN_CORR_SIZE] = {}; // [stm][side][key]

    // Maps piece char to compact 0-11 index for contHist.
    // P/N/B/R/Q/K = 0-5 (white), p/n/b/r/q/k = 6-11 (black).
    static inline int pieceIdx(char p) {
        switch (p) {
            case 'P': return 0; case 'N': return 1; case 'B': return 2;
            case 'R': return 3; case 'Q': return 4; case 'K': return 5;
            case 'p': return 6; case 'n': return 7; case 'b': return 8;
            case 'r': return 9; case 'q': return 10; case 'k': return 11;
            default:  return 0;
        }
    }

    // Peak-magnitude scan over a flat POD table. history, contHist, contHist2 and captHist
    // are all contiguous int16_t buffers regardless of their nominal dimensionality, so one
    // generic pass covers all four instead of four hand-written nested loops.
    static int maxAbsFlat(const int16_t* data, size_t count) {
        int m = 0;
        for (size_t i = 0; i < count; i++) m = std::max(m, std::abs((int)data[i]));
        return m;
    }

    // Same idea for the aging pass itself; the per-element transform (age) differs between
    // the *3/4 tables and captHist's >>=1, so it's passed in rather than hard-coded.
    template <typename F>
    static void ageFlat(int16_t* data, size_t count, F age) {
        for (size_t i = 0; i < count; i++) data[i] = age(data[i]);
    }

    void reset(int threadId) {
        memset(killers, 0, sizeof(killers));
        memset(countermoves, 0, sizeof(countermoves));

        // Instrumentation: report peak magnitudes accumulated during the prior search
        // (before aging) so we can size a history cap against real data.
        // Only thread 0 reports, to avoid interleaved output from Lazy SMP helper threads.
        if (threadId == 0) {
            int hMax   = maxAbsFlat(&history[0][0],         sizeof(history)   / sizeof(int16_t));
            int chMax  = maxAbsFlat(&contHist[0][0][0][0],  sizeof(contHist)  / sizeof(int16_t));
            int ch2Max = maxAbsFlat(&contHist2[0][0][0][0], sizeof(contHist2) / sizeof(int16_t));
            int cpMax  = maxAbsFlat(&captHist[0][0][0],     sizeof(captHist)  / sizeof(int16_t));
            cout << "info string histMax hist " << hMax << " contHist " << chMax
                 << " contHist2 " << ch2Max << " captHist " << cpMax << endl;
        }

        // Age every history table toward zero instead of zeroing it outright.
        // Preserves relative ordering while letting fresh updates dominate.
        auto age34 = [](int16_t v) { return (int16_t)(v * 3 / 4); };
        ageFlat(&history[0][0],         sizeof(history)   / sizeof(int16_t), age34);
        ageFlat(&contHist[0][0][0][0],  sizeof(contHist)  / sizeof(int16_t), age34);
        ageFlat(&contHist2[0][0][0][0], sizeof(contHist2) / sizeof(int16_t), age34);
        ageFlat(&captHist[0][0][0],     sizeof(captHist)  / sizeof(int16_t), [](int16_t v) { return (int16_t)(v >> 1); });
    }
};
