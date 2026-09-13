#pragma once
#include <string>
#include <chrono>
#include <iostream>
using namespace std;
using namespace std::chrono;

// Search-wide diagnostic/pruning counters, reset once per search via `stats = SearchStats{}`
// in Search::initSearch(). Purely instrumentation -- nothing here gates search behavior.
// Also owns the two "info ..." reporting functions below, since they just format these
// counters plus a handful of Search-level values (threadId, startTime, node counts, ...)
// passed in by the thin Search::logIterationResult/logSearchResult wrappers.
struct SearchStats {
    int nullSuccess = 0;
    int nullAttempt = 0;
    int pvsSuccess = 0;
    int pvsFailure = 0;
    int lmrSuccess = 0;
    int lmrFailure = 0;
    int cacheHit = 0;
    int cacheFutileHit = 0;
    int cacheSave = 0;
    int cacheSaveSuccess = 0;
    int deltaPrune = 0;
    int lmpPrune = 0;
    int histLmpPrune = 0;
    int futilePrune = 0;
    int probcutPrune = 0;
    int seePrune = 0;
    // Diagnostic-only (not gating anything): total qsearch loop iterations vs
    // moves actually recursed into, to directly measure qsearch breadth per call.
    int qMovesConsidered = 0;
    int qMovesSearched = 0;
    int aspirationFails = 0;
    int qCacheHit = 0;

    // Emitted once per completed iterative-deepening iteration, so node growth can be
    // diffed depth-by-depth against Stockfish's own per-depth "info depth" lines (SF
    // emits one per ID iteration too). Uses this thread's own node count rather than
    // the pool-wide total used by logResult below -- that total isn't known until
    // every Lazy SMP thread has joined, but own-thread nodes are available immediately.
    // Exact for the common Threads=1 analysis/SPRT case; a slight undercount otherwise.
    void logIteration(int threadId, high_resolution_clock::time_point startTime, int depth, int selDepth,
                       int nodes, int qNodes, int eval, const string& line) const {
        if (threadId != 0) return;
        auto stopTime = high_resolution_clock::now();
        long ms = duration_cast<milliseconds>(stopTime - startTime).count();
        long ownNodes = nodes + qNodes;
        long nps = ms > 0 ? ownNodes * 1000 / ms : 0;
        cout << "info depth " << depth << " seldepth " << selDepth << " nodes " << ownNodes << " nps " << nps << " time " << ms << " score cp " << eval << " pv " << line << endl;
    }

    // totalNodesForReport is the combined node count across every Lazy SMP thread,
    // summed by the pool after all threads have finished. Everything else in this
    // function's output (qnodes, pruning stats, cache stats) is thread 0's own.
    void logResult(int threadId, high_resolution_clock::time_point startTime, int depthEvaluated, int selDepth,
                   int nodes, int qNodes, int bestMoveEval, const string& bestMoveLine, long totalNodesForReport) const {
        // Only the main thread (thread 0) reports UCI info lines; helper threads stay silent.
        if (threadId != 0) return;
        auto stopTime = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(stopTime - startTime);

        long ms = duration.count();
        // nodes/nps reflect the combined total across every Lazy SMP thread, not just this one.
        long nps = ms > 0 ? totalNodesForReport * 1000 / ms : 0;
        cout << "info depth " << depthEvaluated << " seldepth " << selDepth << " nodes " << totalNodesForReport << " nps " << nps << " time " << ms << " score cp " << bestMoveEval << " pv " << bestMoveLine << endl;
        cout << "info qnodes " << qNodes << " qnodes% " << (nodes + qNodes > 0 ? (100 * qNodes) / (nodes + qNodes) : 0) << endl;
        cout << "info nullAttempt " << nullAttempt << " nullCutoff " << nullSuccess
             << " nullSuccess% " << (nullAttempt > 0 ? (100 * nullSuccess) / nullAttempt : 0) << endl;
        cout << "info pvs " << pvsSuccess << " " << pvsFailure << endl;
        cout << "info lmr " << lmrSuccess << " " << lmrFailure << " lmr% " << (lmrSuccess + lmrFailure > 0 ? (100 * lmrSuccess) / (lmrSuccess + lmrFailure) : 0) << endl;
        cout << "info delta " << deltaPrune << " lmp " << lmpPrune << " histLmp " << histLmpPrune << " futile " << futilePrune << " probcut " << probcutPrune << " seePrune " << seePrune << " aspFail " << aspirationFails << endl;
        cout << "info qcache " << qCacheHit << endl;
        cout << "info qBreadth considered " << qMovesConsidered << " searched " << qMovesSearched
             << " avgSearchedPerCall " << (qNodes > 0 ? (float)qMovesSearched / qNodes : 0.0f) << endl;
        cout << "info cache " << "save " << cacheSave << " " << cacheSaveSuccess << " hit " << cacheHit << " " << cacheHit - cacheFutileHit
             << " " << (cacheHit > 0 ? (100*(cacheHit - cacheFutileHit))/cacheHit : 0) << endl;
    }
};
