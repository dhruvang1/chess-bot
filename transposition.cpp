#include <string>
using namespace std;

static const int TTFlagAlpha = 0;  // we couldn't reach the alpha of the position
static const int TTFlagExact = 1;  // we received the definite evaluation
static const int TTFlagBeta = 2;  // the move caused a beta cutoff
//static const int TTKeySize = 9999973; // close to 10M * 50 bytes per entry => 500 MB
//static const int TTSize = 40000003; // close to 40M * 24 bytes per entry => 1G
static const int TTKeySize = 19999999; // close to 20M * 50 bytes per entry => 1G
static const int TTSize = 2 * TTKeySize;

struct TTEntry {
    uint64_t hash = 0;
    string move;
    int eval = 0;
    int depth = 0;
    int flag = 0;

    TTEntry(){};

    void update(uint64_t _hash, const string& _move, int _eval, int _depth, int _flag) {
        this -> hash = _hash;
        this -> move = _move;
        this -> eval = _eval;
        this -> depth = _depth;
        this -> flag = _flag;
    }

    void update(TTEntry* other) {
        this -> hash = other -> hash;
        this -> move = other -> move;
        this -> eval = other -> eval;
        this -> depth = other -> depth;
        this -> flag = other -> flag;
    }
};