#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <climits>
using namespace std;

static const int TTFlagAlpha = 0;  // we couldn't reach the alpha of the position
static const int TTFlagExact = 1;  // we received the definite evaluation
static const int TTFlagBeta = 2;  // the move caused a beta cutoff

// Bucket count (each bucket holds 2 TTEntry slots). Resized at runtime via the
// Hash UCI option. int64_t so multi-GB tables don't overflow the entry math.
static int64_t TTKeySize = 19999999; // default ~305 MB; resized at runtime via Hash UCI option
static int64_t TTSize    = 2 * TTKeySize;

// Generation counter: 5 bits (0-31), stored in bits 6..2 of TTEntry::flag.
static uint8_t ttAge = 0;
inline void incrementTTAge() { ttAge = (ttAge + 1) & 0x1F; }

// 8-byte entry, two per bucket -> a bucket is 16 bytes and, since the vector's
// storage is at least 16-byte aligned, never straddles a 64-byte cache line.
struct alignas(8) TTEntry {
    uint16_t key16 = 0;    // low 16 bits of the position hash (bucket index uses the high bits)
    uint16_t bestMove = 0;
    int16_t eval = 0;
    int8_t depth = 0;
    uint8_t flag = 0;  // bit 7 = occupied, bits 6..2 = age (0-31), bits 1..0 = bound type

    TTEntry(){};

    inline bool occupied()  const { return flag & 0x80; }
    inline int boundType()  const { return flag & 0x3; }
    inline int entryAge()   const { return (flag >> 2) & 0x1F; }

    void update(uint64_t _hash, uint16_t _bestMove, int _eval, int _depth, int _flag) {
        key16 = (uint16_t)_hash;
        bestMove = _bestMove;
        eval = (int16_t)_eval;
        depth = (int8_t)_depth;
        flag = (uint8_t)(0x80 | ((ttAge & 0x1F) << 2) | (_flag & 0x3));
    }

    void update(const TTEntry* other) {
        *this = *other;
    }
};

static_assert(sizeof(TTEntry) == 8, "TTEntry must be 8 bytes");
static_assert(alignof(TTEntry) == 8, "TTEntry must be 8-byte aligned");
