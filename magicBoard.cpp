#include <string>
#include <vector>
#include <cstdint>
#include <format>

#include "move.h"
#include "staticEvals.h"

using namespace std;

class MagicBoard {
public:
    enum Color {
        WHITE = 0,
        BLACK = 1
    };

    struct UndoInfo {
        string move;
        int enPassantCol;
        int castlingRights;
        uint64_t boardHash;
        char capturedPiece;
        int capturedSq;
    };

    Color turn = WHITE;

    vector<UndoInfo> prevMoves;
    // vector<char> prevPiece;
    int enPassantCol = -1;

    static inline int checkmateEval = 15000;
    static inline int stalemateEval = 0;

    int pieceValue[256]{};

    uint64_t whitePawns{};
    uint64_t whiteKnights{};
    uint64_t whiteBishops{};
    uint64_t whiteRooks{};
    uint64_t whiteQueens{};
    uint64_t whiteKing{};

    uint64_t blackPawns{};
    uint64_t blackKnights{};
    uint64_t blackBishops{};
    uint64_t blackRooks{};
    uint64_t blackQueens{};
    uint64_t blackKing{};

    uint64_t allWhite{};   // OR of all white pieces
    uint64_t allBlack{};   // OR of all black pieces
    uint64_t occupied{};   // allWhite | allBlack

    MagicBoard() {
        setup();
        initPieceValues();
        initEvalMap();
        initGamePhaseTable();
        initCastlingMask();
    }

    void logMembers() {
    }

    void processMove(const string& move) {
        if (move.length() != 4 && move.length() != 5) {
            throw std::invalid_argument( "received move with length not 4/5, move: " + move);
        }

        // assume the values are valid
        int currSq = uciToSq(move[0], move[1]);
        int newSq = uciToSq(move[2], move[3]);

        char movedPiece = board[currSq];
        char gonePiece = board[newSq];

        if (!isPieceOfColor(turn, movedPiece)) {
            const string turn_value = turn == WHITE ? "WHITE" : "BLACK";
            throw std::invalid_argument("Moving a piece of wrong color (or empty piece), move: " + move + " movedPiece: " + movedPiece + " turn: " + turn_value);
        }

        evalCalculated = false;

        // Snapshot irreversible state BEFORE any mutation
        UndoInfo info {
         move,
            enPassantCol,
            castlingRights,
            0, // fix when adding hashing
            gonePiece,
            newSq  // default: captured piece is on destination
        };

        // reset en-passant hash
        maybeResetEnPassantHash();

        // handle promotion
        if (move.length() == 5) {
            char newPiece = move[4];
            if (newPiece != 'r' && newPiece != 'q' && newPiece != 'b' && newPiece != 'n') {
                throw std::invalid_argument("Promoted piece is unknown - Move: " + move + " piece: " + newPiece);
            }

            if (turn == WHITE) {
                newPiece -= 32;
            }

            // TODO remove hash of gone piece
            // boardHash ^= hashHelper.getHash(board[newRow][newCol], newRow, newCol);

            // Remove captured piece
            if (gonePiece != ' ') {
                getBitboard(gonePiece) &= ~sqToBB(newSq);
            }

            // delete old pawn
            getBitboard(movedPiece) &= ~sqToBB(currSq);
            board[currSq] = ' ';

            // add new piece
            getBitboard(newPiece) |= sqToBB(newSq);
            board[newSq] = newPiece;

        } else if (isKing(board[currSq]) && abs(newSq - currSq) == 2) {
            // if King is moving two squares it is castling
            // update rook position
            if (newSq == currSq + 2) {
                // kingside castle — move rook
                int rookFrom = currSq + 3;
                int rookTo   = currSq + 1;
                getBitboard(board[rookFrom]) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookTo] = board[rookFrom];
                board[rookFrom] = ' ';
            }
            else if (newSq == currSq - 2) {
                // queenside castle — move rook
                int rookFrom = currSq - 4;
                int rookTo   = currSq - 1;
                getBitboard(board[rookFrom]) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookTo] = board[rookFrom];
                board[rookFrom] = ' ';
            }
            getBitboard(movedPiece) ^= sqToBB(currSq) | sqToBB(newSq);
            board[newSq] = movedPiece;
            board[currSq] = ' ';
        } else {
            if (isPawn(movedPiece) && abs(rowOf(newSq) - rowOf(currSq)) == 2) {
                // handle double pawn moves
                // boardHash ^= hashHelper.getEnPassantHash(currCol);
                enPassantCol = colOf(currSq);
            } else if (isPawn(movedPiece) && abs(colOf(currSq) - colOf(newSq)) == 1 && gonePiece == ' ') {
                // handle en-passant
                // if pawn captured (i.e. changed column) and there is no gonePiece => it's en-passant

                // remove the captured pawn
                // boardHash ^= hashHelper.getHash(board[currRow][newCol], currRow, newCol);

                // capturedSq is one row behind toSq (from the moving side's perspective)
                int capturedSq = newSq + (turn == WHITE ? -8 : 8);
                info.capturedSq = capturedSq;
                info.capturedPiece = board[capturedSq];

                getBitboard(board[capturedSq]) &= ~sqToBB(capturedSq);
                board[capturedSq] = ' ';
            }

            if (gonePiece != ' ') {
                getBitboard(gonePiece) &= ~sqToBB(newSq);
            }

            // Actual piece movement
            getBitboard(movedPiece) ^= sqToBB(currSq) | sqToBB(newSq);
            board[newSq] = movedPiece;
            board[currSq] = ' ';
        }

        prevMoves.push_back(std::move(info));

        castlingRights &= castlingMask[currSq] & castlingMask[newSq];
        allWhite = whitePawns | whiteKnights | whiteBishops | whiteRooks | whiteQueens | whiteKing;
        allBlack = blackPawns | blackKnights | blackBishops | blackRooks | blackQueens | blackKing;
        occupied = allWhite | allBlack;
        flipTurn();
    }

    void undoMove() {
        if (prevMoves.empty()) {
            throw std::invalid_argument("cannot undo from empty list");
        }
        evalCalculated = false;

        // Flip turn first (so 'turn' = the side that made the move)
        flipTurn();

        UndoInfo info = prevMoves.back();
        prevMoves.pop_back();
        const string lastMove = info.move;

        const int fromSq = uciToSq(lastMove[0], lastMove[1]);
        const int toSq = uciToSq(lastMove[2], lastMove[3]);

        if (lastMove.length() == 5) {
            // Promotion: remove promoted piece, restore pawn
            char promotedPiece = board[toSq];
            getBitboard(promotedPiece) &= ~sqToBB(toSq);

            char pawn = (turn == WHITE) ? 'P' : 'p';
            getBitboard(pawn) |= sqToBB(fromSq);
            board[fromSq] = pawn;
            board[toSq] = info.capturedPiece;

            if (info.capturedPiece != ' ') {
                getBitboard(info.capturedPiece) |= sqToBB(toSq);
            }
        }
        else if (isKing(board[toSq]) && abs(fromSq - toSq) == 2) {
            // Castling: reverse king move
            getBitboard(board[toSq]) ^= sqToBB(fromSq) | sqToBB(toSq);
            board[fromSq] = board[toSq];
            board[toSq] = ' ';

            // Reverse rook move
            if (toSq == fromSq + 2) {
                const int rookFrom = fromSq + 3;
                const int rookTo = fromSq + 1;
                getBitboard(board[rookTo]) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookFrom] = board[rookTo];
                board[rookTo] = ' ';
            } else {
                const int rookFrom = fromSq - 4;
                const int rookTo = fromSq - 1;
                getBitboard(board[rookTo]) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookFrom] = board[rookTo];
                board[rookTo] = ' ';
            }
        }
        else {
            // Reverse piece movement
            getBitboard(board[toSq]) ^= sqToBB(fromSq) | sqToBB(toSq);
            board[fromSq] = board[toSq];
            board[toSq] = ' ';

            // Restore captured piece at its actual square, handles en-passant as well
            if (info.capturedPiece != ' ') {
                board[info.capturedSq] = info.capturedPiece;
                getBitboard(info.capturedPiece) |= sqToBB(info.capturedSq);
            }
        }

        // Restore all irreversible state from the snapshot — no re-derivation needed
        enPassantCol = info.enPassantCol;
        castlingRights = info.castlingRights;
        // boardHash = info.boardHash;  // when you add hashing

        // Recompute aggregates
        allWhite = whitePawns | whiteKnights | whiteBishops | whiteRooks | whiteQueens | whiteKing;
        allBlack = blackPawns | blackKnights | blackBishops | blackRooks | blackQueens | blackKing;
        occupied = allWhite | allBlack;
    }

    void processNullMove() {
    }

    void undoNullMove() {
    }

    void getCapturesPromo(vector<Move>& legalMoves) {
    }

    void getLegalMoves(vector<Move>& legalMoves) {
    }

    bool isKingInCheck() {
        return false;
    }

    bool isSquareAttackedByColor(int i, int j, Color color) {
        return false;
    }

    int getBoardEval() {
        return 0;
    }

    int getGamePhase() {
        if (evalCalculated) {
            return gamePhase;
        }
        getBoardEval();
        return gamePhase;
    }

    bool isPositionRepeated() {
        return false;
    }

    bool isKingPresent() {
        return true;
    }

    string printBoard() {
        return "";
    }

    uint64_t getHash() const {
        return 0;
    }

    int getCastlingRights() {
        return castlingRights;
    }

    char getBoardChar(int row, int col) const {
        return board[toSq(row, col)];
    }

    char getFromBitBoards(int row, int col) const {
        uint64_t bit = 1ULL << toSq(row, col);
        int count = 0;
        char ans = ' ';
        if (whitePawns  & bit) {
            ans = 'P';
            count++;
        }
        if (whiteKnights & bit) {
            ans = 'N';
            count++;
        }
        if (whiteBishops & bit) {
            ans = 'B';
            count++;
        }
        if (whiteRooks   & bit) {
            ans = 'R';
            count++;
        }
        if (whiteQueens  & bit) {
            ans = 'Q';
            count++;
        }
        if (whiteKing & bit) {
            ans = 'K';
            count++;
        }
        if (blackPawns & bit) {
            ans = 'p';
            count++;
        }
        if (blackKnights & bit) {
            ans = 'n';
            count++;
        }
        if (blackBishops & bit) {
            ans = 'b';
            count++;
        }
        if (blackRooks & bit) {
            ans = 'r';
            count++;
        }
        if (blackQueens & bit) {
            ans = 'q';
            count++;
        }
        if (blackKing & bit) {
            ans = 'k';
            count++;
        }
        if (count > 1) {
            throw std::invalid_argument(format("Found multiple pieces at: ({}, {})", row, col));
        }
        return ans;
    }

private:
    static constexpr int WHITE_OO  = 1;  // white kingside
    static constexpr int WHITE_OOO = 2;  // white queenside
    static constexpr int BLACK_OO  = 4;  // black kingside
    static constexpr int BLACK_OOO = 8;  // black queenside

    // Capital chars for white, small chars for black
    // P, p -> pawn
    // R, r -> rook
    // N, n -> knight
    // B, b -> bishop
    // Q, q -> queen
    // K, k -> king
    char board[64]{};

    int castlingRights = WHITE_OO | WHITE_OOO | BLACK_OO | BLACK_OOO; // 15 at start
    int castlingMask[64]; // mapping squares to castling rights effects

    int gamePhase = 0;
    int gamePhaseTable[256]{}; // piece, middle game/ end game, row, col

    bool evalCalculated = false;
    int eval = 0;

    int evalTable[256][2][64]{};

    void setup() {
        // Row 1: all 8 white pawns (bits 8-15)
        whitePawns   = 0x000000000000FF00ULL;

        // Row 0: white pieces
        whiteRooks   = (1ULL << 0) | (1ULL << 7);     // a1, h1
        whiteKnights = (1ULL << 1) | (1ULL << 6);     // b1, g1
        whiteBishops = (1ULL << 2) | (1ULL << 5);     // c1, f1
        whiteQueens  = (1ULL << 3);                    // d1
        whiteKing    = (1ULL << 4);                    // e1

        // Row 6: all 8 black pawns (bits 48-55)
        blackPawns   = 0x00FF000000000000ULL;

        // Row 7: black pieces
        blackRooks   = (1ULL << 56) | (1ULL << 63);   // a8, h8
        blackKnights = (1ULL << 57) | (1ULL << 62);   // b8, g8
        blackBishops = (1ULL << 58) | (1ULL << 61);   // c8, f8
        blackQueens  = (1ULL << 59);                   // d8
        blackKing    = (1ULL << 60);                   // e8

        // combined occupancy
        allWhite = whitePawns | whiteRooks | whiteKnights | whiteBishops | whiteQueens | whiteKing;
        allBlack = blackPawns | blackRooks | blackKnights | blackBishops | blackQueens | blackKing;
        occupied = allWhite | allBlack;

        turn = WHITE;

        // setup character board
        for(int sq=0;sq<64;sq++) { // NOLINT(*-loop-convert)
            board[sq] = ' ';
        }

        for(int i=0;i<8;i++) {
            board[toSq(1,i)] = 'P';
            board[toSq(6,i)] = 'p';
        }

        board[toSq(0,0)] = 'R';
        board[toSq(0,1)] = 'N';
        board[toSq(0,2)] = 'B';
        board[toSq(0,3)] = 'Q';
        board[toSq(0,4)] = 'K';
        board[toSq(0,5)] = 'B';
        board[toSq(0,6)] = 'N';
        board[toSq(0,7)] = 'R';

        board[toSq(7,0)] = 'r';
        board[toSq(7,1)] = 'n';
        board[toSq(7,2)] = 'b';
        board[toSq(7,3)] = 'q';
        board[toSq(7,4)] = 'k';
        board[toSq(7,5)] = 'b';
        board[toSq(7,6)] = 'n';
        board[toSq(7,7)] = 'r';

        // TODO add hash and history
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
        for (int sq = 0; sq < 64; sq++) {
            int row = sq / 8;
            int col = sq % 8;
            int flipped = (7 - row) * 8 + col;

            evalTable['P'][0][sq] = mg_pawn_table[flipped];
            evalTable['R'][0][sq] = mg_rook_table[flipped];
            evalTable['N'][0][sq] = mg_knight_table[flipped];
            evalTable['B'][0][sq] = mg_bishop_table[flipped];
            evalTable['Q'][0][sq] = mg_queen_table[flipped];
            evalTable['K'][0][sq] = mg_king_table[flipped];

            evalTable['p'][0][sq] = -mg_pawn_table[row * 8 + col];
            evalTable['r'][0][sq] = -mg_rook_table[row * 8 + col];
            evalTable['n'][0][sq] = -mg_knight_table[row * 8 + col];
            evalTable['b'][0][sq] = -mg_bishop_table[row * 8 + col];
            evalTable['q'][0][sq] = -mg_queen_table[row * 8 + col];
            evalTable['k'][0][sq] = -mg_king_table[row * 8 + col];

            // same pattern for endgame [1]
            evalTable['P'][1][sq] = eg_pawn_table[flipped];
            evalTable['R'][1][sq] = eg_rook_table[flipped];
            evalTable['N'][1][sq] = eg_knight_table[flipped];
            evalTable['B'][1][sq] = eg_bishop_table[flipped];
            evalTable['Q'][1][sq] = eg_queen_table[flipped];
            evalTable['K'][1][sq] = eg_king_table[flipped];

            evalTable['p'][1][sq] = -eg_pawn_table[row * 8 + col];
            evalTable['r'][1][sq] = -eg_rook_table[row * 8 + col];
            evalTable['n'][1][sq] = -eg_knight_table[row * 8 + col];
            evalTable['b'][1][sq] = -eg_bishop_table[row * 8 + col];
            evalTable['q'][1][sq] = -eg_queen_table[row * 8 + col];
            evalTable['k'][1][sq] = -eg_king_table[row * 8 + col];
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

    void initCastlingMask() {
        for (int i = 0; i < 64; i++) {
            castlingMask[i] = 15; // no effect by default
        }

        castlingMask[0]  &= ~WHITE_OOO; // a1 rook
        castlingMask[4]  &= ~(WHITE_OO | WHITE_OOO); // e1 king
        castlingMask[7]  &= ~WHITE_OO;  // h1 rook
        castlingMask[56] &= ~BLACK_OOO; // a8 rook
        castlingMask[60] &= ~(BLACK_OO | BLACK_OOO); // e8 king
        castlingMask[63] &= ~BLACK_OO;  // h8 rook
    }


    uint64_t& getBitboard(char piece) {
        switch (piece) {
            case 'P': return whitePawns;
            case 'N': return whiteKnights;
            case 'B': return whiteBishops;
            case 'R': return whiteRooks;
            case 'Q': return whiteQueens;
            case 'K': return whiteKing;
            case 'p': return blackPawns;
            case 'n': return blackKnights;
            case 'b': return blackBishops;
            case 'r': return blackRooks;
            case 'q': return blackQueens;
            case 'k': return blackKing;
            default: throw std::invalid_argument("Unknown piece: "s + piece);
        }
    }

    inline void flipTurn() {
        turn = turn == WHITE ? BLACK : WHITE;
    }

    inline void maybeResetEnPassantHash() {
        if (enPassantCol != -1) {
            // boardHash ^= hashHelper.getEnPassantHash(enPassantCol);
            enPassantCol = -1;
        }
    }

    static inline bool isPawn(char c) {return c == 'p' || c == 'P';}
    static inline bool isRook(char c) {return c == 'r' || c == 'R';}
    static inline bool isKnight(char c) {return c == 'n' || c == 'N';}
    static inline bool isBishop(char c) {return c == 'b' || c == 'B';}
    static inline bool isQueen(char c) {return c == 'q' || c == 'Q';}
    static inline bool isKing(char c) {return c == 'k' || c == 'K';}

    static inline bool isPieceOfColor(Color color, char c) {
        return c != ' ' && (color == BLACK ? ('a' <= c && c <= 'z') : ('A' <= c && c <= 'Z'));
    }

    // row, col → square index
    static inline int toSq(int row, int col) { return row * 8 + col; }

    // square index → row, col
    static inline int rowOf(int sq) { return sq >> 3; }
    static inline int colOf(int sq) { return sq & 7; }

    // UCI string → square index
    static inline int uciToSq(char col, char row) { return (row - '1') * 8 + (col - 'a'); }

    // square index → UCI string (e.g. 4 → "e1")
    static inline string sqToUci(int sq) {
        string s;
        s += 'a' + colOf(sq);
        s += '1' + rowOf(sq);
        return s;
    }

    // square index → bit mask
    static inline uint64_t sqToBB(int sq) { return 1ULL << sq; }

};
