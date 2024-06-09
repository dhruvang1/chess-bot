#include <string>
#include <vector>
#include <cstdio>
#include <unordered_map>
#include "staticEvals.cpp"
#include "hash.cpp"
#include "transposition.cpp"

using namespace std;

struct Capture{
    int myPieceValue = 0;
    int otherPieceValue = 0;
    string move;
};


class Board {
public:
    enum Color {
        WHITE = 0,
        BLACK = 1
    };

    Color turn = WHITE;

    vector<string> prevMoves;
    vector<char> prevPiece;
    int enPassantCol = -1;

    static inline int checkmateEval = 15000;
    static inline int stalemateEval = 0; // see comments where its used

    public:

    Board() {
        setup();
        initPieceValues();
        initEvalMap();
        initGamePhaseTable();
        initPassPawn();
        initValidMoves();
    }

    void logMembers() {
        cout << "prevMoves " << prevMoves.size() << " " << prevMoves.capacity() << endl;
        cout << "prevPiece " << prevPiece.size() << " " << prevPiece.capacity() << endl;
        cout << "hashHistory " << hashHistory.size() << " " << hashHistory.bucket_count() << endl;

        for (const auto & [ key, value ] : hashHistory) {
            cout << key << ": " << value << endl;
        }
    }

    void processMove(const string& move) {
        if (move.length() != 4 && move.length() != 5) {
            throw std::invalid_argument( "received move with length not 4/5, move: " + move);
        }

        // assume the values are valid
        int currCol = move[0] - 'a';
        int currRow = move[1] - '0' - 1;
        int newCol = move[2] - 'a';
        int newRow = move[3] - '0' - 1;

        char movedPiece = board[currRow][currCol];

        if (!isPieceOfColor(turn, movedPiece)) {
            string turn_value = turn == WHITE ? "WHITE" : "BLACK";
            throw std::invalid_argument("Moving a piece of wrong color (or empty piece), move: " + move + " movedPiece: " + movedPiece + " turn: " + turn_value);
        }

        evalCalculated = false;
        // add moves and eval for backtracking
        prevMoves.push_back(move);
        // add turn hash
        boardHash ^= hashHelper.getTurnHash();
        // reset en-passant hash
        maybeResetEnPassantHash();
        // castling hash
        bool canShortCastleInitially;
        bool canLongCastleInitially;
        if (turn == WHITE) {
            canShortCastleInitially = (whiteShortRookMoved == 0 && whiteKingMoved == 0);
            canLongCastleInitially = (whiteLongRookMoved == 0 && whiteKingMoved == 0);
        } else {
            canShortCastleInitially = (blackShortRookMoved == 0 && blackKingMoved == 0);
            canLongCastleInitially = (blackLongRookMoved == 0 && blackKingMoved == 0);
        }

        // handle promotion
        if (move.length() == 5) {
            char newPiece = move[4];
            char gonePiece = board[newRow][newCol];
            prevPiece.push_back(gonePiece); // add gone piece for backtracking

            if (newPiece != 'r' && newPiece != 'q' && newPiece != 'b' && newPiece != 'n') {
                throw std::invalid_argument("Promoted piece is unknown - Move: " + move + " piece: " + newPiece);
            }

            if (turn == WHITE) {
                newPiece -= 32;
            }

            // remove hash of gone piece
            boardHash ^= hashHelper.getHash(board[newRow][newCol], newRow, newCol);

            // create new piece
            board[newRow][newCol] = newPiece;
            boardHash ^= hashHelper.getHash(board[newRow][newCol], newRow, newCol);

            // delete old pawn
            boardHash ^= hashHelper.getHash(board[currRow][currCol], currRow, currCol);
            board[currRow][currCol] = ' ';

            // flip turn
            flipTurn();

            hashHistory[boardHash]++;
            return;
        }

        // if King is moving two squares it is castling
        if (isKing(board[currRow][currCol]) && (currRow == newRow) && (abs(currCol - newCol) == 2)) {
            // update rook position
            if (newCol > currCol) {
                // short castle
                updateShortRookMoved(1);

                board[currRow][5] = board[currRow][7];
                board[currRow][7] = ' ';
                boardHash ^= hashHelper.getHash(board[currRow][5], currRow, 7);
                boardHash ^= hashHelper.getHash(board[currRow][5], currRow, 5);
            } else {
                // long castle
                updateLongRookMoved(1);

                board[currRow][3] = board[currRow][0];
                board[currRow][0] =  ' ';
                boardHash ^= hashHelper.getHash(board[currRow][3], currRow, 0);
                boardHash ^= hashHelper.getHash(board[currRow][3], currRow, 3);
            }
        }

        char gonePiece = board[newRow][newCol];
        prevPiece.push_back(gonePiece); // add gone piece for backtracking
        boardHash ^= hashHelper.getHash(board[newRow][newCol], newRow, newCol);

        // book keeping rook & king moves for castling
        if (isKing(movedPiece)) {
            updateKingMoved(1);
        } else if (isRook(movedPiece)) {
            if (isRookAtShortHome(turn, currRow, currCol)) {
                updateShortRookMoved(1);
            } else if (isRookAtLongHome(turn, currRow, currCol)) {
                updateLongRookMoved(1);
            }
        }

        // handle double pawn moves
        if (isPawn(movedPiece) && abs(newRow - currRow) == 2 && currCol == newCol) {
            boardHash ^= hashHelper.getEnPassantHash(currCol);
            enPassantCol = currCol;
        } else if (isPawn(movedPiece) && abs(newCol - currCol) == 1 && gonePiece == ' ') {
            // handle en-passant
            // if pawn captured (i.e. changed column) and there is no gonePiece => it's en-passant

            // remove the captured pawn
            boardHash ^= hashHelper.getHash(board[currRow][newCol], currRow, newCol);
            board[currRow][newCol] = ' ';
        }


        // update board
        board[newRow][newCol] = board[currRow][currCol];
        board[currRow][currCol] = ' ';

        boardHash ^= hashHelper.getHash(board[newRow][newCol], newRow, newCol);
        boardHash ^= hashHelper.getHash(board[newRow][newCol], currRow, currCol);

        // check if you can castle now
        bool canShortCastleNow;
        bool canLongCastleNow;
        if (turn == WHITE) {
            canShortCastleNow = (whiteShortRookMoved == 0 && whiteKingMoved == 0);
            canLongCastleNow = (whiteLongRookMoved == 0 && whiteKingMoved == 0);

            // changed status
            if (canShortCastleInitially + canShortCastleNow == 1) {
                boardHash ^= hashHelper.whiteShortCastle;
            }

            if (canLongCastleInitially + canLongCastleNow == 1) {
                boardHash ^= hashHelper.whiteLongCastle;
            }
        } else {
            canShortCastleNow = (blackShortRookMoved == 0 && blackKingMoved == 0);
            canLongCastleNow = (blackLongRookMoved == 0 && blackKingMoved == 0);

            // changed status
            if (canShortCastleInitially + canShortCastleNow == 1) {
                boardHash ^= hashHelper.blackShortCastle;
            }

            if (canLongCastleInitially + canLongCastleNow == 1) {
                boardHash ^= hashHelper.blackLongCastle;
            }
        }

        flipTurn();
        hashHistory[boardHash]++;
    }

    void undoMove() {
        if (prevMoves.empty()) {
            throw std::invalid_argument("cannot undo from empty list");
        }
        evalCalculated = false;

        flipTurn(); // flip turn before to mimic the correct side making an undo move

        hashHistory[boardHash]--;
        int hashCount = hashHistory[boardHash];
        if (hashCount == 0) {
            hashHistory.erase(boardHash);
        }
        else if (hashCount <= 0) {
            cout << "info hash not found: " << boardHash << endl;
            throw std::invalid_argument("cannot undo from empty list");
        }

        // add turn hash
        boardHash ^= hashHelper.getTurnHash();
        // reset en-passant hash
        maybeResetEnPassantHash();

        // castling hash
        bool canShortCastleInitially;
        bool canLongCastleInitially;
        if (turn == WHITE) {
            canShortCastleInitially = (whiteShortRookMoved == 0 && whiteKingMoved == 0);
            canLongCastleInitially = (whiteLongRookMoved == 0 && whiteKingMoved == 0);
        } else {
            canShortCastleInitially = (blackShortRookMoved == 0 && blackKingMoved == 0);
            canLongCastleInitially = (blackLongRookMoved == 0 && blackKingMoved == 0);
        }


        char gonePiece = prevPiece[prevPiece.size() - 1];
        string lastMove = prevMoves[prevMoves.size() - 1];

        prevPiece.pop_back();
        prevMoves.pop_back();

        // assume the values are valid
        int prevCol = lastMove[0] - 'a';
        int prevRow = lastMove[1] - '0' - 1;
        int currCol = lastMove[2] - 'a';
        int currRow = lastMove[3] - '0' - 1;

        // handle promotion
        if (lastMove.length() == 5) {
            // remove hash of newly promoted piece
            boardHash ^= hashHelper.getHash(board[currRow][currCol], currRow, currCol);


            board[prevRow][prevCol] = turn == WHITE ? 'P' : 'p';
            board[currRow][currCol] = gonePiece;

            boardHash ^= hashHelper.getHash(board[prevRow][prevCol], prevRow, prevCol);
            boardHash ^= hashHelper.getHash(board[currRow][currCol], currRow, currCol);

            // add hash double pawn moves
            if (!prevMoves.empty()) {
                string secondLastMove = prevMoves[prevMoves.size() - 1];

                int secPrevCol = secondLastMove[0] - 'a';
                int secPrevRow = secondLastMove[1] - '0' - 1;
                int secCurrCol = secondLastMove[2] - 'a';
                int secCurrRow = secondLastMove[3] - '0' - 1;

                char secMovedPiece = board[secCurrRow][secCurrCol];
                if (isPawn(secMovedPiece) && abs(secCurrRow - secPrevRow) == 2 && secCurrCol == secPrevCol) {
                    boardHash ^= hashHelper.getEnPassantHash(secCurrCol);
                    enPassantCol = secCurrCol;
                }
            }

            return;
        }

        // handle castling
        if(isKing(board[currRow][currCol]) && (prevRow == currRow) && (abs(prevCol - currCol) == 2)) {
            // update rook position
            if (currCol > prevCol) {
                // short castle
                updateShortRookMoved(-1);
                board[prevRow][7] = board[prevRow][5];
                board[prevRow][5] = ' ';
                boardHash ^= hashHelper.getHash(board[prevRow][7], prevRow, 5);
                boardHash ^= hashHelper.getHash(board[prevRow][7], prevRow, 7);
            } else {
                // long castle
                updateLongRookMoved(-1);
                board[prevRow][0] = board[prevRow][3];
                board[prevRow][3] =  ' ';
                boardHash ^= hashHelper.getHash(board[prevRow][0], prevRow, 3);
                boardHash ^= hashHelper.getHash(board[prevRow][0], prevRow, 0);
            }
        }

        // book keeping rook & king moves for castling
        char movedPiece = board[currRow][currCol];
        if (isKing(movedPiece)) {
            updateKingMoved(-1);
        } else if (isRook(movedPiece)) {
            if (isRookAtShortHome(turn, prevRow, prevCol)) {
                updateShortRookMoved(-1);
            } else if (isRookAtLongHome(turn, prevRow, prevCol)) {
                updateLongRookMoved(-1);
            }
        }

        // handle en-passant
        // if pawn captured (i.e. changed column) and there is no gonePiece => it's en-passant
        if (isPawn(movedPiece) && abs(prevCol - currCol) == 1 && gonePiece == ' ') {
            // restore previous pawn
            board[prevRow][currCol] = turn == WHITE ? 'p' : 'P';
            boardHash ^= hashHelper.getHash(board[prevRow][currCol], prevRow, currCol);
        }

        boardHash ^= hashHelper.getHash(board[currRow][currCol], currRow, currCol);
        board[prevRow][prevCol] = board[currRow][currCol];
        board[currRow][currCol] = gonePiece;
        boardHash ^= hashHelper.getHash(board[prevRow][prevCol], prevRow, prevCol);
        boardHash ^= hashHelper.getHash(board[currRow][currCol], currRow, currCol);

        // add hash double pawn moves
        if (!prevMoves.empty()) {
            string secondLastMove = prevMoves[prevMoves.size() - 1];

            int secPrevCol = secondLastMove[0] - 'a';
            int secPrevRow = secondLastMove[1] - '0' - 1;
            int secCurrCol = secondLastMove[2] - 'a';
            int secCurrRow = secondLastMove[3] - '0' - 1;

            char secMovedPiece = board[secCurrRow][secCurrCol];
            if (isPawn(secMovedPiece) && abs(secCurrRow - secPrevRow) == 2 && secCurrCol == secPrevCol) {
                boardHash ^= hashHelper.getEnPassantHash(secCurrCol);
                enPassantCol = secCurrCol;
            }
        }

        // check if you can castle now
        bool canShortCastleNow;
        bool canLongCastleNow;
        if (turn == WHITE) {
            canShortCastleNow = (whiteShortRookMoved == 0 && whiteKingMoved == 0);
            canLongCastleNow = (whiteLongRookMoved == 0 && whiteKingMoved == 0);

            // changed status
            if (canShortCastleInitially + canShortCastleNow == 1) {
                boardHash ^= hashHelper.whiteShortCastle;
            }

            if (canLongCastleInitially + canLongCastleNow == 1) {
                boardHash ^= hashHelper.whiteLongCastle;
            }
        } else {
            canShortCastleNow = (blackShortRookMoved == 0 && blackKingMoved == 0);
            canLongCastleNow = (blackLongRookMoved == 0 && blackKingMoved == 0);

            // changed status
            if (canShortCastleInitially + canShortCastleNow == 1) {
                boardHash ^= hashHelper.blackShortCastle;
            }

            if (canLongCastleInitially + canLongCastleNow == 1) {
                boardHash ^= hashHelper.blackLongCastle;
            }
        }
    }

    void processNullMove() {
        string move = "null";

        evalCalculated = false;

        prevMoves.push_back(move);
        prevPiece.push_back(' ');

        // add turn hash
        boardHash ^= hashHelper.getTurnHash();
        // reset en-passant hash
        maybeResetEnPassantHash();

        // flip turn
        flipTurn();
    }

    void undoNullMove() {
        evalCalculated = false;
        string lastMove = prevMoves[prevMoves.size() - 1];
        if (lastMove != "null") {
            throw std::invalid_argument( "last move is not null, move: " + lastMove);
        }

        prevMoves.pop_back();
        prevPiece.pop_back();

        // add turn hash
        boardHash ^= hashHelper.getTurnHash();

        // add hash double pawn moves
        if (!prevMoves.empty()) {
            string secondLastMove = prevMoves[prevMoves.size() - 1];

            int secPrevCol = secondLastMove[0] - 'a';
            int secPrevRow = secondLastMove[1] - '0' - 1;
            int secCurrCol = secondLastMove[2] - 'a';
            int secCurrRow = secondLastMove[3] - '0' - 1;

            char secMovedPiece = board[secCurrRow][secCurrCol];
            if (isPawn(secMovedPiece) && abs(secCurrRow - secPrevRow) == 2 && secCurrCol == secPrevCol) {
                boardHash ^= hashHelper.getEnPassantHash(secCurrCol);
                enPassantCol = secCurrCol;
            }
        }

        // flip turn
        flipTurn();
    }

    void getLegalMoves(bool capturesOnly, vector<string>& promotion) {
        // simple now, captures before non captures.
        vector<string> remaining;
        vector<Capture> capture; // get capture eval diff with it (who captured whom, eg. pawn take piece > piece takes pawn > queen takes pawn)
        vector<string> castle;

        remaining.reserve(40);
        capture.reserve(40);
        castle.reserve(2);

        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                if (isPieceOfColor(turn, board[i][j])) {
                    char c = board[i][j];
                    if (isPawn(c)) {
                        // push two squares if both empty
                        if (turn == WHITE && i == 1) {
                            if (isValid(i+2, j) && board[i+1][j] == ' ' && board[i+2][j] == ' ') {
                                remaining.push_back(encode(i, j, i + 2, j));
                            }
                        } else if (turn == BLACK && i == 6) {
                            if (isValid(i-2, j) && board[i-1][j] == ' ' && board[i-2][j] == ' ') {
                                remaining.push_back(encode(i, j, i - 2, j));
                            }
                        }

                        int newI = turn == WHITE ? i + 1 : i - 1;
                        int newJ = j;
                        // push one square if its empty
                        if (isValid(newI, newJ) && board[newI][newJ] == ' ') {
                            // consider promotion case
                            if (newI == 7 || newI == 0) {
                                promotion.push_back(encodePromotion(i, j, newI, newJ, 'q'));
                                promotion.push_back(encodePromotion(i, j, newI, newJ, 'r'));
                                promotion.push_back(encodePromotion(i, j, newI, newJ, 'n'));
                                promotion.push_back(encodePromotion(i, j, newI, newJ, 'b'));
                            } else {
                                remaining.push_back(encode(i, j, newI, newJ));
                            }
                        }

                        // capture
                        for(auto dir: capturePawnDirs) {
                            newI = turn == WHITE ? i + dir[0] : i - dir[0];
                            newJ = j + dir[1];

                            if (isValid(newI, newJ) && isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                if (newI == 7 || newI == 0) {
                                    promotion.push_back(encodePromotion(i, j ,newI, newJ, 'q'));
                                    promotion.push_back(encodePromotion(i, j ,newI, newJ, 'r'));
                                    promotion.push_back(encodePromotion(i, j ,newI, newJ, 'n'));
                                    promotion.push_back(encodePromotion(i, j ,newI, newJ, 'b'));
                                } else {
                                    capture.push_back({pieceValue[c], pieceValue[board[newI][newJ]], encode(i, j, newI, newJ)});
                                }
                                continue; // a normal capture or promotion => no en-passant
                            }

                            // en-passant
                            bool enPassantRow = turn == WHITE ? (i == 4) : (i == 3);
                            if (isValid(newI, newJ) && enPassantRow && !prevMoves.empty()) {
                                // check if last move was pawn move
                                string lastMove = prevMoves[prevMoves.size() - 1];

                                // assume the values are valid
                                int prevCol = lastMove[0] - 'a';
                                int prevRow = lastMove[1] - '0' - 1;
                                int currCol = lastMove[2] - 'a';
                                int currRow = lastMove[3] - '0' - 1;

                                // A pawn moved 2 squares in the same column as current capture attempt
                                if (isPawn(board[currRow][currCol]) && abs(currRow - prevRow) == 2 && currCol == newJ) {
                                    capture.push_back({pieceValue[c], pieceValue[board[currRow][currCol]], encode(i, j, newI, newJ)});
                                }
                            }
                        }
                    } else if (isKing(c)) {
                        for(auto& dir: validMoves[c][i][j]) {
                            int newI = dir[0] >> 3;
                            int newJ = dir[0] & 7;

                            if (!isSquareAttackedByColor(newI, newJ, flipColor(turn))) {
                                if (board[newI][newJ] == ' ') {
                                    remaining.push_back(encode(i, j, newI, newJ));
                                } else if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.push_back({pieceValue[c], pieceValue[board[newI][newJ]], encode(i, j, newI, newJ)});
                                }
                            }

                            if (canShortCastle()) {
                                castle.push_back(encode(i , j, i , 6));
                            }
                            if (canLongCastle()) {
                                castle.push_back(encode(i , j, i , 2));
                            }
                        }
                    } else {
                        for(auto& dir: validMoves[c][i][j]) {
                            for(auto& move: dir) {
                                int newI = move >> 3;
                                int newJ = move & 7;
                                // encountered same color piece
                                if (isPieceOfColor(turn, board[newI][newJ])) {
                                    break;
                                }

                                if (board[newI][newJ] == ' ') {
                                    remaining.push_back(encode(i, j, newI, newJ));
                                }

                                // captured a piece and can't go beyond
                                if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.push_back({pieceValue[c], pieceValue[board[newI][newJ]], encode(i, j, newI, newJ)});
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // merge vectors and order them

        // sort the captures
        if (capture.size() > 1) {
            // white wants ascending sorting & black wants descending sorting
            bool isWhiteTurn = turn == WHITE;

            sort(capture.begin(), capture.end(), [&isWhiteTurn](auto &left, auto &right) {
                if (isWhiteTurn) {
                    if (left.otherPieceValue == right.otherPieceValue) {
                        // LVA (least valuable aggressor)
                        // white has +ve pieceValues => sort ascending (i.e. rook (5) before queen (9))
                        return left.myPieceValue < right.myPieceValue;
                    } else {
                        // MVV (most valuable victim)
                        // black have -ve pieceValues => sort ascending (i.e. queen (-9) before rook (-5))
                        return left.otherPieceValue <  right.otherPieceValue;
                    }
                } else {
                    if (left.otherPieceValue == right.otherPieceValue) {
                        // LVA (least valuable aggressor)
                        // black has -ve pieceValues => sort descending (i.e. rook (-5) before queen (-9))
                        return left.myPieceValue > right.myPieceValue;
                    } else {
                        // MVV (most valuable victim)
                        // white has +ve pieceValues => sort descending (i.e. queen before rook)
                        return left.otherPieceValue >  right.otherPieceValue;
                    }
                }
            });
        }


        if (capturesOnly) {
            // check detection is too slow
//            vector<string> check;
//            vector<string> nonCheck;
//
//            // find moves that put king in check
//            for(auto& rmove: remaining) {
//                processMove(rmove);
//                if (isKingInCheck()) {
//                    check.push_back(rmove);
//                } else {
//                    nonCheck.push_back(rmove);
//                }
//                undoMove();
//            }
//
//            for(auto& chkmove: check) {
//                promotion.push_back(chkmove);
//            }

            for(auto& capmove: capture) {
                promotion.push_back(capmove.move);
            }
        } else {
            for(auto& move: castle) {
                promotion.push_back(move);
            }

            for(auto& move: capture) {
                promotion.push_back(move.move);
            }

            for(auto& move: remaining) {
                promotion.push_back(move);
            }
        }
    }

    bool isKingInCheck() {
        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                if(isKingOfColor(turn, board[i][j])) {
                    return isSquareAttackedByColor(i, j, flipColor(turn));
                }
            }
        }
        throw std::invalid_argument("There is no king on board");
    }

    bool isSquareAttackedByColor(int i, int j, Color color) {
        int newI, newJ;

        for(auto dir: capturePawnDirs) {
            // flip the direction compared to legal moves
            newI = color == WHITE ? i - dir[0] : i + dir[0];
            newJ = j + dir[1];

            if (isValid(newI, newJ) && isPawnOfColor(color, board[newI][newJ])) {
                return true;
            }
        }

        for(auto dir: knightDirs) {
            newI = i + dir[0];
            newJ = j + dir[1];
            if (isValid(newI, newJ) && isKnightOfColor(color, board[newI][newJ])) {
                return true;
            }
        }

        for(auto dir: bishopDirsRays) {
            newI = i;
            newJ = j;
            // consider all bishop moves
            while(true) {
                newI = newI + dir[0];
                newJ = newJ + dir[1];
                if (!isValid(newI, newJ)) {
                    break;
                }

                // bishop or queen
                if (isBishopOfColor(color, board[newI][newJ]) || isQueenOfColor(color, board[newI][newJ])) {
                    return true;
                }

                // encountered some other piece (doesn't matter ally or enemy)
                if (board[newI][newJ] != ' ') {
                    break;
                }
            }
        }

        for(auto dir: rookDirsRays) {
            newI = i;
            newJ = j;
            // consider all rook moves
            while(true) {
                newI = newI + dir[0];
                newJ = newJ + dir[1];
                if (!isValid(newI, newJ)) {
                    break;
                }

                // bishop or queen
                if (isRookOfColor(color, board[newI][newJ]) || isQueenOfColor(color, board[newI][newJ])) {
                    return true;
                }

                // encountered some other piece (doesn't matter ally or enemy)
                if (board[newI][newJ] != ' ') {
                    break;
                }
            }
        }

        for(auto dir: kingDirs) {
            newI = i + dir[0];
            newJ = j + dir[1];
            if (isValid(newI, newJ) && isKingOfColor(color, board[newI][newJ])) {
                return true;
            }
        }

        return false;
    }

    int getBoardEval() {
        if (evalCalculated) {
            return eval;
        }

        eval = 0;
        gamePhase = 0;
        int mgEval = 0;
        int egEval = 0;
        int mobility = 0;
        int whitePawns[8]{};
        int blackPawns[8]{};

        uint64_t positions[256]{};

        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                char c = board[i][j];
                if (c == ' ') {
                    continue;
                }

                mgEval += pieceValue[c] + evalTable[c][0][i][j];
                egEval += pieceValue[c] + evalTable[c][1][i][j];
                gamePhase += gamePhaseTable[c];
                if (isPawn(c)) {
                    isPieceOfColor(WHITE, c) ? whitePawns[j]++ : blackPawns[j]++;
                }

                positions[c] |= 1ULL << ((i << 3) + j);
                // mobility
//                mobility += isPieceOfColor(WHITE, c) ? getMobilityScore(i, j): -getMobilityScore(i, j);
//                eval += mobility;
            }
        }

        // handle early promotion
        if (gamePhase > 24)
            gamePhase = 24;

        eval += (gamePhase * mgEval + (24 - gamePhase) * egEval) / 24;

        double bishopPairBonus = (gamePhase * 0.05 + (24 - gamePhase) * 0.3) / 24;

        // bishop pair
        if ((positions['B'] & lightSquares) > 0 && (positions['B'] & darkSquares) > 0) {
            eval += bishopPairBonus * pieceValue['P'];
        }

        if ((positions['b'] & lightSquares) > 0 && (positions['b'] & darkSquares) > 0) {
            eval += bishopPairBonus * pieceValue['p'];
        }

        // Doubled & isolated pawns don't matter much in opening
        double doubledPawnsPenalty = (gamePhase * 0 + (24 - gamePhase) * 0.4) / 24;
        double doubledIsolatedPawnsPenalty = (gamePhase * 0 + (24 - gamePhase) * 0.2) / 24; // doubled pawns if they are also isolated
        double isolatedPawnsPenalty = (gamePhase * 0 + (24 - gamePhase) * 0.4) / 24;
        double passedPawnsBonus = (gamePhase * 0.2 + (24 - gamePhase) * 0.8) / 24;

        // process pawns
        for(int j=0; j < 8; j++) {
            if (whitePawns[j] >= 1) {
                // isolated pawns
                bool isolated = true;
                if ((j > 0 && whitePawns[j - 1] > 0) || (j < 7 && whitePawns[j + 1] > 0)){
                    isolated = false;
                }
                if (isolated) {
                    eval -= int(pieceValue['P'] * whitePawns[j] * isolatedPawnsPenalty);
                    eval -= int(pieceValue['P'] * (whitePawns[j] - 1) * doubledIsolatedPawnsPenalty);
                } else {
                    // doubled pawns
                    eval -= int(pieceValue['P'] * (whitePawns[j] - 1) * doubledPawnsPenalty);
                }

                if (gamePhase <= 18) {
                    // passed pawns are valuable only if they are a bit advanced
                    for (int i = 6; i > 3; i--) {
                        // check if whitePawn is present
                        if (positions['P'] & (1 << (i * 8 + j))) {
                            // check if there is no black pawn in passing zone
                            if ((whitePassPawn[i][j] & positions['p']) == 0) {
                                eval += int(pieceValue['P'] * passedPawnsBonus);
                            }
                            break;
                        }
                    }
                }
            }

            if (blackPawns[j] >= 1) {
                // isolated pawns
                bool isolated = true;
                if ((j > 0 && blackPawns[j - 1] > 0) || (j < 7 && blackPawns[j + 1] > 0)){
                    isolated = false;
                }
                if (isolated) {
                    eval -= int(pieceValue['p'] * blackPawns[j] * isolatedPawnsPenalty);
                    eval -= int(pieceValue['p'] * (blackPawns[j] - 1) * doubledIsolatedPawnsPenalty);
                } else {
                    // doubled pawns
                    eval -= int(pieceValue['p'] * (blackPawns[j] - 1) * doubledPawnsPenalty);
                }

                if (gamePhase <= 18) {
                    // passed pawns are valuable only if they are a bit advanced
                    for (int i = 1; i < 4; i++) {
                        // check if blackPawn is present
                        if (positions['p'] & (1 << (i * 8 + j))) {
                            // check if there is no white pawn in passing zone
                            if ((blackPassPawn[i][j] & positions['P']) == 0) {
                                eval += int(pieceValue['p'] * passedPawnsBonus);
                            }
                            break;
                        }
                    }
                }
            }
        }


        // king safety in early & middle-game
        if (gamePhase >= 16) {
            // white king
            int kingPos = __builtin_ctzll(positions['K']);
            int kRow = kingPos >> 3, kCol = kingPos & 7;
            uint64_t pawnPosition = positions['P'];
            // b & g file
            if (kRow == 0 && kCol == 6) {
                eval += (isBitSet(pawnPosition, 8 + 5) + isBitSet(pawnPosition, 8 + 6) + isBitSet(pawnPosition, 8 + 7)) * 0.15 * pieceValue['P'];
                eval += (isBitSet(pawnPosition, 16 + 5) + isBitSet(pawnPosition, 16 + 6) + isBitSet(pawnPosition, 16 + 7)) * 0.1 * pieceValue['P'];
            } else if (kRow == 0 && kCol == 1) {
                eval += (isBitSet(pawnPosition, 8 + 0) + isBitSet(pawnPosition, 8 + 1) + isBitSet(pawnPosition, 8 + 2)) * 0.15 * pieceValue['P'];
                eval += (isBitSet(pawnPosition, 16 + 0) + isBitSet(pawnPosition, 16 + 1) + isBitSet(pawnPosition, 16 + 2)) * 0.1 * pieceValue['P'];
            }

            // black king
            kingPos = __builtin_ctzll(positions['k']);
            kRow = kingPos >> 3, kCol = kingPos & 7;
            pawnPosition = positions['p'];
            // b & g file
            if (kRow == 7 && kCol == 6) {
                eval += (isBitSet(pawnPosition, 48 + 5) + isBitSet(pawnPosition, 48 + 6) + isBitSet(pawnPosition, 48 + 7)) * 0.15 * pieceValue['p'];
                eval += (isBitSet(pawnPosition, 40 + 5) + isBitSet(pawnPosition, 40 + 6) + isBitSet(pawnPosition, 40 + 7)) * 0.1 * pieceValue['p'];
            } else if (kRow == 7 && kCol == 1) {
                eval += (isBitSet(pawnPosition, 48 + 0) + isBitSet(pawnPosition, 48 + 1) + isBitSet(pawnPosition, 48 + 2)) * 0.15 * pieceValue['p'];
                eval += (isBitSet(pawnPosition, 40 + 0) + isBitSet(pawnPosition, 40 + 1) + isBitSet(pawnPosition, 40 + 2)) * 0.1 * pieceValue['p'];
            }
        }


        evalCalculated = true;
        if (turn == BLACK)
            eval = -eval;

        return eval;
    }

    int getGamePhase() {
        if (evalCalculated) {
            return gamePhase;
        }
        getBoardEval();
        return gamePhase;
    }

    bool isPositionRepeated() {
        auto it = hashHistory.find(boardHash);
        return it != hashHistory.end() && it->second >= 2;
    }

    bool isKingPresent() {
        int count = 0;
        for(auto & i : board) {
            for(char j : i) {
                count += isKing(j);
            }
        }
        return count == 2;
    }

    string printBoard() {
        string ans;
        for(int i=7;i>=0;i--) {
            for(int j=0;j<8;j++) {
                ans += board[i][j];
                ans += "  ";
            }
            ans += "\n";
        }
        return ans;
    }

    uint64_t getHash() const {
        return boardHash;
    }


private:
    // Capital chars for white, small chars for black
    // P, p -> pawn
    // R, r -> rook
    // N, n -> knight
    // B, b -> bishop
    // Q, q -> queen
    // K, k -> king
    char board[8][8]{};

    int whiteShortRookMoved = 0;
    int whiteLongRookMoved = 0;
    int whiteKingMoved = 0;

    int blackShortRookMoved = 0;
    int blackLongRookMoved = 0;
    int blackKingMoved = 0;

    bool evalCalculated = false;
    int eval = 0;
    int evalTable[256][2][8][8]{}; // piece, middle game/ end game, row, col
    int gamePhase = 0;
    int gamePhaseTable[256]{}; // piece, middle game/ end game, row, col
    int pieceValue[256]{};
    vector<vector<int>> validMoves[256][8][8]{};
    uint64_t whitePassPawn[8][8]{};
    uint64_t blackPassPawn[8][8]{};
    uint64_t darkSquares = 0xAA55AA55AA55AA55;
    uint64_t lightSquares = 0x55AA55AA55AA55AA;

    // board hash
    uint64_t boardHash = 0;
    unordered_map<uint64_t, int> hashHistory;
    Hash hashHelper;

    void setup() {
        for(int i=0;i<8;i++) { // NOLINT(*-loop-convert)
            for(int j=0;j<8;j++) {
                board[i][j] = ' ';
            }
        }

        for(int i=0;i<8;i++) {
            board[1][i] = 'P';
            board[6][i] = 'p';
        }

        board[0][0] = 'R';
        board[0][1] = 'N';
        board[0][2] = 'B';
        board[0][3] = 'Q';
        board[0][4] = 'K';
        board[0][5] = 'B';
        board[0][6] = 'N';
        board[0][7] = 'R';

        board[7][0] = 'r';
        board[7][1] = 'n';
        board[7][2] = 'b';
        board[7][3] = 'q';
        board[7][4] = 'k';
        board[7][5] = 'b';
        board[7][6] = 'n';
        board[7][7] = 'r';

        boardHash = 0;
        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                boardHash ^= hashHelper.getHash(board[i][j], i, j);
            }
        }

        boardHash ^= hashHelper.whiteShortCastle;
        boardHash ^= hashHelper.whiteLongCastle;
        boardHash ^= hashHelper.blackShortCastle;
        boardHash ^= hashHelper.blackLongCastle;

        hashHistory[boardHash]++;
    }

    void initPieceValues() {
        pieceValue['P'] = 88;
        pieceValue['R'] = 495;
        pieceValue['N'] = 309;
        pieceValue['B'] = 331;
        pieceValue['Q'] = 980;
        pieceValue['K'] = 20000;

        pieceValue['p'] = -pieceValue['P'];
        pieceValue['r'] = -pieceValue['R'];
        pieceValue['n'] = -pieceValue['N'];
        pieceValue['b'] = -pieceValue['B'];
        pieceValue['q'] = -pieceValue['Q'];
        pieceValue['k'] = -pieceValue['K'];

        pieceValue[' '] = 0;
    }

    void initEvalMap() {
        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                evalTable['P'][0][i][j] = mg_pawn_table[(7 - i)*8 + j];
                evalTable['R'][0][i][j] = mg_rook_table[(7 - i)*8 + j];
                evalTable['N'][0][i][j] = mg_knight_table[(7 - i)*8 + j];
                evalTable['B'][0][i][j] = mg_bishop_table[(7 - i)*8 + j];
                evalTable['Q'][0][i][j] = mg_queen_table[(7 - i)*8 + j];
                evalTable['K'][0][i][j] = mg_king_table[(7 - i)*8 + j];

                evalTable['p'][0][i][j] = -mg_pawn_table[i*8 + j];
                evalTable['r'][0][i][j] = -mg_rook_table[i*8 + j];
                evalTable['n'][0][i][j] = -mg_knight_table[i*8 + j];
                evalTable['b'][0][i][j] = -mg_bishop_table[i*8 + j];
                evalTable['q'][0][i][j] = -mg_queen_table[i*8 + j];
                evalTable['k'][0][i][j] = -mg_king_table[i*8 + j];


                evalTable['P'][1][i][j] = eg_pawn_table[(7 - i)*8 + j];
                evalTable['R'][1][i][j] = eg_rook_table[(7 - i)*8 + j];
                evalTable['N'][1][i][j] = eg_knight_table[(7 - i)*8 + j];
                evalTable['B'][1][i][j] = eg_bishop_table[(7 - i)*8 + j];
                evalTable['Q'][1][i][j] = eg_queen_table[(7 - i)*8 + j];
                evalTable['K'][1][i][j] = eg_king_table[(7 - i)*8 + j];


                evalTable['p'][1][i][j] = -eg_pawn_table[i*8 + j];
                evalTable['r'][1][i][j] = -eg_rook_table[i*8 + j];
                evalTable['n'][1][i][j] = -eg_knight_table[i*8 + j];
                evalTable['b'][1][i][j] = -eg_bishop_table[i*8 + j];
                evalTable['q'][1][i][j] = -eg_queen_table[i*8 + j];
                evalTable['k'][1][i][j] = -eg_king_table[i*8 + j];
            }
        }
    }

    void initPassPawn() {
        // passed pawns
        uint64_t whitePawnAheadCol[8][8]{};
        uint64_t blackPawnAheadCol[8][8]{};

        for(int i=6;i>=1;i--) {
            for(int j=0;j<8;j++) {
                whitePawnAheadCol[i][j] |= 1 << (8 * (i + 1) + j);
                whitePawnAheadCol[i][j] |= whitePawnAheadCol[i + 1][j];
            }
        }

        for(int i=1;i<7;i++) {
            for(int j=0;j<8;j++) {
                blackPawnAheadCol[i][j] |= 1 << (8 * (i - 1) + j);
                blackPawnAheadCol[i][j] |= blackPawnAheadCol[i - 1][j];
            }
        }

        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                whitePassPawn[i][j] = whitePawnAheadCol[i][j];
                blackPassPawn[i][j] = blackPawnAheadCol[i][j];
                if (j > 0) {
                    whitePassPawn[i][j] |= whitePawnAheadCol[i][j - 1];
                    blackPassPawn[i][j] |= blackPawnAheadCol[i][j - 1];
                }
                if (j < 7) {
                    whitePassPawn[i][j] |= whitePawnAheadCol[i][j + 1];
                    blackPassPawn[i][j] |= blackPawnAheadCol[i][j + 1];
                }
            }
        }
    }

    void initValidMoves() {
        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                for(auto dir: kingDirs) {
                    int newI = i + dir[0];
                    int newJ = j + dir[1];
                    vector<int> temp;
                    if (isValid(newI, newJ)) {
                        temp.push_back(getCoord(newI,newJ));
                        validMoves['K'][i][j].push_back(temp);
                        validMoves['k'][i][j].push_back(temp);
                    }
                }

                for(auto dir: knightDirs) {
                    int newI = i + dir[0];
                    int newJ = j + dir[1];
                    vector<int> temp;
                    if (isValid(newI, newJ)) {
                        temp.push_back(getCoord(newI,newJ));
                        validMoves['N'][i][j].push_back(temp);
                        validMoves['n'][i][j].push_back(temp);
                    }
                }

                for(auto dir: bishopDirsRays) {
                    int newI = i;
                    int newJ = j;

                    vector<int> temp;
                    while(true) {
                        newI = newI + dir[0];
                        newJ = newJ + dir[1];
                        if (!isValid(newI, newJ)) {
                            break;
                        }
                        temp.push_back(getCoord(newI, newJ));
                    }

                    if (!temp.empty()) {
                        validMoves['B'][i][j].push_back(temp);
                        validMoves['b'][i][j].push_back(temp);
                    }
                }

                for(auto dir: rookDirsRays) {
                    int newI = i;
                    int newJ = j;

                    vector<int> temp;
                    while(true) {
                        newI = newI + dir[0];
                        newJ = newJ + dir[1];
                        if (!isValid(newI, newJ)) {
                            break;
                        }
                        temp.push_back(getCoord(newI, newJ));
                    }

                    if (!temp.empty()) {
                        validMoves['R'][i][j].push_back(temp);
                        validMoves['r'][i][j].push_back(temp);
                    }
                }

                for(auto dir: queenDirsRays) {
                    int newI = i;
                    int newJ = j;

                    vector<int> temp;
                    while(true) {
                        newI = newI + dir[0];
                        newJ = newJ + dir[1];
                        if (!isValid(newI, newJ)) {
                            break;
                        }
                        temp.push_back(getCoord(newI, newJ));
                    }

                    if (!temp.empty()) {
                        validMoves['Q'][i][j].push_back(temp);
                        validMoves['q'][i][j].push_back(temp);
                    }
                }
            }
        }
    }

    void initGamePhaseTable() {
        gamePhaseTable['P'] = 0;
        gamePhaseTable['p'] = 0;

        gamePhaseTable['R'] = 2;
        gamePhaseTable['r'] = 2;

        gamePhaseTable['N'] = 1;
        gamePhaseTable['n'] = 1;

        gamePhaseTable['B'] = 1;
        gamePhaseTable['b'] = 1;

        gamePhaseTable['Q'] = 4;
        gamePhaseTable['q'] = 4;

        gamePhaseTable['K'] = 0;
        gamePhaseTable['k'] = 0;
    }

    inline void flipTurn() {
        turn = turn == WHITE ? BLACK : WHITE;
    }

    inline void updateKingMoved(int diff) {
        if (turn == WHITE) {
            whiteKingMoved += diff;
        } else{
            blackKingMoved += diff;
        }

        if (whiteKingMoved < 0 || blackKingMoved < 0) {
            throw std::invalid_argument( "negative moves: " + to_string(whiteKingMoved) + " " + to_string(blackKingMoved));
        }
    }

    inline void updateShortRookMoved(int diff) {
        if (turn == WHITE) {
            whiteShortRookMoved += diff;
        } else{
            blackShortRookMoved += diff;
        }

        if (whiteShortRookMoved < 0 || blackShortRookMoved < 0) {
            throw std::invalid_argument( "negative moves: " + to_string(whiteShortRookMoved) + " " + to_string(blackShortRookMoved));
        }
    }

    inline void updateLongRookMoved(int diff) {
        if (turn == WHITE) {
            whiteLongRookMoved += diff;
        } else{
            blackLongRookMoved += diff;
        }

        if (whiteLongRookMoved < 0 || blackLongRookMoved < 0) {
            throw std::invalid_argument( "negative moves: " + to_string(whiteLongRookMoved) + " " + to_string(blackLongRookMoved));
        }
    }

    inline bool canShortCastle() {
        int row = turn == WHITE ? 0: 7;
        if (board[row][5] != ' ' || board[row][6] != ' ') {
            return false;
        }

        if (isRookOfColor(turn, board[row][7]) &&
            ((turn == WHITE && whiteShortRookMoved == 0 && whiteKingMoved == 0) ||
             (turn == BLACK && blackShortRookMoved == 0 && blackKingMoved == 0))) {
            return !isSquareAttackedByColor(row, 4, flipColor(turn)) &&
                  !isSquareAttackedByColor(row, 5, flipColor(turn)) &&
                  !isSquareAttackedByColor(row, 6, flipColor(turn));
        } else {
            return false;
        }
    }

    inline bool canLongCastle() {
        int row = turn == WHITE ? 0: 7;
        if(board[row][1] != ' ' || board[row][2] != ' ' || board[row][3] != ' ') {
            return false;
        }

        if (isRookOfColor(turn, board[row][0]) &&
            ((turn == WHITE && whiteShortRookMoved == 0 && whiteKingMoved == 0) ||
             (turn == BLACK && blackShortRookMoved == 0 && blackKingMoved == 0))) {
            return !isSquareAttackedByColor(row, 2, flipColor(turn)) &&
                   !isSquareAttackedByColor(row, 3, flipColor(turn)) &&
                   !isSquareAttackedByColor(row, 4, flipColor(turn));
        } else {
            return false;
        }
    }

    inline void maybeResetEnPassantHash() {
        if (enPassantCol != -1) {
            boardHash ^= hashHelper.getEnPassantHash(enPassantCol);
            enPassantCol = -1;
        }
    }

    inline int getMobilityScore(int i, int j) {
        char c = board[i][j];
        int moves = 0;
        if (isKnight(c)) {
            for(auto dir: knightDirs) {
                int newI = i + dir[0];
                int newJ = j + dir[1];
                if (isValid(newI, newJ) && !isPieceOfColor(turn, board[newI][newJ])) {
                    moves++;
                }
            }
            return (int)(knightMobility * sqrt(moves));
        } else if (isBishop(c)) {
            for(auto dir: bishopDirsRays) {
                int newI = i;
                int newJ = j;
                // consider all bishop moves
                while(true) {
                    newI = newI + dir[0];
                    newJ = newJ + dir[1];
                    if (!isValid(newI, newJ)) {
                        break;
                    }

                    // encountered same color piece
                    if (isPieceOfColor(turn, board[newI][newJ])) {
                        break;
                    }

                    if (board[newI][newJ] == ' ') {
                        moves++;
                    }

                    // captured a piece and can't go beyond
                    if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                        moves++;
                        break;
                    }
                }
            }
            return (int)(bishopMobility * sqrt(moves));
        }
        return 0;
//        else if (isRook(c)) {
//            for(auto dir: rookDirsRays) {
//                int newI = i;
//                int newJ = j;
//                // consider all rook moves
//                while(true) {
//                    newI = newI + dir[0];
//                    newJ = newJ + dir[1];
//                    if (!isValid(newI, newJ)) {
//                        break;
//                    }
//
//                    // encountered same color piece
//                    if (isPieceOfColor(turn, board[newI][newJ])) {
//                        break;
//                    }
//
//                    if (board[newI][newJ] == ' ') {
//                        moves++;
//                    }
//
//                    // captured a piece and can't go beyond
//                    if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
//                        moves++;
//                        break;
//                    }
//                }
//            }
//            return (int)(rookMobility * sqrt(moves));
//        } else if (isQueen(c)) {
//            for(auto dir: queenDirsRays) {
//                int newI = i;
//                int newJ = j;
//                // consider all bishop moves
//                while(true) {
//                    newI = newI + dir[0];
//                    newJ = newJ + dir[1];
//                    if (!isValid(newI, newJ)) {
//                        break;
//                    }
//
//                    // encountered same color piece
//                    if (isPieceOfColor(turn, board[newI][newJ])) {
//                        break;
//                    }
//
//                    if (board[newI][newJ] == ' ') {
//                        moves++;
//                    }
//
//                    // captured a piece and can't go beyond
//                    if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
//                        moves++;
//                        break;
//                    }
//                }
//            }
//            return (int)(queenMobility * sqrt(moves));

    }

    static inline int capturePawnDirs[][2]  = {{1,1}, {1, -1}};
    static inline int knightDirs[][2] = {{2,1}, {2,-1}, {-2,1}, {-2,-1}, {1,2}, {1,-2}, {-1,2}, {-1,-2}};
    static inline int kingDirs[][2] = {{1,1}, {1,-1}, {-1,1}, {-1,-1}, {0,1}, {0,-1}, {-1,0}, {1,0}};
    static inline int bishopDirsRays[][2] = {{1,1}, {1,-1}, {-1,1}, {-1,-1}};
    static inline int rookDirsRays[][2] = {{0,1}, {0,-1}, {-1,0}, {1,0}};
    static inline int queenDirsRays[][2] = {{1,1}, {1,-1}, {-1,1}, {-1,-1}, {0,1}, {0,-1}, {-1,0}, {1,0}};
    static inline bool isPawn(char c) {return c == 'p' || c == 'P';}
    static inline bool isRook(char c) {return c == 'r' || c == 'R';}
    static inline bool isKnight(char c) {return c == 'n' || c == 'N';}
    static inline bool isBishop(char c) {return c == 'b' || c == 'B';}
    static inline bool isQueen(char c) {return c == 'q' || c == 'Q';}
    static inline bool isKing(char c) {return c == 'k' || c == 'K';}
    static inline int rookMobility = 3;
    static inline int knightMobility = 2;
    static inline int bishopMobility = 4;
    static inline int queenMobility = 3;

    static inline int isBitSet(uint64_t val, int position) {
        return (val >> position) & 1;
    }


    static inline int getCoord(int i, int j) {
        return (i << 3) + j;
    }

    static inline bool isValid(int r, int c) { return r >= 0 && c >= 0 && r < 8 && c < 8; }

    static inline string encode(int i, int j, int newI, int newJ) {
        string ans;

        ans.push_back('a' + j);
        ans.push_back('1' + i);
        ans.push_back('a' + newJ);
        ans.push_back('1' + newI);

        return ans;
    }

    static inline string encodePromotion(int i, int j, int newI, int newJ, char piece) {
        string ans;

        ans.push_back('a' + j);
        ans.push_back('1' + i);
        ans.push_back('a' + newJ);
        ans.push_back('1' + newI);
        ans.push_back(piece);

        return ans;
    }

    static inline bool isPieceOfColor(Color color, char c) {
        return c != ' ' && (color == BLACK ? ('a' <= c && c <= 'z') : ('A' <= c && c <= 'Z'));
    }

    static inline bool isPawnOfColor(Color color, char c) {
        return color == WHITE ? (c == 'P') : (c == 'p');
    }

    static inline bool isRookOfColor(Color color, char c) {
        return color == WHITE ? (c == 'R') : (c == 'r');
    }

    static inline bool isKnightOfColor(Color color, char c) {
        return color == WHITE ? (c == 'N') : (c == 'n');
    }

    static inline bool isBishopOfColor(Color color, char c) {
        return color == WHITE ? (c == 'B') : (c == 'b');
    }

    static inline bool isQueenOfColor(Color color, char c) {
        return color == WHITE ? (c == 'Q') : (c == 'q');
    }

    static inline bool isKingOfColor(Color color, char c) {
        return color == WHITE ? (c == 'K') : (c == 'k');
    }

    static inline bool isRookAtShortHome(Color color, int row, int col) {
        return col == 7 && (color == WHITE ? row == 0 : row == 7);
    }

    static inline bool isRookAtLongHome(Color color, int row, int col) {
        return col == 7 && (color == WHITE ? row == 0 : row == 7);
    }

    static inline Color flipColor(Color color) {
        return color == WHITE ? BLACK : WHITE;
    }

    static inline bool isPieceOfOppositeColor(Color color, char c) {
        return c != ' ' && (color == WHITE ? ('a' <= c && c <= 'z') : ('A' <= c && c <= 'Z'));
    }
};