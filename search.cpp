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
            lmrTable[d][m] = 0.75 + log(d) * log(m) / 1.75;
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

    // Continuation history: contHist[prevPieceIdx][prevToSq][curPieceIdx][curToSq]
    // Tracks which moves are good given what the previous move was — a 1-ply context window
    // on top of main history. Uses compact piece indices 0-11 to keep the table at ~2.3MB.
    int contHist[12][64][12][64] = {};

    // Capture history: captHist[movingPieceIdx][toSq][capturedPieceType]
    // Separates capture ordering from quiet ordering. capturedPieceType = pieceIdx(cap)/2
    // so color is ignored (white queen captured == black queen captured). ~18KB.
    int captHist[12][64][6] = {};

    // Triangular PV table: pvTable[ply][ply..ply+pvLength[ply]-1] stores the PV from that ply.
    // After search, pvTable[0][0..pvLength[0]-1] contains the full principal variation.
    uint16_t pvTable[MAX_PLY][MAX_PLY] = {};
    int pvLength[MAX_PLY] = {};
    // Static eval at each ply for the improving heuristic.
    // Initialised to NEGATIVE_NUM (sentinel = "not set / in check") at the start of each node.
    int evalStack[MAX_PLY] = {};

    int nodes = 0;
    int qNodes = 0;
    int nullSuccess = 0;
    int nullAttempt = 0;
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
    int histLmpPrune = 0;
    int futilePrune = 0;
    int probcutPrune = 0;
    int qCacheHit = 0;
    int bestMoveNodes = 0;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    int BASE_NULL_MOVE_REDUCTION = 3;
    high_resolution_clock::time_point startTime;
    bool shouldStop = false;
    long softTimeLimitMs{};
    long hardTimeLimitMs{};
    int handicapTimeLeftMs = INT_MAX;
    long timeBankMs = 0;  // saved time from previous moves
    int myIncMs = 0;      // increment for current time control, used for timeScale floor
    MoveList orderedMovesLastRound;
    uint16_t prevBestMove = MOVE_NONE;
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
        nullSuccess = 0;
        nullAttempt = 0;
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
        histLmpPrune = 0;
        futilePrune = 0;
        probcutPrune = 0;
        qCacheHit = 0;
        bestMoveNodes = 0;
        orderedMovesLastRound.clear();
        initKillers();
        incrementTTAge();
        startTime = high_resolution_clock::now();
        softTimeLimitMs = LONG_MAX;
        hardTimeLimitMs = LONG_MAX;
        shouldStop = false;
    }

    void computeTimeLimits(int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        const int actualTimeLeft = (board->turn == BoardType::WHITE) ? whiteTimeMs : blackTimeMs;
        int myInc = (board->turn == BoardType::WHITE) ? whiteIncMs : blackIncMs;
        int myTimeLeft = min(actualTimeLeft, handicapTimeLeftMs);

        // phase-aware time allocation: spend less in opening, more in middle game.
        // Divisors interpolate between no-increment (conservative, avoids flagging)
        // and full-increment (aggressive) based on actual increment: t=0 at 0ms, t=1 at 1s+.
        myIncMs = myInc;
        float t = min(1.0f, (float)myInc / 1000.0f);
        int divisorNoInc, divisorFullInc;
        if (board->moveCount() < 16) {
            // first 16 plies of opening: rely on development patterns, save time
            divisorNoInc = 100;  divisorFullInc = 40;
        } else if (board->moveCount() < 32) {
            // late opening / early middle game
            divisorNoInc = 80;   divisorFullInc = 35;
        } else if (board->moveCount() < 64) {
            // pure middle game: spend the most here
            divisorNoInc = 50;   divisorFullInc = 18;
        } else {
            divisorNoInc = 60;   divisorFullInc = 22;
        }
        int divisor = (int)(divisorNoInc * (1.0f - t) + divisorFullInc * t);
        softTimeLimitMs = myTimeLeft / divisor + ((long)myInc * 0.8f);

        // limit the soft time limit to 60% of the time left
        softTimeLimitMs = min(softTimeLimitMs, (long)(myTimeLeft * 0.5f));


        // tiered hard limits: expressed as multiples of soft limit
        // so they scale naturally with any time control
        if (myTimeLeft < 10 * softTimeLimitMs) {
            // ~10 moves left at current pace — panic, no extensions
            hardTimeLimitMs = softTimeLimitMs;
        } else if (myTimeLeft < 20 * softTimeLimitMs) {
            // ~20 moves left — tight, small extension allowed
            hardTimeLimitMs = 2 * softTimeLimitMs;
        } else {
            // plenty of time left — normal extension + proportional reserve
            long reserve = myTimeLeft / 20;  // keep 20% of clock as cushion
            hardTimeLimitMs = min(3 * softTimeLimitMs, myTimeLeft - reserve);
        }

    }

    void logSearchResult(int depthEvaluated, int bestMoveEval, const string& bestMoveLine) {
        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);

        long ms = duration.count();
        long nps = ms > 0 ? (long)nodes * 1000 / ms : 0;
        cout << "info depth " << depthEvaluated << " nodes " << nodes << " nps " << nps << " time " << ms << " score cp " << bestMoveEval << " pv " << bestMoveLine << endl;
        cout << "info qnodes " << qNodes << " qnodes% " << (nodes + qNodes > 0 ? (100 * qNodes) / (nodes + qNodes) : 0) << endl;
        cout << "info nullAttempt " << nullAttempt << " nullCutoff " << nullSuccess
             << " nullSuccess% " << (nullAttempt > 0 ? (100 * nullSuccess) / nullAttempt : 0) << endl;
        cout << "info pvs " << pvsSuccess << " " << pvsFailure << endl;
        cout << "info lmr " << lmrSuccess << " " << lmrFailure << " lmr% " << (lmrSuccess + lmrFailure > 0 ? (100 * lmrSuccess) / (lmrSuccess + lmrFailure) : 0) << endl;
        cout << "info delta " << deltaPrune << " lmp " << lmpPrune << " histLmp " << histLmpPrune << " futile " << futilePrune << " probcut " << probcutPrune << endl;
        cout << "info qcache " << qCacheHit << endl;
        cout << "info cache " << "save " << cacheSave << " " << cacheSaveSuccess << " hit " << cacheHit << " " << cacheHit - cacheFutileHit
             << " " << (cacheHit > 0 ? (100*(cacheHit - cacheFutileHit))/cacheHit : 0) << endl;
    }

    string runSearch(int maxDepth) {
        int bestMoveEval = 0;
        uint16_t bestMove = MOVE_NONE;
        string bestMoveLine;
        int depthEvaluated = 0;

        float timeScale = 1.0f;
        int prevIterEval = 0;
        for (int depth = START_DEPTH; depth <= maxDepth; depth++) {
            // don't start a new iteration past the scaled soft limit
            auto currentTime = high_resolution_clock::now();
            auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
            if (softTimeLimitMs != LONG_MAX && elapsedTime >= (long)(softTimeLimitMs * timeScale)) {
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

            const MoveList savedMoves = orderedMovesLastRound;
            int nodesAtDepthStart = nodes;
            int eval;
            while (true) {
                eval = negamax(alpha, beta, depth, 0, false);

                if (shouldQuit()) break;

                if (eval <= alpha) {
                    alpha = max(alpha - aspiration, NEGATIVE_NUM);
                    aspiration *= 2;
                    orderedMovesLastRound = savedMoves;
                } else if (eval >= beta) {
                    beta = min(beta + aspiration, POSITIVE_NUM);
                    aspiration *= 2;
                    orderedMovesLastRound = savedMoves;
                } else {
                    break;
                }
            }

            // hard time limit has passed, don't use the above result
            if (shouldQuit() && depth != START_DEPTH) {
                break;
            }

            depthEvaluated = depth;
            bestMoveLine = pvToString();
            bestMoveEval = eval;
            prevBestMove = bestMove;
            bestMove = pvLength[0] > 0 ? pvTable[0][0] : MOVE_NONE;

            if (eval >= BoardType::mateThreshold && depth >= 7) {
                break;
            }

            // If score is falling, draw from the time bank to keep searching.
            // Boost relative to soft limit: 100cp drop ≈ 1x soft limit extra; capped at half the bank.
            if (softTimeLimitMs != LONG_MAX && timeBankMs > 0 && depth >= 7) {
                const int drop = prevIterEval - eval;  // positive = score fell
                if (drop > 10) {
                    const long boost = min({softTimeLimitMs * (long)drop / 50, timeBankMs / 2, 2 * softTimeLimitMs});
                    softTimeLimitMs += boost;
                    timeBankMs -= boost;
                }
            }
            prevIterEval = eval;

            // Compute timeScale for next iteration: high nodeFraction = confident = exit earlier,
            // low nodeFraction = uncertain = allow more time. Clamped to [0.5, 1.5].
            // Floor is also increment-aware: ensure we spend at least slightly more than the
            // increment so the clock actually decreases in bullet/blitz games.
            if (softTimeLimitMs != LONG_MAX) {
                int depthNodes = nodes - nodesAtDepthStart;
                float nodeFraction = (float)bestMoveNodes / max(1, depthNodes);
                float incFloor = softTimeLimitMs > 0
                    ? (float)(myIncMs * 5 / 4) / (float)softTimeLimitMs  // spend at least 1.25x increment
                    : 0.0f;
                timeScale = max({0.5f, incFloor, 1.5f - 1.5f * nodeFraction});
            }
        }

        logSearchResult(depthEvaluated, bestMoveEval, bestMoveLine);
        lastEval = bestMoveEval;
        return moveToUci(bestMove);
    }

    public:
    int lastEval = 0;
    int maxSearchDepth = 64;

    // Resize and clear the shared TT to fit the requested number of megabytes.
    // Safe to call at any time; automatically adjusts TTKeySize/TTSize globals.
    static void resizeTT(int mb) {
        int totalEntries = (mb * 1024 * 1024) / (int)sizeof(TTEntry);
        TTKeySize = totalEntries / 2;  // two slots per bucket
        TTSize    = TTKeySize * 2;
        // Assign from a new vector to force deallocation of the old allocation.
        // assign() only shrinks size, not capacity — the old memory would stay reserved.
        ttable = vector<TTEntry>(TTSize, TTEntry{});
    }

    Search() {
        if (isManual()) ofile.open("log.txt");
        initLMR();
        if (ttable.empty()) {
            // First-ever construction: allocate at the current default size.
            ttable.assign(TTSize, TTEntry{});
        } else {
            // Subsequent construction (e.g. ucinewgame): clear entries but keep the
            // existing allocation size (which may have been set by resizeTT).
            // Only reallocate if the size has changed to avoid redundant work.
            if ((int)ttable.size() != TTSize) {
                ttable = vector<TTEntry>(TTSize, TTEntry{});
            } else {
                std::ranges::fill(ttable, TTEntry{});
            }
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

        // bank unused soft-limit time for harder positions later
        long saved = softTimeLimitMs - (long)duration.count();
        timeBankMs += max(0L, saved);

        return bestMove;
    }

    int negamax(int alpha, int beta, int depth, int ply, bool nullAllowed, uint16_t prevMove = MOVE_NONE, char prevPiece = ' ', uint16_t excludedMove = MOVE_NONE) {
        nodes++;
        pvLength[ply] = 0;
        evalStack[ply] = NEGATIVE_NUM;  // default sentinel; overwritten below if not in check

        if (shouldStop) return 0;

        if (nullAllowed && board->isPositionRepeated()) {
            // give three-fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
            return 0;
        }

        if (board->isFiftyMoveDraw()) {
            return 0;
        }

        if (!board->isKingPresent()) {
            return -(BoardType::checkmateEval - ply);
        }

        // Check transposition table for cached result
        const TTEntry* ttEntry = getTTEntry(board->getHash());
        uint16_t ttMove = MOVE_NONE;
        int ttEval = 0;
        if (ttEntry != nullptr) {
            ttEval = mateScoreFromTT(ttEntry->eval, ply);
            // Skip TT cutoffs during singular search: the TT score was computed with the
            // excluded move available, so it would give a wrong result here.
            if (excludedMove == MOVE_NONE) {
                if (alpha == beta - 1) {
                    cacheHit++;
                    if (ttEntry->depth >= depth) {
                        if (ttEntry->boundType() == TTFlagExact) {
                            return ttEval;
                        } else if (ttEntry->boundType() == TTFlagBeta && ttEval >= beta) {
                            return beta;
                        } else if (ttEntry->boundType() == TTFlagAlpha && ttEval <= alpha) {
                            return alpha;
                        }
                    }
                    ttMove = ttEntry->bestMove;
                    cacheFutileHit++;
                } else {
                    // PV node: only use TT for move ordering, never return early
                    ttMove = ttEntry->bestMove;
                }
            }
        } else if (depth > 3 && excludedMove == MOVE_NONE) {
            // internal iterative reduction
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

        // Compute static eval at every non-check node; stored in evalStack for the improving
        // heuristic and reused by RFP, futility, and null move — no redundant calls needed.
        int staticEval = 0;
        if (!inCheck) {
            staticEval = board->getBoardEval();
            evalStack[ply] = staticEval;
        }
        // "Improving": true when the position is trending better vs two half-moves ago.
        // When improving the static eval is on an upward trend and more trustworthy;
        // when not improving the eval may be a temporary spike and less reliable.
        bool improving = ply >= 2 && !inCheck
                      && evalStack[ply - 2] != NEGATIVE_NUM
                      && staticEval > evalStack[ply - 2];

        // Reverse futility pruning: position is so far above beta, skip searching.
        // Improving → smaller margin → prune more (eval is reliable, position trending up).
        // Not improving → larger margin → prune less (eval might be a temporary spike).
        if (depth <= 4 && alpha == beta - 1 && !inCheck
            && abs(beta) < BoardType::mateThreshold) {
            int rfpMargin = improving ? 120 : 175;
            if (staticEval - depth * rfpMargin >= beta) {
                return beta;
            }
        }

        // Singular extension: if the TT move is significantly better than all alternatives,
        // extend it by +1 ply. Only at non-PV nodes with a reliable TT entry.
        // Double extension: if the margin is extreme (very singular), extend by +2.
        int singularExtension = 0;
        if (excludedMove == MOVE_NONE
            && alpha == beta - 1
            && depth >= 8
            && ttEntry != nullptr
            && ttEntry->depth >= depth - 3
            && ttEntry->flag != TTFlagAlpha
            && abs(ttEval) < BoardType::mateThreshold) {
            int sBeta = ttEval - 2 * depth;
            int sScore = negamax(sBeta - 1, sBeta, (depth - 1) / 2, ply, false, prevMove, prevPiece, ttMove);
            if (!shouldStop && sScore < sBeta)
                singularExtension = (sScore < sBeta - depth * 3) ? 2 : 1;
        }

        uint16_t counterMove = MOVE_NONE;
        if (prevMove != MOVE_NONE) {
            counterMove = countermoves[(int)prevPiece][toSq(prevMove)];
        }

        MoveList legalMoves;
        if (ply == 0 && !orderedMovesLastRound.empty()) {
            // Reuse last iteration's move list for root move ordering stability,
            // but re-score so fresh TT move, killers, and history are applied.
            // Also boost the previous iteration's best move to second priority so it
            // stays near the top even if TT changes mid-search.
            legalMoves = orderedMovesLastRound;
            scoreMoves(legalMoves, ttMove, killers[0], killers[1], counterMove);
            for (auto& m : legalMoves) {
                if (m.move == prevBestMove && m.move != ttMove)
                    m.score = 59000 * 20000;
            }
        } else {
            board->getLegalMoves(legalMoves);
            int prevSq = (prevMove != MOVE_NONE) ? toSq(prevMove) : -1;
            scoreMoves(legalMoves, ttMove, killers[2*ply], killers[2*ply + 1], counterMove, prevPiece, prevSq);
        }

        if (ply == 0 && isManual()) {
            // Sort a copy so we can log moves in order without disturbing the lazy selection sort
            MoveList sorted = legalMoves;
            std::sort(sorted.begin(), sorted.end(), [](const Move& a, const Move& b){ return a.score > b.score; });
            ofile << "=== depth " << depth << " root move order ===" << endl;
            for (const auto& m : sorted) {
                ofile << moveToUci(m.move) << " score=" << m.score
                      << (m.isCapture ? " cap" : "") << (m.isLosingCapture ? "(losing)" : "")
                      << endl;
            }
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

        // null move pruning (reuses staticEval computed above — no extra getBoardEval call)
        if (nullAllowed && board->getGamePhase() > 0 && depth > 2 && abs(beta) < BoardType::mateThreshold && staticEval >= beta) {
            nullAttempt++;
            board->processNullMove();
            int nullEval = -negamax(-beta, -beta + 1, depth - 1 - (BASE_NULL_MOVE_REDUCTION + (depth - 1)/5), ply + 1, false);
            board->undoNullMove();

            if (nullEval >= beta) {
                nullSuccess++;
                return nullEval;
            }
        }

        // probcut: if a capture beats beta + margin at reduced depth, prune immediately.
        // Only at non-PV nodes, depth >= 5, not in check, away from mate.
        static constexpr int PROBCUT_MARGIN = 200;
        if (alpha == beta - 1 && depth >= 5 && !inCheck && abs(beta) < BoardType::mateThreshold) {
            int pcBeta = beta + PROBCUT_MARGIN;
            MoveList pcMoves = legalMoves;  // copy since we'll selection-sort destructively
            for (int i = 0; i < pcMoves.size(); i++) {
                for (int j = i + 1; j < pcMoves.size(); j++) {
                    if (pcMoves[j].score > pcMoves[i].score)
                        std::swap(pcMoves[i], pcMoves[j]);
                }
                const Move& m = pcMoves[i];
                if (!m.isCapture && !m.isPromotion) continue;
                if (m.move == excludedMove) continue;
                if (board->see(m) < PROBCUT_MARGIN) continue;
                board->processMove(m.move);
                int pcEval = -negamax(-pcBeta, -pcBeta + 1, depth - 4, ply + 1, false, m.move, m.movePiece);
                board->undoMove();
                if (pcEval >= pcBeta) {
                    probcutPrune++;
                    return pcEval;
                }
            }
        }

        vector<pair<int, Move>> resultList;
        uint16_t bestMove = MOVE_NONE;
        int ttflag = TTFlagAlpha;
        int maxEval = NEGATIVE_NUM;
        // Track quiet moves actually searched (not pruned) for history malus on cutoff.
        // Fixed-size array avoids heap allocation; 64 is more than enough quiet moves per node.
        uint16_t triedQuiets[64];
        char triedQuietPieces[64];
        int numTriedQuiets = 0;
        uint16_t triedCaptures[64];
        char triedCapturePieces[64];
        char triedCaptureTypes[64];
        int numTriedCaptures = 0;

        // LMP threshold: keep high when improving (position dynamic, search more moves),
        // halve when not improving (position stagnant/falling, prune more aggressively).
        static constexpr int lmpThresholdImp[]    = {0, 8, 14};
        static constexpr int lmpThresholdNotImp[] = {0, 4, 7};
        const int* lmpThreshold = improving ? lmpThresholdImp : lmpThresholdNotImp;
        for(int i = 0; i < legalMoves.size(); i++) {
            // Selection sort: swap best remaining move to current position
            for (int j = i + 1; j < legalMoves.size(); j++) {
                if (legalMoves[j].score > legalMoves[i].score)
                    std::swap(legalMoves[i], legalMoves[j]);
            }
            const Move& m = legalMoves[i];
            if (m.move == excludedMove) continue;
            bool isQuiet = !(m.isPromotion || m.isCapture);

            // late move pruning: at shallow depth, skip late quiet moves
            if (ply > 0 && isQuiet && !inCheck && depth <= 2 && i >= lmpThreshold[depth]
                && m.move != killers[2*ply] && m.move != killers[2*ply+1] && m.move != counterMove) {
                lmpPrune++;
                continue;
            }

            // history-gated LMP: at depth 3-4, skip late quiet moves with negative history.
            // LMP thresholds are higher than LMR (i >= 3) since pruning is irreversible.
            static constexpr int lmpHistThreshold[] = {0, 0, 0, 20, 30};
            if (ply > 0 && isQuiet && !inCheck && depth >= 3 && depth <= 4
                && i >= lmpHistThreshold[depth]
                && m.move != killers[2*ply] && m.move != killers[2*ply+1] && m.move != counterMove
                && history[(int)m.movePiece][toSq(m.move)] < 0) {
                histLmpPrune++;
                continue;
            }

            // futility pruning: at depth 1-2, if static eval is so far below alpha that
            // even a significant material swing can't raise it, skip quiet moves
            static constexpr int futilityMargin[] = {0, 150, 300};
            if (ply > 0 && isQuiet && !inCheck && depth <= 2 && i > 0
                && alpha == beta - 1
                && abs(alpha) < BoardType::mateThreshold
                && staticEval + futilityMargin[depth] <= alpha) {
                futilePrune++;
                continue;
            }

            if (isQuiet && numTriedQuiets < 64) {
                triedQuiets[numTriedQuiets] = m.move;
                triedQuietPieces[numTriedQuiets] = m.movePiece;
                numTriedQuiets++;
            }
            if (m.isCapture && numTriedCaptures < 64) {
                triedCaptures[numTriedCaptures] = m.move;
                triedCapturePieces[numTriedCaptures] = m.movePiece;
                triedCaptureTypes[numTriedCaptures] = m.capturePiece;
                numTriedCaptures++;
            }

            int nodesBefore = (ply == 0) ? nodes : 0;
            board->processMove(m.move);
            int ext = (m.move == ttMove) ? singularExtension : 0;
            int eval;
            if (i == 0) {
                eval = -negamax(-beta, -alpha, depth - 1 + ext, ply + 1, true, m.move, m.movePiece);
            } else {
                bool doPvs = true;
                bool reducible = isQuiet || (m.isLosingCapture && !m.isPromotion);
                // inCheck refers to pre-move position: don't reduce when responding to check
                if (i >= 3 && reducible && depth >= 3 && !inCheck && m.move != killers[2*ply] && m.move != killers[2*ply+1] && m.move != counterMove) {
                    int R = lmrTable[min(depth, 63)][min(i, 63)];
                    if (alpha != beta - 1) R -= 1; // reduce less at PV nodes

                    if (m.isLosingCapture) R += 1;
                    // Improving: position is trending up, eval is reliable — search deeper.
                    if (improving) R--;
                    int hist = history[(int)m.movePiece][toSq(m.move)];
                    // Clamp the history contribution to [-2, +2] so a single piece-square
                    // combination with extreme negative history can't inflate R beyond reason.
                    R -= max(hist / 300, -2);
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
                if (ply == 0) {
                    bestMoveNodes = nodes - nodesBefore;
                }
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
                int bonus = depth * depth;
                if (isQuiet) {
                    if (killers[2*ply] != m.move) {
                        killers[2*ply + 1] = killers[2*ply];
                        killers[2*ply] = m.move;
                    }
                    int ci = pieceIdx(m.movePiece);
                    int csq = toSq(m.move);
                    history[(int)m.movePiece][csq] += bonus;
                    // Penalise every quiet move that was searched before this cutoff move.
                    // They failed to produce a cutoff, so they deserve a lower ordering score.
                    for (int q = 0; q < numTriedQuiets - 1; q++) {
                        history[(int)triedQuietPieces[q]][toSq(triedQuiets[q])] -= bonus;
                    }
                    // Update continuation history with the same bonus/malus.
                    if (prevMove != MOVE_NONE) {
                        int pi  = pieceIdx(prevPiece);
                        int psq = toSq(prevMove);
                        contHist[pi][psq][ci][csq] += bonus;
                        for (int q = 0; q < numTriedQuiets - 1; q++) {
                            contHist[pi][psq][pieceIdx(triedQuietPieces[q])][toSq(triedQuiets[q])] -= bonus;
                        }
                        countermoves[(int)prevPiece][psq] = m.move;
                    }
                } else if (m.isCapture) {
                    int capType = pieceIdx(m.capturePiece) / 2;
                    captHist[pieceIdx(m.movePiece)][toSq(m.move)][capType] += bonus;
                    for (int q = 0; q < numTriedCaptures - 1; q++) {
                        captHist[pieceIdx(triedCapturePieces[q])][toSq(triedCaptures[q])][pieceIdx(triedCaptureTypes[q]) / 2] -= bonus;
                    }
                }
                break;
            }
        }

        if (ply == 0) {
            stable_sort(resultList.begin(), resultList.end(), [](auto left, auto right) {
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

        const TTEntry* ttEntry = getTTEntry(board->getHash());
        if (ttEntry != nullptr && alpha == beta - 1) {
            int ttEval = mateScoreFromTT(ttEntry->eval, ply);
            if (ttEntry->boundType() == TTFlagExact) { qCacheHit++; return ttEval; }
            if (ttEntry->boundType() == TTFlagBeta  && ttEval >= beta)  { qCacheHit++; return beta; }
            if (ttEntry->boundType() == TTFlagAlpha && ttEval <= alpha) { qCacheHit++; return alpha; }
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
        scoreMoves(legalMoves, noMove, noMove, noMove);

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

        // Age penalty: each generation of staleness reduces the entry's effective depth by 1.
        // Uses & 63 (bitmask) instead of % 64 since 64 is a power of two.
        int ageDiff = (ttAge - entry->entryAge()) & 63;
        int effectiveDepth = (int)entry->depth - ageDiff;

        if (depth >= effectiveDepth) {
            // New entry wins: push old primary to secondary (still useful for move ordering),
            // then write new entry to primary.
            if (entry->hash != 0) secondEntry->update(entry);
            entry->update(board->getHash(), bestMove, ttEval, depth, flag);
        } else {
            // Old primary is still valuable (deep and not too stale): new entry goes to secondary.
            secondEntry->update(board->getHash(), bestMove, ttEval, depth, flag);
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

    // Maps piece char to compact 0-11 index for contHist.
    // P/N/B/R/Q/K = 0-5 (white), p/n/b/r/q/k = 6-11 (black).
    static inline int pieceIdx(char p) {
        switch (p) {
            case 'P': return 0; case 'N': return 1; case 'B': return 2;
            case 'R': return 3; case 'Q': return 4; case 'K': return 5;
            case 'p': return 6; case 'n': return 7; case 'b': return 8;
            case 'r': return 9; case 'q': return 10; case 'k': return 11;
            default:  return 0;
        }
    }

    void initKillers() {
        memset(killers, 0, sizeof(killers));
        memset(countermoves, 0, sizeof(countermoves));
        // Age both history tables toward zero instead of zeroing them.
        // Preserves relative ordering while letting fresh updates dominate.
        for (auto& row : history)
            for (auto& val : row)
                val >>= 1;
        for (auto& a : contHist)
            for (auto& b : a)
                for (auto& c : b)
                    for (auto& val : c)
                        val >>= 1;
        for (auto& a : captHist)
            for (auto& b : a)
                for (auto& val : b)
                    val >>= 1;
    }

    // Assigns a score to each move for ordering purposes. Actual ordering is done lazily
    // via selection sort in negamax. Priority (highest to lowest):
    //   1. TT move         — best move from a previous search of this position
    //   2. Promotions      — always winning or very likely winning
    //   3. Good captures   — winning/neutral exchanges by SEE, ranked by MVV-LVA
    //   4. Killer 1/2      — quiet moves that caused beta cutoffs at this ply recently
    //   5. Counter move    — quiet move that refuted the opponent's last move historically
    //   6. Quiet moves     — ranked by history + 1-ply continuation history
    //   7. Losing captures — losing exchanges by SEE, ranked by MVV-LVA
    void scoreMoves(MoveList &legalMoves, uint16_t& ttMove, uint16_t& killer1, uint16_t& killer2,
                      uint16_t counterMove = MOVE_NONE, char prevPc = ' ', int prevSq = -1) {
        const int* pv = board->pieceValue;
        // Precompute whether we have a valid previous-move context for contHist lookup.
        bool hasContHist = (prevPc != ' ' && prevSq >= 0);
        int pi = hasContHist ? pieceIdx(prevPc) : 0;
        for (auto& m : legalMoves) {
            if (m.move == ttMove)               { m.score = 60000 * 20000; continue; }
            if (m.isPromotion)                  { m.score = 50000 * 20000; continue; }
            if (m.isCapture) {
                m.isLosingCapture = board->see(m) < 0;
                int mvvlva = abs(40000 * pv[m.capturePiece] + pv[m.movePiece]);
                int ch = captHist[pieceIdx(m.movePiece)][toSq(m.move)][pieceIdx(m.capturePiece) / 2];
                m.score = m.isLosingCapture ? -(50000 * 20000) + mvvlva + ch : 30000 * 20000 + mvvlva + ch;
            } else if (m.move == killer1)       m.score = 10000;
            else if (m.move == killer2)         m.score = 9000;
            else if (m.move == counterMove)     m.score = 8000;
            else {
                int sq = toSq(m.move);
                int ci = pieceIdx(m.movePiece);
                // Blend main history with 1-ply continuation history.
                // Both tables use the same update magnitude (depth²) and aging (>>= 1),
                // so they're on the same scale and can be summed directly.
                m.score = history[(int)m.movePiece][sq]
                        + (hasContHist ? contHist[pi][prevSq][ci][sq] : 0);
            }
        }
        // No sort here — negamax uses selection sort to pick best move lazily
    }
};






