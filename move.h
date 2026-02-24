#pragma once

#include <string>

using namespace std;

struct Move {
    string move;
    char movePiece = ' ';
    char capturePiece = ' ';
    bool isCapture = false;
    bool isPromotion = false;
    bool isCastle = false;

    Move(const string& move, char movePiece) {
        this -> move = move;
        this -> movePiece = movePiece;
    }

    Move(const string& move, char movePiece, bool isCastle, bool isPromotion) {
        this -> move = move;
        this -> movePiece = movePiece;
        this -> isCastle = isCastle;
        this -> isPromotion = isPromotion;
    }

    Move(const string& move, char movePiece, char capturePiece) {
        this -> move = move;
        this -> movePiece = movePiece;
        this -> capturePiece = capturePiece;
        this -> isCapture = true;
    }
};
