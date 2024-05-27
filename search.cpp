#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <format>
#include <sstream>
#include <fstream>
#include "board.cpp"

using namespace std;
using namespace std::chrono;

class Search {
    static const int POSITIVE_NUM = 1 << 30;
    static const int NEGATIVE_NUM = - POSITIVE_NUM;

    Board* board;

    int nodes = 0;
    int qNodes = 0;
    int pvsSuccessW = 0;
    int pvsSuccessB = 0;
    int pvsFailureW= 0;
    int pvsFailureB= 0;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    high_resolution_clock::time_point startTime;
    long hardTimeLimitMs{};
    vector<string> orderedMovesLastRound;
    ofstream ofile;

    struct Node {
        int eval;
        string moves;
        Node(){};

        Node(int eval, string moves) {
            this->eval = eval;
            this->moves = std::move(moves);
        }
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

    public:

    Search() {
        ofile.open("log.txt");
    }

    string getBestMove(Board& currentBoard, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        nodes = 0;
        qNodes = 0;
        Board boardCopy = currentBoard;
        this->board = &boardCopy;

        int myTimeLeft = (boardCopy.turn == Board::WHITE) ? whiteTimeMs : blackTimeMs;

        // assume 30 moves for the game
        long softTimeLimitMs = myTimeLeft / 30;
        if (board->prevMoves.size() < 16) {
            // keep lower time limit in 16 plies of opening
            softTimeLimitMs = myTimeLeft / 90;
        } else if (board->prevMoves.size() < 32) {
            // next 16 plies of late opening / middle game
            softTimeLimitMs = myTimeLeft / 50;
        }

        if (myTimeLeft < 5 * 1000) {
            // you can use 2x soft time limit
            hardTimeLimitMs = 2 * softTimeLimitMs;
        } else {
            // you can use min of (3x as much softLimit, or remaining time leaving 4s)
            hardTimeLimitMs = min(3 * softTimeLimitMs, myTimeLeft - 4000L);
        }


        startTime = high_resolution_clock::now();
        int bestMoveEval;
        string bestMove;
        string bestMoveLine;
        int depthEvaluated = 0;

        for(int depth = START_DEPTH; ; depth++) {
            // if timer > softThreshold => quit loop
            auto currentTime = high_resolution_clock::now();
            auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
            if (elapsedTime >= softTimeLimitMs) {
                break;
            }

            auto result = minimax(NEGATIVE_NUM, POSITIVE_NUM, depth, depth);

            // hard time limit has passed, don't use the above result
            if (shouldQuit() && depth != START_DEPTH) {
                break;
            }

            // gather result
            depthEvaluated = depth;
            bestMoveLine = result.moves;
            bestMoveEval = result.eval;

            stringstream ss(result.moves);
            bestMove = "";
            ss >> bestMove;
        }

        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);

        cout << "info depth " << depthEvaluated << " nodes " << nodes << " time " << duration.count() << " score cp " << bestMoveEval << " pv " << bestMoveLine << endl;
        cout << "info nodes " << qNodes << endl;
        cout << "info pvsW " << pvsSuccessW << " " << pvsFailureW << " pvsB " << pvsSuccessB << " " << pvsFailureB << endl;

        return bestMove;
    }

    Node minimax(int alpha, int beta, int depth, int maxDepth) {
        nodes++;
        if (board->isCheckmate()) {
            return { board->turn == Board::WHITE ? -(Board::checkmateEval + depth): (Board::checkmateEval + depth), ""};
        }

        if (board->isThreeFold()) {
            // give three fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
            return {0, ""};
        }

        if (depth == 0) {
            return quiescenceSearch(alpha, beta, QSEARCH_MAX_DEPTH, maxDepth);
        }

        if (shouldQuit()) {
            // only evaluate if we are just starting
            return {maxDepth == START_DEPTH ? board->getBoardEval(): 0, ""};
        }

        vector<string> legalMoves;
        // use last round's move as starting point in search
        if (maxDepth != START_DEPTH && depth == maxDepth) {
            legalMoves = orderedMovesLastRound;
        } else {
            legalMoves = board->getLegalMoves(false);
        }

        if (legalMoves.empty()) {
            // it might look like we are checking twice for checkmate, once above and once now.
            // The above checks if there was an illegal move (not handling check) and the king is captured.
            // The below checks the line when we reach down a valid path and there are no legal moves.
            if (board->isKingInCheck()) {
                // this is checkmate
                return { board->turn == Board::WHITE ? -(Board::checkmateEval + depth): (Board::checkmateEval + depth), ""};
            } else {
                // this is stalemate.
                // stalemate is still considered "bad" to not incentivize going for it in good or slightly bad positions.
                // its given eval of a minor piece => if the position is worse than a minor piece, than stalemate is considered better so "try" to go for it.
                return { board->turn == Board::WHITE ? -(Board::stalemateEval + depth): (Board::stalemateEval + depth), ""};
            }
        }

        vector<pair<int, string>> resultList;
        string bestMoves;
        int eval;
        int index = 0;

        string prefix;
        for(int i=0;i<maxDepth - depth;i++) {
            prefix += "  ";
        }

        // max
        if (board->turn == Board::WHITE) {
            int maxEval = NEGATIVE_NUM;
            for(const auto& move: legalMoves) {
                // logmsg(format("{}m {} md {} d {}", prefix, move, maxDepth, depth));
                board->processMove(move);
                Node result{};
                if (index == 0) {
                    result = minimax(alpha, beta, depth - 1, maxDepth);
                } else {
                    result = minimax(alpha, alpha + 1, depth-1, maxDepth);
                    if (result.eval > alpha && result.eval < beta) {
                        // pvs failed, do full search
                        // logmsg(format("{}m {} md {} d {} search failed", prefix, move, maxDepth, depth));
                        pvsFailureW++;
                        result = minimax(alpha, beta, depth - 1, maxDepth);
                    } else {
                        pvsSuccessW++;
                    }
                }
                // logmsg(format("{}m {} md {} d {} cp {}", prefix, move, maxDepth, depth, result.eval));
                board->undoMove();
                if (result.eval > maxEval) {
                    maxEval = result.eval;
                    bestMoves = move + " " + result.moves;
                }
                alpha = max(alpha, result.eval);

                if (depth == maxDepth) {
                    resultList.emplace_back(result.eval, move);
                }

                if (beta <= alpha) {
                    break;
                }
                index++;
            }
            eval = maxEval;
        } else {
            int minEval = POSITIVE_NUM;
            for(const auto& move: legalMoves) {
                // logmsg(format("{}m {} md {} d {}", prefix, move, maxDepth, depth));
                board->processMove(move);
                Node result{};
                if (index == 0) {
                    result = minimax(alpha, beta, depth - 1, maxDepth);
                } else {
                    result = minimax(beta-1, beta, depth-1, maxDepth);
                    if (result.eval > alpha && result.eval < beta) {
                        // pvs failed, do full search
                        pvsFailureB++;
                        result = minimax(alpha, beta, depth - 1, maxDepth);
                    } else {
                        pvsSuccessB++;
                    }
                }
                // logmsg(format("{}m {} md {} d {} cp {}", prefix, move, maxDepth, depth, result.eval));
                board->undoMove();
                if (result.eval < minEval) {
                    minEval = result.eval;
                    bestMoves = move + " " + result.moves;
                }
                beta = min(beta, result.eval);

                if (depth == maxDepth) {
                    resultList.emplace_back(result.eval, move);
                }

                if (beta <= alpha) {
                    break;
                }
                index++;
            }
            eval = minEval;
        }

        if (depth == maxDepth) {
            // white aims to get a higher eval, hence it should sort by descending
            // its opposite for black
            bool ascending = board->turn == Board::BLACK;
            sort(resultList.begin(), resultList.end(), [&ascending](auto left, auto right) {
                return ascending ? left.first < right.first : right.first < left.first;
            });

            orderedMovesLastRound.clear();

            // fill orderedMoves for next round
            for (auto &i: resultList) {
                orderedMovesLastRound.push_back(i.second);
            }
        }
        return {eval, bestMoves};
    }


    Node quiescenceSearch(int alpha, int beta, int depth, int minmaxDepth) {
        qNodes++;
        if (board->isCheckmate()) {
            return { board->turn == Board::WHITE ? -(Board::checkmateEval + depth): (Board::checkmateEval + depth), ""};
        }

        if (depth == 0) {
            return {board->getBoardEval(), ""};
        }

        int boardEval = board->getBoardEval();
        if (board->turn == Board::WHITE)
            alpha = max(alpha, boardEval);
        else
            beta = min(beta, boardEval);


        vector<string> legalMoves = board->getLegalMoves(true);
        if (legalMoves.empty()) {
            // not perfect
            return {board->getBoardEval(), ""};
        }

        string prefix;
        for(int i=0;i<minmaxDepth + (QSEARCH_MAX_DEPTH - depth);i++) {
            prefix += "  ";
        }

        // max
        string bestMoves;
        if (board->turn == Board::WHITE) {
            int maxEval = alpha;
            for(const auto& move: legalMoves) {
                // logmsg(format("{}qSearch m {} md {} d {}", prefix, move, minmaxDepth, depth));
                board->processMove(move);
                auto result = quiescenceSearch(alpha, beta, depth - 1, minmaxDepth);
                board->undoMove();
                if (result.eval > maxEval) {
                    maxEval = result.eval;
                    bestMoves = move + " " + result.moves;
                }
                // logmsg(format("{}qSearch m {} md {} d {} cp {}", prefix, move, minmaxDepth, depth, result.eval));
                alpha = max(alpha, result.eval);
                if (beta <= alpha) {
                    break;
                }
            }
            if (depth == QSEARCH_MAX_DEPTH)
                return {maxEval, "q " + bestMoves};
            else
                return {maxEval, bestMoves};
        } else {
            int minEval = beta;
            for(const auto& move: legalMoves) {
                // logmsg(format("{}qSearch m {} md {} d {}", prefix, move, minmaxDepth, depth));
                board->processMove(move);
                auto result = quiescenceSearch(alpha, beta, depth - 1, minmaxDepth);
                board->undoMove();
                if (result.eval < minEval) {
                    minEval = result.eval;
                    bestMoves = move + " " + result.moves;
                }
                // logmsg(format("{}qSearch m {} md {} d {} cp {}", prefix, move, minmaxDepth, depth, result.eval));
                beta = min(beta, result.eval);
                if (beta <= alpha) {
                    break;
                }
            }
            if (depth == QSEARCH_MAX_DEPTH)
                return {minEval, "q " + bestMoves};
            else
                return {minEval, bestMoves};
        }
    }

};