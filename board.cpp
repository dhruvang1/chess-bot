#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>

using namespace std;

class Board {
public:
    enum Color {
        WHITE = 0,
        BLACK = 1
    };

    Color turn = WHITE;
    int eval = 0;

    vector<string> prevMoves;
    vector<int> prevEvals;
    vector<char> prevPiece;

    static inline int checkmateEval = 15000;
    static inline int stalemateEval = 2000; // see comments where its used

    public:

    Board() {
        reset();
    }

    void reset() {
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

        eval = 0;
    }

    void processMove(const string& move) {
        if (move.length() != 4 && move.length() != 5) {
            throw std::invalid_argument( "received move with length not 4/5, move: " + move);
        }

        // add moves and eval for backtracking
        prevMoves.push_back(move);
        prevEvals.push_back(eval);

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

            // create new piece
            board[newRow][newCol] = newPiece;

            // update eval
            eval += pieceValue[newPiece] + getEval(turn, newPiece, newRow, newCol);
            eval -= (pieceValue[gonePiece] + getEval(turn, gonePiece, currRow, currCol));

            // delete old pawn
            board[currRow][currCol] = ' ';

            // flip turn
            turn = turn == WHITE ? BLACK : WHITE;

            return;
        }

        // if King is moving two squares it is castling
        if (isKing(board[currRow][currCol]) && (currRow == newRow) && (abs(currCol - newCol) == 2)) {
            // update rook position
            if (newCol > currCol) {
                // short castle
                updateShortRookMoved(1);

                eval += getEval(turn, board[currRow][7], currRow, 5) - getEval(turn, board[currRow][7], currRow, 7);

                board[currRow][5] = board[currRow][7];
                board[currRow][7] = ' ';
            } else {
                // long castle
                updateLongRookMoved(1);

                eval += getEval(turn, board[currRow][0], currRow, 3) - getEval(turn, board[currRow][0], currRow, 0);

                board[currRow][3] = board[currRow][0];
                board[currRow][0] =  ' ';
            }
        }

        // update eval from gone piece
        char gonePiece = board[newRow][newCol];
        prevPiece.push_back(gonePiece); // add gone piece for backtracking
        eval -= pieceValue[gonePiece];
        eval -= getEval(turn, gonePiece, newRow, newCol);

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

        // handle en-passant
        // if pawn captured (i.e. changed column) and there is no gonePiece => it's en-passant
        if (isPawn(movedPiece) && abs(newCol - currCol) == 1 && gonePiece == ' ') {
            // Remove the captured pawn eval, it won't be covered by above eval
            char disappearedPawn = board[currRow][newCol];

            eval -= pieceValue[disappearedPawn];
            eval -= getEval(turn, disappearedPawn, currRow, newCol);

            // remove the captured pawn
            board[currRow][newCol] = ' ';
        }


        // update eval because of moved piece
        eval += (getEval(turn, movedPiece, newRow, newCol) - getEval(turn, movedPiece, currRow, currCol));


        // update board
        board[newRow][newCol] = board[currRow][currCol];
        board[currRow][currCol] = ' ';

        turn = turn == WHITE ? BLACK : WHITE;
    }

    vector<string> getLegalMoves() {

        // simple now, captures before non captures.
        vector<string> remaining;
        vector<pair<int, string>> capture; // get capture eval diff with it (who captured whom, eg. pawn take piece > piece takes pawn > queen takes pawn)
        vector<string> castle;
        vector<string> promotion;

        bool attacked[8][8]{};
        calculateAttacked(attacked);

        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                if (isPieceOfColor(turn, board[i][j])) {
                    char c = board[i][j];
                    if (isKnight(c)) {
                        for(auto dir: knightDirs) {
                            int newI = i + dir[0];
                            int newJ = j + dir[1];
                            if (isValid(newI, newJ)) {
                                if (board[newI][newJ] == ' ') {
                                    remaining.push_back(encode(i, j, newI, newJ));
                                } else if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                }
                            }
                        }
                    }
                    else if (isRook(c)) {
                        for(auto dir: rookDirsRays) {
                            int newI = i;
                            int newJ = j;
                            // consider all rook moves
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
                                    remaining.push_back(encode(i, j, newI, newJ));
                                }

                                // captured a piece and can't go beyond
                                if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    break;
                                }
                            }
                        }
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
                                    remaining.push_back(encode(i, j, newI, newJ));
                                }

                                // captured a piece and can't go beyond
                                if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    break;
                                }
                            }
                        }
                    } else if (isQueen(c)) {
                        for(auto dir: queenDirsRays) {
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
                                    remaining.push_back(encode(i, j, newI, newJ));
                                }

                                // captured a piece and can't go beyond
                                if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    break;
                                }
                            }
                        }
                    }
                    else if (isKing(c)) {
                        for(auto dir: kingDirs) {
                            int newI = i + dir[0];
                            int newJ = j + dir[1];
                            if (isValid(newI, newJ) && !attacked[newI][newJ]) {
                                if (board[newI][newJ] == ' ') {
                                    remaining.push_back(encode(i, j, newI, newJ));
                                } else if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                }
                            }
                        }

                        if (canShortCastle(attacked)) {
                            castle.push_back(encode(i , j, i , 6));
                        } else if (canLongCastle(attacked)) {
                            castle.push_back(encode(i , j, i , 2));
                        }
                    }
                    else if (isPawn(c)) {
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
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
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
                                        capture.emplace_back(0, encode(i, j, newI, newJ));
                                }
                            }
                        }

                    }
                }
            }
        }

        // merge vectors
        for(auto& move: castle) {
            promotion.push_back(move);
        }

        // white wants ascending sorting & black wants descending sorting
        bool ascending = turn == WHITE;

        if (capture.size() > 1) {
            std::sort(capture.begin(), capture.end(), [&ascending](auto &left, auto &right) {
                if (ascending) {
                    return left.first < right.first;
                } else {
                    return left.first > right.first;
                }
            });
        }

        for(auto& move: capture) {
            promotion.push_back(move.second);
        }

        for(auto& move: remaining) {
            promotion.push_back(move);
        }

        return promotion;
    }

    bool isKingInCheck() {
        bool attackGrid[8][8]{};
        calculateAttacked(attackGrid);

        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                if(isPieceOfColor(turn, board[i][j]) && isKing(board[i][j])) {
                    return attackGrid[i][j];
                }
            }
        }
        throw std::invalid_argument("There is no king on board");
    }

    void calculateAttacked(bool grid[8][8]) {
        for(int i=0;i<8;i++) {
            for(int j=0;j<8;j++) {
                char c = board[i][j];
                if(isPieceOfOppositeColor(turn, c)) {
                    if (isPawn(c)) {
                        for(auto dir: capturePawnDirs) {
                            // flip the direction compared to legal moves
                            int newI = turn == WHITE ? i - dir[0] : i + dir[0];
                            int newJ = j + dir[1];

                            if (isValid(newI, newJ)) {
                                grid[newI][newJ] = true;
                            }
                        }
                    } else if (isRook(c)) {
                        for(auto dir: rookDirsRays) {
                            int newI = i;
                            int newJ = j;
                            // consider all rook moves
                            while(true) {
                                newI = newI + dir[0];
                                newJ = newJ + dir[1];
                                if (!isValid(newI, newJ)) {
                                    break;
                                }

                                grid[newI][newJ] = true;

                                // encountered a piece
                                if (board[newI][newJ] != ' ') {
                                    break;
                                }
                            }
                        }
                    } else if (isKnight(c)){
                        for(auto dir: knightDirs) {
                            int newI = i + dir[0];
                            int newJ = j + dir[1];
                            if (isValid(newI, newJ)) {
                                grid[newI][newJ] = true;
                            }
                        }
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

                                grid[newI][newJ] = true;

                                // encountered a piece
                                if (board[newI][newJ] != ' ') {
                                    break;
                                }
                            }
                        }
                    } else if (isQueen(c)) {
                        for(auto dir: queenDirsRays) {
                            int newI = i;
                            int newJ = j;
                            // consider all queen moves
                            while(true) {
                                newI = newI + dir[0];
                                newJ = newJ + dir[1];
                                if (!isValid(newI, newJ)) {
                                    break;
                                }

                                grid[newI][newJ] = true;

                                // encountered a piece
                                if (board[newI][newJ] != ' ') {
                                    break;
                                }
                            }
                        }
                    } else if (isKing(c)) {
                        for(auto dir: kingDirs) {
                            int newI = i + dir[0];
                            int newJ = j + dir[1];
                            if (isValid(newI, newJ)) {
                                grid[newI][newJ] = true;
                            }
                        }
                    }
                }
            }
        }
    }

    void undoMove() {
        if (prevMoves.empty()) {
            throw std::invalid_argument("cannot undo from empty list");
        }

        eval = prevEvals[prevEvals.size() - 1];
        turn = turn == WHITE ? BLACK : WHITE; // flip turn before to mimic the correct side making an undo move

        char gonePiece = prevPiece[prevPiece.size() - 1];
        string lastMove = prevMoves[prevMoves.size() - 1];

        prevEvals.pop_back();
        prevPiece.pop_back();
        prevMoves.pop_back();

        // assume the values are valid
        int prevCol = lastMove[0] - 'a';
        int prevRow = lastMove[1] - '0' - 1;
        int currCol = lastMove[2] - 'a';
        int currRow = lastMove[3] - '0' - 1;

        // handle promotion
        if (lastMove.length() == 5) {
            board[prevRow][prevCol] = turn == WHITE ? 'P' : 'p';
            board[currRow][currCol] = gonePiece;

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
            } else {
                // long castle
                updateLongRookMoved(-1);
                board[prevRow][0] = board[prevRow][3];
                board[prevRow][3] =  ' ';
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
        }

        board[prevRow][prevCol] = board[currRow][currCol];
        board[currRow][currCol] = gonePiece;
    }

    bool isCheckmate() const {
        return eval > checkmateEval || eval < -checkmateEval;
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

    inline bool canShortCastle(bool attacked[8][8]) {
        bool ans;
        int row = 0;
        if (turn == WHITE) {
            ans = whiteShortRookMoved == 0 && whiteKingMoved == 0 && !attacked[row][4] && !attacked[row][5] && !attacked[row][6];
        } else {
            row = 7;
            ans = blackShortRookMoved == 0 && blackKingMoved == 0 && !attacked[row][4] && !attacked[row][5] && !attacked[row][6];
        }
        return ans && isPieceOfColor(turn, board[row][7]) && isRook(board[row][7]) && (board[row][5] == ' ') && (board[row][6] == ' ');
    }

    inline bool canLongCastle(bool attacked[8][8]) {
        bool ans;
        int row = 0;
        if (turn == WHITE) {
            ans = whiteLongRookMoved == 0 && whiteKingMoved == 0 && !attacked[row][2] && !attacked[row][3] && !attacked[row][4];
        } else {
            row = 7;
            ans = blackLongRookMoved == 0 && blackKingMoved == 0 && !attacked[row][2] && !attacked[row][3] && !attacked[row][4];
        }

        return ans && isPieceOfColor(turn, board[row][0]) && isRook(board[row][0]) && (board[row][1] == ' ') && (board[row][2] == ' ') && (board[row][3] == ' ');
    }

    static inline int getEval(Color color, char piece, int row, int col) {
        // the matrix are for white but inverse
        if (color == WHITE) {
            row = 7 - row;
            // change column if eval board is not symmetric
        }

        int pieceEval = 0;
        if (piece == ' ') {
        } else if (isPawn(piece)) {
            pieceEval = pawnEvals[row][col];
        } else if (isKnight(piece)) {
            pieceEval = knightEvals[row][col];
        } else if (isBishop(piece)) {
            pieceEval = bishopEvals[row][col];
        } else if (isRook(piece)) {
            pieceEval = rookEvals[row][col];
        } else if (isQueen(piece)) {
            pieceEval = queenEvals[row][col];
        } else if (isKing(piece)) {
            pieceEval = kingEvals[row][col];
        }

        if (color == BLACK) {
            pieceEval = -pieceEval;
        }
        return pieceEval;
    }

    static inline unordered_map<char, int> pieceValue = {
            {'p', -100},
            {'r', -500},
            {'n', -320},
            {'b', -325},
            {'q', -900},
            {'k', -20000},
            {'P', 100},
            {'R', 500},
            {'N', 320},
            {'B', 325},
            {'Q', 900},
            {'K', 20000},
            {' ', 0}
    };

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

    static inline bool isRookAtShortHome(Color color, int row, int col) {
        return col == 7 && (color == WHITE ? row == 0 : row == 7);
    }

    static inline bool isRookAtLongHome(Color color, int row, int col) {
        return col == 7 && (color == WHITE ? row == 0 : row == 7);
    }

    static inline bool isPieceOfOppositeColor(Color color, char c) {
        return c != ' ' && (color == WHITE ? ('a' <= c && c <= 'z') : ('A' <= c && c <= 'Z'));
    }

    static inline int pawnEvals[8][8] =  {
        {0,  0,  0,  0,  0,  0,  0,  0},
        {50, 50, 50, 50, 50, 50, 50, 50},
        {10, 10, 20, 30, 30, 20, 10, 10},
        {5,  5, 10, 25, 25, 10,  5,  5},
        {0,  0,  0, 20, 20,  0,  0,  0},
        {5, -5,-10,  0,  0,-10, -5,  5},
        {5, 10, 10,-20,-20, 10, 10,  5},
        {0,  0,  0,  0,  0,  0,  0,  0}
    };

    static inline int knightEvals[8][8] = {
        {-50,-40,-30,-30,-30,-30,-40,-50},
        {-40,-20,  0,  0,  0,  0,-20,-40},
        {-30,  0, 10, 15, 15, 10,  0,-30},
        {-30,  5, 15, 20, 20, 15,  5,-30},
        {-30,  0, 15, 20, 20, 15,  0,-30},
        {-30,  5, 10, 15, 15, 10,  5,-30},
        {-40,-20,  0,  5,  5,  0,-20,-40},
        {-50,-40,-30,-30,-30,-30,-40,-50},
    };

    static inline int bishopEvals[8][8] = {
        {-20,-10,-10,-10,-10,-10,-10,-20},
        {-10,  0,  0,  0,  0,  0,  0,-10},
        {-10,  0,  5, 10, 10,  5,  0,-10},
        {-10,  5,  5, 10, 10,  5,  5,-10},
        {-10,  0, 10, 10, 10, 10,  0,-10},
        {-10, 10, 10, 10, 10, 10, 10,-10},
        {-10,  5,  0,  0,  0,  0,  5,-10},
        {-20,-10,-10,-10,-10,-10,-10,-20},
    };

    // default
//    static inline int rookEvals[8][8] = {
//        { 0,  0,  0,  0,  0,  0,  0,  0},
//        { 5, 10, 10, 10, 10, 10, 10,  5},
//        {-5,  0,  0,  0,  0,  0,  0, -5},
//        {-5,  0,  0,  0,  0,  0,  0, -5},
//        {-5,  0,  0,  0,  0,  0,  0, -5},
//        {-5,  0,  0,  0,  0,  0,  0, -5},
//        {-5,  0,  0,  0,  0,  0,  0, -5},
//        { 0,  0,  0,  5,  5,  0,  0,  0}
//    };

    static inline int rookEvals[8][8] = {
        { 0,  0,  0,  0,  0,  0,  0,  0},
        { 5, 10, 10, 10, 10, 10, 10,  5},
        {-5,  0,  0,  0,  0,  0,  0, -5},
        {-5,  0,  0,  0,  0,  0,  0, -5},
        {-5,  0,  0,  0,  0,  0,  0, -5},
        {-5,  0,  0,  0,  0,  0,  0, -5},
        {-5,  0,  0,  0,  0,  0,  0, -5},
        {-5,  0,  0,  5,  5,  5,  0, -5}
    };

    // default
//    static inline int queenEvals[8][8] = {
//        {-20,-10,-10, -5, -5,-10,-10,-20},
//        {-10,  0,  0,  0,  0,  0,  0,-10},
//        {-10,  0,  5,  5,  5,  5,  0,-10},
//        { -5,  0,  5,  5,  5,  5,  0, -5},
//        {  0,  0,  5,  5,  5,  5,  0, -5},
//        {-10,  5,  5,  5,  5,  5,  0,-10},
//        {-10,  0,  5,  0,  0,  0,  0,-10},
//        {-20,-10,-10, -5, -5,-10,-10,-20}
//    };

    // don't move queen from her starting square
    static inline int queenEvals[8][8] = {
        {-20,-10,-10, -5, -5,-10,-10,-20},
        {-10,  0,  0,  0,  0,  0,  0,-10},
        {-10,  0,  5,  5,  5,  5,  0,-10},
        { -5,  0,  5,  5,  5,  5,  0, -5},
        {  0,  0,  5,  5,  5,  5,  0, -5},
        {-10,  5,  5,  5,  5,  5,  0,-10},
        {-10,  0,  5,  0,  0,  0,  0,-10},
        {-20,-10,-10, 40, -5,-10,-10,-20}
    };

    // no king activity
//    static inline int kingEvals[8][8] = {
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  0,  0,  0,  0,  0,  0,  0},
//            {0,  5,  5, -5, -5,  0,  5,  0}
//    };

    static inline int kingEvals[8][8] = {
        {0,  0,  0,  0,  0,  0,  0,  0},
        {0,  0,  0,  0,  0,  0,  0,  0},
        {0,  0,  0,  0,  0,  0,  0,  0},
        {0,  0,  0,  0,  0,  0,  0,  0},
        {0,  0,  0,  0,  0,  0,  0,  0},
        {0,  0,  0,  0,  0,  0,  0,  0},
        {0,  0,  0,-10,-10,-10,  0,  0},
        {0,  5, 10,-15,-15,-10, 10,  0}
    };
};