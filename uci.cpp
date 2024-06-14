#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <array>
#include "search.cpp"

using namespace std;

class Uci {

    private:
    Board board;
    Search search;
    int moves = 0;
    static inline vector<string> openingsWhite{"e2e4", "d2d4", "c2c4"};
    static inline unordered_map<string, vector<string>> openingsBlack;

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
    Uci() {
        srand(time(NULL));

        openingsBlack["e2e4"] = vector<string>{"e7e5", "c7c5"};
        openingsBlack["d2d4"] = vector<string>{"d7d5"};
        openingsBlack["c2c4"] = vector<string>{"e7e5"};

        // nf3 opening
        openingsBlack["g1f3"] = vector<string>{"d7d5"};
        // nc3 opening
        openingsBlack["b1c3"] = vector<string>{"d7d5", "e7e5"};
    }

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
            board = Board();
            search = Search();
            moves = 0;
        } else if(tokens[0] == "position") {
            // debug condition to process new moves
            if(tokens[1] != "startpos") {
               for(int i=1;i<tokens.size(); i++) {
                   if (tokens[i] == "null") {
                       board.processNullMove();
                   } else{
                       board.processMove(tokens[i]);
                   }
                   moves++;
               }
            } else if (tokens.size() > 3) {
                // compare positions and process the new move
                for(int i = moves + 3; i < tokens.size() ; i++) {
                    if (tokens[i] == "null") {
                        board.processNullMove();
                    } else{
                        board.processMove(tokens[i]);
                    }
                    moves++;
                }
            }

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

            string bestMove;
            if (board.prevMoves.empty()) {
                bestMove = openingsWhite[rand() % openingsWhite.size()];
            } else if (board.prevMoves.size() == 1) {
                string firstMove = board.prevMoves[0];
                if (openingsBlack.find(firstMove) != openingsBlack.end()) {
                    bestMove = openingsBlack[firstMove][rand() % openingsBlack[firstMove].size()];
                } else {
                    bestMove = search.getBestMove(board, whiteTime, blackTime, whiteInc, blackInc);
                }
            } else {
                bestMove = search.getBestMove(board, whiteTime, blackTime, whiteInc, blackInc);
            }

            board.processMove(bestMove);
            moves++;

            cout << "bestmove " << bestMove << endl;
        } else if (tokens[0] == "undo") {
            if (board.prevMoves[board.prevMoves.size() - 1] == "null") {
                board.undoNullMove();
            } else {
                board.undoMove();
            }
            moves--;
        } else if (tokens[0] == "eval") {
            if (tokens.size() > 1) {
                Board boardCpy = board;
                for(int i = 1; i < tokens.size();i++) {
                    boardCpy.processMove(tokens[i]);
                    cout << tokens[i] << " " << boardCpy.getBoardEval() << endl;
                }
            } else {
                cout << board.getBoardEval() << endl;
            }
        } else if (tokens[0] == "legal") {
            bool capturesOnly = false;
            if (tokens.size() > 1 && tokens[1] == "capture") {
                capturesOnly = true;
            }
            vector<Move> legalMoveList;
            board.getLegalMoves(legalMoveList);
            for(auto& m : legalMoveList) {
                cout << format("mv:{} mp:{} cp:{} icp:{} ics:{} ipm:{}", m.move, m.movePiece, m.capturePiece, m.isCapture, m.isCastle, m.isPromotion) << endl;
            }

            cout << "----ordered moves----" << endl;
            string empty;
            search.reorderMoves(legalMoveList, empty, empty, board.pieceValue);
            for(auto& m : legalMoveList) {
                cout << format("mv:{} mp:{} cp:{} icp:{} ics:{} ipm:{}", m.move, m.movePiece, m.capturePiece, m.isCapture, m.isCastle, m.isPromotion) << endl;
            }

            cout << endl;
        }

        if(isManual()) {
            cout << board.printBoard() << endl;
            cout << board.getHash() << endl;
        }
    }
};