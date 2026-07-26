#pragma once
// Endgame-specific eval helpers used by MagicBoard::getBoardEval(). A genuinely
// standalone header (all functions take the board state they need as explicit
// parameters) so it compiles and lints on its own — split out purely to keep
// magicBoard.cpp from growing without bound.
#include <cstdint>
#include <bitset>
#include <vector>
#include <algorithm>
#include <cstdlib>

namespace EndgameEval {

using std::bitset;
using std::vector;
using std::min;
using std::max;
using std::abs;

// KBN vs K: square table oriented toward target corner a1.
// Higher value = losing king is closer to being mated.
// Non-zero everywhere so the engine always has directional guidance.
constexpr int kbnTable[64] = {
     700,  560,  430,  310,  200,  110,   50,   10,
     560,  440,  320,  220,  140,   70,   30,    0,
     430,  320,  220,  140,   80,   40,   10,    0,
     310,  220,  140,   80,   40,   10,    0,    0,
     200,  140,   80,   40,   10,    0,    0,    0,
     110,   70,   40,   10,    0,    0,    0,    0,
      50,   30,   10,    0,    0,    0,    0,    0,
      10,    0,    0,    0,    0,    0,    0,    0,
};

// Bonus for driving the losing king to the corner/edge, bringing the
// winning king close, and rewarding all winning pieces for proximity
// to the losing king (tropism). Fires only in basic mating endgames
// where NNUE lacks training data for the forcing patterns.
inline int matingBonus(bool winnerIsWhite, bool sideToMoveIsWhite,
                       uint64_t whiteKing, uint64_t blackKing,
                       uint64_t allWhite, uint64_t allBlack) {
    int wkSq = __builtin_ctzll(whiteKing);
    int bkSq = __builtin_ctzll(blackKing);
    int loserSq  = winnerIsWhite ? bkSq  : wkSq;
    int winnerSq = winnerIsWhite ? wkSq  : bkSq;

    int lf = loserSq & 7, lr = loserSq >> 3;
    int edgeDist = min({lf, 7 - lf, lr, 7 - lr});

    int df = abs((winnerSq & 7) - lf);
    int dr = abs((winnerSq >> 3) - lr);
    int kingDist = max(df, dr);

    // Push losing king to edge/corner (+0..+600), bring winning king close (+0..+350)
    int bonus = (3 - edgeDist) * 200 + (7 - kingDist) * 50;

    // Piece tropism: reward every winning piece (excluding own king, already counted)
    // for proximity to the losing king. Uses Chebyshev distance so a rook cutting
    // a rank/file scores the same as a king adjacent — directionally correct and
    // naturally guides rook/queen/bishop toward the mating zone without special-casing.
    uint64_t pieces = (winnerIsWhite ? allWhite : allBlack)
                    & ~(winnerIsWhite ? whiteKing : blackKing);
    while (pieces) {
        int psq = __builtin_ctzll(pieces); pieces &= pieces - 1;
        int pDist = max(abs((psq & 7) - lf), abs((psq >> 3) - lr));
        bonus += (7 - pDist) * 15;
    }

    return winnerIsWhite == sideToMoveIsWhite ? bonus : -bonus;
}

// Transform a square so that the nearest correct corner maps to a1 (index 0)
// for lookup in kbnTable[]. Dark bishop correct corners: a1, h8.
// Light bishop correct corners: h1, a8.
inline int transformForKBN(int sq, bool lightBishop) {
    int file = sq & 7, rank = sq >> 3;
    if (lightBishop) {
        int distH1 = max(7 - file, rank);
        int distA8 = max(file, 7 - rank);
        return (distH1 <= distA8) ? (sq ^ 7) : (sq ^ 56);
    } else {
        int distA1 = max(file, rank);
        int distH8 = max(7 - file, 7 - rank);
        return (distA1 <= distH8) ? sq : (sq ^ 63);
    }
}

// KBN vs K: must drive the king to a corner matching the bishop's color.
// Light-square bishop → h1 (sq 7) or a8 (sq 56).
// Dark-square bishop  → a1 (sq 0) or h8 (sq 63).
inline int kbnMatingBonus(bool winnerIsWhite, bool sideToMoveIsWhite,
                          uint64_t whiteKing, uint64_t blackKing,
                          uint64_t whiteBishops, uint64_t blackBishops,
                          uint64_t allWhite, uint64_t allBlack,
                          uint64_t lightSquareMask) {
    int wkSq = __builtin_ctzll(whiteKing);
    int bkSq = __builtin_ctzll(blackKing);
    int loserSq  = winnerIsWhite ? bkSq  : wkSq;
    int winnerSq = winnerIsWhite ? wkSq  : bkSq;

    uint64_t bishops = winnerIsWhite ? whiteBishops : blackBishops;
    bool lightBishop = (bishops & lightSquareMask) != 0;

    int transformed = transformForKBN(loserSq, lightBishop);
    int bonus = 500 + kbnTable[transformed];

    int lf = loserSq & 7, lr = loserSq >> 3;

    // Reward losing king being in the outer 2 rows/files
    int fileDist = min(lf, 7 - lf);  // 0 on edge, 1 one step in, 2-3 center
    int rankDist = min(lr, 7 - lr);
    int outerDist = min(fileDist, rankDist);  // 0 = on edge, 1 = second row
    if (outerDist == 0) bonus += 200;
    else if (outerDist == 1) bonus += 100;

    int df = abs((winnerSq & 7) - lf);
    int dr = abs((winnerSq >> 3) - lr);
    int kingDist = max(df, dr);
    bonus += (7 - kingDist) * 50;

    // Piece tropism: reward knight and bishop for proximity to the losing king.
    uint64_t pieces = (winnerIsWhite ? allWhite : allBlack)
                    & ~(winnerIsWhite ? whiteKing : blackKing);
    while (pieces) {
        int psq = __builtin_ctzll(pieces); pieces &= pieces - 1;
        int pDist = max(abs((psq & 7) - lf), abs((psq >> 3) - lr));
        bonus += (7 - pDist) * 15;
    }

    return winnerIsWhite == sideToMoveIsWhite ? bonus : -bonus;
}

// ---- KPK bitbase: exact win/draw classification for King+Pawn vs King. ----
// Ported from historical Stockfish (src/bitbase.cpp, pre-NNUE, commit
// af110e02~1) — opposition/key-squares make this endgame NOT always winning
// (a rook pawn draws once the defending king reaches the corner), and that's
// a discontinuous condition NNUE and plain search both miss. Generated once
// via retrograde analysis over the full state space on first use — self
// contained, no external file, ~24KB resident (196,608 positions / 8).
//
// All squares here are in the "normalized" frame: the side with the pawn is
// always White, and the pawn's file is always mirrored into A-D.
struct KPKPosition {
    static constexpr unsigned MAX_INDEX = 2u * 24u * 64u * 64u; // stm*psq(A-D)*wksq*bksq
    enum Result { INVALID = 0, UNKNOWN = 1, DRAW = 2, WIN = 4 };

    int stm, wksq, bksq, psq;
    int result;

    static uint64_t kingAttacksFrom(int sq) {
        int r = sq >> 3, c = sq & 7;
        uint64_t bb = 0;
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) bb |= 1ULL << (nr * 8 + nc);
            }
        return bb;
    }

    // Only the strong side's (White's, in this normalized frame) pawn attacks matter.
    static uint64_t whitePawnAttacksFrom(int sq) {
        int r = sq >> 3, c = sq & 7;
        uint64_t bb = 0;
        if (r < 7 && c > 0) bb |= 1ULL << (sq + 7);
        if (r < 7 && c < 7) bb |= 1ULL << (sq + 9);
        return bb;
    }

    static int kingDistance(int a, int b) {
        return max(abs((a >> 3) - (b >> 3)), abs((a & 7) - (b & 7)));
    }

    // bit 0-5: wksq, 6-11: bksq, 12: stm, 13-14: pawn file (A-D), 15-17: 6-rank
    static unsigned index(int stm, int bksq, int wksq, int psq) {
        return (unsigned)wksq | ((unsigned)bksq << 6) | ((unsigned)stm << 12)
             | ((unsigned)(psq & 7) << 13) | ((unsigned)(6 - (psq >> 3)) << 15);
    }

    KPKPosition() = default;
    explicit KPKPosition(unsigned idx) {
        wksq = idx & 0x3F;
        bksq = (idx >> 6) & 0x3F;
        stm  = (idx >> 12) & 0x01;
        psq  = ((6 - ((idx >> 15) & 0x7)) << 3) | ((idx >> 13) & 0x3);

        if (kingDistance(wksq, bksq) <= 1 || wksq == psq || bksq == psq
            || (stm == 0 && (whitePawnAttacksFrom(psq) & (1ULL << bksq))))
            result = INVALID;
        else if (stm == 0 && (psq >> 3) == 6 && wksq != psq + 8
                 && (kingDistance(bksq, psq + 8) > 1 || kingDistance(wksq, psq + 8) == 1))
            result = WIN;
        else if (stm == 1
                 && (!(kingAttacksFrom(bksq) & ~(kingAttacksFrom(wksq) | whitePawnAttacksFrom(psq)))
                     || (kingAttacksFrom(bksq) & ~kingAttacksFrom(wksq) & (1ULL << psq))))
            result = DRAW;
        else
            result = UNKNOWN;
    }

    int classify(vector<KPKPosition>& db) {
        int good = (stm == 0) ? WIN : DRAW;
        int bad  = (stm == 0) ? DRAW : WIN;

        int r = INVALID;
        uint64_t b = kingAttacksFrom(stm == 0 ? wksq : bksq);
        while (b) {
            int to = __builtin_ctzll(b); b &= b - 1;
            r |= (stm == 0) ? db[index(1, bksq, to, psq)].result
                            : db[index(0, to, wksq, psq)].result;
        }
        if (stm == 0) {
            if ((psq >> 3) < 6)
                r |= db[index(1, bksq, wksq, psq + 8)].result;
            if ((psq >> 3) == 1 && psq + 8 != wksq && psq + 8 != bksq)
                r |= db[index(1, bksq, wksq, psq + 16)].result;
        }
        result = (r & good) ? good : (r & UNKNOWN) ? UNKNOWN : bad;
        return result;
    }
};

inline const bitset<KPKPosition::MAX_INDEX>& kpkBitbase() {
    static const bitset<KPKPosition::MAX_INDEX> table = [] {
        bitset<KPKPosition::MAX_INDEX> bb;
        vector<KPKPosition> db(KPKPosition::MAX_INDEX);
        for (unsigned idx = 0; idx < KPKPosition::MAX_INDEX; idx++)
            db[idx] = KPKPosition(idx);

        bool repeat = true;
        while (repeat) {
            repeat = false;
            for (unsigned idx = 0; idx < KPKPosition::MAX_INDEX; idx++)
                if (db[idx].result == KPKPosition::UNKNOWN
                    && db[idx].classify(db) != KPKPosition::UNKNOWN)
                    repeat = true;
        }
        for (unsigned idx = 0; idx < KPKPosition::MAX_INDEX; idx++)
            if (db[idx].result == KPKPosition::WIN) bb.set(idx);
        return bb;
    }();
    return table;
}

// wksq/wpsq/bksq/stm must already be normalized (strong side = White, pawn file A-D).
inline bool kpkWin(int wksq, int wpsq, int bksq, int stm) {
    return kpkBitbase()[KPKPosition::index(stm, bksq, wksq, wpsq)];
}

} // namespace EndgameEval
