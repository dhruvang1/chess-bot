#include <unordered_map>
#include <random>
#include <iostream>

using namespace std;

// we don't care about castling or en-passant to keep life simple :)
class Hash {
    uint64_t hashArray[256][8][8]{};
    uint64_t turnHash; // hash used when black makes a move

    public:
    Hash() {
        mt19937 rng;
        rng.seed(4132983); // random seed

        uniform_int_distribution<uint64_t> distribution(2L << 32, 2L << 63); // range [10^6, 10^]
        vector<char> pieces;
        pieces.push_back('p');
        pieces.push_back('r');
        pieces.push_back('n');
        pieces.push_back('b');
        pieces.push_back('q');
        pieces.push_back('k');
        pieces.push_back('P');
        pieces.push_back('R');
        pieces.push_back('N');
        pieces.push_back('B');
        pieces.push_back('Q');
        pieces.push_back('K');

        for(auto& piece: pieces) {
            for(int i=0;i<8;i++) {
                for(int j=0;j<8;j++) {
                    hashArray[piece][i][j] = distribution(rng);
                }
            }
        }

        turnHash = distribution(rng);
    }

    inline uint64_t getHash(char piece, int i, int j) { return hashArray[piece][i][j]; }
    inline uint64_t getTurnHash() { return turnHash; }
};
