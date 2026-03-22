#include <string>
#include <cstdint>
#include <cstring>
#include <climits>
using namespace std;

static const int TTFlagAlpha = 0;  // we couldn't reach the alpha of the position
static const int TTFlagExact = 1;  // we received the definite evaluation
static const int TTFlagBeta = 2;  // the move caused a beta cutoff
static int TTKeySize = 19999999; // default ~320 MB; resized at runtime via Hash UCI option
static int TTSize    = 2 * TTKeySize;

// flag layout: upper 6 bits = age (0-63), lower 2 bits = bound type (TTFlagAlpha/Exact/Beta)
static uint8_t ttAge = 0;
inline void incrementTTAge() { ttAge = (ttAge + 1) & 63; }

struct TTEntry {
    uint64_t hash = 0;
    uint16_t bestMove = 0;
    int16_t eval = 0;
    int8_t depth = 0;
    uint8_t flag = 0;  // upper 6 bits = age, lower 2 bits = bound type

    TTEntry(){};

    inline int boundType() const { return flag & 0x3; }
    inline int entryAge()  const { return flag >> 2; }

    void update(uint64_t _hash, uint16_t _bestMove, int _eval, int _depth, int _flag) {
        hash = _hash;
        bestMove = _bestMove;
        eval = (int16_t)_eval;
        depth = (int8_t)_depth;
        flag = (uint8_t)((ttAge << 2) | (_flag & 0x3));
    }

    void update(TTEntry* other) {
        hash = other->hash;
        bestMove = other->bestMove;
        eval = other->eval;
        depth = other->depth;
        flag = other->flag;
    }
};
