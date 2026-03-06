#include <string>
#include <cstdint>
#include <cstring>
#include <climits>
using namespace std;

static const int TTFlagAlpha = 0;  // we couldn't reach the alpha of the position
static const int TTFlagExact = 1;  // we received the definite evaluation
static const int TTFlagBeta = 2;  // the move caused a beta cutoff
// static const int TTKeySize = 9999973; // close to 10M * 16 bytes per entry => 160 MB
static const int TTKeySize = 19999999; // close to 20M * 16 bytes per entry => 320 MB
static const int TTSize = 2 * TTKeySize;

struct TTEntry {
    uint64_t hash = 0;
    uint16_t bestMove = 0;
    int16_t eval = 0;
    int8_t depth = 0;
    uint8_t flag = 0;

    TTEntry(){};

    void update(uint64_t _hash, uint16_t _bestMove, int _eval, int _depth, int _flag) {
        hash = _hash;
        bestMove = _bestMove;
        eval = (int16_t)_eval;
        depth = (int8_t)_depth;
        flag = (uint8_t)_flag;
    }

    void update(TTEntry* other) {
        hash = other->hash;
        bestMove = other->bestMove;
        eval = other->eval;
        depth = other->depth;
        flag = other->flag;
    }
};
