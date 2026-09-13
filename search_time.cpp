#pragma once
#include <algorithm>
#include <climits>
#include "magicBoard.cpp"  // for MagicBoard; pragma-once no-ops when search.cpp already pulled it in
using namespace std;

// Time-control state for one search: soft/hard limits (re-derived once per `go` by
// compute()) plus the bank of time saved from previous moves. Mirrors Stockfish's
// TimeManagement, kept small since a Search instance only ever owns one.
struct SearchTime {
    long softLimitMs{};
    long hardLimitMs{};
    long bankMs = 0;  // saved time from previous moves
    int incMs = 0;    // increment for current time control, used for timeScale floor

    static constexpr int MOVE_OVERHEAD_MS = 20;  // reserve for GUI/UCI round-trip latency

    // Per-search reset. bankMs is NOT touched here -- it persists across searches
    // (Search::getBestMove credits unused soft-limit time back into it).
    void reset() {
        softLimitMs = LONG_MAX;
        hardLimitMs = LONG_MAX;
    }

    // phase-aware time allocation: spend less in opening, more in middle game.
    // Divisors interpolate between no-increment (conservative, avoids flagging)
    // and full-increment (aggressive) based on actual increment: t=0 at 0ms, t=1 at 1s+.
    void compute(MagicBoard* board, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        const int actualTimeLeft = (board->turn == MagicBoard::WHITE) ? whiteTimeMs : blackTimeMs;
        int myInc = (board->turn == MagicBoard::WHITE) ? whiteIncMs : blackIncMs;
        int myTimeLeft = max(1, actualTimeLeft - MOVE_OVERHEAD_MS);

        incMs = myInc;
        float t = min(1.0f, (float)myInc / 1000.0f);
        int divisorNoInc, divisorFullInc;
        if (board->moveCount() < 16) {
            // first 16 plies of opening: rely on development patterns, save time
            divisorNoInc = 100;  divisorFullInc = 40;
        } else if (board->moveCount() < 32) {
            // late opening / early middle game
            divisorNoInc = 80;   divisorFullInc = 35;
        } else if (board->moveCount() < 64) {
            // pure middle game: spend the most here
            divisorNoInc = 50;   divisorFullInc = 18;
        } else {
            divisorNoInc = 60;   divisorFullInc = 22;
        }
        int divisor = (int)(divisorNoInc * (1.0f - t) + divisorFullInc * t);
        softLimitMs = myTimeLeft / divisor + ((long)myInc * 0.8f);

        // limit the soft time limit to 50% of the time left
        softLimitMs = min(softLimitMs, (long)(myTimeLeft * 0.5f));

        // tiered hard limits: expressed as multiples of soft limit
        // so they scale naturally with any time control
        if (myTimeLeft < 10 * softLimitMs) {
            // ~10 moves left at current pace — panic, no extensions
            hardLimitMs = softLimitMs;
        } else if (myTimeLeft < 20 * softLimitMs) {
            // ~20 moves left — tight, small extension allowed
            hardLimitMs = 2 * softLimitMs;
        } else {
            // plenty of time left — normal extension + proportional reserve
            long reserve = myTimeLeft / 20;  // keep 5% of clock as cushion
            hardLimitMs = min(3 * softLimitMs, myTimeLeft - reserve);
        }

        softLimitMs = max(softLimitMs, 1L);
        hardLimitMs = max(hardLimitMs, 1L);
    }
};
