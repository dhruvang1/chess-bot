#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <sstream>
#include <fstream>
#include <cmath>

#include "magicBoard.cpp"
using BoardType = MagicBoard;

#include "transposition.cpp"

using namespace std;
using namespace std::chrono;

// Late Move Reduction table: lmrTable[depth][moveIndex] gives the reduction amount.
// Late quiet moves at high depth get reduced more aggressively (up to 3-4 plies).
// Uses logarithmic formula so reductions scale naturally with both depth and move index.
static int lmrTable[64][64];
static bool lmrInitialized = false;

static void initLMR() {
    if (lmrInitialized) return;
    lmrTable[0][0] = 0;
    for (int d = 1; d < 64; d++) {
        for (int m = 1; m < 64; m++) {
            lmrTable[d][m] = 0.75 + log(d) * log(m) / 2.25;
        }
    }
    lmrInitialized = true;
}

class Search {
    static const int POSITIVE_NUM = 1 << 30;
    static const int NEGATIVE_NUM = - POSITIVE_NUM;
    static const int MAX_PV = 64;

    BoardType* board;
    vector<TTEntry> ttable;
    vector<TTEntry> qttable;
    uint16_t killers[100] = {};
    // History heuristic: history[pieceChar][toSquare] tracks how often a quiet move causes beta cutoffs.
    // Quiet moves that frequently cause cutoffs get ordered earlier, making LMR more effective
    // since the truly bad moves end up at high indices where they get aggressively reduced.
    // Indexed by ASCII char value (e.g. 'N'=78, 'p'=112) so 128 covers all pieces.
    int history[128][64] = {};

    int nodes = 0;
    int qNodes = 0;
    int cutOff = 0;
    int pvsSuccess = 0;
    int pvsFailure = 0;
    int lmrSuccess = 0;
    int lmrFailure = 0;
    int cacheHit= 0;
    int cacheFutileHit= 0;
    int cachePvHit= 0;
    int cacheSave= 0;
    int cacheSaveSuccess= 0;
    int deltaPrune = 0;
    int lmpPrune = 0;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    int BASE_NULL_MOVE_REDUCTION = 2;
    high_resolution_clock::time_point startTime;
    long softTimeLimitMs{};
    long hardTimeLimitMs{};
    int handicapTimeLeftMs = INT_MAX;
    MoveList orderedMovesLastRound;
    ofstream ofile;

    struct Node {
        int eval{0};
        uint16_t pv[MAX_PV] = {};
        int pvLen = 0;
        Node(){};

        Node(int eval) : eval(eval), pvLen(0) {}
    };

    static inline bool isManual() {
        auto isManual = getenv("manual");
        return isManual != nullptr && strcmp(isManual, "1") == 0;
    }

    inline void logMsg(string msg) {
        if (isManual()) {
            ofile << msg << "\n";
        }
    }

    inline bool shouldQuit() {
        auto currentTime = high_resolution_clock::now();
        auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
        return elapsedTime >= hardTimeLimitMs;
    }

    static string pvToString(const uint16_t* pv, int pvLen) {
        string s;
        for (int i = 0; i < pvLen; i++) {
            if (i > 0) s += ' ';
            s += moveToUci(pv[i]);
        }
        return s;
    }

    void initSearch(BoardType& currentBoard) {
        this->board = &currentBoard;
        nodes = 0;
        qNodes = 0;
        cutOff = 0;
        pvsSuccess = 0;
        pvsFailure = 0;
        lmrSuccess = 0;
        lmrFailure = 0;
        cacheHit = 0;
        cacheFutileHit = 0;
        cachePvHit = 0;
        cacheSave = 0;
        cacheSaveSuccess = 0;
        deltaPrune = 0;
        lmpPrune = 0;
        orderedMovesLastRound.clear();
        initKillers();
        startTime = high_resolution_clock::now();
    }

    void computeTimeLimits(int whiteTimeMs, int blackTimeMs) {
        const int actualTimeLeft = (board->turn == BoardType::WHITE) ? whiteTimeMs : blackTimeMs;
        int myTimeLeft = min(actualTimeLeft, handicapTimeLeftMs);

        // assume 60 moves for the game
        softTimeLimitMs = myTimeLeft / 60;
        if (board->moveCount() < 16) {
            // keep lower time limit in 16 plies of opening
            softTimeLimitMs = myTimeLeft / 100;
        } else if (board->moveCount() < 32) {
            // next 16 plies of late opening / middle game
            softTimeLimitMs = myTimeLeft / 80;
        } else if (board->moveCount() < 64) {
            // next 16 plies of pure middle game
            softTimeLimitMs = myTimeLeft / 50;
        }

        if (myTimeLeft < 10 * 1000) {
            // you can use 1x soft time limit
            hardTimeLimitMs = softTimeLimitMs;
        } else if (myTimeLeft < 15 * 1000) {
            // you can use 2x soft time limit
            hardTimeLimitMs = 2 * softTimeLimitMs;
        } else {
            // you can use min of (3x as much softLimit, or remaining time leaving 10s)
            hardTimeLimitMs = min(3 * softTimeLimitMs, myTimeLeft - 10000L);
        }
    }

    void logSearchResult(int depthEvaluated, int bestMoveEval, const string& bestMoveLine) {
        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);

        cout << "info depth " << depthEvaluated << " nodes " << nodes << " time " << duration.count() << " score cp " << bestMoveEval << " pv " << bestMoveLine << endl;
        cout << "info qnodes " << qNodes << " nullCutoff " << cutOff << endl;
        cout << "info pvs " << pvsSuccess << " " << pvsFailure << endl;
        cout << "info lmr " << lmrSuccess << " " << lmrFailure << endl;
        cout << "info delta " << deltaPrune << " lmp " << lmpPrune << endl;
        cout << "info cache " << "save " << cacheSave << " " << cacheSaveSuccess << " hit " << cacheHit << " " << cacheHit - cacheFutileHit - cachePvHit
             << " " << (cacheHit > 0 ? (100*(cacheHit - cacheFutileHit - cachePvHit))/cacheHit : 0) << " pvHit " << cachePvHit << endl;
    }

    string runSearch(int maxDepth) {
        int bestMoveEval = 0;
        uint16_t bestMove = MOVE_NONE;
        string bestMoveLine;
        int depthEvaluated = 0;

        for (int depth = START_DEPTH; depth <= maxDepth; depth++) {
            // if timer > softThreshold => quit loop
            auto currentTime = high_resolution_clock::now();
            auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
            if (elapsedTime >= softTimeLimitMs) {
                break;
            }

            auto result = negamax(NEGATIVE_NUM, POSITIVE_NUM, depth, 0, false);

            // hard time limit has passed, don't use the above result
            if (shouldQuit() && depth != START_DEPTH) {
                break;
            }

            // gather result
            depthEvaluated = depth;
            bestMoveLine = pvToString(result.pv, result.pvLen);
            bestMoveEval = result.eval;
            bestMove = result.pvLen > 0 ? result.pv[0] : MOVE_NONE;

            // if we find checkmate, there is no need to search deeper
            if (result.eval >= board->checkmateEval) {
                break;
            }
        }

        logSearchResult(depthEvaluated, bestMoveEval, bestMoveLine);
        return moveToUci(bestMove);
    }

    public:

    Search() {
//        ofile.open("log.txt");
        initLMR();
        ttable.reserve(TTSize);
        for(int i=0;i<TTSize;i++) {
            ttable.emplace_back();
        }
    }

    void setBoard(BoardType& b) {
        this->board = &b;
    }

    void logMembers() {
        cout << "orderedMovesLastRound: " << orderedMovesLastRound.size() << endl;
        cout << "TTable: " << ttable.size() << "  " << ttable.capacity() << endl;
    }

    string getBestMove(BoardType& currentBoard, int maxDepth) {
        initSearch(currentBoard);
        softTimeLimitMs = LONG_MAX;
        hardTimeLimitMs = LONG_MAX;
        return runSearch(maxDepth);
    }

    string getBestMove(BoardType& currentBoard, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        initSearch(currentBoard);
        computeTimeLimits(whiteTimeMs, blackTimeMs);

        string bestMove = runSearch(29);

        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);
        int myInc = (board->turn == BoardType::WHITE) ? whiteIncMs : blackIncMs;
        handicapTimeLeftMs = handicapTimeLeftMs - (int)duration.count() + myInc;
        if (handicapTimeLeftMs < 0) handicapTimeLeftMs = 0;

        return bestMove;
    }

    Node negamax(int alpha, int beta, int depth, int ply, bool nullAllowed) {
        nodes++;

        if (nullAllowed && board->isPositionRepeated()) {
            // give three-fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
            return {0};
        }

        if (!board->isKingPresent()) {
            return {-(BoardType::checkmateEval + depth)};
        }

        // Check transposition table for cached result
        const TTEntry* ttEntry = getTTEntry(board -> getHash());
        uint16_t ttMove = MOVE_NONE;
        if (ttEntry != nullptr) {
            if (alpha == beta -1 ) {
                cacheHit++;
                if (ttEntry->depth >= depth) {
                    if (ttEntry->flag == TTFlagExact) {
                        Node n(ttEntry->eval);
                        memcpy(n.pv, ttEntry->pv, ttEntry->pvLen * sizeof(uint16_t));
                        n.pvLen = ttEntry->pvLen;
                        return n;
                    } else if (ttEntry->flag == TTFlagBeta && ttEntry->eval >= beta) {
                        return {beta};
                    } else if (ttEntry->flag == TTFlagAlpha && ttEntry->eval <= alpha) {
                        return {alpha};
                    }
                }
                ttMove = ttEntry->pvLen > 0 ? ttEntry->pv[0] : MOVE_NONE;
                cacheFutileHit++;
            } else if (ttEntry->flag == TTFlagExact && ttEntry->depth >= depth && alpha < ttEntry->eval && ttEntry->eval < beta){
                cacheHit++;
                cachePvHit++;
                ttMove = ttEntry->pvLen > 0 ? ttEntry->pv[0] : MOVE_NONE;
                // TODO: We can just return directly here
            }
        } else if (depth > 3){
            // internal iterative deepening
            depth -= 1;
        }

        if (depth <= 0) {
            Node result = quiescenceSearch(alpha, beta, QSEARCH_MAX_DEPTH, ply + 1);
            saveInTT(result.pv, result.pvLen, result.eval, depth, TTFlagExact);
            return result;
        }

        if (shouldQuit()) {
            // only evaluate if we are just starting
            return {ply == 0 && depth == START_DEPTH ? board->getBoardEval(): 0};
        }

        bool inCheck = board->isKingInCheck();

        // check extension: being in check is tactically critical, search deeper
        // depth >= 2 avoids cascading explosion at the qsearch boundary
        if (inCheck && depth >= 2 && ply < 40) depth++;

        // reverse futility pruning: position is so far above beta, skip searching
        if (depth <= 3 && alpha == beta - 1 && !inCheck
            && abs(beta) < BoardType::checkmateEval) {
            int rfpMargin = depth * 150;
            if (board->getBoardEval() - rfpMargin >= beta) {
                return Node(beta);
            }
        }

        MoveList legalMoves;
        // use last round's move as starting point in search
        if (ply == 0 && !orderedMovesLastRound.empty()) {
            legalMoves = orderedMovesLastRound;
        } else {
            board->getLegalMoves(legalMoves);
            reorderMoves(legalMoves, ttMove, killers[2*ply], killers[2*ply + 1]);
        }

        if (legalMoves.empty()) {
            // it might look like we are checking twice for checkmate, once above and once now.
            // The above checks if there was an illegal move (not handling check) and the king is captured.
            // The below checks the line when we reach down a valid path and there are no legal moves.
            if (inCheck) {
                // this is checkmate
                return Node(-(BoardType::checkmateEval + depth));
            } else {
                // this is stalemate.
                // stalemate is still considered "bad" to not incentivize going for it in good or slightly bad positions.
                // its given eval of a minor piece => if the position is worse than a minor piece, than stalemate is considered better so "try" to go for it.
                return Node(-(BoardType::stalemateEval));
            }
        }

        // do null move
        if (nullAllowed && board->getGamePhase() > 0 && depth > 2) {
            board->processNullMove();
            Node result = negamax(-beta, -beta + 1, depth - 1 - (BASE_NULL_MOVE_REDUCTION + (depth / 7)), ply + 1, false);
            result.eval = -result.eval;

            // undo null move
            board->undoNullMove();

            if (result.eval >= beta) { // cutoff
                cutOff++;
                return result;
            }
        }

//        string prefix;
//        for(int i=0;i<maxDepth - depth;i++) {
//            prefix += "  ";
//        }


        vector<pair<int, Move>> resultList;
        uint16_t bestPv[MAX_PV] = {};
        int bestPvLen = 0;
        int index = 0;
        int ttflag = TTFlagAlpha;
        int maxEval = NEGATIVE_NUM;
        static constexpr int lmpThreshold[] = {0, 8, 14};
        for(const auto& m: legalMoves) {
            bool isQuiet = !(m.isPromotion || m.isCapture);

            // late move pruning: at shallow depth, skip late quiet moves
            if (ply > 0 && isQuiet && !inCheck && depth <= 2 && index >= lmpThreshold[depth]
                && m.move != killers[2*ply] && m.move != killers[2*ply+1]) {
                lmpPrune++;
                index++;
                continue;
            }

            board->processMove(m.move);
            Node result;
            if (index == 0) {
                result = negamax(-beta, -alpha, depth - 1, ply + 1, true);
                result.eval = -result.eval;
            } else {
                bool doPvs = true;
                // TODO: also apply LMR to losing captures (see(m) < 0)
                // inCheck refers to pre-move position: don't reduce when responding to check
                if (index >= 3 && isQuiet && depth >= 3 && !inCheck && m.move != killers[2*ply] && m.move != killers[2*ply+1]) {
                    int R = lmrTable[min(depth, 63)][min(index, 63)];
                    if (alpha != beta - 1) R -= 1; // reduce less at PV nodes
                    R = max(R, 1);
                    int newDepth = max(depth - 1 - R, 1);
                    result = negamax(-alpha - 1, -alpha, newDepth, ply + 1, true);
                    result.eval = -result.eval;
                    if (result.eval > alpha) {
                        // LMR failed, re-search at full depth via PVS below
                        lmrFailure++;
                    } else {
                        // LMR success, skip full-depth search
                        doPvs = false;
                        lmrSuccess++;
                    }
                }

                if (doPvs) {
                    result = negamax(-alpha - 1, -alpha, depth - 1, ply + 1, true);
                    result.eval = -result.eval;
                    if (result.eval > alpha && result.eval < beta) {
                        // pvs failed, do full search
                        pvsFailure++;
                        result = negamax(-beta, -alpha, depth - 1, ply + 1, true);
                        result.eval = -result.eval;
                    } else {
                        pvsSuccess++;
                    }
                }
            }
            // logmsg(format("{}m {} md {} d {} cp {}", prefix, move, maxDepth, depth, result.eval));
            board->undoMove();
            if (result.eval > maxEval) {
                maxEval = result.eval;
                bestPv[0] = m.move;
                int copyLen = min(result.pvLen, MAX_PV - 1);
                memcpy(bestPv + 1, result.pv, copyLen * sizeof(uint16_t));
                bestPvLen = copyLen + 1;
            }

            if (result.eval > alpha) {
                alpha = max(alpha, result.eval);
                ttflag = TTFlagExact;
            }

            if (ply == 0) {
                resultList.emplace_back(result.eval, m);
            }

            if (beta <= alpha) {
                ttflag = TTFlagBeta;
                if (isQuiet) {
                    if (killers[2*ply] != m.move) {
                        killers[2*ply + 1] = killers[2*ply];
                        killers[2*ply] = m.move;
                    }
                    history[(int)m.movePiece][toSq(m.move)] += depth * depth;
                }
                break;
            }
            index++;
        }


        if (ply == 0) {
            // with negamax we should always sort by descending
            sort(resultList.begin(), resultList.end(), [](auto left, auto right) {
                return right.first < left.first;
            });

            orderedMovesLastRound.clear();

            // fill orderedMoves for next round
            for (auto &i: resultList) {
                orderedMovesLastRound.push_back(i.second);
            }
        }

        saveInTT(bestPv, bestPvLen, maxEval, depth, ttflag);

        Node n(maxEval);
        memcpy(n.pv, bestPv, bestPvLen * sizeof(uint16_t));
        n.pvLen = bestPvLen;
        return n;
    }


    Node quiescenceSearch(int alpha, int beta, int depth, int minmaxDepth) {
        qNodes++;

        if (!board->isKingPresent()) {
            return Node(-(BoardType::checkmateEval + depth));
        }

        if (depth == 0) {
            return Node(board->getBoardEval());
        }

        int boardEval = board->getBoardEval();
        if (boardEval >= beta) {
            return Node(boardEval);
        }
        alpha = max(alpha, boardEval);


        MoveList legalMoves;
        board->getCapturesPromo(legalMoves);
        if (legalMoves.empty()) {
            // not perfect
            return Node(boardEval);
        }

        uint16_t noMove = MOVE_NONE;
        reorderMoves(legalMoves, noMove, noMove, noMove);

//        string prefix;
//        for(int i=0;i<minmaxDepth + (QSEARCH_MAX_DEPTH - depth);i++) {
//            prefix += "  ";
//        }

        uint16_t bestPv[MAX_PV] = {};
        int bestPvLen = 0;
        int maxEval = alpha;
        int delta = 300;
        for(const auto& m: legalMoves) {
            // delta pruning: even winning this piece cleanly can't raise alpha
            if (m.isCapture && (boardEval + abs(board->pieceValue[m.capturePiece]) + delta < alpha)) {
                deltaPrune++;
                continue;
            }

            // SEE pruning: this capture loses material in the exchange
            if (m.isCapture && board->see(m) < 0) {
                continue;
            }

            board->processMove(m.move);
            auto result = quiescenceSearch(-beta, -alpha, depth - 1, minmaxDepth);
            result.eval = -result.eval;
            board->undoMove();

            if (result.eval > maxEval) {
                maxEval = result.eval;
                bestPv[0] = m.move;
                int copyLen = min(result.pvLen, MAX_PV - 1);
                memcpy(bestPv + 1, result.pv, copyLen * sizeof(uint16_t));
                bestPvLen = copyLen + 1;
            }

            if (result.eval > alpha) {
                alpha = max(alpha, result.eval);
            }

            if (beta <= alpha) {
                break;
            }
        }

        Node n(maxEval);
        memcpy(n.pv, bestPv, bestPvLen * sizeof(uint16_t));
        n.pvLen = bestPvLen;
        return n;
    }

    void saveInTT(const uint16_t* pv, int pvLen, int eval, int depth, int flag) {
        cacheSave++;
        int index = (board->getHash() % TTKeySize) * 2;

        auto entry = &ttable[index];
        auto secondEntry = &ttable[index + 1];

        // fill up the first entry before moving ahead
        if (entry -> hash == 0) {
            entry -> update(board->getHash(), pv, pvLen, eval, depth, flag);
        } else if (depth >= entry -> depth) {
            secondEntry->update(entry);
            entry -> update(board->getHash(), pv, pvLen, eval, depth, flag);
        } else {
            secondEntry -> update(board->getHash(), pv, pvLen, eval, depth, flag);
        }
        cacheSaveSuccess++;
    }

    TTEntry* getTTEntry(uint64_t hash) {
        int index = (hash % TTKeySize) * 2;
        if (ttable[index].hash == hash) {
            return &ttable[index];
        } else if (ttable[index + 1].hash == hash) {
            return &ttable[index + 1];
        }

        return nullptr;
    }

    void initKillers() {
        memset(killers, 0, sizeof(killers));
        memset(history, 0, sizeof(history));
    }

    void reorderMoves(MoveList &legalMoves, uint16_t& ttMove, uint16_t& killer1, uint16_t& killer2) {
        const int* pv = board->pieceValue;
        sort(legalMoves.begin(), legalMoves.end(),[&, pv](const auto &left, const auto &right){
            auto score = [&](const Move& m) -> int {
                if (m.move == ttMove) return 60000 * 20000;
                if (m.isPromotion) return 50000 * 20000;
                if (m.isCapture) {
                    // MVV-LVA within tier, SEE as binary gate between tiers
                    int mvvlva = abs(40000 * pv[m.capturePiece] + pv[m.movePiece]);
                    if (board->see(m) >= 0) return 30000 * 20000 + mvvlva;
                    // below quiet moves, but still ordered by MVV-LVA within bad captures
                    return -(50000 * 20000) + mvvlva;
                }
                if (m.move == killer1) return 10000;
                if (m.move == killer2) return 9000;
                return history[(int)m.movePiece][toSq(m.move)];
            };
            return score(right) < score(left);
        });
    }
};
