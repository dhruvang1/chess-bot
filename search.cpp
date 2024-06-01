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
    int cutOff = 0;
    int pvsSuccess = 0;
    int pvsFailure= 0;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    int NULL_MOVE_REDUCTION = 2;
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
        cutOff = 0;
        Board boardCopy = currentBoard;
        this->board = &boardCopy;

        int myTimeLeft = (boardCopy.turn == Board::WHITE) ? whiteTimeMs : blackTimeMs;

        // assume 30 moves for the game
        long softTimeLimitMs = myTimeLeft / 30;
        if (board->prevMoves.size() < 16) {
            // keep lower time limit in 16 plies of opening
            softTimeLimitMs = myTimeLeft / 100;
        } else if (board->prevMoves.size() < 32) {
            // next 16 plies of late opening / middle game
            softTimeLimitMs = myTimeLeft / 80;
        } else if (board->prevMoves.size() < 64) {
            // next 16 plies of pure middle game
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

            auto result = negamax(NEGATIVE_NUM, POSITIVE_NUM, depth, depth, true);

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
        cout << "info qnodes " << qNodes << " cutoff " << cutOff << endl;
        cout << "info pvsW " << pvsSuccess << " " << pvsFailure << endl;

        return bestMove;
    }

    Node negamax(int alpha, int beta, int depth, int maxDepth, bool nullAllowed) {
        nodes++;
        if (!board->isKingPresent()) {
            return {-(Board::checkmateEval + depth), ""};
        }

        if (board->isThreeFold()) {
            // give three-fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
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
                return {-(Board::checkmateEval + depth), ""};
            } else {
                // this is stalemate.
                // stalemate is still considered "bad" to not incentivize going for it in good or slightly bad positions.
                // its given eval of a minor piece => if the position is worse than a minor piece, than stalemate is considered better so "try" to go for it.
                return {-(Board::stalemateEval), ""};
            }
        }

        // do null move
        if (nullAllowed && board->getGamePhase() > 0 && depth > 4) {
            board->processNullMove();
            Node result = negamax(-beta, -beta + 1, depth - 1 - NULL_MOVE_REDUCTION, maxDepth, false);
            result.eval = -result.eval;

            // undo null move
            board->undoNullMove();

            if (result.eval >= beta) { // cutoff
                cutOff++;
                return result;
            }
        }


        vector<pair<int, string>> resultList;
        string bestMoves;
        int eval;
        int index = 0;

//        string prefix;
//        for(int i=0;i<maxDepth - depth;i++) {
//            prefix += "  ";
//        }

        int maxEval = NEGATIVE_NUM;
        for(const auto& move: legalMoves) {
            // logmsg(format("{}m {} md {} d {}", prefix, move, maxDepth, depth));

            board->processMove(move);
            Node result;
            if (index == 0) {
                result = negamax(-beta, -alpha, depth - 1, maxDepth, nullAllowed);
                result.eval = -result.eval;
            } else {
                result = negamax(-alpha - 1, -alpha, depth - 1, maxDepth, nullAllowed);
                result.eval = -result.eval;
                if (result.eval > alpha && result.eval < beta) {
                    // pvs failed, do full search
                    // logmsg(format("{}m {} md {} d {} search failed", prefix, move, maxDepth, depth));
                    pvsFailure++;
                    result = negamax(-beta, -alpha, depth - 1, maxDepth, nullAllowed);
                    result.eval = -result.eval;
                } else {
                    pvsSuccess++;
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


        if (depth == maxDepth) {
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
        return {eval, bestMoves};
    }


    Node quiescenceSearch(int alpha, int beta, int depth, int minmaxDepth) {
        qNodes++;
        if (!board->isKingPresent()) {
            return { -(Board::checkmateEval + depth), ""};
        }

        if (depth == 0) {
            return {board->getBoardEval(), ""};
        }

        int boardEval = board->getBoardEval();
        alpha = max(alpha, boardEval);


        vector<string> legalMoves = board->getLegalMoves(true);
        if (legalMoves.empty()) {
            // not perfect
            return {board->getBoardEval(), ""};
        }

//        string prefix;
//        for(int i=0;i<minmaxDepth + (QSEARCH_MAX_DEPTH - depth);i++) {
//            prefix += "  ";
//        }

        string bestMoves;
        int maxEval = alpha;
        for(const auto& move: legalMoves) {
            // logmsg(format("{}qSearch m {} md {} d {}", prefix, move, minmaxDepth, depth));
            board->processMove(move);
            auto result = quiescenceSearch(-beta, -alpha, depth - 1, minmaxDepth);
            result.eval = -result.eval;
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
    }

};