// Standard chess perft regression test — the well-known 6-position CPW
// "Perft Results" suite (castling, en passant, pins, promotions, discovered
// checks) that a plain startpos perft doesn't exercise.
//
// perftDiff() also asserts `mismatches` == 0 (its isLegalMove vs. make/unmake
// cross-check) — a legality bug can match the right node count by
// coincidence, so the count alone isn't enough.
//
// Depths 1-5 by default. Pass "deep" to also run depth 6 (Positions 2 and 6
// are in the billions of nodes — takes several minutes).
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include "../magicBoard.cpp"

using namespace std;

struct PerftCase {
    string name;
    string fen;
    vector<uint64_t> expected; // expected[i] = perft(i+1)
};

static const vector<PerftCase> kCases = {
    {"Position 1 (startpos)",
     "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     {20, 400, 8902, 197281, 4865609, 119060324}},

    {"Position 2 (Kiwipete)",
     "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     {48, 2039, 97862, 4085603, 193690690, 8031647685}},

    {"Position 3",
     "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     {14, 191, 2812, 43238, 674624, 11030083}},

    {"Position 4",
     "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     {6, 264, 9467, 422333, 15833292, 706045033}},

    // No commonly-cited depth-6 value for Position 5 — stops at 5 rather than fabricate one.
    {"Position 5",
     "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     {44, 1486, 62379, 2103487, 89941194}},

    {"Position 6",
     "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     {46, 2079, 89890, 3894594, 164075551, 6923051137}},
};

int main(int argc, char** argv) {
    bool deep = argc > 1 && string(argv[1]) == "deep";
    int maxDepth = deep ? 6 : 5;

    for (const auto& tc : kCases) {
        MagicBoard board;
        board.setupFromFen(tc.fen);

        int depthLimit = min((size_t)maxDepth, tc.expected.size());
        for (int depth = 1; depth <= depthLimit; depth++) {
            uint64_t mismatches = 0;
            uint64_t nodes = board.perftDiff(depth, mismatches);
            uint64_t expected = tc.expected[depth - 1];

            if (nodes != expected) {
                cerr << "FAIL: " << tc.name << " perft(" << depth << ") = " << nodes
                     << ", expected " << expected << " | fen=" << tc.fen << endl;
                assert(false);
            }
            if (mismatches != 0) {
                cerr << "FAIL: " << tc.name << " perft(" << depth << ") had "
                     << mismatches << " isLegalMove/oracle mismatches | fen=" << tc.fen << endl;
                assert(false);
            }
        }

        cout << "PASS: " << tc.name << " (depth 1-" << depthLimit << ")" << endl;
    }

    cout << "\nAll perft tests passed!" << endl;
    if (!deep) cout << "(pass \"deep\" as an argument to also run depth 6)" << endl;
    return 0;
}
