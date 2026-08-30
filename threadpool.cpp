#pragma once
#include <thread>
#include <memory>
#include <functional>

#include "search.cpp"

// Lazy SMP orchestration: N Search instances (one per thread), each with its own
// BoardType copy, sharing Search's single static TT. Thread 0 (the "main" thread)
// always owns time management and is authoritative for the reported bestmove/eval/PV;
// helper threads run the same root position independently and their results are
// discarded except for their node counts.
class SearchThreadPool {
    vector<unique_ptr<BoardType>> boards;
    vector<unique_ptr<Search>> workers;

    string runOnAllThreads(BoardType& root, const std::function<string(Search&, BoardType&)>& runOne) {
        Search::ensureTTAllocated();  // install the default-size TT if no `setoption Hash` did
        int n = (int)workers.size();
        for (int i = 0; i < n; i++) {
            *boards[i] = root;
        }

        Search::globalStop = false;
        incrementTTAge();

        vector<std::thread> helpers;
        helpers.reserve(n - 1);
        for (int i = 1; i < n; i++) {
            helpers.emplace_back([this, i, &runOne] { runOne(*workers[i], *boards[i]); });
        }

        string bestMove = runOne(*workers[0], *boards[0]);

        // Main thread finished (time limit or max depth reached) — signal any still-running
        // helper threads to unwind rather than waiting for their own independent time checks.
        Search::globalStop = true;
        for (auto& t : helpers) t.join();

        // Only now, with every thread stopped, do we know the true combined node count.
        long totalNodes = 0;
        for (auto& w : workers) totalNodes += w->totalNodes();
        workers[0]->reportResult(totalNodes);

        return bestMove;
    }

public:
    SearchThreadPool() {
        setThreads(1);
    }

    // Only ever called between searches (go is synchronous, so no search is active here).
    void setThreads(int n) {
        int oldSize = (int)workers.size();
        workers.resize(n);
        boards.resize(n);
        for (int i = oldSize; i < n; i++) {
            workers[i] = make_unique<Search>(i);
            boards[i] = make_unique<BoardType>();
        }
    }

    void setMaxDepth(int depth) {
        for (auto& w : workers) w->maxSearchDepth = depth;
    }

    // Wipes the shared TT and gives every worker fresh killers/history/contHist/etc.
    void newGame() {
        Search::clearTT();
        for (int i = 0; i < (int)workers.size(); i++) {
            workers[i] = make_unique<Search>(i);
        }
    }

    int lastEval() const { return workers[0]->lastEval; }

    // Thread 0's worker, for single-threaded debug/introspection commands (e.g. `legal`).
    Search& main() { return *workers[0]; }

    string search(BoardType& root, int maxDepth) {
        return runOnAllThreads(root, [maxDepth](Search& w, BoardType& b) {
            return w.getBestMove(b, maxDepth);
        });
    }

    string search(BoardType& root, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        return runOnAllThreads(root, [=](Search& w, BoardType& b) {
            return w.getBestMove(b, whiteTimeMs, blackTimeMs, whiteIncMs, blackIncMs);
        });
    }
};
