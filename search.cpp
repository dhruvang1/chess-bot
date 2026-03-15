#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
#include <cmath>
#include <chrono>

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
    static const int MAX_PLY = 128;

    BoardType* board;
    static inline vector<TTEntry> ttable;
    uint16_t killers[128] = {};
    // History heuristic: history[pieceChar][toSquare] tracks how often a quiet move causes beta cutoffs.
    // Quiet moves that frequently cause cutoffs get ordered earlier, making LMR more effective
    // since the truly bad moves end up at high indices where they get aggressively reduced.
    // Indexed by ASCII char value (e.g. 'N'=78, 'p'=112) so 128 covers all pieces.
    int history[128][64] = {};
    uint16_t countermoves[128][64] = {};

    // Triangular PV table: pvTable[ply][ply..ply+pvLength[ply]-1] stores the PV from that ply.
    // After search, pvTable[0][0..pvLength[0]-1] contains the full principal variation.
    uint16_t pvTable[MAX_PLY][MAX_PLY] = {};
    int pvLength[MAX_PLY] = {};

    int nodes = 0;
    int qNodes = 0;
    int cutOff = 0;
    int pvsSuccess = 0;
    int pvsFailure = 0;
    int lmrSuccess = 0;
    int lmrFailure = 0;
    int cacheHit= 0;
    int cacheFutileHit= 0;
    int cacheSave= 0;
    int cacheSaveSuccess= 0;
    int deltaPrune = 0;
    int lmpPrune = 0;
    int futilePrune = 0;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    int BASE_NULL_MOVE_REDUCTION = 2;
    high_resolution_clock::time_point startTime;
    bool shouldStop = false;
    long softTimeLimitMs{};
    long hardTimeLimitMs{};
    float earlyExitFraction{};
    long minThinkMs{};
    int handicapTimeLeftMs = INT_MAX;
    MoveList orderedMovesLastRound;
    ofstream ofile;

    inline void updatePv(int ply, uint16_t move) {
        pvTable[ply][ply] = move;
        int childLen = pvLength[ply + 1];
        int copyLen = min(childLen, MAX_PLY - ply - 1);
        memcpy(&pvTable[ply][ply + 1], &pvTable[ply + 1][ply + 1], copyLen * sizeof(uint16_t));
        pvLength[ply] = copyLen + 1;
    }

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
        if (shouldStop) {
            return true;
        }
        auto currentTime = high_resolution_clock::now();
        auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
        shouldStop = elapsedTime >= hardTimeLimitMs;
        return shouldStop;
    }

    string pvToString(int ply = 0) {
        string s;
        for (int i = 0; i < pvLength[ply]; i++) {
            if (i > 0) s += ' ';
            s += moveToUci(pvTable[ply][ply + i]);
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
        cacheSave = 0;
        cacheSaveSuccess = 0;
        deltaPrune = 0;
        lmpPrune = 0;
        futilePrune = 0;
        orderedMovesLastRound.clear();
        initKillers();
        startTime = high_resolution_clock::now();
        softTimeLimitMs = LONG_MAX;
        hardTimeLimitMs = LONG_MAX;
        shouldStop = false;
    }

    void computeTimeLimits(int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        const int actualTimeLeft = (board->turn == BoardType::WHITE) ? whiteTimeMs : blackTimeMs;
        int myInc = (board->turn == BoardType::WHITE) ? whiteIncMs : blackIncMs;
        int myTimeLeft = min(actualTimeLeft, handicapTimeLeftMs);

        // phase-aware time allocation: spend less in opening, more in middle game
        softTimeLimitMs = myTimeLeft / 60;
        if (board->moveCount() < 16) {
            // first 16 plies of opening: rely on development patterns, save time
            softTimeLimitMs = myTimeLeft / 100;
        } else if (board->moveCount() < 32) {
            // late opening / early middle game
            softTimeLimitMs = myTimeLeft / 80;
        } else if (board->moveCount() < 64) {
            // pure middle game: spend the most here
            softTimeLimitMs = myTimeLeft / 50;
        }
        softTimeLimitMs += myInc * 3 / 4;

        // tiered hard limits: get progressively tighter as time runs low
        if (myTimeLeft < 10 * 1000) {
            hardTimeLimitMs = softTimeLimitMs;
        } else if (myTimeLeft < 15 * 1000) {
            hardTimeLimitMs = 2 * softTimeLimitMs;
        } else {
            // 3x soft limit, but always keep a 10s reserve
            hardTimeLimitMs = min(3 * softTimeLimitMs, myTimeLeft - 10000L);
        }

        // Tapered early-exit fraction: how much of the soft limit must elapse
        // before stability can trigger an early exit.
        // Computed once per game from the initial clock so bullet games don't
        // get a shrinking fraction as time is consumed move-by-move.
        // 600s+ → 0.5 (spend at least half the budget), ~10s → 0.1 (exit ASAP).
        if (earlyExitFraction == 0.0f) {
            earlyExitFraction = 0.1f + 0.4f * min(1.0f, myTimeLeft / 600000.0f);
        }

        // Spend at least 75% of the increment before any stability exit fires.
        // Prevents gaining time every move in increment-heavy time controls (e.g. 1+1).
        // Zero for no-increment games so behavior is unchanged.
        minThinkMs = myInc * 3 / 4;
    }

    void logSearchResult(int depthEvaluated, int bestMoveEval, const string& bestMoveLine) {
        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);

        cout << "info depth " << depthEvaluated << " nodes " << nodes << " time " << duration.count() << " score cp " << bestMoveEval << " pv " << bestMoveLine << endl;
        cout << "info qnodes " << qNodes << " nullCutoff " << cutOff << endl;
        cout << "info pvs " << pvsSuccess << " " << pvsFailure << endl;
        cout << "info lmr " << lmrSuccess << " " << lmrFailure << endl;
        cout << "info delta " << deltaPrune << " lmp " << lmpPrune << " futile " << futilePrune << endl;
        cout << "info cache " << "save " << cacheSave << " " << cacheSaveSuccess << " hit " << cacheHit << " " << cacheHit - cacheFutileHit
             << " " << (cacheHit > 0 ? (100*(cacheHit - cacheFutileHit))/cacheHit : 0) << endl;
    }

    string runSearch(int maxDepth) {
        int bestMoveEval = 0;
        uint16_t bestMove = MOVE_NONE;
        string bestMoveLine;
        int depthEvaluated = 0;
        uint16_t lastBestMove = MOVE_NONE;
        int stabilityCount = 0;
        int prevEval = 0;

        for (int depth = START_DEPTH; depth <= maxDepth; depth++) {
            // don't start a new iteration past the soft limit
            auto currentTime = high_resolution_clock::now();
            auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
            if (elapsedTime >= softTimeLimitMs) {
                break;
            }

            // aspiration window: use narrow window around previous score,
            // widen exponentially on fail-high/fail-low
            int alpha = NEGATIVE_NUM, beta = POSITIVE_NUM;
            int aspiration = 50;
            if (depth >= 4) {
                alpha = bestMoveEval - aspiration;
                beta = bestMoveEval + aspiration;
            }

            int eval;
            while (true) {
                eval = negamax(alpha, beta, depth, 0, false);

                if (shouldQuit()) break;

                if (eval <= alpha) {
                    alpha = max(alpha - aspiration, NEGATIVE_NUM);
                    aspiration *= 2;
                } else if (eval >= beta) {
                    beta = min(beta + aspiration, POSITIVE_NUM);
                    aspiration *= 2;
                } else {
                    break;
                }
            }

            // hard time limit has passed, don't use the above result
            if (shouldQuit() && depth != START_DEPTH) {
                break;
            }

            int evalShift = abs(eval - prevEval);
            prevEval = eval;
            depthEvaluated = depth;
            bestMoveLine = pvToString();
            bestMoveEval = eval;
            bestMove = pvLength[0] > 0 ? pvTable[0][0] : MOVE_NONE;

            // move stability: if best move unchanged for 3+ iterations after a reasonable time, we are done
            if (bestMove == lastBestMove) {
                stabilityCount++;
            } else {
                stabilityCount = 0;
                lastBestMove = bestMove;
            }

            // eval instability: if eval is shifting a lot, extend soft limit up to 3x
            // small shifts (< 10cp) → no extension; large shifts (> 50cp) → full 3x extension
            float instabilityScale = 1.0f + min(2.0f, evalShift / 25.0f);
            long effectiveSoftLimit = (long)(softTimeLimitMs * instabilityScale);

            auto postSearchTime = duration_cast<milliseconds>(high_resolution_clock::now() - startTime).count();
            if (stabilityCount >= 15 && postSearchTime > (long)(effectiveSoftLimit * earlyExitFraction)) {
                break;
            }
            if (stabilityCount >= 7 && postSearchTime > max((long)(effectiveSoftLimit * earlyExitFraction), minThinkMs)) {
                break;
            }
            if (stabilityCount >= 3 && postSearchTime > max((long)(effectiveSoftLimit * (earlyExitFraction + 0.15f)), minThinkMs)) {
                break;
            }

            if (eval >= BoardType::mateThreshold) {
                break;
            }
        }

        logSearchResult(depthEvaluated, bestMoveEval, bestMoveLine);
        lastEval = bestMoveEval;
        return moveToUci(bestMove);
    }

    public:
    int lastEval = 0;
    int maxSearchDepth = 64;

    Search() {
//        ofile.open("log.txt");
        initLMR();
        if (ttable.empty()) {
            ttable.reserve(TTSize);
            for (int i = 0; i < TTSize; i++) {
                ttable.emplace_back();
            }
        } else {
            std::ranges::fill(ttable, TTEntry{});
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
        return runSearch(maxDepth);
    }

    string getBestMove(BoardType& currentBoard, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        initSearch(currentBoard);
        computeTimeLimits(whiteTimeMs, blackTimeMs, whiteIncMs, blackIncMs);

        string bestMove = runSearch(maxSearchDepth);

        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);
        int myInc = (board->turn == BoardType::WHITE) ? whiteIncMs : blackIncMs;
        handicapTimeLeftMs = handicapTimeLeftMs - (int)duration.count() + myInc;
        if (handicapTimeLeftMs < 0) handicapTimeLeftMs = 0;

        return bestMove;
    }

    int negamax(int alpha, int beta, int depth, int ply, bool nullAllowed, uint16_t prevMove = MOVE_NONE, char prevPiece = ' ') {
        nodes++;
        pvLength[ply] = 0;

        if (shouldStop) return 0;

        if (nullAllowed && board->isPositionRepeated()) {
            // give three-fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
            return 0;
        }

        if (!board->isKingPresent()) {
            return -(BoardType::checkmateEval - ply);
        }

        // Check transposition table for cached result
        const TTEntry* ttEntry = getTTEntry(board -> getHash());
        uint16_t ttMove = MOVE_NONE;
        if (ttEntry != nullptr) {
            int ttEval = mateScoreFromTT(ttEntry->eval, ply);
            if (alpha == beta - 1) {
                cacheHit++;
                if (ttEntry->depth >= depth) {
                    if (ttEntry->flag == TTFlagExact) {
                        return ttEval;
                    } else if (ttEntry->flag == TTFlagBeta && ttEval >= beta) {
                        return beta;
                    } else if (ttEntry->flag == TTFlagAlpha && ttEval <= alpha) {
                        return alpha;
                    }
                }
                ttMove = ttEntry->bestMove;
                cacheFutileHit++;
            } else {
                // PV node: only use TT for move ordering, never return early
                ttMove = ttEntry->bestMove;
            }
        } else if (depth > 3){
            // internal iterative deepening
            depth -= 1;
        }

        if (depth <= 0) {
            int eval = quiescenceSearch(alpha, beta, QSEARCH_MAX_DEPTH, ply);
            saveInTT(pvLength[ply] > 0 ? pvTable[ply][ply] : MOVE_NONE, eval, depth, TTFlagExact, ply);
            return eval;
        }

        if ((nodes & 4095) == 0 && shouldQuit()) {
            return ply == 0 && depth == START_DEPTH ? board->getBoardEval() : 0;
        }

        bool inCheck = board->isKingInCheck();

        // check extension: being in check is tactically critical, search deeper
        // depth >= 2 avoids cascading explosion at the qsearch boundary
        if (inCheck && depth >= 2 && ply < 40) depth++;

        // compute static eval once for shallow non-PV nodes; reused by RFP and futility pruning
        int staticEval = (depth <= 3 && alpha == beta - 1 && !inCheck) ? board->getBoardEval() : 0;

        // reverse futility pruning: position is so far above beta, skip searching
        if (depth <= 3 && alpha == beta - 1 && !inCheck
            && abs(beta) < BoardType::mateThreshold) {
            if (staticEval - depth * 150 >= beta) {
                return beta;
            }
        }

        uint16_t counterMove = MOVE_NONE;
        if (prevMove != MOVE_NONE) {
            counterMove = countermoves[(int)prevPiece][toSq(prevMove)];
        }

        MoveList legalMoves;
        // use last round's move as starting point in search
        if (ply == 0 && !orderedMovesLastRound.empty()) {
            legalMoves = orderedMovesLastRound;
        } else {
            board->getLegalMoves(legalMoves);
            reorderMoves(legalMoves, ttMove, killers[2*ply], killers[2*ply + 1], counterMove);
        }

        if (legalMoves.empty()) {
            // it might look like we are checking twice for checkmate, once above and once now.
            // The above checks if there was an illegal move (not handling check) and the king is captured.
            // The below checks the line when we reach down a valid path and there are no legal moves.
            if (inCheck) {
                // this is checkmate
                return -(BoardType::checkmateEval - ply);
            } else {
                // stalemate is still considered "bad" to not incentivize going for it in good or slightly bad positions.
                // its given eval of a minor piece => if the position is worse than a minor piece, than stalemate is considered better so "try" to go for it.
                return -(BoardType::stalemateEval);
            }
        }

        // null move pruning
        if (nullAllowed && board->getGamePhase() > 0 && depth > 2 && abs(beta) < BoardType::mateThreshold) {
            board->processNullMove();
            int nullEval = -negamax(-beta, -beta + 1, depth - 1 - (BASE_NULL_MOVE_REDUCTION + (depth / 7)), ply + 1, false);
            board->undoNullMove();

            if (nullEval >= beta) {
                cutOff++;
                return nullEval;
            }
        }

        vector<pair<int, Move>> resultList;
        uint16_t bestMove = MOVE_NONE;
        int index = 0;
        int ttflag = TTFlagAlpha;
        int maxEval = NEGATIVE_NUM;
        static constexpr int lmpThreshold[] = {0, 8, 14};
        for(const auto& m: legalMoves) {
            bool isQuiet = !(m.isPromotion || m.isCapture);

            // late move pruning: at shallow depth, skip late quiet moves
            if (ply > 0 && isQuiet && !inCheck && depth <= 2 && index >= lmpThreshold[depth]
                && m.move != killers[2*ply] && m.move != killers[2*ply+1] && m.move != counterMove) {
                lmpPrune++;
                index++;
                continue;
            }

            // futility pruning: at depth 1-2, if static eval is so far below alpha that
            // even a significant material swing can't raise it, skip quiet moves
            static constexpr int futilityMargin[] = {0, 150, 300};
            if (ply > 0 && isQuiet && !inCheck && depth <= 2 && index > 0
                && alpha == beta - 1
                && abs(alpha) < BoardType::mateThreshold
                && staticEval + futilityMargin[depth] <= alpha) {
                futilePrune++;
                index++;
                continue;
            }

            board->processMove(m.move);
            int eval;
            if (index == 0) {
                eval = -negamax(-beta, -alpha, depth - 1, ply + 1, true, m.move, m.movePiece);
            } else {
                bool doPvs = true;
                // inCheck refers to pre-move position: don't reduce when responding to check
                if (index >= 3 && isQuiet && depth >= 3 && !inCheck && m.move != killers[2*ply] && m.move != killers[2*ply+1] && m.move != counterMove) {
                    int R = lmrTable[min(depth, 63)][min(index, 63)];
                    if (alpha != beta - 1) R -= 1; // reduce less at PV nodes
                    R = max(R, 1);
                    int newDepth = max(depth - 1 - R, 1);
                    eval = -negamax(-alpha - 1, -alpha, newDepth, ply + 1, true, m.move, m.movePiece);
                    if (eval > alpha) {
                        lmrFailure++;
                    } else {
                        doPvs = false;
                        lmrSuccess++;
                    }
                }

                if (doPvs) {
                    eval = -negamax(-alpha - 1, -alpha, depth - 1, ply + 1, true, m.move, m.movePiece);
                    if (eval > alpha && eval < beta) {
                        pvsFailure++;
                        eval = -negamax(-beta, -alpha, depth - 1, ply + 1, true, m.move, m.movePiece);
                    } else {
                        pvsSuccess++;
                    }
                }
            }
            board->undoMove();

            if (eval > maxEval) {
                maxEval = eval;
                bestMove = m.move;
                updatePv(ply, m.move);
            }

            if (eval > alpha) {
                alpha = eval;
                ttflag = TTFlagExact;
            }

            if (ply == 0) {
                resultList.emplace_back(eval, m);
            }

            if (beta <= alpha) {
                ttflag = TTFlagBeta;
                if (isQuiet) {
                    if (killers[2*ply] != m.move) {
                        killers[2*ply + 1] = killers[2*ply];
                        killers[2*ply] = m.move;
                    }
                    history[(int)m.movePiece][toSq(m.move)] += depth * depth;
                    if (prevMove != MOVE_NONE) {
                        countermoves[(int)prevPiece][toSq(prevMove)] = m.move;
                    }
                }
                break;
            }
            index++;
        }

        if (ply == 0) {
            sort(resultList.begin(), resultList.end(), [](auto left, auto right) {
                return right.first < left.first;
            });

            orderedMovesLastRound.clear();
            for (auto &i: resultList) {
                orderedMovesLastRound.push_back(i.second);
            }
        }

        saveInTT(bestMove, maxEval, depth, ttflag, ply);
        return maxEval;
    }


    int quiescenceSearch(int alpha, int beta, int depth, int ply) {
        qNodes++;
        pvLength[ply] = 0;

        if (shouldStop) return 0;

        if (!board->isKingPresent()) {
            return -(BoardType::checkmateEval - ply);
        }

        if (depth == 0) {
            return board->getBoardEval();
        }

        int boardEval = board->getBoardEval();
        if (boardEval >= beta) {
            return boardEval;
        }
        alpha = max(alpha, boardEval);

        MoveList legalMoves;
        board->getCapturesPromo(legalMoves);
        if (legalMoves.empty()) {
            return boardEval;
        }

        uint16_t noMove = MOVE_NONE;
        reorderMoves(legalMoves, noMove, noMove, noMove);

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
            int eval = -quiescenceSearch(-beta, -alpha, depth - 1, ply + 1);
            board->undoMove();

            if (eval > maxEval) {
                maxEval = eval;
                updatePv(ply, m.move);
            }

            if (eval > alpha) {
                alpha = eval;
            }

            if (beta <= alpha) {
                break;
            }
        }

        return maxEval;
    }

    // Convert root-relative mate score to position-relative for TT storage
    static inline int mateScoreToTT(int eval, int ply) {
        if (eval >= BoardType::mateThreshold) return eval + ply;
        if (eval <= -BoardType::mateThreshold) return eval - ply;
        return eval;
    }

    // Convert position-relative mate score from TT back to root-relative
    static inline int mateScoreFromTT(int eval, int ply) {
        if (eval >= BoardType::mateThreshold) return eval - ply;
        if (eval <= -BoardType::mateThreshold) return eval + ply;
        return eval;
    }

    void saveInTT(uint16_t bestMove, int eval, int depth, int flag, int ply) {
        cacheSave++;
        int ttEval = mateScoreToTT(eval, ply);
        int index = (board->getHash() % TTKeySize) * 2;

        auto entry = &ttable[index];
        auto secondEntry = &ttable[index + 1];

        if (entry -> hash == 0) {
            entry -> update(board->getHash(), bestMove, ttEval, depth, flag);
        } else if (depth >= entry -> depth) {
            secondEntry->update(entry);
            entry -> update(board->getHash(), bestMove, ttEval, depth, flag);
        } else {
            secondEntry -> update(board->getHash(), bestMove, ttEval, depth, flag);
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
        memset(countermoves, 0, sizeof(countermoves));
    }

    void reorderMoves(MoveList &legalMoves, uint16_t& ttMove, uint16_t& killer1, uint16_t& killer2, uint16_t counterMove = MOVE_NONE) {
        const int* pv = board->pieceValue;
        // pre-compute SEE once per capture; cached in isLosingCapture to avoid
        // repeated SEE calls inside the sort comparator
        for (auto& m : legalMoves) {
            if (m.isCapture) {
                m.isLosingCapture = board->see(m) < 0;
            }
        }
        sort(legalMoves.begin(), legalMoves.end(),[&, pv](const auto &left, const auto &right){
            auto score = [&](const Move& m) -> int {
                if (m.move == ttMove) return 60000 * 20000;
                if (m.isPromotion) return 50000 * 20000;
                if (m.isCapture) {
                    // MVV-LVA within tier, SEE as binary gate between tiers
                    int mvvlva = abs(40000 * pv[m.capturePiece] + pv[m.movePiece]);
                    if (!m.isLosingCapture) return 30000 * 20000 + mvvlva;
                    return -(50000 * 20000) + mvvlva;
                }
                if (m.move == killer1) return 10000;
                if (m.move == killer2) return 9000;
                if (m.move == counterMove) return 8000;
                return history[(int)m.movePiece][toSq(m.move)];
            };
            return score(right) < score(left);
        });
    }
};

