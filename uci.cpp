#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <set>
#include "search.cpp"

using namespace std;

class Uci {

    private:
    Board board;
    Search search;
    int moves = 0;


    vector<string> tokenize(const string &msg) {
        stringstream ss(msg);
        vector<string> tokens;
        string word;
        while (ss >> word) {
            tokens.push_back(word);
        }
        return tokens;
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
            int whiteTime = 1000 * 1000;
            int blackTime = 1000 * 1000;

            if (tokens[1] == "wtime") {
                whiteTime = stoi(tokens[2]);
            }
            if (tokens[3] == "btime") {
                blackTime = stoi(tokens[4]);

            }

            string bestMove = search.getBestMove(board, whiteTime, blackTime);
            board.processMove(bestMove);
            moves++;

            cout << "bestmove " << bestMove << endl;
            cout << "info score cp " << board.eval << endl;
        } else if (tokens[0] == "undo") {
            board.undoMove();
            moves--;
        } else if (tokens[0] == "legal") {
            vector<string> legalMoveList = board.getLegalMoves();
            set<string> legalMoveSet(legalMoveList.begin(), legalMoveList.end());

            if (legalMoveList.size() != legalMoveSet.size()) {
                cout << "Size don't match - Set: " << legalMoveSet.size() << " List: " << legalMoveList.size() << endl;
            }

            for(auto& m : legalMoveSet) {
                cout << m << ", ";
            }
            cout << endl;
        }

        auto showBoard = getenv("showBoard");
        if(showBoard != nullptr and strcmp(getenv("showBoard"), "1") == 0) {
            cout << board.printBoard() << endl;
        }
    }
};