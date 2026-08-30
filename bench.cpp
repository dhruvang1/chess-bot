#pragma once
#include <iostream>
#include <chrono>
#include <fstream>

#include "threadpool.cpp"


static const char* benchFens[] = {
    "rnbqkb1r/p1ppp1p1/1p5n/5pNp/P7/6P1/1PPPPP1P/RNBQKB1R w KQkq f6 0 2",
    "rn1qkb1r/p1pppp1p/bp3np1/1B6/8/2N1P3/PPPP1PPP/R1BQK1NR w KQkq - 0 2",
    "r1bqkbnr/ppp1pppp/8/8/1np3P1/1Q5N/PP1PPP1P/RNB1KB1R w KQkq - 0 2",
    "r1bqkbnr/p1ppp1p1/np3p2/7p/P7/1P4PP/2PPPP2/RNBQKBNR w KQkq - 0 2",
    "r1bqk1nr/pppp1p1p/n5p1/4p1B1/3P4/b6P/PPP1PPP1/RN1QKBNR w KQkq - 0 2",
    "r1bq1b1r/ppppkppp/n3p2n/1B6/4P1Q1/5N2/PPPP1PPP/RNB1K2R w KQ - 0 2",
    "rnbqkbnr/2pp2pp/1p2pp2/p7/8/1P2P2N/PBPP1PPP/RN1QKB1R w KQkq - 0 2",
    "rnb1kb1r/pppp1p1p/6pn/3Bp1q1/1P6/6P1/P1PPPP1P/RNBQK1NR w KQkq - 0 2",
    "rn1qkbnr/p1pppp2/b5p1/1p5p/1P6/B1N1P3/P1PP1PPP/R2QKBNR w KQkq - 0 2",
    "r1bqkb1r/pppppppp/8/2n2n2/8/2P4P/PPNPPPP1/R1BQKBNR w KQkq - 0 2",
    "rnb1kb1r/pppp1ppp/4p3/3n2q1/1P2P3/3P4/PBP2PPP/RN1QKBNR w KQkq - 0 2",
    "rnbq1b1r/ppppkpp1/4pn1p/8/4P1P1/3P1Q2/PPP2P1P/RNB1KBNR w KQ - 0 2",
    "r1bqkbnr/pppn2pp/3p1P2/4p3/8/8/PPPPPPBP/RNBQK1NR w KQkq e6 0 2",
    "rnb1kb1r/pp1ppppp/2p4n/8/4P3/2N4N/PPPP1qPP/R1BQKBR1 w Qkq - 0 2",
    "r1bqkbnr/p3pppp/2np4/1pp5/8/N1P4N/PP1PPPPP/R1BQKBR1 w Qkq - 0 2",
    "r1bqkbn1/pppppp1r/n7/6pp/1P2P3/B2B4/P1PP1PPP/RN1QK1NR w KQq - 0 2",
    "rnbqkb1r/p1pppp2/5n2/1p4pp/P1N5/5N2/1PPPPPPP/R1BQKB1R w KQkq g6 0 2",
    "rnbqkbnr/1p2pp1p/p2p4/2p3p1/2P3P1/3PP3/PP3P1P/RNBQKBNR w KQkq - 0 2",
    "r1bqkb1r/2pBpppp/1pn4n/p7/8/4P1P1/PPPP1P1P/RNBQK1NR w KQkq - 0 2",
    "rnbqk1nr/pppp1p2/4p3/2b3pp/8/1PP4N/P1QPPPPP/RNB1KB1R w KQkq - 0 2",
    "rnbqkb1r/1p1pnppp/p1p1p3/8/1P4P1/4P3/P1PP1P1P/RNBQKBNR w KQkq - 0 2",
    "r1bqkbnr/p1p1pp1p/np1p4/1B4p1/8/4P3/PPPP1PPP/RNBQK1NR w KQkq - 0 2",
    "rnbqkbnr/1pppp2p/8/p4p2/6Q1/1P2P3/P1PP1PPP/RNB1KBNR w KQkq f6 0 2",
    "r1b1kb1r/pppqpppp/2np1n2/8/6P1/P1P2N2/1P1PPP1P/RNBQKB1R w KQkq - 0 2",
    "rnb2bnr/pppkpppp/7q/3P4/P7/7P/1PPP1PP1/RNBQKBNR w KQ - 0 2",
    "rnbqkbnr/1p1ppppp/p7/2p5/6P1/PP3N2/2PPPP1P/RNBQKB1R w KQkq - 0 2",
    "rn1qkbnr/2p1pppp/p7/1p1p1b2/1P6/2N4P/PBPPPPP1/R2QKBNR w KQkq b6 0 2",
    "rnbqkbnr/pp2pppp/3p4/2p5/5Q2/2N1P3/PPPP1PPP/R1B1KBNR w KQkq c6 0 2",
    "rnb1kbnr/ppqppp1p/2p5/6p1/4PP2/1P5N/P1PP2PP/RNBQKB1R w KQkq - 0 2",
    "rn1qkbnr/p1pppppp/8/1p6/8/PPN2b2/2PPPPPP/1RBQKBNR w Kkq - 0 2",
    "rnbqk2r/ppppbp1p/4p1pn/P7/7P/4P3/1PPP1PP1/RNBQKBNR w KQkq - 0 2",
    "rnbqkbnr/2p1ppp1/8/ppPp3p/3P4/6P1/PP2PP1P/RNBQKBNR w KQkq b6 0 2",
    "rnbqkbnr/1p1p1ppp/2p5/p7/2PPp3/4P3/PP2NPPP/RNBQKB1R w KQkq a6 0 2",
    "r1b1k1nr/pppp1ppp/n7/2b1p3/7q/1P3PPN/P1PPP2P/RNBQKB1R w KQkq - 0 2",
    "rnbqkbnr/3ppppp/p1p5/1p6/6P1/1QP4N/PP1PPP1P/RNB1KB1R w KQkq - 0 2",
    "rn1qkb1r/pp1bpppp/2pB3n/3p4/8/1P5N/P1PPPPPP/RN1QKB1R w KQkq - 0 2",
    "rnbq1bnr/p1p1pppp/1p1k4/3P4/6Q1/6P1/PPPP1P1P/RNB1KBNR w KQ - 0 2",
    "rn2kbnr/pp2pppp/1q1p4/2p5/8/2NPB2b/PPPQPPPP/R3KBNR w KQkq - 0 2",
    "rnb1k1nr/pppp1ppp/8/1Pb1p3/6Pq/5N2/P1PPPP1P/RNBQKB1R w KQkq - 0 2",
    "rnbqkbnr/2p1p1pp/3p4/pp3p2/1P6/7N/PBPPPPPP/RN1QKB1R w KQkq a6 0 2",
    "rn1qkbnr/p1pppp1p/b7/1p4p1/8/N4PPB/PPPPP2P/R1BQK1NR w KQkq - 0 2",
};

inline int runBench(int depth = 10) {
    if (!nnueLoaded) {
        std::cerr << "bench: no NNUE loaded — run `./chess_magic --nnue <path> bench`" << std::endl;
        return 1;
    }
    Search::resizeTT(16);  // small, freshly-zeroed shared TT — deterministic

    long totalNodes = 0;

    // Silence each search's per-iteration `info` output; restore for the summary.
    std::ofstream nullSink("/dev/null");
    std::streambuf* realCout = std::cout.rdbuf();

    auto start = std::chrono::high_resolution_clock::now();
    for (const char* fen : benchFens) {
        BoardType board;
        board.setupFromFen(fen);  // rebuilds the accumulator when an NNUE is loaded

        SearchThreadPool pool;  // 1 thread, fresh killers/history — determinism requirement
        std::cout.rdbuf(nullSink.rdbuf());
        pool.search(board, depth);
        std::cout.rdbuf(realCout);

        totalNodes += pool.main().totalNodes();
    }
    auto stop = std::chrono::high_resolution_clock::now();

    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count();
    long nps = ms > 0 ? totalNodes * 1000 / ms : totalNodes;

    std::cout << totalNodes << " nodes " << nps << " nps" << std::endl;
    return 0;
}
