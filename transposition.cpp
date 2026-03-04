#include <string>
#include <cstdint>
#include <cstring>
using namespace std;

static const int TTFlagAlpha = 0;  // we couldn't reach the alpha of the position
static const int TTFlagExact = 1;  // we received the definite evaluation
static const int TTFlagBeta = 2;  // the move caused a beta cutoff
static const int TTKeySize = 9999973; // close to 10M * 50 bytes per entry => 500 MB
// static const int TTKeySize = 19999999; // close to 20M * 50 bytes per entry => 1G
static const int TTSize = 2 * TTKeySize;

struct TTEntry {
    uint64_t hash = 0;
    uint16_t pv[4] = {};
    uint8_t pvLen = 0;
    int eval = 0;
    int depth = 0;
    int flag = 0;

    TTEntry(){};

    void update(uint64_t _hash, const uint16_t* _pv, int _pvLen, int _eval, int _depth, int _flag) {
        hash = _hash;
        pvLen = min(_pvLen, 4);
        memcpy(pv, _pv, pvLen * sizeof(uint16_t));
        for (int i = pvLen; i < 4; i++) pv[i] = 0;
        eval = _eval;
        depth = _depth;
        flag = _flag;
    }

    void update(TTEntry* other) {
        hash = other->hash;
        memcpy(pv, other->pv, sizeof(pv));
        pvLen = other->pvLen;
        eval = other->eval;
        depth = other->depth;
        flag = other->flag;
    }
};
