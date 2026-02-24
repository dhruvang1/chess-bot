#include <cassert>
#include <iostream>
#include <string>
#include "../board.cpp"
#include "../magicBoard.cpp"

using namespace std;

void verifyMatch(Board& ob, MagicBoard& mb, const string& context) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const char oldP = ob.getBoardChar(i, j);
            const char newP = mb.getBoardChar(i, j);
            if (oldP != newP) {
                cerr << "MISMATCH at (" << i << "," << j << "): "
                     << "old=" << oldP << " new=" << newP
                     << " | " << context << endl;
                assert(false);
            }

            const char newBitBoardP = mb.getFromBitBoards(i,j);
            if (newBitBoardP != oldP) {
                cerr << "BitBoard MISMATCH at (" << i << "," << j << "): "
                     << "old=" << oldP << " new=" << newBitBoardP
                     << " | " << context << endl;
                assert(false);
            }
        }
    }
    assert(static_cast<int>(ob.turn) == static_cast<int>(mb.turn));
    assert(ob.enPassantCol == mb.enPassantCol);

    // castling check
    const int obCR = ob.getCastlingRights();
    const int mbCR = mb.getCastlingRights();
    if (obCR != mbCR) {
        cerr << "CASTLING MISMATCH: "
             << "WhiteOO=" << bool(obCR & 1) << "/" << bool(mbCR & 1)
             << " WhiteOOO=" << bool(obCR & 2) << "/" << bool(mbCR & 2)
             << " BlackOO=" << bool(obCR & 4) << "/" << bool(mbCR & 4)
             << " BlackOOO=" << bool(obCR & 8) << "/" << bool(mbCR & 8)
             << " | " << context << endl;
        // assert(false); // don't uncomment because there is a bug in old board
    }
}

void testGame(vector<string> moves, const string& name) {
    Board ob;
    MagicBoard mb;
    
    for (auto& m : moves) {
        ob.processMove(m);
        mb.processMove(m);
        verifyMatch(ob, mb, name + " after " + m);
    }

    // undo all and verify
    for (int i = moves.size() - 1; i >= 0; i--) {
        ob.undoMove();
        mb.undoMove();
        verifyMatch(ob, mb, name + " undo " + moves[i]);
    }

    cout << "PASS: " << name << endl;
}

int main() {
    testGame({"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4"}, "Ruy Lopez opening");

    testGame({"e2e4", "e7e5", "e1e2", "e8e7"}, "King moves (no castle)");

    testGame({"e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "g8f6", "e1g1"}, "White kingside castle");

    testGame({"d2d4", "d7d5", "c1e3", "c8e6", "b1c3", "b8c6", "d1d2", "d8d7", "e1c1"}, "White queenside castle");

    testGame({"e2e4", "d7d5", "e4d5", "c7c5", "d5c6"}, "En passant capture");

    testGame({"e2e4", "f7f5", "e4f5"}, "Pawn capture");

    testGame({"a2a4", "b7b5", "a4b5", "a7a5", "b5a6"}, "En passant on a-file");

    testGame({"e2e4", "d7d5", "e4e5", "f7f5", "e5f6"}, "En passant by white");

    testGame({"b1c3", "e7e5", "e2e4", "d8h4", "g1f3", "h4f2"}, "Queen captures");

    testGame({"e2e4", "e7e5", "d2d4", "e5d4", "c2c3", "d4c3", "b1c3"}, "Recapture");

    // promotion
    testGame({"e2e4", "d7d5", "e4d5", "g8f6", "d5d6", "e7e5", "d6d7", "e8e7", "d7d8q"},
        "Pawn promotion to queen");

    testGame({"e2e4", "d7d5", "e4d5", "c7c6", "d5c6", "e7e5", "c6b7", "f7f5", "b7a8q"},
        "Pawn promotion with capture to queen");

    testGame({"e2e4", "d7d5", "e4d5", "c7c6", "d5c6", "e7e5", "c6b7", "f7f5", "b7a8r"},
    "Pawn promotion with capture to rook");

    testGame({"e2e4", "d7d5", "e4d5", "c7c6", "d5c6", "e7e5", "c6b7", "f7f5", "b7a8n"},
    "Pawn promotion with capture to knight");

    testGame({"e2e4", "d7d5", "e4d5", "c7c6", "d5c6", "e7e5", "c6b7", "f7f5", "b7a8b"},
    "Pawn promotion with capture to bishop");

    cout << "\nAll tests passed!" << endl;
    return 0;
}