#include <string>
#include <vector>
#include <unordered_map>
#include "staticEvals.cpp"
#include "hash.cpp"

using namespace std;

class Board {
public:
    enum Color {
        WHITE = 0,
        BLACK = 1
    };

    Color turn = WHITE;

    vector<string> prevMoves;
    vector<char> prevPiece;

    static inline int checkmateEval = 15000;
    static inline int stalemateEval = 300; // see comments where its used

    public:

    Board() {
        setup();
        initPieceValues();
        initEvalMap();
        initGamePhaseTable();
    }

    void processMove(const string& move) {
        if (move.length() != 4 && move.length() != 5) {
            throw std::invalid_argument( "received move with length not 4/5, move: " + move);
        }

        evalCalculated = false;

        // add moves and eval for backtracking
        prevMoves.push_back(move);

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

        // add turn hash
        boardHash ^= hashHelper.getTurnHash();


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

        // handle en-passant
        // if pawn captured (i.e. changed column) and there is no gonePiece => it's en-passant
        if (isPawn(movedPiece) && abs(newCol - currCol) == 1 && gonePiece == ' ') {
            // remove the captured pawn
            boardHash ^= hashHelper.getHash(board[currRow][newCol], currRow, newCol);
            board[currRow][newCol] = ' ';
        }


        // update board
        board[newRow][newCol] = board[currRow][currCol];
        board[currRow][currCol] = ' ';

        boardHash ^= hashHelper.getHash(board[newRow][newCol], newRow, newCol);
        boardHash ^= hashHelper.getHash(board[newRow][newCol], currRow, currCol);

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
        // add turn hash
        boardHash ^= hashHelper.getTurnHash();

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
    }

    vector<string> getLegalMoves(bool capturesOnly) {
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
                                    if (attacked[newI][newJ]) {
                                        capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    } else{
                                        capture.emplace_back(pieceValue[board[newI][newJ]], encode(i, j, newI, newJ));
                                    }
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
                                    if (attacked[newI][newJ]) {
                                        capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    } else {
                                        capture.emplace_back(pieceValue[board[newI][newJ]], encode(i, j, newI, newJ));
                                    }
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
                                    if (attacked[newI][newJ]) {
                                        capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    } else {
                                        capture.emplace_back(pieceValue[board[newI][newJ]], encode(i, j, newI, newJ));
                                    }
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
                                    if (attacked[newI][newJ]) {
                                        capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                    } else {
                                        capture.emplace_back(pieceValue[board[newI][newJ]], encode(i, j, newI, newJ));
                                    }
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
                                } else if (attacked[newI][newJ]){
                                    capture.emplace_back(pieceValue[board[newI][newJ]] + pieceValue[c], encode(i, j, newI, newJ));
                                } else {
                                    capture.emplace_back(pieceValue[board[newI][newJ]], encode(i, j, newI, newJ));
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
                                    if(attacked[newI][newJ]) {
                                        capture.emplace_back(0, encode(i, j, newI, newJ));
                                    } else {
                                        capture.emplace_back(pieceValue[board[currRow][currCol]], encode(i, j, newI, newJ));
                                    }
                                }
                            }
                        }

                    }
                }
            }
        }

        // merge vectors
        if (!capturesOnly) {
            for(auto& move: castle) {
                promotion.push_back(move);
            }
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

        if (!capturesOnly) {
            for(auto& move: remaining) {
                promotion.push_back(move);
            }
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

    int getBoardEval() {
        if (evalCalculated) {
            return eval;
        }

        eval = 0;
        int mgEval = 0;
        int egEval = 0;
        int mobility = 0;
        int gamePhase = 0;
        int whitePawns[8]{};
        int blackPawns[8]{};

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

                // mobility
//                mobility += isPieceOfColor(WHITE, c) ? getMobilityScore(i, j): -getMobilityScore(i, j);
            }
        }

        // handle early promotion
        if (gamePhase > 24)
            gamePhase = 24;

        eval += (gamePhase * mgEval + (24 - gamePhase) * egEval) / 24;

        // Doubled & isolated pawns don't matter much in opening
        int doubledPawnsPenalty = (gamePhase * 5 + (24 - gamePhase) * 2) / 24;
        int isolatedPawnsPenalty = (gamePhase * 4 + (24 - gamePhase) * 2) / 24;

        // process pawns
        for(int i=0;i<8;i++) {
            if (whitePawns[i] >= 1) {
                // doubled pawns
                eval -= (pieceValue['P'] * (whitePawns[i] - 1))/doubledPawnsPenalty;

                // isolated pawns
                bool isolated = true;
                if ((i > 0 && whitePawns[i-1] > 0) || (i < 7 && whitePawns[i+1] > 0)){
                    isolated = false;
                }
                if (isolated)
                    eval -= (pieceValue['P'] * whitePawns[i])/isolatedPawnsPenalty;
            }

            if (blackPawns[i] >= 1) {
                // doubled pawns
                eval -= (pieceValue['p'] * (blackPawns[i] - 1))/doubledPawnsPenalty;

                // isolated pawns
                bool isolated = true;
                if ((i > 0 && blackPawns[i-1] > 0) || (i < 7 && blackPawns[i+1] > 0)){
                    isolated = false;
                }
                if (isolated)
                    eval -= (pieceValue['p'] * blackPawns[i])/isolatedPawnsPenalty;
            }
        }

        evalCalculated = true;

        return eval;
    }

    bool isThreeFold() {
        return hashHistory[boardHash] >= 3;
    }

    bool isCheckmate() {
        int boardEval = getBoardEval();
        return boardEval > checkmateEval || boardEval < -checkmateEval;
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

    uint64_t getHash() {
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
    int gamePhaseTable[256]{}; // piece, middle game/ end game, row, col
    int pieceValue[256]{};

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
        hashHistory[boardHash]++;
    }

    void initPieceValues() {
        pieceValue['p'] = -100;
        pieceValue['r'] = -500;
        pieceValue['n'] = -320;
        pieceValue['b'] = -325;
        pieceValue['q'] = -900;
        pieceValue['k'] = -20000;

        pieceValue['P'] = 100;
        pieceValue['R'] = 500;
        pieceValue['N'] = 320;
        pieceValue['B'] = 325;
        pieceValue['Q'] = 900;
        pieceValue['K'] = 20000;

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
            return (int)(rookMobility * sqrt(moves));
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
                        moves++;
                    }

                    // captured a piece and can't go beyond
                    if (isPieceOfOppositeColor(turn, board[newI][newJ])) {
                        moves++;
                        break;
                    }
                }
            }
            return (int)(queenMobility * sqrt(moves));
        } else {
            return 0;
        }
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
};