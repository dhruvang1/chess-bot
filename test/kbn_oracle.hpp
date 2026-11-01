#pragma once
// Exact K+B+N vs K solver, used ONLY as a test oracle (never linked into the engine).
//
// Everything is expressed in the "attacker = White" frame with a1 = 0, h1 = 7, a8 = 56.
// A position is (wk, bk, n, b): attacker king, defender king, knight, bishop.
// Two tables of distance-to-mate in plies, indexed by that 24-bit key:
//   A[idx] = attacker to move, mate in k plies (k odd, 1..65)
//   D[idx] = defender to move, mated in k plies (k even, 0..66)
// UNRES means "no forced mate" (a hanging-piece draw, stalemate, or not a legal position).
//
// Built by retrograde analysis (unmove generation + successor counters), then checked by an
// independent forward pass (validate()) that re-derives every entry from the Bellman equations.
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstdlib>

namespace kbn {

constexpr int UNRES = 255;
constexpr uint32_t TABLE_SIZE = 1u << 24;

inline int fileOf(int s) { return s & 7; }
inline int rankOf(int s) { return s >> 3; }
inline uint64_t bit(int s) { return 1ULL << s; }
inline int cheb(int a, int b) {
    return std::max(std::abs(fileOf(a) - fileOf(b)), std::abs(rankOf(a) - rankOf(b)));
}
inline bool isLightSq(int s) { return (fileOf(s) + rankOf(s)) & 1; }

struct Geo {
    uint64_t king[64], knight[64];
    Geo() {
        static const int kd[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        static const int nd[8][2] = {{1,2},{2,1},{-1,2},{-2,1},{1,-2},{2,-1},{-1,-2},{-2,-1}};
        for (int s = 0; s < 64; s++) {
            king[s] = knight[s] = 0;
            for (auto& d : kd) {
                int f = fileOf(s) + d[0], r = rankOf(s) + d[1];
                if (f >= 0 && f < 8 && r >= 0 && r < 8) king[s] |= bit(r * 8 + f);
            }
            for (auto& d : nd) {
                int f = fileOf(s) + d[0], r = rankOf(s) + d[1];
                if (f >= 0 && f < 8 && r >= 0 && r < 8) knight[s] |= bit(r * 8 + f);
            }
        }
    }
};
inline const Geo& geo() { static const Geo g; return g; }

// Squares a bishop on `sq` attacks given blockers `occ` (blockers themselves are included).
inline uint64_t bishopAtt(int sq, uint64_t occ) {
    static const int dr[4] = {1, 1, -1, -1}, df[4] = {1, -1, 1, -1};
    uint64_t a = 0;
    for (int d = 0; d < 4; d++) {
        int r = rankOf(sq) + dr[d], f = fileOf(sq) + df[d];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int s = r * 8 + f;
            a |= bit(s);
            if (occ & bit(s)) break;
            r += dr[d]; f += df[d];
        }
    }
    return a;
}

struct Pos { int wk, bk, n, b; };

inline uint32_t indexOf(const Pos& p) { return (uint32_t)(((p.wk * 64 + p.bk) * 64 + p.n) * 64 + p.b); }
inline Pos unpack(uint32_t i) { return {(int)(i >> 18), (int)((i >> 12) & 63), (int)((i >> 6) & 63), (int)(i & 63)}; }

// Four distinct squares and non-adjacent kings.
inline bool legalShape(const Pos& p) {
    if (p.wk == p.bk || p.wk == p.n || p.wk == p.b || p.bk == p.n || p.bk == p.b || p.n == p.b) return false;
    return cheb(p.wk, p.bk) > 1;
}

inline bool defenderInCheck(const Pos& p) {
    if (geo().knight[p.n] & bit(p.bk)) return true;
    return bishopAtt(p.b, bit(p.wk) | bit(p.n) | bit(p.bk)) & bit(p.bk);
}

struct DefMoves {
    int to[8];
    int count = 0;          // legal non-capturing king moves
    bool hasCapture = false; // a legal capture of the knight or bishop exists (=> draw)
};

inline DefMoves defenderMoves(const Pos& p) {
    DefMoves m;
    const Geo& g = geo();
    uint64_t bAtt = bishopAtt(p.b, bit(p.wk) | bit(p.n));  // defender king lifted off the board
    uint64_t nAtt = g.knight[p.n];
    uint64_t wkAtt = g.king[p.wk];
    uint64_t targets = g.king[p.bk];
    while (targets) {
        int t = __builtin_ctzll(targets); targets &= targets - 1;
        if (wkAtt & bit(t)) continue;
        if (t == p.n) { if (!(bAtt & bit(t))) m.hasCapture = true; continue; }
        if (t == p.b) { if (!(nAtt & bit(t))) m.hasCapture = true; continue; }
        if ((nAtt | bAtt) & bit(t)) continue;
        m.to[m.count++] = t;
    }
    return m;
}

struct AtkMove { int piece; int to; };  // piece: 0 = king, 1 = knight, 2 = bishop

inline int attackerMoves(const Pos& p, AtkMove* out) {
    const Geo& g = geo();
    uint64_t occ = bit(p.wk) | bit(p.bk) | bit(p.n) | bit(p.b);
    int c = 0;
    uint64_t a = g.king[p.wk] & ~occ & ~g.king[p.bk];
    while (a) { out[c++] = {0, __builtin_ctzll(a)}; a &= a - 1; }
    a = g.knight[p.n] & ~occ;
    while (a) { out[c++] = {1, __builtin_ctzll(a)}; a &= a - 1; }
    a = bishopAtt(p.b, occ) & ~occ;
    while (a) { out[c++] = {2, __builtin_ctzll(a)}; a &= a - 1; }
    return c;
}

inline Pos applyAttacker(Pos p, const AtkMove& m) {
    if (m.piece == 0) p.wk = m.to; else if (m.piece == 1) p.n = m.to; else p.b = m.to;
    return p;
}

inline bool legalAttackerToMove(const Pos& p) { return legalShape(p) && !defenderInCheck(p); }

struct Table {
    std::vector<uint8_t> A, D;
    int maxA = 0, maxD = 0;
    std::vector<uint32_t> countA, countD;  // number of positions per DTM level
};

inline Table solve() {
    Table t;
    t.A.assign(TABLE_SIZE, UNRES);
    t.D.assign(TABLE_SIZE, UNRES);
    t.countA.assign(128, 0);
    t.countD.assign(128, 0);
    const Geo& g = geo();
    constexpr uint8_t NEVER = 0xFF;
    std::vector<uint8_t> cnt(TABLE_SIZE, NEVER);  // defender positions: successors still unresolved
    std::vector<uint32_t> curD;

    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        Pos p = unpack(i);
        if (!legalShape(p)) continue;
        DefMoves m = defenderMoves(p);
        if (m.hasCapture) continue;
        if (m.count == 0) {
            if (defenderInCheck(p)) { t.D[i] = 0; curD.push_back(i); t.countD[0]++; }
            continue;  // stalemate otherwise
        }
        cnt[i] = (uint8_t)m.count;
    }

    int L = 0;
    while (!curD.empty()) {
        // defender lost in L  ->  attacker wins in L+1
        std::vector<uint32_t> nextA;
        for (uint32_t idx : curD) {
            Pos x = unpack(idx);
            uint64_t occ = bit(x.wk) | bit(x.bk) | bit(x.n) | bit(x.b);
            auto tryPred = [&](Pos y) {
                if (!legalAttackerToMove(y)) return;
                uint32_t yi = indexOf(y);
                if (t.A[yi] != UNRES) return;
                t.A[yi] = (uint8_t)(L + 1);
                t.countA[L + 1]++;
                nextA.push_back(yi);
            };
            uint64_t s = g.king[x.wk] & ~occ;
            while (s) { int q = __builtin_ctzll(s); s &= s - 1; Pos y = x; y.wk = q; tryPred(y); }
            s = g.knight[x.n] & ~occ;
            while (s) { int q = __builtin_ctzll(s); s &= s - 1; Pos y = x; y.n = q; tryPred(y); }
            static const int dr[4] = {1, 1, -1, -1}, df[4] = {1, -1, 1, -1};
            for (int d = 0; d < 4; d++) {
                int r = rankOf(x.b) + dr[d], f = fileOf(x.b) + df[d];
                while (r >= 0 && r < 8 && f >= 0 && f < 8) {
                    int q = r * 8 + f;
                    if (occ & bit(q)) break;
                    Pos y = x; y.b = q; tryPred(y);
                    r += dr[d]; f += df[d];
                }
            }
        }
        // attacker wins in L+1  ->  defender lost in L+2 once every successor is resolved
        std::vector<uint32_t> nextD;
        for (uint32_t idx : nextA) {
            Pos x = unpack(idx);
            uint64_t occ = bit(x.wk) | bit(x.n) | bit(x.b);
            uint64_t s = g.king[x.bk] & ~occ & ~g.king[x.wk];
            while (s) {
                int u = __builtin_ctzll(s); s &= s - 1;
                Pos z = x; z.bk = u;
                uint32_t zi = indexOf(z);
                if (cnt[zi] == NEVER || t.D[zi] != UNRES) continue;
                if (--cnt[zi] == 0) {
                    t.D[zi] = (uint8_t)(L + 2);
                    t.countD[L + 2]++;
                    nextD.push_back(zi);
                }
            }
        }
        curD.swap(nextD);
        L += 2;
    }
    for (int i = 0; i < 128; i++) {
        if (t.countA[i]) t.maxA = i;
        if (t.countD[i]) t.maxD = i;
    }
    return t;
}

// Independent forward check of every table entry against the definition. Returns the number of
// inconsistent entries (0 = the table is exactly right for the game as modelled).
inline long validate(const Table& t) {
    long bad = 0;
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        Pos p = unpack(i);
        if (!legalShape(p)) {
            if (t.A[i] != UNRES || t.D[i] != UNRES) bad++;
            continue;
        }
        // defender to move
        {
            DefMoves m = defenderMoves(p);
            int expect = UNRES;
            if (!m.hasCapture) {
                if (m.count == 0) { if (defenderInCheck(p)) expect = 0; }
                else {
                    int worst = 0; bool ok = true;
                    for (int k = 0; k < m.count && ok; k++) {
                        Pos q = p; q.bk = m.to[k];
                        int v = t.A[indexOf(q)];
                        if (v == UNRES) ok = false; else worst = std::max(worst, v);
                    }
                    if (ok) expect = worst + 1;
                }
            }
            if (t.D[i] != expect) bad++;
        }
        // attacker to move
        {
            int expect = UNRES;
            if (!defenderInCheck(p)) {
                AtkMove mv[64];
                int c = attackerMoves(p, mv);
                int best = UNRES;
                for (int k = 0; k < c; k++) {
                    int v = t.D[indexOf(applyAttacker(p, mv[k]))];
                    if (v != UNRES) best = std::min(best, v);
                }
                if (best != UNRES) expect = best + 1;
            }
            if (t.A[i] != expect) bad++;
        }
    }
    return bad;
}

}  // namespace kbn
