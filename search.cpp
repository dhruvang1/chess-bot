#pragma once
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
#include <cmath>
#include <chrono>
#include <atomic>

#include "magicBoard.cpp"

#include "transposition.cpp"
#include "search_diagnostics.cpp"
#include "search_time.cpp"

using namespace std;
using namespace std::chrono;

#include "search_history.cpp"

// Late Move Reduction table: lmrTable[depth][moveIndex] gives the reduction amount.
// Late quiet moves at high depth get reduced more aggressively (up to 3-4 plies).
// Uses logarithmic formula so reductions scale naturally with both depth and move index.
static int lmrTable[64][64];
static bool lmrInitialized = false;

// Must be called once, single-threaded, before any Search threads are spawned.
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

static int roundDownToPow2(long v) {
    long p = 1;
    while (p * 2 <= v) p *= 2;
    return (int)p;
}

// Bookkeeping about the root's current best move, threaded between negamax's root-level
// move loop (ply == 0) and runSearch's iterative-deepening loop. Reset once per search
// in initSearch(); prevMove/stability persist across depths, nodes is refreshed every
// time a root move improves on the current best.
struct RootMoveInfo {
    uint16_t prevMove = MOVE_NONE;  // best move from the last completed iteration
    int nodes = 0;                  // nodes spent searching the current iteration's best move
    int stability = 0;              // consecutive iterations where the best move didn't change
};

class Search {
    static constexpr int POSITIVE_NUM = 1 << 30;
    static constexpr int NEGATIVE_NUM = -POSITIVE_NUM;

    MagicBoard* board;
    static inline vector<TTEntry> ttable;
    // Set by SearchThreadPool before searching; 0 = main thread (authoritative for
    // reported bestmove/PV/info output), >0 = Lazy SMP helper threads.
    int threadId = 0;
    // Move-ordering history tables (history/contHist/contHist2/captHist/killers/countermoves),
    // eval-correction tables (pawnCorrHist/nonPawnCorrHist), and the pieceIdx()/updateHist()
    // helpers that index/update them. See search_history.cpp.
    HistoryTables hist;

    // Per-ply move stack: stores the move and piece made at each ply so any depth of
    // continuation history can be looked up without threading params through the call stack.
    uint16_t moveStack[MAX_PLY] = {};
    char pieceStack[MAX_PLY] = {};

    // Triangular PV table: pvTable[ply][ply..ply+pvLength[ply]-1] stores the PV from that ply.
    // After search, pvTable[0][0..pvLength[0]-1] contains the full principal variation.
    uint16_t pvTable[MAX_PLY][MAX_PLY] = {};
    int pvLength[MAX_PLY] = {};
    // Static eval at each ply for the improving heuristic.
    // Initialised to NEGATIVE_NUM (sentinel = "not set / in check") at the start of each node.
    int evalStack[MAX_PLY] = {};
    // cutoffCount[ply]: how many children of the node at `ply` have cut off so far.
    // Reset for a node's children right before its move loop starts; read mid-loop to
    // decide whether the subtree looks "easy" enough to trust a fuller LMR reduction.
    int cutoffCount[MAX_PLY] = {};

    int nodes = 0;
    int qNodes = 0;
    // Deepest ply reached along the PV, including quiescence — UCI "seldepth".
    // Reset once per iterative-deepening depth (like Stockfish), not per aspiration retry.
    int selDepth = 0;
    SearchStats stats;
    RootMoveInfo rootMove;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    int iterDepth = 0;  // depth of the current iteration; used to cap total extension depth
    high_resolution_clock::time_point startTime;
    bool shouldStop = false;
    SearchTime time;
    long maxNodes = 0;   // hard node budget for this search (0 = unlimited)
    int nodeCheckInterval = 4096; // How often (in nodes) the negamax/qsearch loops poll shouldQuit()
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
        if (shouldStop || globalStop.load(memory_order_relaxed)) {
            shouldStop = true;
            return true;
        }
        if (maxNodes > 0 && (long)nodes + qNodes >= maxNodes) {
            shouldStop = true;
            return true;
        }
        auto currentTime = high_resolution_clock::now();
        auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
        shouldStop = elapsedTime >= time.hardLimitMs;
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

    void initSearch(MagicBoard& currentBoard) {
        this->board = &currentBoard;
        nodes = 0;
        qNodes = 0;
        stats = SearchStats{};
        rootMove = RootMoveInfo{};
        orderedMovesLastRound.clear();
        hist.reset(threadId);
        // TT age is incremented once per `go` by SearchThreadPool.
        startTime = high_resolution_clock::now();
        time.reset();
        maxNodes = 0;
        nodeCheckInterval = 4096;
        shouldStop = false;
    }

    // See SearchStats::logIteration/logResult (search_diagnostics.cpp) for the actual
    // formatting logic; these just forward this thread's context and the counters.
    void logIterationResult(int depth, int eval, const string& line) {
        stats.logIteration(threadId, startTime, depth, lastSelDepth, nodes, qNodes, eval, line);
    }

    void logSearchResult(int depthEvaluated, int bestMoveEval, const string& bestMoveLine, long totalNodesForReport) {
        stats.logResult(threadId, startTime, depthEvaluated, lastSelDepth, nodes, qNodes, bestMoveEval, bestMoveLine, totalNodesForReport);
    }

    // Returns the single legal move at the root, or MOVE_NONE if 0 or 2+ legal moves.
    uint16_t forcedRootMove() {
        MoveList moves;
        board->getLegalMoves(moves, true);
        if (moves.size() != 1) return MOVE_NONE;
        return moves[0].move;
    }

    string runSearch(int maxDepth) {
        int bestMoveEval = 0;
        uint16_t bestMove = MOVE_NONE;
        string bestMoveLine;
        int depthEvaluated = 0;

        board->snapshotRootHistory();

        uint16_t forced = forcedRootMove();
        if (forced != MOVE_NONE) {
            lastDepthEvaluated = 0;
            lastEval = 0;
            lastSelDepth = 0;
            lastPvLine = moveToUci(forced);
            return moveToUci(forced);
        }

        float timeScale = 1.0f;
        int prevIterEval = 0;
        for (int depth = START_DEPTH; depth <= maxDepth; depth++) {
            // don't start a new iteration past the scaled soft limit;
            // always complete depth=1 so we have at least one valid move.
            auto currentTime = high_resolution_clock::now();
            auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
            if (depth > START_DEPTH && time.softLimitMs != LONG_MAX && elapsedTime >= (long)(time.softLimitMs * timeScale)) {
                break;
            }
            // don't start a new iteration once the node budget is spent (depth 1 always runs).
            if (depth > START_DEPTH && maxNodes > 0 && (long)nodes + qNodes >= maxNodes) {
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

            iterDepth = depth;
            selDepth = 0;
            const MoveList savedMoves = orderedMovesLastRound;
            int nodesAtDepthStart = nodes;
            int eval;
            int depthAspFails = 0;
            while (true) {
                eval = negamax(alpha, beta, depth, 0, false);

                if (shouldQuit()) break;

                if (eval <= alpha) {
                    stats.aspirationFails++;
                    if (++depthAspFails >= 3) {
                        alpha = NEGATIVE_NUM;
                        beta = POSITIVE_NUM;
                    } else {
                        alpha = max(alpha - aspiration, NEGATIVE_NUM);
                        aspiration *= 2;
                    }
                    orderedMovesLastRound = savedMoves;
                } else if (eval >= beta) {
                    stats.aspirationFails++;
                    if (++depthAspFails >= 3) {
                        alpha = NEGATIVE_NUM;
                        beta = POSITIVE_NUM;
                    } else {
                        beta = min(beta + aspiration, POSITIVE_NUM);
                        aspiration *= 2;
                    }
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
            lastSelDepth = selDepth;
            rootMove.prevMove = bestMove;
            bestMove = pvLength[0] > 0 ? pvTable[0][0] : MOVE_NONE;

            logIterationResult(depth, eval, bestMoveLine);

            if (eval >= MagicBoard::mateThreshold && depth >= 7) {
                break;
            }

            if (bestMove == rootMove.prevMove) rootMove.stability++;
            else rootMove.stability = 0;

            // Soft-limit multiplier from node fraction, best-move stability, and score trend.
            if (time.softLimitMs != LONG_MAX) {
                int depthNodes = nodes - nodesAtDepthStart;
                float nodeFraction = (float)rootMove.nodes / max(1, depthNodes);
                bool obviousMove = nodeFraction > 0.85f;

                float nodeMult = 1.5f - 1.5f * nodeFraction;

                float stabilityMult = 1.0f;
                float scoreMult = 1.0f;
                if (!obviousMove && depth >= 7) {
                    stabilityMult = clamp(0.85f + 0.5f * powf(0.5f, (float)rootMove.stability), 0.85f, 1.35f);

                    const int drop = prevIterEval - eval;  // positive = score fell
                    if (drop > 10) {
                        scoreMult = clamp(1.0f + (float)drop / 300.0f, 1.0f, 1.4f);
                    }
                }

                // Ramp 0.5 -> 0.3 across depth 12-25 on obvious moves, so deep rapid/classical
                // searches can spend less
                float depthRampFraction = 0.5f;
                if (obviousMove) {
                    float depthT = clamp(((float)depth - 12.0f) / (25.0f - 12.0f), 0.0f, 1.0f);
                    depthRampFraction = 0.5f + depthT * (0.3f - 0.5f);
                }
                float depthRampMs = (float)time.softLimitMs * depthRampFraction;

                // Floor spend relative to increment so the clock doesn't drift up; scaled by
                // the same depth ramp so it also relaxes once obvious+deep.
                float incrementMinMs = (float)time.incMs * 1.25f * (2.0f * depthRampFraction);

                // Cap below the 2x ceiling first -- clamp() requires lo <= hi.
                float minSpendMs = min(max(depthRampMs, incrementMinMs), (float)time.softLimitMs * 2.0f);

                float targetMs = clamp((float)time.softLimitMs * nodeMult * stabilityMult * scoreMult,
                                        minSpendMs, (float)time.softLimitMs * 2.0f);
                timeScale = targetMs / (float)time.softLimitMs;
            }
            prevIterEval = eval;
        }

        lastDepthEvaluated = depthEvaluated;
        lastEval = bestMoveEval;
        lastPvLine = bestMoveLine;
        return moveToUci(bestMove);
    }

    public:
    int lastEval = 0;
    int lastDepthEvaluated = 0;
    int lastSelDepth = 0;
    string lastPvLine;
    int maxSearchDepth = 64;

    // Shared across all Search instances/threads: SearchThreadPool sets this once the
    // main thread's search finishes, so helper threads unwind promptly.
    static inline atomic<bool> globalStop{false};

    // Used by SearchThreadPool to report an accurate total-nodes figure across all threads.
    long totalNodes() const { return (long)nodes + qNodes; }

    // Called by SearchThreadPool once every thread has finished, so the reported
    // nodes/nps figure reflects the true combined count across all threads.
    void reportResult(long totalNodesAcrossThreads) {
        logSearchResult(lastDepthEvaluated, lastEval, lastPvLine, totalNodesAcrossThreads);
    }

    // Resize and clear the shared TT to fit the requested number of megabytes.
    // Safe to call at any time; automatically adjusts TTKeySize/TTSize globals.
    // No-op when the table is already the requested size (so a GUI re-sending the
    // current Hash value doesn't needlessly reallocate and wipe it).
    static void resizeTT(int mb) {
        int64_t totalEntries = ((int64_t)mb * 1024 * 1024) / (int64_t)sizeof(TTEntry);
        int64_t newKeySize = totalEntries / 2;  // two slots per bucket
        if (newKeySize == TTKeySize && !ttable.empty()) return;
        TTKeySize = newKeySize;
        TTSize    = TTKeySize * 2;
        // Assign from a new vector to force deallocation of the old allocation.
        // assign() only shrinks size, not capacity — the old memory would stay reserved.
        ttable = vector<TTEntry>(TTSize, TTEntry{});
    }

    // Allocation is deferred until the first search so that a `setoption name Hash`
    // (which GUIs always send before searching) doesn't first build the default
    // table and then immediately throw it away. Called once per `go`; cheap after
    // the first. If Hash was never set, this installs the default-size table.
    static void ensureTTAllocated() {
        if (ttable.empty()) resizeTT(DEFAULT_HASH_MB);
    }

    // Unconditionally wipes the shared TT. Only call for an explicit new-game reset,
    // never as a side effect of resizing the thread pool.
    // Clears in place rather than reallocating to reuse the existing allocation 
    static void clearTT() {
        std::fill(ttable.begin(), ttable.end(), TTEntry{});
    }

    Search(int threadId = 0) : threadId(threadId) {
        if (isManual()) ofile.open("log-" + to_string(threadId) + ".txt");
        // TT allocation is deferred to the first search (see ensureTTAllocated).
    }

    void setBoard(MagicBoard& b) {
        this->board = &b;
    }

    void logMembers() {
        cout << "orderedMovesLastRound: " << orderedMovesLastRound.size() << endl;
        cout << "TTable: " << ttable.size() << "  " << ttable.capacity() << endl;
    }

    string getBestMove(MagicBoard& currentBoard, int maxDepth) {
        initSearch(currentBoard);
        string bestMove = runSearch(maxDepth);
        board->endSearchAccumulator();
        return bestMove;
    }

    // `go nodes <n>`: iterate to maxSearchDepth but stop once this thread has
    // searched `nodeBudget` nodes (see shouldQuit()).
    string getBestMoveNodeLimited(MagicBoard& currentBoard, long nodeBudget) {
        initSearch(currentBoard);
        maxNodes = max(1L, nodeBudget);
        nodeCheckInterval = roundDownToPow2(max(1L, min(4096L, maxNodes / 10)));
        string bestMove = runSearch(maxSearchDepth);
        board->endSearchAccumulator();
        return bestMove;
    }

    string getBestMove(MagicBoard& currentBoard, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        initSearch(currentBoard);
        time.compute(board, whiteTimeMs, blackTimeMs, whiteIncMs, blackIncMs);

        string bestMove = runSearch(maxSearchDepth);
        board->endSearchAccumulator();

        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);

        // bank unused soft-limit time for harder positions later
        long saved = time.softLimitMs - (long)duration.count();
        time.bankMs += max(0L, saved);

        return bestMove;
    }

    int negamax(int alpha, int beta, int depth, int ply, bool nullAllowed, bool cutNode = false, uint16_t excludedMove = MOVE_NONE) {
        pvLength[ply] = 0;
        evalStack[ply] = NEGATIVE_NUM;  // default sentinel; overwritten below if not in check
        bool pvNode = (alpha + 1 < beta);
        if (pvNode && selDepth < ply + 1) selDepth = ply + 1;

        uint16_t prevMove  = (ply >= 1) ? moveStack[ply - 1] : MOVE_NONE;
        char     prevPiece = (ply >= 1) ? pieceStack[ply - 1] : ' ';
        uint16_t prev2Move  = (ply >= 2) ? moveStack[ply - 2] : MOVE_NONE;
        char     prev2Piece = (ply >= 2) ? pieceStack[ply - 2] : ' ';

        if (shouldStop) return 0;

        if (nullAllowed && board->isPositionRepeated()) {
            // give three-fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
            return 0;
        }

        if (board->isFiftyMoveDraw()) {
            return 0;
        }

        if (!board->isKingPresent()) {
            return -(MagicBoard::checkmateEval - ply);
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
                if (!pvNode) {
                    stats.cacheHit++;
                    if (ttEntry->depth >= depth) {
                        // Don't trust TT mate scores near the leaves: the score may have
                        // been produced via pseudo-legal move gen (stalemate misread as mate).
                        // Falling through forces getLegalMoves(filterIllegal=true) to run,
                        // which correctly identifies stalemate. ttMove is still kept for ordering.
                        bool mateScore = abs(ttEval) >= MagicBoard::mateThreshold;
                        bool nearLeaf  = (ply + depth <= 4);
                        if (!(mateScore && nearLeaf)) {
                            if (ttEntry->boundType() == TTFlagExact) {
                                return ttEval;
                            } else if (ttEntry->boundType() == TTFlagBeta && ttEval >= beta) {
                                return beta;
                            } else if (ttEntry->boundType() == TTFlagAlpha && ttEval <= alpha) {
                                return alpha;
                            }
                        }
                    }
                    ttMove = ttEntry->bestMove;
                    stats.cacheFutileHit++;
                } else {
                    // PV node: only use TT for move ordering, never return early
                    ttMove = ttEntry->bestMove;
                }
            }
        }

        // IIR: no TT move means poor move ordering — reduce depth to avoid wasting a deep search
        if (depth > 3 && ttMove == MOVE_NONE && excludedMove == MOVE_NONE)
            depth -= 1;

        // Enter qsearch when depth is exhausted, or when the ply cap is hit.
        // The ply cap (iterDepth + 8) bounds total extensions across all types
        // (check, singular, double) preventing unbounded cascade at fixed depth.
        if (depth <= 0 || ply >= iterDepth + 8) {
            int eval = quiescenceSearch(alpha, beta, QSEARCH_MAX_DEPTH, ply);
            // eval is only a true Exact score when it falls strictly inside (alpha, beta);
            // otherwise it's a bound, and tagging it Exact lets a window-dependent score
            // get trusted as ground truth by unrelated paths that transpose into the TT.
            int qFlag = (eval >= beta) ? TTFlagBeta : (eval <= alpha) ? TTFlagAlpha : TTFlagExact;
            saveInTT(pvLength[ply] > 0 ? pvTable[ply][ply] : MOVE_NONE, eval, depth, qFlag, ply);
            return eval;
        }

        if ((nodes & (nodeCheckInterval - 1)) == 0 && shouldQuit()) {
            return ply == 0 && depth == START_DEPTH ? board->getBoardEval() : 0;
        }

        bool inCheck = board->isKingInCheck();

        // check extension: being in check is tactically critical, search deeper
        // depth >= 2 avoids cascading explosion at the qsearch boundary
        if (inCheck && depth >= 2 && ply < 40) depth++;

        int rawEval = 0;
        int staticEval = 0;
        if (!inCheck) {
            rawEval = board->getBoardEval();
            int stm = board->turn;
            int pawnKey = board->getPawnHash() % HistoryTables::CORR_HIST_SIZE;
            auto [wHash, bHash] = board->getNonPawnHashes();
            int wKey = wHash % HistoryTables::NONPAWN_CORR_SIZE;
            int bKey = bHash % HistoryTables::NONPAWN_CORR_SIZE;
            int correction = (hist.pawnCorrHist[stm][pawnKey]
                            + hist.nonPawnCorrHist[stm][0][wKey]
                            + hist.nonPawnCorrHist[stm][1][bKey]) / 256;
            staticEval = std::clamp(rawEval + correction, -MagicBoard::mateThreshold + 1, MagicBoard::mateThreshold - 1);
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
        if (depth <= 4 && !pvNode && !inCheck
            && abs(beta) < MagicBoard::mateThreshold) {
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
            && !pvNode
            && depth >= 8
            && ttEntry != nullptr
            && ttEntry->depth >= depth - 3
            && ttEntry->boundType() != TTFlagAlpha
            && abs(ttEval) < MagicBoard::mateThreshold) {
            int sBeta = ttEval - 2 * depth;
            int sScore = negamax(sBeta - 1, sBeta, (depth - 1) / 2, ply, false, true, ttMove);
            if (!shouldStop && sScore < sBeta) {
                singularExtension = (sScore < sBeta - depth * 3) ? 2 : 1;
            } else if (!shouldStop && sScore >= beta && abs(sScore) < MagicBoard::mateThreshold) {
                // Multi-cut: ttMove is assumed to fail high (that's the premise of this
                // whole check), and this reduced search excluding it shows some OTHER
                // move also fails high over the real beta. Two+ moves cut here, so the
                // node isn't singular — prune the whole subtree instead of searching it.
                return sScore;
            }
        }

        // Fresh cutoff-count slot for this node's own children, about to be iterated below.
        if (ply + 1 < MAX_PLY) cutoffCount[ply + 1] = 0;

        uint16_t counterMove = MOVE_NONE;
        if (prevMove != MOVE_NONE) {
            counterMove = hist.countermoves[hist.pieceIdx(prevPiece)][toSq(prevMove)];
        }

        MoveList legalMoves;
        if (ply == 0 && !orderedMovesLastRound.empty()) {
            // Reuse last iteration's move list for root move ordering stability,
            // but re-score so fresh TT move, killers, and history are applied.
            // Also boost the previous iteration's best move to second priority so it
            // stays near the top even if TT changes mid-search.
            legalMoves = orderedMovesLastRound;
            scoreMoves(legalMoves, ttMove, hist.killers[0], hist.killers[1], counterMove);
            for (auto& m : legalMoves) {
                if (m.move == rootMove.prevMove && m.move != ttMove)
                    m.score = SCORE_PREV_BEST;
            }
        } else {
            board->getLegalMoves(legalMoves, false);
            int prevSq  = (prevMove  != MOVE_NONE) ? toSq(prevMove)  : -1;
            int prev2Sq = (prev2Move != MOVE_NONE) ? toSq(prev2Move) : -1;
            scoreMoves(legalMoves, ttMove, hist.killers[2*ply], hist.killers[2*ply + 1], counterMove,
                       prevPiece, prevSq, prev2Piece, prev2Sq);
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

        // Terminal detection: the move list is pseudo-legal, so it may contain only
        // illegal (pinned) moves. Verify at least one legal move exists — done before
        // null-move/pruning so a true stalemate can never be masked. hasLegalMove
        // early-exits on the first legal move, so it is ~1 cheap test in normal positions.
        if (legalMoves.empty() || !board->hasLegalMove(legalMoves)) {
            if (inCheck) {
                return -(MagicBoard::checkmateEval - ply);   // checkmate
            } else {
                return -(MagicBoard::stalemateEval);         // stalemate (draw, == 0)
            }
        }

        // null move pruning (reuses staticEval computed above — no extra getBoardEval call)
        if (nullAllowed && !pvNode && board->getGamePhase() > 0 && depth > 2 && abs(beta) < MagicBoard::mateThreshold && staticEval >= beta) {
            stats.nullAttempt++;
            moveStack[ply] = MOVE_NONE;
            pieceStack[ply] = ' ';
            board->processNullMove();
            prefetchTT(board->getHash());
            int R = min(4 + depth / 3, depth);
            int nullEval = -negamax(-beta, -beta + 1, depth - R, ply + 1, false, !cutNode);
            board->undoNullMove();

            if (nullEval >= beta) {
                stats.nullSuccess++;
                return nullEval;
            }
        }

        // probcut: if a capture beats beta + margin at reduced depth, prune immediately.
        // Only at non-PV nodes, depth >= 5, not in check, away from mate.
        static constexpr int PROBCUT_MARGIN = 200;
        if (!pvNode && depth >= 5 && !inCheck && abs(beta) < MagicBoard::mateThreshold) {
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
                if (!board->isLegalMove(m)) continue;   // skip pseudo-legal-but-illegal (pinned) moves
                if (board->see(m) < PROBCUT_MARGIN) continue;
                moveStack[ply] = m.move;
                pieceStack[ply] = m.movePiece;
                nodes++;
                board->processMove(m.move);
                prefetchTT(board->getHash());
                int pcEval = -negamax(-pcBeta, -pcBeta + 1, depth - 4, ply + 1, false, true);
                board->undoMove();
                if (pcEval >= pcBeta) {
                    stats.probcutPrune++;
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
        static constexpr int lmpThresholdImp[]    = {0, 8, 14, 22};
        static constexpr int lmpThresholdNotImp[] = {0, 4, 7, 11};
        const int* lmpThreshold = improving ? lmpThresholdImp : lmpThresholdNotImp;
        bool skipQuietMoves = false;
        int moveIdx = -1;
        for(int i = 0; i < legalMoves.size(); i++) {
            // Selection sort: swap best remaining move to current position
            for (int j = i + 1; j < legalMoves.size(); j++) {
                if (legalMoves[j].score > legalMoves[i].score)
                    std::swap(legalMoves[i], legalMoves[j]);
            }
            const Move& m = legalMoves[i];
            if (m.move == excludedMove) continue;
            if (!board->isLegalMove(m)) continue;   // skip pseudo-legal-but-illegal (pinned) moves
            moveIdx++;
            bool isQuiet = !(m.isPromotion || m.isCapture);

            if (skipQuietMoves && isQuiet) {
                stats.histLmpPrune++;
                continue;
            }

            // late move pruning: at shallow depth, skip late quiet moves
            if (ply > 0 && !pvNode && isQuiet && !inCheck && depth <= 3 && moveIdx >= lmpThreshold[depth]
                && m.move != hist.killers[2*ply] && m.move != hist.killers[2*ply+1] && m.move != counterMove) {
                stats.lmpPrune++;
                continue;
            }

            // history pruning: once a quiet move's combined history score drops below the threshold,
            // skip all remaining quiets — they're ordered by score so all subsequent will be worse.
            if (ply > 0 && !pvNode && isQuiet && !inCheck && depth <= 4 && moveIdx > 0 && m.score < -100 * depth) {
                stats.histLmpPrune++;
                skipQuietMoves = true;
                continue;
            }

            // futility pruning: if static eval is so far below alpha that even a significant
            // material swing can't raise it, skip quiet moves.
            // Improving → position trending up, might recover → add margin (prune less).
            static constexpr int futilityMargin[] = {0, 150, 300, 450, 600};
            if (ply > 0 && isQuiet && !inCheck && depth <= 4 && moveIdx > 0
                && !pvNode
                && abs(alpha) < MagicBoard::mateThreshold
                && staticEval + futilityMargin[depth] + (improving ? 80 : 0) <= alpha) {
                stats.futilePrune++;
                continue;
            }

            // SEE pruning: skip moves that lose too much material in the exchange.
            // Only call SEE if the destination is actually defended (isSquareThreatened gate)
            // to avoid the expensive getAttackersTo call on uncontested squares.
            if (ply > 0 && !pvNode && !inCheck && depth <= 6 && moveIdx > 0) {
                // Quiet SEE threshold: -50 * d * sqrt(d), precomputed for d=0..6
                static constexpr int quietSeeThreshold[] = {0, -50, -141, -260, -400, -559, -735};
                int seeThreshold = isQuiet ? quietSeeThreshold[depth] : -100 * depth;
                MagicBoard::Color opp = (board->turn == MagicBoard::WHITE) ? MagicBoard::BLACK : MagicBoard::WHITE;
                if (board->isSquareAttackedByColor(toSq(m.move), opp)
                    && board->see(m) < seeThreshold) {
                    stats.seePrune++;
                    continue;
                }
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
            moveStack[ply] = m.move;
            pieceStack[ply] = m.movePiece;
            nodes++;
            board->processMove(m.move);
            prefetchTT(board->getHash());
            int ext = (m.move == ttMove) ? singularExtension : 0;
            int eval;
            if (moveIdx == 0) {
                // first child: PV child if pvNode, else expected all-node (parent is cut → child is all)
                eval = -negamax(-beta, -alpha, depth - 1 + ext, ply + 1, true, pvNode ? false : !cutNode);
            } else {
                bool doPvs = true;
                int nextDepth = depth - 1;
                bool reducible = isQuiet || (m.isLosingCapture && !m.isPromotion);
                // inCheck refers to pre-move position: don't reduce when responding to check
                if (moveIdx >= 2 && reducible && depth >= 3 && !inCheck && m.move != hist.killers[2*ply] && m.move != hist.killers[2*ply+1] && m.move != counterMove) {
                    int R = lmrTable[min(depth, 63)][min(moveIdx, 63)];
                    if (pvNode) R -= 1;

                    // Improving: position is trending up, eval is reliable — search deeper.
                    if (improving) R--;
                    R += cutNode;  // at cut nodes, non-first moves are very unlikely to be best
                    // This subtree hasn't shown itself to be "easy" yet, so don't trust the full reduction.
                    if (cutoffCount[ply] < 4) R--;
                    int histScore = hist.history[hist.pieceIdx(m.movePiece)][toSq(m.move)];
                    // Clamp the history contribution to [-2, +2] so a single piece-square
                    // combination with extreme negative history can't inflate R beyond reason.
                    R -= max(histScore / 300, -2);
                    R = max(R, 1);
                    int newDepth = max(depth - 1 - R, 1);
                    eval = -negamax(-alpha - 1, -alpha, newDepth, ply + 1, true, true);
                    if (eval > alpha) {
                        stats.lmrFailure++;
                        // Re-search deeper if result was surprisingly good (score well above current best).
                        nextDepth = depth - 1 + (eval > maxEval + 35);
                    } else {
                        doPvs = false;
                        stats.lmrSuccess++;
                    }
                }

                if (doPvs) {
                    eval = -negamax(-alpha - 1, -alpha, nextDepth, ply + 1, true, !cutNode);
                    if (eval > alpha && eval < beta) {
                        stats.pvsFailure++;
                        eval = -negamax(-beta, -alpha, depth - 1, ply + 1, true, false);
                    } else {
                        stats.pvsSuccess++;
                    }
                }
            }
            board->undoMove();

            if (eval > maxEval) {
                maxEval = eval;
                bestMove = m.move;
                updatePv(ply, m.move);
                if (ply == 0) {
                    rootMove.nodes = nodes - nodesBefore;
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
                if (ply != 0) cutoffCount[ply - 1]++;
                int bonus = std::min(depth * depth, HistoryTables::MAX_HISTORY);
                if (isQuiet) {
                    if (hist.killers[2*ply] != m.move) {
                        hist.killers[2*ply + 1] = hist.killers[2*ply];
                        hist.killers[2*ply] = m.move;
                    }
                    int ci = hist.pieceIdx(m.movePiece);
                    int csq = toSq(m.move);
                    hist.updateHist(hist.history[ci][csq], bonus);
                    // Penalise every quiet move that was searched before this cutoff move.
                    // They failed to produce a cutoff, so they deserve a lower ordering score.
                    for (int q = 0; q < numTriedQuiets - 1; q++) {
                        hist.updateHist(hist.history[hist.pieceIdx(triedQuietPieces[q])][toSq(triedQuiets[q])], -bonus);
                    }
                    // Update continuation history (1-ply and 2-ply) with the same bonus/malus.
                    if (prevMove != MOVE_NONE) {
                        int pi  = hist.pieceIdx(prevPiece);
                        int psq = toSq(prevMove);
                        hist.updateHist(hist.contHist[pi][psq][ci][csq], bonus);
                        for (int q = 0; q < numTriedQuiets - 1; q++) {
                            hist.updateHist(hist.contHist[pi][psq][hist.pieceIdx(triedQuietPieces[q])][toSq(triedQuiets[q])], -bonus);
                        }
                        hist.countermoves[pi][psq] = m.move;
                    }
                    if (prev2Move != MOVE_NONE) {
                        int p2i  = hist.pieceIdx(prev2Piece);
                        int p2sq = toSq(prev2Move);
                        hist.updateHist(hist.contHist2[p2i][p2sq][ci][csq], bonus);
                        for (int q = 0; q < numTriedQuiets - 1; q++) {
                            hist.updateHist(hist.contHist2[p2i][p2sq][hist.pieceIdx(triedQuietPieces[q])][toSq(triedQuiets[q])], -bonus);
                        }
                    }

                } else if (m.isCapture) {
                    int capType = hist.pieceIdx(m.capturePiece) / 2;
                    hist.updateHist(hist.captHist[hist.pieceIdx(m.movePiece)][toSq(m.move)][capType], bonus);
                    for (int q = 0; q < numTriedCaptures - 1; q++) {
                        hist.updateHist(hist.captHist[hist.pieceIdx(triedCapturePieces[q])][toSq(triedCaptures[q])][hist.pieceIdx(triedCaptureTypes[q]) / 2], -bonus);
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

        if (!inCheck && std::abs(maxEval) < MagicBoard::mateThreshold) {
            int stm = board->turn;
            int refEval = (rawEval + staticEval) / 2;
            int diff = (maxEval - refEval) * 256;
            int weight = std::min(16, depth + 1);
            auto update = [&](int32_t& val) {
                val = ((HistoryTables::CORR_HIST_INERTIA - weight) * val + weight * diff) / HistoryTables::CORR_HIST_INERTIA;
                val = std::clamp(val, -HistoryTables::CORR_HIST_CAP, HistoryTables::CORR_HIST_CAP);
            };
            update(hist.pawnCorrHist[stm][board->getPawnHash() % HistoryTables::CORR_HIST_SIZE]);
            auto [wHash, bHash] = board->getNonPawnHashes();
            update(hist.nonPawnCorrHist[stm][0][wHash % HistoryTables::NONPAWN_CORR_SIZE]);
            update(hist.nonPawnCorrHist[stm][1][bHash % HistoryTables::NONPAWN_CORR_SIZE]);
        }

        saveInTT(bestMove, maxEval, depth, ttflag, ply);
        return maxEval;
    }


    int quiescenceSearch(int alpha, int beta, int depth, int ply) {
        // Hard ply ceiling. Bail to a static eval.
        if (ply >= MAX_PLY - 1)  {
            return board->getBoardEval();
        }
        
        pvLength[ply] = 0;
        if (alpha + 1 < beta && selDepth < ply + 1) selDepth = ply + 1;

        if (shouldStop || ((qNodes & (nodeCheckInterval - 1)) == 0 && shouldQuit())) {
            return 0;
        }
        
        // Unlike negamax, the in-check evasion path below has no depth cutoff — a forced
        // check sequence must otherwise resolve entirely (checkmate or leaving check) before
        // returning. Repetition/50-move are the only real bounds on that, so they must be
        // checked here too, not just in negamax.
        if (board->isPositionRepeated() || board->isFiftyMoveDraw()) {
            return 0;
        }

        if (!board->isKingPresent()) {
            return -(MagicBoard::checkmateEval - ply);
        }

        bool inCheck = board->isKingInCheck();

        if (depth <= 0 && !inCheck) {
            return board->getBoardEval();
        }

        const TTEntry* ttEntry = getTTEntry(board->getHash());
        if (ttEntry != nullptr && alpha == beta - 1) {
            int ttEval = mateScoreFromTT(ttEntry->eval, ply);
            if (ttEntry->boundType() == TTFlagExact) { stats.qCacheHit++; return ttEval; }
            if (ttEntry->boundType() == TTFlagBeta  && ttEval >= beta)  { stats.qCacheHit++; return beta; }
            if (ttEntry->boundType() == TTFlagAlpha && ttEval <= alpha) { stats.qCacheHit++; return alpha; }
        }

        int boardEval;
        if (inCheck) {
            // Can't stand pat under check — worst case is checkmate here
            boardEval = -(MagicBoard::checkmateEval - ply);
        } else {
            boardEval = board->getBoardEval();
            if (boardEval >= beta) return boardEval;
            alpha = max(alpha, boardEval);
        }

        MoveList legalMoves;
        if (inCheck) {
            // Must consider all evasions, not just captures
            board->getLegalMoves(legalMoves, false);
        } else {
            board->getCapturesPromo(legalMoves);
        }

        if (legalMoves.empty()) {
            // In check with no moves = checkmate; otherwise no captures = quiet position
            return inCheck ? -(MagicBoard::checkmateEval - ply) : boardEval;
        }

        uint16_t noMove = MOVE_NONE;
        scoreMoves(legalMoves, noMove, noMove, noMove);

        int maxEval = inCheck ? -(MagicBoard::checkmateEval - ply) : alpha;
        int delta = 300;
        for(int i = 0; i < legalMoves.size(); i++) {
            for (int j = i + 1; j < legalMoves.size(); j++) {
                if (legalMoves[j].score > legalMoves[i].score)
                    std::swap(legalMoves[i], legalMoves[j]);
            }
            const Move& m = legalMoves[i];
            bool isQuiet = !(m.isCapture || m.isPromotion);
            stats.qMovesConsidered++;

            // Once we've found a non-losing evasion, stop searching quiet evasions
            if (inCheck && maxEval > -MagicBoard::mateThreshold && isQuiet) break;

            // delta pruning and SEE pruning don't apply when forced to evade check
            if (!inCheck) {
                if (m.isCapture && (boardEval + abs(board->pieceValue[m.capturePiece]) + delta < alpha)) {
                    stats.deltaPrune++;
                    continue;
                }
                if (m.isCapture && m.isLosingCapture) continue;
            }

            if (!board->isLegalMove(m)) continue;

            stats.qMovesSearched++;
            qNodes++;
            board->processMove(m.move);
            prefetchTT(board->getHash());
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
        if (eval >= MagicBoard::mateThreshold) return eval + ply;
        if (eval <= -MagicBoard::mateThreshold) return eval - ply;
        return eval;
    }

    // Convert position-relative mate score from TT back to root-relative
    static inline int mateScoreFromTT(int eval, int ply) {
        if (eval >= MagicBoard::mateThreshold) return eval - ply;
        if (eval <= -MagicBoard::mateThreshold) return eval + ply;
        return eval;
    }

    // Base index of the 2-slot bucket for a hash. 64-bit multiplicative hashing
    // (umulh) instead of a 64-bit modulo: cheaper, and it mixes the whole hash
    // into the high bits so the low 16 bits stay free to use as the stored key.
    static inline size_t ttBucket(uint64_t hash) {
        uint64_t b = (uint64_t)(((__uint128_t)hash * (__uint128_t)(uint64_t)TTKeySize) >> 64);
        return (size_t)(b * 2);
    }

    void saveInTT(uint16_t bestMove, int eval, int depth, int flag, int ply) {
        stats.cacheSave++;
        int ttEval = mateScoreToTT(eval, ply);
        size_t index = ttBucket(board->getHash());

        auto entry = &ttable[index];
        auto secondEntry = &ttable[index + 1];

        // Age penalty: each generation of staleness reduces the entry's effective depth by 1.
        // Uses & 0x1F (bitmask) since the age field is 5 bits.
        int ageDiff = (ttAge - entry->entryAge()) & 0x1F;
        int effectiveDepth = (int)entry->depth - ageDiff;

        if (depth >= effectiveDepth) {
            // New entry wins: push old primary to secondary (still useful for move ordering),
            // then write new entry to primary.
            if (entry->occupied()) secondEntry->update(entry);
            entry->update(board->getHash(), bestMove, ttEval, depth, flag);
        } else {
            // Old primary is still valuable (deep and not too stale): new entry goes to secondary.
            secondEntry->update(board->getHash(), bestMove, ttEval, depth, flag);
        }
        stats.cacheSaveSuccess++;
    }

    // Prefetch the TT bucket for a hash into cache. Call right after processMove()
    // so the line is resident by the time the child node probes it.
    static inline void prefetchTT(uint64_t hash) {
        __builtin_prefetch(&ttable[ttBucket(hash)]);
    }

    TTEntry* getTTEntry(uint64_t hash) {
        size_t index = ttBucket(hash);
        uint16_t key = (uint16_t)hash;
        if (ttable[index].occupied() && ttable[index].key16 == key) {
            return &ttable[index];
        } else if (ttable[index + 1].occupied() && ttable[index + 1].key16 == key) {
            return &ttable[index + 1];
        }

        return nullptr;
    }

    // Move ordering score bands (highest to lowest), spaced with enough margin that no
    // in-band term can ever cross into a neighboring band:
    //   - Quiet moves score history+contHist+contHist2, each gravity-capped at MAX_HISTORY
    //     (16384), so their sum is bounded by +-3*MAX_HISTORY = +-49152.
    //   - Capture moves score mvvlva +- captHist, where captHist is bounded by +-MAX_HISTORY
    //     (16384) and mvvlva's smallest gap between two distinct victim piece values is
    //     scaled by MVVLVA_MULT to stay well clear of that noise (see margin check below).
    // Bands, highest to lowest:
    //   1. TT move         — best move from a previous search of this position
    //   2. Promotions      — always winning or very likely winning
    //   3. Good captures   — winning/neutral exchanges by SEE, ranked by MVV-LVA
    //   4. Killer 1/2      — quiet moves that caused beta cutoffs at this ply recently
    //   5. Counter move    — quiet move that refuted the opponent's last move historically
    //   6. Quiet moves     — ranked by history + 1-ply/2-ply continuation history
    //   7. Losing captures — losing exchanges by SEE, ranked by MVV-LVA
    static constexpr int SCORE_TT            = 5'000'000;
    static constexpr int SCORE_PROMOTION     = 4'000'000;
    static constexpr int SCORE_GOOD_CAPTURE  =   200'000; // + mvvlva [~244K..2.94M] + captHist
    static constexpr int SCORE_KILLER1       =   120'000; // > max quiet score (49152), < min good capture (447704)
    static constexpr int SCORE_KILLER2       =   110'000;
    static constexpr int SCORE_COUNTERMOVE   =   100'000;
    static constexpr int SCORE_LOSING_CAPTURE = -3'500'000; // + mvvlva + captHist, still < min quiet score
    // Root-only: previous iteration's best move, boosted to sit just below the actual TT
    // move (so a fresh TT hit still wins) but above everything else. Derived from SCORE_TT
    // rather than a standalone constant so it can't drift out of range if SCORE_TT changes.
    static constexpr int SCORE_PREV_BEST = (SCORE_TT + SCORE_PROMOTION) / 2;
    // Must clear the worst-case mover(20000)+2*captHist(32768) swing on the smallest real
    // victim gap (22, bishop vs knight) so MVV-LVA can't be reordered: 3000*22=66000 > 52768.
    static constexpr int MVVLVA_MULT = 3000;
    void scoreMoves(MoveList &legalMoves, uint16_t& ttMove, uint16_t& killer1, uint16_t& killer2,
                      uint16_t counterMove = MOVE_NONE,
                      char prevPc = ' ', int prevSq = -1,
                      char prev2Pc = ' ', int prev2Sq = -1) {
        const int* pv = board->pieceValue;
        bool hasContHist  = (prevPc  != ' ' && prevSq  >= 0);
        bool hasCont2Hist = (prev2Pc != ' ' && prev2Sq >= 0);
        int pi  = hasContHist  ? hist.pieceIdx(prevPc)  : 0;
        int p2i = hasCont2Hist ? hist.pieceIdx(prev2Pc) : 0;
        for (auto& m : legalMoves) {
            if (m.move == ttMove)               { m.score = SCORE_TT; continue; }
            if (m.isPromotion)                  { m.score = SCORE_PROMOTION; continue; }
            if (m.isCapture) {
                m.isLosingCapture = board->see(m) < 0;
                // abs() for color-independent material-magnitude ranking.
                int mvvlva = abs(MVVLVA_MULT * pv[m.capturePiece] + pv[m.movePiece]);
                int ch = hist.captHist[hist.pieceIdx(m.movePiece)][toSq(m.move)][hist.pieceIdx(m.capturePiece) / 2];
                m.score = m.isLosingCapture ? SCORE_LOSING_CAPTURE + mvvlva + ch : SCORE_GOOD_CAPTURE + mvvlva + ch;
            } else if (m.move == killer1)       m.score = SCORE_KILLER1;
            else if (m.move == killer2)         m.score = SCORE_KILLER2;
            else if (m.move == counterMove)     m.score = SCORE_COUNTERMOVE;
            else {
                int sq = toSq(m.move);
                int ci = hist.pieceIdx(m.movePiece);
                // Blend main history with 1-ply continuation history.
                // Both tables use the same update magnitude (depth²) and aging (>>= 1),
                // so they're on the same scale and can be summed directly.
                m.score = hist.history[ci][sq]
                        + (hasContHist  ? hist.contHist [pi ][prevSq ][ci][sq] : 0)
                        + (hasCont2Hist ? hist.contHist2[p2i][prev2Sq][ci][sq] : 0);
            }
        }
        // No sort here — negamax uses selection sort to pick best move lazily
    }
};

