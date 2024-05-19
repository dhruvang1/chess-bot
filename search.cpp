#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <set>
#include <sstream>
#include "board.cpp"

using namespace std;
using namespace std::chrono;

class Search {
    static const int POSITIVE_NUM = 1 << 30;
    static const int NEGATIVE_NUM = - POSITIVE_NUM;

    int nodes = 0;
    int MAX_DEPTH = 6;
    int START_DEPTH = 1;
    high_resolution_clock::time_point startTime;
    long hardTimeLimitMs;

    struct Node {
        int eval;
        string moves;
        Node(int eval, string moves) {
            this->eval = eval;
            this->moves = std::move(moves);
        }
    };

    inline bool shouldQuit() {
        auto currentTime = high_resolution_clock::now();
        auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
        return elapsedTime >= hardTimeLimitMs;
    }

    public:

    string getBestMove(Board& board, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        nodes = 0;
        Board boardCopy = board;

        int myTimeLeft = (boardCopy.turn == Board::WHITE) ? whiteTimeMs : blackTimeMs;
//        int myTimeInc = (boardCopy.turn == Board::WHITE) ? whiteIncMs : blackIncMs;

        // assume 30 moves for the game
        long softTimeLimitMs = myTimeLeft / 30;
        if (board.prevMoves.size() < 10) {
            // keep lower time limit in opening
            softTimeLimitMs = myTimeLeft / 90;
        }


        if (myTimeLeft < 15 * 1000) {
            // you can use min of (2x as much softLimit, or remaining time leaving 5s)
            hardTimeLimitMs = 2 * softTimeLimitMs;
        } else {
            // you can use min of (5x as much softLimit, or remaining time leaving 10s)
            hardTimeLimitMs = min(3 * softTimeLimitMs, myTimeLeft - 10000L);
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

            auto result = minimax(boardCopy, NEGATIVE_NUM, POSITIVE_NUM, depth);

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

        return bestMove;
    }

    Node minimax(Board& board, int alpha, int beta, int depth) {
        nodes++;
        if (board.isCheckmate()) {
            return { board.turn == Board::WHITE ? -(Board::checkmateEval + depth): (Board::checkmateEval + depth), ""};
        }

        // trivial quick check for threefold repetition (not fully correct, but I think should be fine)
        if (board.prevMoves.size() >= 8) {
            int lastElem = board.prevMoves.size() - 1;
            if (board.prevMoves[lastElem] == board.prevMoves[lastElem - 4] &&
                board.prevMoves[lastElem - 2] == board.prevMoves[lastElem - 6] &&
                board.prevMoves[lastElem - 1] == board.prevMoves[lastElem - 5] &&
                board.prevMoves[lastElem - 3] == board.prevMoves[lastElem - 7]
            ){
                // give stalemate eval with same logic, maybe a slightly lower eval is better. Will do later.
                return { board.turn == Board::WHITE ? (Board::stalemateEval + depth): -(Board::stalemateEval + depth), ""};
            }
        }

        if (depth == 0) {
            // We probably don't need to add jitter with 3 fold check above
//            int jitter = 0;
//            // +- 10% eval jitter after 16 ply
//            if (abs(board.eval) > 5 && board.prevMoves.size() > 16) {
//                jitter = rand() % (board.eval / 5);
//                jitter -= jitter / 2;
//            }
//            return {board.eval + jitter, ""};
            return {board.eval, ""};
        }

        if (shouldQuit()) {
            return {board.eval, ""};
        }

        vector<string> legalMoves = board.getLegalMoves();
        if (legalMoves.empty()) {
            // it might look like we are checking twice for checkmate, once above and once now.
            // The above checks if there was an illegal move (not handling check) and the king is captured.
            // The below checks the line when we reach down a valid path and there are no legal moves.
            if (board.isKingInCheck()) {
                // this is checkmate
                return { board.turn == Board::WHITE ? -(Board::checkmateEval + depth): (Board::checkmateEval + depth), ""};
            } else {
                // this is stalemate.
                // stalemate is still considered "bad" to not incentivize going for it in good or slightly bad positions.
                // its given eval > 2x queen => if the position is worse than 2x queen, than stalemate is considered better so "try" to go for it.
                return { board.turn == Board::WHITE ? -(Board::stalemateEval + depth): (Board::stalemateEval + depth), ""};
            }
        }

        // max
        if (board.turn == Board::WHITE) {
            int maxEval = NEGATIVE_NUM;
            string bestMoves;
            for(const auto& move: legalMoves) {
                board.processMove(move);
                auto result = minimax(board, alpha, beta, depth - 1);
                board.undoMove();
                if (result.eval > maxEval) {
                    maxEval = result.eval;
                    bestMoves = move + " " + result.moves;
                }
                alpha = max(alpha, result.eval);
                if (beta <= alpha) {
                    break;
                }
            }
            return {maxEval, bestMoves};
        } else {
            int minEval = POSITIVE_NUM;
            string bestMoves;
            for(const auto& move: legalMoves) {
                board.processMove(move);
                auto result = minimax(board, alpha, beta, depth - 1);
                board.undoMove();
                if (result.eval < minEval) {
                    minEval = result.eval;
                    bestMoves = move + " " + result.moves;
                }
                beta = min(beta, result.eval);
                if (beta <= alpha) {
                    break;
                }
            }
            return {minEval, bestMoves};
        }
    }

};