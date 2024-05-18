#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include "board.cpp"

using namespace std;

class Search {
    int nodes = 0;
    long lastSearchDurationMs = 0;
    int MAX_DEPTH = 6;

    struct Node {
        int eval;
        string moves;
        Node(int eval, string moves) {
            this->eval = eval;
            this->moves = moves;
        }
    };

    public:

    string getBestMove(Board& board, int whiteTimeMs, int blackTimeMs) {
        nodes = 0;
        Board boardCopy = board;
        int depthDiff = 0;
        int myTimeLeft = (boardCopy.turn == Board::WHITE) ? whiteTimeMs : blackTimeMs;
        if (lastSearchDurationMs > 5000 || myTimeLeft < 20000) {
            depthDiff = -1;
        } else if (myTimeLeft < 10000) {
            depthDiff = -2;
        }

        auto start = chrono::high_resolution_clock::now();

        auto result = minimax(boardCopy, -(1 << 30), (1 << 30), MAX_DEPTH + depthDiff, MAX_DEPTH + depthDiff);

        auto stop = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stop - start);

        cout << "info nodes " << nodes << " time " << duration.count() << " score cp " << result.eval << " pv " << result.moves << endl;
        lastSearchDurationMs = duration.count();

        stringstream ss(result.moves);
        string bestMove;
        while(ss >> bestMove) {
            break;
        }

        return bestMove;
    }

    Node minimax(Board& board, int alpha, int beta, int depth, int maxDepth) {
        nodes++;
        if (board.isCheckmate()) {
            return { board.turn == Board::WHITE ? -(Board::checkmateEval + depth): (Board::checkmateEval + depth), ""};
        }

        if (depth == 0) {
            return {board.eval, ""};
        }

        vector<string> legalMoves = board.getLegalMoves();


        // max
        if (board.turn == Board::WHITE) {
            int maxEval = -(1 << 30);
            string bestMoves;
            for(const auto& move: legalMoves) {
                board.processMove(move);
                auto result = minimax(board, alpha, beta, depth - 1, maxDepth);
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
            int minEval = (1 << 30);
            string bestMoves;
            for(const auto& move: legalMoves) {
                board.processMove(move);
                auto result = minimax(board, alpha, beta, depth - 1, maxDepth);
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