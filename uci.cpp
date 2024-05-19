#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <set>
#include <array>
#include "search.cpp"

using namespace std;

class Uci {

    private:
    Board board;
    Search search;
    int moves = 0;
    static inline string openings[12][2] = {
            // e4 opening
            {"e2e4", "e7e5"},
            {"e2e4", "c7c5"},
            {"e2e4", "b2b3"},
            // d4 opening
            {"d2d4", "d7d5"},
            {"d2d4", "g8f6"},
            {"d2d4", "g7g6"},
            // nf3 opening
            {"g1f3", "d7d5"},
            {"g1f3", "b7b6"},
            {"g1f3", "g8f6"},
            // nc3 opening
            {"b1c3", "d7d5"},
            {"b1c3", "g8f6"},
            {"b1c3", "g7g6"},
    };


    vector<string> tokenize(const string &msg) {
        stringstream ss(msg);
        vector<string> tokens;
        string word;
        while (ss >> word) {
            tokens.push_back(word);
        }
        return tokens;
    }

    inline bool isManual() {
        auto isManual = getenv("manual");
        return isManual != nullptr && strcmp(isManual, "1") == 0;
    }

    public:

    void handle(const string &msg) {
        vector<string> tokens = tokenize(msg);

        if (msg == "quit") {
            exit(1);
        }
        else if (msg == "uci") {
            cout << "id name simple-bot" << endl;
            cout << "id author Dhruvang" << endl;
            cout << "uciok" << endl;
        } else if (msg == "isready") {
            cout << "readyok" << endl;
        } else if (msg == "ucinewgame") {
            board.reset();
            moves = 0;
        } else if(tokens[0] == "position") {
            // compare positions and process the new move
            if(tokens[1] != "startpos") {
                throw std::invalid_argument( "Cannot understand this msg: " + msg);
            }

            if (tokens.size() > 3) {
                for(int i = moves + 3; i < tokens.size() ; i++) {
                    board.processMove(tokens[i]);
                    moves++;
                }
            }

            cout << "info score cp " << board.eval << endl;
        } else if (tokens[0] == "go") {
            int whiteTime = 60 * 1000;
            int blackTime = 60 * 1000;

            int whiteInc = 0;
            int blackInc = 0;

            if (tokens.size() > 2 && tokens[1] == "wtime") {
                whiteTime = stoi(tokens[2]);
            }
            if (tokens.size() > 4 && tokens[3] == "btime") {
                blackTime = stoi(tokens[4]);
            }
            if (tokens.size() > 6 && tokens[5] == "winc") {
                whiteInc = stoi(tokens[6]);
            }
            if (tokens.size() > 8 && tokens[7] == "binc") {
                blackInc = stoi(tokens[8]);
            }

            string bestMove = "";
            if (board.prevMoves.empty()) {
                bestMove = openings[rand() % 12][0];
            } else {
                bestMove = search.getBestMove(board, whiteTime, blackTime, whiteInc, blackInc);
            }

            board.processMove(bestMove);
            moves++;

            cout << "bestmove " << bestMove << endl;
            cout << "info score cp " << board.eval << endl;
        } else if (tokens[0] == "undo") {
            board.undoMove();
            moves--;
        } else if (tokens[0] == "legal") {
            vector<string> legalMoveList = board.getLegalMoves();
            for(auto& m : legalMoveList) {
                cout << m << ", ";
            }
            cout << endl;
        }

        if(isManual()) {
            cout << board.printBoard() << endl;
        }
    }
};