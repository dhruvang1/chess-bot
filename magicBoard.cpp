#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <format>
#include <unordered_map>

#include "move.h"
#include "staticEvals.h"
#include "magic_constants.h"
#include "hash.cpp"
#include "nnue.h"

using namespace std;

class MagicBoard {
public:
    enum Color {
        WHITE = 0,
        BLACK = 1
    };

    struct UndoInfo {
        uint16_t move;
        int enPassantCol;
        int castlingRights;
        uint64_t boardHash;
        char capturedPiece;
        int capturedSq;
        int mgEval;
        int egEval;
        int gamePhase;
    };

    Color turn = WHITE;

    vector<UndoInfo> prevMoves;
    // vector<char> prevPiece;
    int enPassantCol = -1;

    static inline int checkmateEval = 15000;
    static inline int mateThreshold = checkmateEval - 100;
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
        castlingHashKeys[0] = hashHelper.whiteShortCastle;
        castlingHashKeys[1] = hashHelper.whiteLongCastle;
        castlingHashKeys[2] = hashHelper.blackShortCastle;
        castlingHashKeys[3] = hashHelper.blackLongCastle;

        loadNNUE("nnue/quantised.bin");

        initPieceValues();
        initEvalMap();
        initGamePhaseTable();
        initCastlingMask();
        setup();
        initPassPawnMasks();
        initKnightAttacks();
        initKingAttacks();
        initPawnAttacks();
        initBishopMasks();
        initRookMasks();
        initSliderTables();
    }

    void setupFromFen(const string& fen) {
        // Reset all state
        whitePawns = whiteKnights = whiteBishops = whiteRooks = whiteQueens = whiteKing = 0;
        blackPawns = blackKnights = blackBishops = blackRooks = blackQueens = blackKing = 0;
        allWhite = allBlack = occupied = 0;
        if (nnueLoaded) {
            memcpy(whiteAcc, nnueWeights.l0b, sizeof(whiteAcc));
            memcpy(blackAcc, nnueWeights.l0b, sizeof(blackAcc));
        }
        for (int sq = 0; sq < 64; sq++) board[sq] = ' ';
        prevMoves.clear();
        hashHistory.clear();
        boardHash = 0;
        mgEval = 0;
        egEval = 0;
        gamePhase = 0;
        evalCalculated = false;
        enPassantCol = -1;
        castlingRights = 0;

        stringstream ss(fen);
        string placement, activeColor, castling, enPassant;
        ss >> placement >> activeColor >> castling >> enPassant;

        // Parse piece placement (FEN starts at rank 8 = row 7, file a = col 0)
        int rank = 7, col = 0;
        for (char c : placement) {
            if (c == '/') {
                rank--;
                col = 0;
            } else if (c >= '1' && c <= '8') {
                col += (c - '0');
            } else {
                int sq = toSq(rank, col);
                board[sq] = c;
                uint64_t bb = sqToBB(sq);
                switch (c) {
                    case 'P': whitePawns   |= bb; break;
                    case 'N': whiteKnights |= bb; break;
                    case 'B': whiteBishops |= bb; break;
                    case 'R': whiteRooks   |= bb; break;
                    case 'Q': whiteQueens  |= bb; break;
                    case 'K': whiteKing    |= bb; break;
                    case 'p': blackPawns   |= bb; break;
                    case 'n': blackKnights |= bb; break;
                    case 'b': blackBishops |= bb; break;
                    case 'r': blackRooks   |= bb; break;
                    case 'q': blackQueens  |= bb; break;
                    case 'k': blackKing    |= bb; break;
                }
                col++;
            }
        }

        allWhite = whitePawns | whiteKnights | whiteBishops | whiteRooks | whiteQueens | whiteKing;
        allBlack = blackPawns | blackKnights | blackBishops | blackRooks | blackQueens | blackKing;
        occupied = allWhite | allBlack;

        turn = (activeColor == "b") ? BLACK : WHITE;

        for (char c : castling) {
            switch (c) {
                case 'K': castlingRights |= WHITE_OO;  break;
                case 'Q': castlingRights |= WHITE_OOO; break;
                case 'k': castlingRights |= BLACK_OO;  break;
                case 'q': castlingRights |= BLACK_OOO; break;
            }
        }

        if (enPassant != "-") {
            enPassantCol = enPassant[0] - 'a';
        }

        // Build hash
        for (int sq = 0; sq < 64; sq++) {
            if (board[sq] != ' ') {
                boardHash ^= hashHelper.getHash(board[sq], rowOf(sq), colOf(sq));
                addPieceEval(board[sq], sq);
            }
        }
        if (castlingRights & WHITE_OO)  boardHash ^= castlingHashKeys[0];
        if (castlingRights & WHITE_OOO) boardHash ^= castlingHashKeys[1];
        if (castlingRights & BLACK_OO)  boardHash ^= castlingHashKeys[2];
        if (castlingRights & BLACK_OOO) boardHash ^= castlingHashKeys[3];
        if (turn == BLACK) boardHash ^= hashHelper.getTurnHash();
        if (enPassantCol != -1) boardHash ^= hashHelper.getEnPassantHash(enPassantCol);

        hashHistory[boardHash]++;
    }

    void logMembers() {
    }

    void processMove(const string& uciStr) {
        processMove(uciToMove(uciStr));
    }

    void processMove(uint16_t move) {
        int currSq = ::fromSq(move);
        int newSq = ::toSq(move);

        char movedPiece = board[currSq];
        char gonePiece = board[newSq];

        if (!isPieceOfColor(turn, movedPiece)) {
            const string turn_value = turn == WHITE ? "WHITE" : "BLACK";
            throw std::invalid_argument("Moving a piece of wrong color (or empty piece), move: " + moveToUci(move) + " movedPiece: " + movedPiece + " turn: " + turn_value);
        }

        evalCalculated = false;

        // Snapshot irreversible state BEFORE any mutation
        UndoInfo info {
            move,
            enPassantCol,
            castlingRights,
            boardHash,
            gonePiece,
            newSq,  // default: captured piece is on destination
            mgEval,
            egEval,
            gamePhase
        };
        // add turn hash
        boardHash ^= hashHelper.getTurnHash();
        // reset en-passant hash
        maybeResetEnPassantHash();

        // handle promotion
        if (isPromoMove(move)) {
            char newPiece = promoChar(move);

            if (turn == WHITE) {
                newPiece -= 32;
            }

            // Remove captured piece
            if (gonePiece != ' ') {
                boardHash ^= pieceHash(gonePiece, newSq);
                getBitboard(gonePiece) &= ~sqToBB(newSq);
                removePieceEval(gonePiece, newSq);
            }

            // delete old pawn
            boardHash ^= pieceHash(movedPiece, currSq);
            getBitboard(movedPiece) &= ~sqToBB(currSq);
            board[currSq] = ' ';
            removePieceEval(movedPiece, currSq);

            // add new piece
            getBitboard(newPiece) |= sqToBB(newSq);
            board[newSq] = newPiece;
            boardHash ^= pieceHash(newPiece, newSq);
            addPieceEval(newPiece, newSq);

        } else if (isKing(board[currSq]) && abs(newSq - currSq) == 2) {
            // if King is moving two squares it is castling
            // update rook position
            if (newSq == currSq + 2) {
                // kingside castle — move rook
                int rookFrom = currSq + 3;
                int rookTo   = currSq + 1;
                char rook = board[rookFrom];
                boardHash ^= pieceHash(rook, rookFrom);
                boardHash ^= pieceHash(rook, rookTo);
                getBitboard(rook) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookTo] = rook;
                board[rookFrom] = ' ';
                movePieceEval(rook, rookFrom, rookTo);
            }
            else if (newSq == currSq - 2) {
                // queenside castle — move rook
                int rookFrom = currSq - 4;
                int rookTo   = currSq - 1;
                char rook = board[rookFrom];
                boardHash ^= pieceHash(rook, rookFrom);
                boardHash ^= pieceHash(rook, rookTo);
                getBitboard(rook) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookTo] = rook;
                board[rookFrom] = ' ';
                movePieceEval(rook, rookFrom, rookTo);
            }
            boardHash ^= pieceHash(movedPiece, currSq);
            boardHash ^= pieceHash(movedPiece, newSq);
            getBitboard(movedPiece) ^= sqToBB(currSq) | sqToBB(newSq);
            board[newSq] = movedPiece;
            board[currSq] = ' ';
            movePieceEval(movedPiece, currSq, newSq);
        } else {
            // handle double pawn moves
            if (isPawn(movedPiece) && abs(rowOf(newSq) - rowOf(currSq)) == 2) {
                boardHash ^= hashHelper.getEnPassantHash(colOf(currSq));
                enPassantCol = colOf(currSq);
            } else if (isPawn(movedPiece) && abs(colOf(currSq) - colOf(newSq)) == 1 && gonePiece == ' ') {
                // handle en-passant
                // if pawn captured (i.e. changed column) and there is no gonePiece => it's en-passant

                // capturedSq is one row behind toSq (from the moving side's perspective)
                int capturedSq = newSq + (turn == WHITE ? -8 : 8);
                info.capturedSq = capturedSq;
                info.capturedPiece = board[capturedSq];

                // remove the captured pawn
                boardHash ^= pieceHash(board[capturedSq], capturedSq);
                removePieceEval(board[capturedSq], capturedSq);
                getBitboard(board[capturedSq]) &= ~sqToBB(capturedSq);
                board[capturedSq] = ' ';
            }

            if (gonePiece != ' ') {
                boardHash ^= pieceHash(gonePiece, newSq);
                getBitboard(gonePiece) &= ~sqToBB(newSq);
                removePieceEval(gonePiece, newSq);
            }

            // Actual piece movement
            boardHash ^= pieceHash(movedPiece, currSq);
            boardHash ^= pieceHash(movedPiece, newSq);
            getBitboard(movedPiece) ^= sqToBB(currSq) | sqToBB(newSq);
            board[newSq] = movedPiece;
            board[currSq] = ' ';
            movePieceEval(movedPiece, currSq, newSq);
        }

        prevMoves.push_back(std::move(info));

        int oldCastlingRights = castlingRights;
        castlingRights &= castlingMask[currSq] & castlingMask[newSq];
        hashCastlingDelta(oldCastlingRights, castlingRights);

        allWhite = whitePawns | whiteKnights | whiteBishops | whiteRooks | whiteQueens | whiteKing;
        allBlack = blackPawns | blackKnights | blackBishops | blackRooks | blackQueens | blackKing;
        occupied = allWhite | allBlack;
        flipTurn();

        hashHistory[boardHash]++;
    }

    void undoMove() {
        if (prevMoves.empty()) {
            throw std::invalid_argument("cannot undo from empty list");
        }
        evalCalculated = false;

        // Flip turn first (so 'turn' = the side that made the move)
        flipTurn();

        hashHistory[boardHash]--;
        if (hashHistory[boardHash] == 0) {
            hashHistory.erase(boardHash);
        }

        UndoInfo info = prevMoves.back();
        prevMoves.pop_back();

        const int from = ::fromSq(info.move);
        const int to = ::toSq(info.move);

        // Reverse accumulator updates before board state changes (we need current board[] to read piece types)
        if (nnueLoaded) {
            int wi, bi;
            if (isPromoMove(info.move)) {
                // makeMove did: remove pawn@from, [remove capture@to], add promotedPiece@to
                // reverse:      add pawn@from,   [add capture@to],     remove promotedPiece@to
                char pawn = (turn == WHITE) ? 'P' : 'p';
                char promotedPiece = board[to];
                getFeatureIndices(promotedPiece, to, wi, bi);
                accSub(whiteAcc, wi); accSub(blackAcc, bi);
                getFeatureIndices(pawn, from, wi, bi);
                accAdd(whiteAcc, wi); accAdd(blackAcc, bi);
                if (info.capturedPiece != ' ') {
                    getFeatureIndices(info.capturedPiece, to, wi, bi);
                    accAdd(whiteAcc, wi); accAdd(blackAcc, bi);
                }
            } else if (isKing(board[to]) && abs(from - to) == 2) {
                // Castling: reverse king move and rook move
                char king = board[to];
                getFeatureIndices(king, to, wi, bi);
                accSub(whiteAcc, wi); accSub(blackAcc, bi);
                getFeatureIndices(king, from, wi, bi);
                accAdd(whiteAcc, wi); accAdd(blackAcc, bi);
                int rookFrom, rookTo;
                if (to == from + 2) { rookFrom = from + 3; rookTo = from + 1; }
                else                { rookFrom = from - 4; rookTo = from - 1; }
                char rook = board[rookTo];
                getFeatureIndices(rook, rookTo, wi, bi);
                accSub(whiteAcc, wi); accSub(blackAcc, bi);
                getFeatureIndices(rook, rookFrom, wi, bi);
                accAdd(whiteAcc, wi); accAdd(blackAcc, bi);
            } else {
                // Normal move (including en-passant)
                // makeMove did: [remove capture@capturedSq], move movedPiece from->to
                // reverse:      [add capture@capturedSq],    move movedPiece to->from
                char movedPiece = board[to];
                getFeatureIndices(movedPiece, to, wi, bi);
                accSub(whiteAcc, wi); accSub(blackAcc, bi);
                getFeatureIndices(movedPiece, from, wi, bi);
                accAdd(whiteAcc, wi); accAdd(blackAcc, bi);
                if (info.capturedPiece != ' ') {
                    getFeatureIndices(info.capturedPiece, info.capturedSq, wi, bi);
                    accAdd(whiteAcc, wi); accAdd(blackAcc, bi);
                }
            }
        }

        if (isPromoMove(info.move)) {
            // Promotion: remove promoted piece, restore pawn
            char promotedPiece = board[to];
            getBitboard(promotedPiece) &= ~sqToBB(to);

            char pawn = (turn == WHITE) ? 'P' : 'p';
            getBitboard(pawn) |= sqToBB(from);
            board[from] = pawn;
            board[to] = info.capturedPiece;

            if (info.capturedPiece != ' ') {
                getBitboard(info.capturedPiece) |= sqToBB(to);
            }
        }
        else if (isKing(board[to]) && abs(from - to) == 2) {
            // Castling: reverse king move
            getBitboard(board[to]) ^= sqToBB(from) | sqToBB(to);
            board[from] = board[to];
            board[to] = ' ';

            // Reverse rook move
            if (to == from + 2) {
                const int rookFrom = from + 3;
                const int rookTo = from + 1;
                getBitboard(board[rookTo]) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookFrom] = board[rookTo];
                board[rookTo] = ' ';
            } else {
                const int rookFrom = from - 4;
                const int rookTo = from - 1;
                getBitboard(board[rookTo]) ^= sqToBB(rookFrom) | sqToBB(rookTo);
                board[rookFrom] = board[rookTo];
                board[rookTo] = ' ';
            }
        }
        else {
            // Reverse piece movement
            getBitboard(board[to]) ^= sqToBB(from) | sqToBB(to);
            board[from] = board[to];
            board[to] = ' ';

            // Restore captured piece at its actual square, handles en-passant as well
            if (info.capturedPiece != ' ') {
                board[info.capturedSq] = info.capturedPiece;
                getBitboard(info.capturedPiece) |= sqToBB(info.capturedSq);
            }
        }

        // Restore all irreversible state from the snapshot — no re-derivation needed
        enPassantCol = info.enPassantCol;
        castlingRights = info.castlingRights;
        boardHash = info.boardHash;
        mgEval = info.mgEval;
        egEval = info.egEval;
        gamePhase = info.gamePhase;

        // Recompute aggregates
        allWhite = whitePawns | whiteKnights | whiteBishops | whiteRooks | whiteQueens | whiteKing;
        allBlack = blackPawns | blackKnights | blackBishops | blackRooks | blackQueens | blackKing;
        occupied = allWhite | allBlack;
    }

    void processNullMove() {
        evalCalculated = false;
        UndoInfo info {MOVE_NONE, enPassantCol, castlingRights, boardHash, ' ', -1, mgEval, egEval, gamePhase};
        prevMoves.push_back(std::move(info));
        boardHash ^= hashHelper.getTurnHash();
        maybeResetEnPassantHash();
        flipTurn();
    }

    void undoNullMove() {
        evalCalculated = false;
        const UndoInfo info = std::move(prevMoves.back());
        prevMoves.pop_back();
        enPassantCol = info.enPassantCol;
        boardHash = info.boardHash;
        mgEval = info.mgEval;
        egEval = info.egEval;
        gamePhase = info.gamePhase;
        flipTurn();
    }

    void getCapturesPromo(MoveList& legalMoves) {
        generateMoves(legalMoves, true);
    }

    void getLegalMoves(MoveList& legalMoves) {
        generateMoves(legalMoves, false);
    }

    bool isKingInCheck() {
        uint64_t myKing = (turn == WHITE) ? whiteKing : blackKing;
        const int kingSq = __builtin_ctzll(myKing);
        return isSquareAttackedByColor(kingSq, flipColor(turn));
    }

    bool isSquareAttackedByColor(int sq, Color color) {
        uint64_t pawns, knights, bishops, rooks, queens, king;
        if (color == WHITE) {
            pawns = whitePawns; knights = whiteKnights; bishops = whiteBishops;
            rooks = whiteRooks; queens = whiteQueens; king = whiteKing;
        } else {
            pawns = blackPawns; knights = blackKnights; bishops = blackBishops;
            rooks = blackRooks; queens = blackQueens; king = blackKing;
        }

        // pawn attack is not symmetric, so check the attack with other color
        return (pawnAttackTable[1 - color][sq] & pawns)
             | (knightAttackTable[sq] & knights)
             | (kingAttackTable[sq] & king)
             | (getBishopAttacks(sq, occupied) & (bishops | queens))
             | (getRookAttacks(sq, occupied) & (rooks | queens));
    }

    bool isSquareAttackedByColor(int row, int col, Color color) {
        return isSquareAttackedByColor(toSq(row, col), color);
    }

    int getBoardEval() {
        if (evalCalculated) {
            return eval;
        }

        if (nnueLoaded) {
            bool stmWhite = (turn == WHITE);
            eval = nnueForward(stmWhite ? whiteAcc : blackAcc,
                               stmWhite ? blackAcc : whiteAcc);
            evalCalculated = true;
            return eval;
        }

        eval = 0;
        int wpCounts[8]{};  // white pawns per file
        int bpCounts[8]{};  // black pawns per file

        // Count pawns per file for pawn structure eval
        uint64_t wp = whitePawns;
        while (wp) { int sq = __builtin_ctzll(wp); wpCounts[colOf(sq)]++; wp &= wp - 1; }
        uint64_t bp = blackPawns;
        while (bp) { int sq = __builtin_ctzll(bp); bpCounts[colOf(sq)]++; bp &= bp - 1; }

        int gp = min(gamePhase, 24);

        // Tapered eval: blend incrementally maintained middlegame and endgame scores
        eval += (gp * mgEval + (24 - gp) * egEval) / 24;

        // Bishop pair: bonus scales from 5% to 30% of pawn value as pieces come off
        double bishopPairBonus = (gp * 0.05 + (24 - gp) * 0.3) / 24;

        if ((whiteBishops & lightSquareMask) && (whiteBishops & darkSquareMask)) {
            eval += int(bishopPairBonus * pieceValue['P']);
        }
        if ((blackBishops & lightSquareMask) && (blackBishops & darkSquareMask)) {
            eval += int(bishopPairBonus * pieceValue['p']);
        }

        // Pawn structure penalties scale up toward endgame
        double doubledPawnsPenalty = (24 - gp) * 0.4 / 24;
        double doubledIsolatedPawnsPenalty = (24 - gp) * 0.2 / 24;
        double isolatedPawnsPenalty = (24 - gp) * 0.4 / 24;
        // Passed pawn bonus scales by rank: further advanced = exponentially more valuable
        // Index by rank for white (rank 2=row1 through rank 7=row6)
        static const int passedPawnByRank[] = {0, 5, 10, 20, 40, 80, 120, 0};
        double passedPawnPhase = (gp * 0.3 + (24 - gp) * 1.0) / 24;

        // king squares for passed pawn proximity (king escort / interception)
        int wkSq = __builtin_ctzll(whiteKing);
        int bkSq = __builtin_ctzll(blackKing);
        int wkR = rowOf(wkSq), wkC = colOf(wkSq);
        int bkR = rowOf(bkSq), bkC = colOf(bkSq);
        // proximity weight scales from 0 (opening) to 5cp per distance unit (endgame)
        int proxWeight = 5 * (24 - gp) / 24;

        for (int j = 0; j < 8; j++) {
            if (wpCounts[j] >= 1) {
                bool isolated = (j == 0 || wpCounts[j-1] == 0) && (j == 7 || wpCounts[j+1] == 0);
                if (isolated) {
                    eval -= int(pieceValue['P'] * wpCounts[j] * isolatedPawnsPenalty);
                    eval -= int(pieceValue['P'] * (wpCounts[j] - 1) * doubledIsolatedPawnsPenalty);
                } else {
                    eval -= int(pieceValue['P'] * (wpCounts[j] - 1) * doubledPawnsPenalty);
                }

                // Passed pawn: no enemy pawns ahead on same or adjacent files
                for (int r = 6; r >= 1; r--) {
                    if (whitePawns & sqToBB(toSq(r, j))) {
                        if ((whitePassPawnMask[r][j] & blackPawns) == 0) {
                            eval += int(passedPawnByRank[r] * passedPawnPhase);
                            // king-pawn proximity: reward our king near, enemy king far
                            int friendDist = max(abs(wkR - r), abs(wkC - j));
                            int enemyDist  = max(abs(bkR - r), abs(bkC - j));
                            eval += proxWeight * (enemyDist - friendDist);
                        }
                        break;
                    }
                }
            }

            if (bpCounts[j] >= 1) {
                bool isolated = (j == 0 || bpCounts[j-1] == 0) && (j == 7 || bpCounts[j+1] == 0);
                if (isolated) {
                    eval -= int(pieceValue['p'] * bpCounts[j] * isolatedPawnsPenalty);
                    eval -= int(pieceValue['p'] * (bpCounts[j] - 1) * doubledIsolatedPawnsPenalty);
                } else {
                    eval -= int(pieceValue['p'] * (bpCounts[j] - 1) * doubledPawnsPenalty);
                }

                // For black, row 1 = rank 2 (most advanced), so bonus = passedPawnByRank[7-r]
                for (int r = 1; r <= 6; r++) {
                    if (blackPawns & sqToBB(toSq(r, j))) {
                        if ((blackPassPawnMask[r][j] & whitePawns) == 0) {
                            eval -= int(passedPawnByRank[7 - r] * passedPawnPhase);
                            // king-pawn proximity: reward black's king near, white's king far
                            int friendDist = max(abs(bkR - r), abs(bkC - j));
                            int enemyDist  = max(abs(wkR - r), abs(wkC - j));
                            eval -= proxWeight * (enemyDist - friendDist);
                        }
                        break;
                    }
                }
            }
        }

        // King safety: reward pawn shield in front of castled king
        if (gp >= 16) {
            int wkSq = __builtin_ctzll(whiteKing);
            int wkRow = rowOf(wkSq), wkCol = colOf(wkSq);
            if (wkRow == 0 && wkCol == 6) {  // kingside castle position
                eval += int((__builtin_popcountll(whitePawns & 0x0000000000E000ULL)) * 0.15 * pieceValue['P']); // f2,g2,h2
                eval += int((__builtin_popcountll(whitePawns & 0x00000000E00000ULL)) * 0.1 * pieceValue['P']);  // f3,g3,h3
            } else if (wkRow == 0 && wkCol == 1) {  // queenside castle position
                eval += int((__builtin_popcountll(whitePawns & 0x0000000000000700ULL)) * 0.15 * pieceValue['P']); // a2,b2,c2
                eval += int((__builtin_popcountll(whitePawns & 0x0000000000070000ULL)) * 0.1 * pieceValue['P']);  // a3,b3,c3
            }

            int bkSq = __builtin_ctzll(blackKing);
            int bkRow = rowOf(bkSq), bkCol = colOf(bkSq);
            if (bkRow == 7 && bkCol == 6) {
                eval += int((__builtin_popcountll(blackPawns & 0x00E0000000000000ULL)) * 0.15 * pieceValue['p']); // f7,g7,h7
                eval += int((__builtin_popcountll(blackPawns & 0x0000E00000000000ULL)) * 0.1 * pieceValue['p']);  // f6,g6,h6
            } else if (bkRow == 7 && bkCol == 1) {
                eval += int((__builtin_popcountll(blackPawns & 0x0007000000000000ULL)) * 0.15 * pieceValue['p']); // a7,b7,c7
                eval += int((__builtin_popcountll(blackPawns & 0x0000070000000000ULL)) * 0.1 * pieceValue['p']);  // a6,b6,c6
            }
        }

        // Minor piece mobility: scales down toward endgame
        int minorDiff = getMinorMobility(WHITE) - getMinorMobility(BLACK);
        eval += (gp * minorDiff + (24 - gp) * minorDiff / 3) / 24;

        // Rook mobility: scales UP toward endgame (rooks dominate open positions)
        int rookDiff = getRookMobility(WHITE) - getRookMobility(BLACK);
        eval += (gp * rookDiff / 2 + (24 - gp) * rookDiff) / 24;

        // Queen mobility: flat weight
        eval += getQueenMobility(WHITE) - getQueenMobility(BLACK);

        evalCalculated = true;
        if (turn == BLACK) eval = -eval;

        return eval;
    }

    int see(int toSq, int fromSq) const {
        int gain[32];
        int depth = 0;

        char target = board[toSq];
        char attacker = board[fromSq];

        uint64_t occ = occupied;
        uint64_t fromBB = sqToBB(fromSq);
        occ ^= fromBB;

        gain[depth] = abs(pieceValue[target]);

        uint64_t attackers = getAttackersTo(toSq, occ) & occ;

        Color sideToMove = isPieceOfColor(WHITE, attacker) ? BLACK : WHITE;
        int attackerVal = abs(pieceValue[attacker]);

        while (true) {
            depth++;
            gain[depth] = attackerVal - gain[depth - 1];

            if (max(-gain[depth - 1], gain[depth]) < 0) break;

            uint64_t attackerBB = getLeastValuableAttacker(attackers, sideToMove, attackerVal);
            if (!attackerBB) break;

            occ ^= attackerBB;
            attackers = getAttackersTo(toSq, occ) & occ;

            sideToMove = flipColor(sideToMove);
        }

        while (--depth) {
            gain[depth - 1] = -max(-gain[depth - 1], gain[depth]);
        }

        return gain[0];
    }

    int see(const Move& m) const {
        return see(::toSq(m.move), ::fromSq(m.move));
    }

    int getGamePhase() {
        return min(gamePhase, 24);
    }

    bool isPositionRepeated() {
        auto it = hashHistory.find(boardHash);
        return it != hashHistory.end() && it->second > 1;
    }

    bool isKingPresent() {
        return whiteKing != 0 && blackKing != 0;
    }

    string printBoard() {
        string ans;
        for (int r = 7; r >= 0; r--) {
            for (int c = 0; c < 8; c++) {
                ans += board[toSq(r, c)];
                ans += "  ";
            }
            ans += "\n";
        }
        return ans;
    }

    uint64_t getHash() const {
        return boardHash;
    }

    string getFen() const {
        string fen;

        // Piece placement (rank 8 down to rank 1)
        for (int rank = 7; rank >= 0; rank--) {
            int empty = 0;
            for (int col = 0; col < 8; col++) {
                char p = board[toSq(rank, col)];
                if (p == ' ') {
                    empty++;
                } else {
                    if (empty > 0) { fen += char('0' + empty); empty = 0; }
                    fen += p;
                }
            }
            if (empty > 0) fen += char('0' + empty);
            if (rank > 0) fen += '/';
        }

        // Active color
        fen += (turn == WHITE) ? " w " : " b ";

        // Castling
        string castling;
        if (castlingRights & WHITE_OO)  castling += 'K';
        if (castlingRights & WHITE_OOO) castling += 'Q';
        if (castlingRights & BLACK_OO)  castling += 'k';
        if (castlingRights & BLACK_OOO) castling += 'q';
        fen += castling.empty() ? "-" : castling;

        // En passant
        if (enPassantCol == -1) {
            fen += " -";
        } else {
            int epRank = (turn == BLACK) ? 2 : 5;
            fen += ' ';
            fen += char('a' + enPassantCol);
            fen += char('1' + epRank);
        }

        // Halfmove clock and fullmove number (not tracked, use defaults)
        fen += " 0 " + to_string(moveCount() / 2 + 1);

        return fen;
    }

    size_t moveCount() const { return prevMoves.size(); }
    bool hasMoves() const { return !prevMoves.empty(); }
    string getMoveStr(size_t i) const { return moveToUci(prevMoves[i].move); }
    string getLastMoveStr() const {
        uint16_t m = prevMoves.back().move;
        return m == MOVE_NONE ? "null" : moveToUci(m);
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

    int mgEval = 0;
    int egEval = 0;
    int gamePhase = 0;
    int gamePhaseTable[256]{};

    bool evalCalculated = false;
    int eval = 0;

    int16_t whiteAcc[NNUE_HIDDEN]{};  // accumulator from white's perspective
    int16_t blackAcc[NNUE_HIDDEN]{};  // accumulator from black's perspective

    inline void addPieceEval(char piece, int sq) {
        mgEval += pieceValue[(int)piece] + evalTable[(int)piece][0][sq];
        egEval += pieceValue[(int)piece] + evalTable[(int)piece][1][sq];
        gamePhase += gamePhaseTable[(int)piece];
        if (nnueLoaded) {
            int wi, bi;
            getFeatureIndices(piece, sq, wi, bi);
            accAdd(whiteAcc, wi);
            accAdd(blackAcc, bi);
        }
    }

    inline void removePieceEval(char piece, int sq) {
        mgEval -= pieceValue[(int)piece] + evalTable[(int)piece][0][sq];
        egEval -= pieceValue[(int)piece] + evalTable[(int)piece][1][sq];
        gamePhase -= gamePhaseTable[(int)piece];
        if (nnueLoaded) {
            int wi, bi;
            getFeatureIndices(piece, sq, wi, bi);
            accSub(whiteAcc, wi);
            accSub(blackAcc, bi);
        }
    }

    inline void movePieceEval(char piece, int fromSq, int toSq) {
        mgEval += evalTable[(int)piece][0][toSq] - evalTable[(int)piece][0][fromSq];
        egEval += evalTable[(int)piece][1][toSq] - evalTable[(int)piece][1][fromSq];
        if (nnueLoaded) {
            int wi, bi;
            getFeatureIndices(piece, fromSq, wi, bi);
            accSub(whiteAcc, wi);
            accSub(blackAcc, bi);
            getFeatureIndices(piece, toSq, wi, bi);
            accAdd(whiteAcc, wi);
            accAdd(blackAcc, bi);
        }
    }

    int evalTable[256][2][64]{};

    uint64_t boardHash = 0;
    std::unordered_map<uint64_t, int> hashHistory;
    Hash hashHelper;
    uint64_t castlingHashKeys[4]{};

    static constexpr uint64_t darkSquareMask  = 0xAA55AA55AA55AA55ULL;
    static constexpr uint64_t lightSquareMask = 0x55AA55AA55AA55AAULL;
    static constexpr uint64_t fileAMask = 0x0101010101010101ULL;
    static constexpr uint64_t fileHMask = 0x8080808080808080ULL;

    static constexpr int knightMobilityWeight = 4;
    static constexpr int bishopMobilityWeight = 5;
    static constexpr int rookMobilityWeight   = 3;
    static constexpr int queenMobilityWeight  = 1;

    uint64_t whitePassPawnMask[8][8]{};
    uint64_t blackPassPawnMask[8][8]{};

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

        boardHash = 0;
        mgEval = 0;
        egEval = 0;
        gamePhase = 0;
        if (nnueLoaded) {
            memcpy(whiteAcc, nnueWeights.l0b, sizeof(whiteAcc));
            memcpy(blackAcc, nnueWeights.l0b, sizeof(blackAcc));
        }
        for (int sq = 0; sq < 64; sq++) {
            if (board[sq] != ' ') {
                boardHash ^= hashHelper.getHash(board[sq], rowOf(sq), colOf(sq));
                mgEval += pieceValue[(int)board[sq]] + evalTable[(int)board[sq]][0][sq];
                egEval += pieceValue[(int)board[sq]] + evalTable[(int)board[sq]][1][sq];
                gamePhase += gamePhaseTable[(int)board[sq]];
                if (nnueLoaded) {
                    int wi, bi;
                    getFeatureIndices(board[sq], sq, wi, bi);
                    accAdd(whiteAcc, wi);
                    accAdd(blackAcc, bi);
                }
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

    void initPassPawnMasks() {
        uint64_t whiteAheadCol[8][8]{};
        uint64_t blackAheadCol[8][8]{};

        for (int r = 6; r >= 1; r--) {
            for (int c = 0; c < 8; c++) {
                whiteAheadCol[r][c] |= 1ULL << toSq(r + 1, c);
                whiteAheadCol[r][c] |= whiteAheadCol[r + 1][c];
            }
        }
        for (int r = 1; r < 7; r++) {
            for (int c = 0; c < 8; c++) {
                blackAheadCol[r][c] |= 1ULL << toSq(r - 1, c);
                blackAheadCol[r][c] |= blackAheadCol[r - 1][c];
            }
        }
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                whitePassPawnMask[r][c] = whiteAheadCol[r][c];
                blackPassPawnMask[r][c] = blackAheadCol[r][c];
                if (c > 0) {
                    whitePassPawnMask[r][c] |= whiteAheadCol[r][c - 1];
                    blackPassPawnMask[r][c] |= blackAheadCol[r][c - 1];
                }
                if (c < 7) {
                    whitePassPawnMask[r][c] |= whiteAheadCol[r][c + 1];
                    blackPassPawnMask[r][c] |= blackAheadCol[r][c + 1];
                }
            }
        }
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
        turn = flipColor(turn);
    }

    inline void maybeResetEnPassantHash() {
        if (enPassantCol != -1) {
            boardHash ^= hashHelper.getEnPassantHash(enPassantCol);
            enPassantCol = -1;
        }
    }

    inline uint64_t pieceHash(char piece, int sq) {
        return hashHelper.getHash(piece, rowOf(sq), colOf(sq));
    }

    inline void hashCastlingDelta(int oldRights, int newRights) {
        int changed = oldRights ^ newRights;
        for (int bit = 0; bit < 4; bit++) {
            if (changed & (1 << bit)) {
                boardHash ^= castlingHashKeys[bit];
            }
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

    static inline Color flipColor(Color color) {
        return color == WHITE ? BLACK : WHITE;
    }

    static inline uint16_t encodeSq(int from, int to) {
        return encodeMove(from, to);
    }

    static inline uint16_t encodePromoSq(int from, int to, char piece) {
        return encodePromo(from, to, piece);
    }

    void addPromoMoves(MoveList& moves, int from, int to, char pawnChar) {
        moves.emplace_back(encodePromoSq(from, to, 'q'), pawnChar, false, true);
        moves.emplace_back(encodePromoSq(from, to, 'r'), pawnChar, false, true);
        moves.emplace_back(encodePromoSq(from, to, 'n'), pawnChar, false, true);
        moves.emplace_back(encodePromoSq(from, to, 'b'), pawnChar, false, true);
    }

    void addPromoCaptMoves(MoveList& moves, int from, int to, char pawnChar) {
        moves.emplace_back(encodePromoSq(from, to, 'q'), pawnChar, false, true);
        moves.emplace_back(encodePromoSq(from, to, 'r'), pawnChar, false, true);
        moves.emplace_back(encodePromoSq(from, to, 'n'), pawnChar, false, true);
        moves.emplace_back(encodePromoSq(from, to, 'b'), pawnChar, false, true);
    }

    bool canShortCastle() {
        int kingSq = (turn == WHITE) ? 4 : 60;
        int rights = (turn == WHITE) ? WHITE_OO : BLACK_OO;
        if (!(castlingRights & rights)) return false;
        if (board[kingSq + 1] != ' ' || board[kingSq + 2] != ' ') return false;
        Color enemy = flipColor(turn);
        return !isSquareAttackedByColor(kingSq, enemy)
            && !isSquareAttackedByColor(kingSq + 1, enemy)
            && !isSquareAttackedByColor(kingSq + 2, enemy);
    }

    bool canLongCastle() {
        int kingSq = (turn == WHITE) ? 4 : 60;
        int rights = (turn == WHITE) ? WHITE_OOO : BLACK_OOO;
        if (!(castlingRights & rights)) return false;
        if (board[kingSq - 1] != ' ' || board[kingSq - 2] != ' ' || board[kingSq - 3] != ' ') return false;
        Color enemy = flipColor(turn);
        return !isSquareAttackedByColor(kingSq, enemy)
            && !isSquareAttackedByColor(kingSq - 1, enemy)
            && !isSquareAttackedByColor(kingSq - 2, enemy);
    }

    void addPieceMoves(MoveList& legalMoves, uint64_t attacks, int fromSq, char piece, bool capturesOnly) {
        uint64_t friendly = (turn == WHITE) ? allWhite : allBlack;
        uint64_t moves = attacks & ~friendly;
        while (moves) {
            int to = __builtin_ctzll(moves);
            moves &= moves - 1;
            if (board[to] != ' ')
                legalMoves.emplace_back(encodeSq(fromSq, to), piece, board[to]);
            else if (!capturesOnly)
                legalMoves.emplace_back(encodeSq(fromSq, to), piece);
        }
    }

    void generateMoves(MoveList& legalMoves, bool capturesOnly) {
        uint64_t enemy = (turn == WHITE) ? allBlack : allWhite;
        Color enemyColor = flipColor(turn);

        // --- Pawns ---
        uint64_t pawns = (turn == WHITE) ? whitePawns : blackPawns;
        char pawnChar = (turn == WHITE) ? 'P' : 'p';
        int pushDir   = (turn == WHITE) ? 8 : -8;
        int startRank = (turn == WHITE) ? 1 : 6;
        int promoRank = (turn == WHITE) ? 7 : 0;

        uint64_t temp = pawns;
        while (temp) {
            int sq = __builtin_ctzll(temp);
            temp &= temp - 1;

            int r = rowOf(sq);

            // Single push
            int pushSq = sq + pushDir;
            if (pushSq >= 0 && pushSq < 64 && board[pushSq] == ' ') {
                if (rowOf(pushSq) == promoRank) {
                    addPromoMoves(legalMoves, sq, pushSq, pawnChar);
                } else if (!capturesOnly) {
                    legalMoves.emplace_back(encodeSq(sq, pushSq), pawnChar);
                }

                // Double push
                if (!capturesOnly) {
                    int doubleSq = sq + pushDir * 2;
                    if (r == startRank && board[doubleSq] == ' ') {
                        legalMoves.emplace_back(encodeSq(sq, doubleSq), pawnChar);
                    }
                }
            }

            // Captures
            uint64_t attacks = pawnAttackTable[turn][sq];
            uint64_t captures = attacks & enemy;
            while (captures) {
                int to = __builtin_ctzll(captures);
                captures &= captures - 1;
                if (rowOf(to) == promoRank) {
                    addPromoCaptMoves(legalMoves, sq, to, pawnChar);
                } else {
                    legalMoves.emplace_back(encodeSq(sq, to), pawnChar, board[to]);
                }
            }

            // En passant
            if (enPassantCol >= 0) {
                int epRank = (turn == WHITE) ? 5 : 2;
                int epSq = toSq(epRank, enPassantCol);
                if (attacks & sqToBB(epSq)) {
                    int capturedSq = epSq - pushDir;
                    legalMoves.emplace_back(encodeSq(sq, epSq), pawnChar, board[capturedSq]);
                }
            }
        }

        // --- Knights ---
        uint64_t knights = (turn == WHITE) ? whiteKnights : blackKnights;
        char knightChar = (turn == WHITE) ? 'N' : 'n';
        temp = knights;
        while (temp) {
            int sq = __builtin_ctzll(temp);
            temp &= temp - 1;
            addPieceMoves(legalMoves, knightAttackTable[sq], sq, knightChar, capturesOnly);
        }

        // --- Bishops ---
        uint64_t bishops = (turn == WHITE) ? whiteBishops : blackBishops;
        char bishopChar = (turn == WHITE) ? 'B' : 'b';
        temp = bishops;
        while (temp) {
            int sq = __builtin_ctzll(temp);
            temp &= temp - 1;
            addPieceMoves(legalMoves, getBishopAttacks(sq, occupied), sq, bishopChar, capturesOnly);
        }

        // --- Rooks ---
        uint64_t rooks = (turn == WHITE) ? whiteRooks : blackRooks;
        char rookChar = (turn == WHITE) ? 'R' : 'r';
        temp = rooks;
        while (temp) {
            int sq = __builtin_ctzll(temp);
            temp &= temp - 1;
            addPieceMoves(legalMoves, getRookAttacks(sq, occupied), sq, rookChar, capturesOnly);
        }

        // --- Queens ---
        uint64_t queens = (turn == WHITE) ? whiteQueens : blackQueens;
        char queenChar = (turn == WHITE) ? 'Q' : 'q';
        temp = queens;
        while (temp) {
            int sq = __builtin_ctzll(temp);
            temp &= temp - 1;
            addPieceMoves(legalMoves, getQueenAttacks(sq, occupied), sq, queenChar, capturesOnly);
        }

        // --- King ---
        uint64_t myKing = (turn == WHITE) ? whiteKing : blackKing;
        char kingChar = (turn == WHITE) ? 'K' : 'k';
        int kingSq = __builtin_ctzll(myKing);
        uint64_t kingMoves = kingAttackTable[kingSq] & ~((turn == WHITE) ? allWhite : allBlack);
        while (kingMoves) {
            int to = __builtin_ctzll(kingMoves);
            kingMoves &= kingMoves - 1;
            if (!isSquareAttackedByColor(to, enemyColor)) {
                if (board[to] != ' ')
                    legalMoves.emplace_back(encodeSq(kingSq, to), kingChar, board[to]);
                else if (!capturesOnly)
                    legalMoves.emplace_back(encodeSq(kingSq, to), kingChar);
            }
        }

        // Castling
        if (!capturesOnly) {
            if (canShortCastle()) {
                legalMoves.emplace_back(encodeSq(kingSq, kingSq + 2), kingChar, true, false);
            }
            if (canLongCastle()) {
                legalMoves.emplace_back(encodeSq(kingSq, kingSq - 2), kingChar, true, false);
            }
        }
    }

    // --- Attack tables ---
    uint64_t knightAttackTable[64]{};
    uint64_t kingAttackTable[64]{};
    uint64_t pawnAttackTable[2][64]{};  // [WHITE][sq], [BLACK][sq]
    uint64_t bishopMasks[64]{};
    uint64_t rookMasks[64]{};
    uint64_t bishopLookup[64][512]{};   // max 2^9
    uint64_t rookLookup[64][4096]{};    // max 2^12

    void initKnightAttacks() {
        const int offsets[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int sq = 0; sq < 64; sq++) {
            uint64_t bb = 0;
            int r = sq / 8, c = sq % 8;
            for (auto& o : offsets) {
                int nr = r + o[0], nc = c + o[1];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8)
                    bb |= 1ULL << (nr * 8 + nc);
            }
            knightAttackTable[sq] = bb;
        }
    }

    void initKingAttacks() {
        const int offsets[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int sq = 0; sq < 64; sq++) {
            uint64_t bb = 0;
            int r = sq / 8, c = sq % 8;
            for (auto& o : offsets) {
                int nr = r + o[0], nc = c + o[1];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8)
                    bb |= 1ULL << (nr * 8 + nc);
            }
            kingAttackTable[sq] = bb;
        }
    }

    void initPawnAttacks() {
        for (int sq = 0; sq < 64; sq++) {
            int r = sq / 8, c = sq % 8;
            uint64_t white = 0, black = 0;
            if (r < 7 && c > 0) white |= 1ULL << (sq + 7);
            if (r < 7 && c < 7) white |= 1ULL << (sq + 9);
            if (r > 0 && c > 0) black |= 1ULL << (sq - 9);
            if (r > 0 && c < 7) black |= 1ULL << (sq - 7);
            pawnAttackTable[WHITE][sq] = white;
            pawnAttackTable[BLACK][sq] = black;
        }
    }

    void initBishopMasks() {
        for (int sq = 0; sq < 64; sq++) {
            uint64_t mask = 0;
            int r = sq / 8, c = sq % 8;
            const int dirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
            for (auto& d : dirs) {
                int nr = r + d[0], nc = c + d[1];
                while (nr > 0 && nr < 7 && nc > 0 && nc < 7) {
                    mask |= 1ULL << (nr * 8 + nc);
                    nr += d[0]; nc += d[1];
                }
            }
            bishopMasks[sq] = mask;
        }
    }

    void initRookMasks() {
        for (int sq = 0; sq < 64; sq++) {
            uint64_t mask = 0;
            int r = sq / 8, c = sq % 8;
            for (int nc = 1; nc < 7; nc++) if (nc != c) mask |= 1ULL << (r * 8 + nc);
            for (int nr = 1; nr < 7; nr++) if (nr != r) mask |= 1ULL << (nr * 8 + c);
            rookMasks[sq] = mask;
        }
    }

    void initSliderTables() {
        for (int sq = 0; sq < 64; sq++) {
            // Bishop table
            uint64_t mask = bishopMasks[sq];
            int shift = 64 - BISHOP_BITS[sq];
            uint64_t subset = 0;
            do {
                int index = static_cast<int>((subset * BISHOP_MAGICS[sq]) >> shift);
                bishopLookup[sq][index] = bishopAttacksSlow(sq, subset);
                subset = (subset - mask) & mask;
            } while (subset);

            // Rook table
            mask = rookMasks[sq];
            shift = 64 - ROOK_BITS[sq];
            subset = 0;
            do {
                int index = static_cast<int>((subset * ROOK_MAGICS[sq]) >> shift);
                rookLookup[sq][index] = rookAttacksSlow(sq, subset);
                subset = (subset - mask) & mask;
            } while (subset);
        }
    }

    uint64_t getBishopAttacks(int sq, uint64_t occ) const {
        uint64_t relevant = occ & bishopMasks[sq];
        int index = static_cast<int>((relevant * BISHOP_MAGICS[sq]) >> (64 - BISHOP_BITS[sq]));
        return bishopLookup[sq][index];
    }

    uint64_t getRookAttacks(int sq, uint64_t occ) const {
        uint64_t relevant = occ & rookMasks[sq];
        int index = static_cast<int>((relevant * ROOK_MAGICS[sq]) >> (64 - ROOK_BITS[sq]));
        return rookLookup[sq][index];
    }

    uint64_t getQueenAttacks(int sq, uint64_t occ) const {
        return getBishopAttacks(sq, occ) | getRookAttacks(sq, occ);
    }

    // Compute pawn attack bitboard for one side from its pawn positions
    static uint64_t computePawnAttacks(uint64_t pawns, Color color) {
        if (color == WHITE) {
            return ((pawns & ~fileAMask) << 7) | ((pawns & ~fileHMask) << 9);
        } else {
            return ((pawns & ~fileHMask) >> 7) | ((pawns & ~fileAMask) >> 9);
        }
    }

    uint64_t getAttacks(int sq, char piece) const {
        switch (piece) {
            case 'N': case 'n': return knightAttackTable[sq];
            case 'B': case 'b': return getBishopAttacks(sq, occupied);
            case 'R': case 'r': return getRookAttacks(sq, occupied);
            case 'Q': case 'q': return getQueenAttacks(sq, occupied);
            default: return 0;
        }
    }

    int countPieceMobility(uint64_t pieces, uint64_t friendly, uint64_t safeMask,
                           int weight, char piece) const {
        int score = 0;
        while (pieces) {
            int sq = __builtin_ctzll(pieces);
            pieces &= pieces - 1;
            score += __builtin_popcountll(getAttacks(sq, piece) & ~friendly & safeMask) * weight;
        }
        return score;
    }

    int getMinorMobility(Color color) const {
        uint64_t friendly = (color == WHITE) ? allWhite : allBlack;
        uint64_t enemyPawnAttacks = computePawnAttacks(
            (color == WHITE) ? blackPawns : whitePawns, flipColor(color)
        );
        uint64_t safeMask = ~enemyPawnAttacks;
        char knightChar = (color == WHITE) ? 'N' : 'n';
        char bishopChar = (color == WHITE) ? 'B' : 'b';

        return countPieceMobility((color == WHITE) ? whiteKnights : blackKnights,
                                  friendly, safeMask, knightMobilityWeight, knightChar)
             + countPieceMobility((color == WHITE) ? whiteBishops : blackBishops,
                                  friendly, safeMask, bishopMobilityWeight, bishopChar);
    }

    int getRookMobility(Color color) const {
        uint64_t friendly = (color == WHITE) ? allWhite : allBlack;
        char rookChar = (color == WHITE) ? 'R' : 'r';

        return countPieceMobility((color == WHITE) ? whiteRooks : blackRooks,
                                  friendly, ~0ULL, rookMobilityWeight, rookChar);
    }

    int getQueenMobility(Color color) const {
        uint64_t friendly = (color == WHITE) ? allWhite : allBlack;
        char queenChar = (color == WHITE) ? 'Q' : 'q';

        return countPieceMobility((color == WHITE) ? whiteQueens : blackQueens,
                                  friendly, ~0ULL, queenMobilityWeight, queenChar);
    }

    // --- Static Exchange Evaluation (SEE) ---

    uint64_t getAttackersTo(int sq, uint64_t occ) const {
        return (pawnAttackTable[BLACK][sq] & whitePawns)
             | (pawnAttackTable[WHITE][sq] & blackPawns)
             | (knightAttackTable[sq] & (whiteKnights | blackKnights))
             | (getBishopAttacks(sq, occ) & (whiteBishops | blackBishops | whiteQueens | blackQueens))
             | (getRookAttacks(sq, occ) & (whiteRooks | blackRooks | whiteQueens | blackQueens))
             | (kingAttackTable[sq] & (whiteKing | blackKing));
    }

    // Returns the bitboard of the single least valuable attacker for the given color,
    // and sets pieceVal to its absolute piece value.
    uint64_t getLeastValuableAttacker(uint64_t attackers, Color color, int& pieceVal) const {
        uint64_t subset;

        subset = attackers & (color == WHITE ? whitePawns : blackPawns);
        if (subset) { pieceVal = pieceValue['P']; return subset & -subset; }

        subset = attackers & (color == WHITE ? whiteKnights : blackKnights);
        if (subset) { pieceVal = pieceValue['N']; return subset & -subset; }

        subset = attackers & (color == WHITE ? whiteBishops : blackBishops);
        if (subset) { pieceVal = pieceValue['B']; return subset & -subset; }

        subset = attackers & (color == WHITE ? whiteRooks : blackRooks);
        if (subset) { pieceVal = pieceValue['R']; return subset & -subset; }

        subset = attackers & (color == WHITE ? whiteQueens : blackQueens);
        if (subset) { pieceVal = pieceValue['Q']; return subset & -subset; }

        subset = attackers & (color == WHITE ? whiteKing : blackKing);
        if (subset) { pieceVal = pieceValue['K']; return subset & -subset; }

        return 0;
    }
};
