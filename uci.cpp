#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <array>
#include <chrono>
#include <unistd.h>
#include "search.cpp"

using namespace std;

class Uci {

    private:
    BoardType board;
    Search search;
    int maxDepth = 64;
    int moves = 0;
    bool fromFen = false;
    bool datagen = false;
    ofstream datagenFile;
    int gameCount = 0;
    string openingFen;
    static inline vector<string> openingsWhite{"e2e4", "d2d4", "c2c4"};
    static inline unordered_map<string, vector<string>> openingsBlack;

    void initNnue(const string& nnuePath) {
        if (nnuePath.empty()) return; // no path → use HCE
        if (!loadNNUE(nnuePath)) {
            cerr << "Error: NNUE file not found or invalid: " << nnuePath << endl;
            exit(1);
        }
    }

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
    Uci(bool datagen = false, const string& nnuePath = "") : datagen(datagen) {
        initNnue(nnuePath);
        srand(time(NULL));
        if (datagen) {
            auto ts = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
            string filename = "datagen_" + to_string(ts) + "_" + to_string(getpid()) + ".txt";
            datagenFile.open(filename, ios::app);
        }

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
            cout << "option name MaxDepth type spin default 64 min 1 max 64" << endl;
            cout << "option name NNUEPath type string default <empty>" << endl;
            cout << "uciok" << endl;
        } else if (msg == "isready") {
            cout << "readyok" << endl;
        } else if (tokens[0] == "setoption") {
            // setoption name <Name> value <Value>
            if (tokens.size() >= 5 && tokens[1] == "name" && tokens[3] == "value") {
                if (tokens[2] == "MaxDepth") {
                    maxDepth = stoi(tokens[4]);
                    search.maxSearchDepth = maxDepth;
                } else if (tokens[2] == "NNUEPath") {
                    // join remaining tokens to support paths with spaces
                    string path;
                    for (int i = 4; i < (int)tokens.size(); i++) {
                        if (i > 4) path += " ";
                        path += tokens[i];
                    }
                    if (!loadNNUE(path)) {
                        cout << "info string NNUE error: file not found: " << path << endl;
                    } else {
                        cout << "info string NNUE enabled: " << path << endl;
                    }
                }
            }
        } else if (msg == "ucinewgame") {
            board = BoardType();
            search = Search();
            search.maxSearchDepth = maxDepth;
            moves = 0;
            fromFen = false;
            openingFen = "";
            if (datagen) {
                gameCount++;
                datagenFile << "GAME " << gameCount << "\n";
                datagenFile.flush();
            }
        } else if(tokens[0] == "position") {
            if (tokens[1] == "fen") {
                fromFen = true;
                // collect FEN fields (up to 6 tokens) then optional "moves"
                int movesIdx = tokens.size();
                for (int i = 2; i < (int)tokens.size(); i++) {
                    if (tokens[i] == "moves") { movesIdx = i; break; }
                }
                string fenStr;
                for (int i = 2; i < movesIdx; i++) {
                    if (i > 2) fenStr += ' ';
                    fenStr += tokens[i];
                }
                board.setupFromFen(fenStr);
                moves = 0;
                if (datagen && openingFen.empty()) {
                    openingFen = fenStr;
                    datagenFile << "OPENING " << fenStr << "\n";
                    datagenFile.flush();
                }
                for (int i = movesIdx + 1; i < (int)tokens.size(); i++) {
                    if (tokens[i] == "null") {
                        board.processNullMove();
                    } else {
                        board.processMove(tokens[i]);
                    }
                    moves++;
                }
            } else if (tokens[1] == "startpos") {
                fromFen = false;
                if (tokens.size() > 3) {
                    // compare positions and process the new move
                    for(int i = moves + 3; i < (int)tokens.size() ; i++) {
                        if (tokens[i] == "null") {
                            board.processNullMove();
                        } else{
                            board.processMove(tokens[i]);
                        }
                        moves++;
                    }
                }
            } else {
                // debug condition to process new moves
                fromFen = true;
                for(int i=1;i<(int)tokens.size(); i++) {
                    if (tokens[i] == "null") {
                        board.processNullMove();
                    } else{
                        board.processMove(tokens[i]);
                    }
                    moves++;
                }
            }

        } else if (tokens[0] == "go") {
            string bestMove;

            if (tokens.size() > 2 && tokens[1] == "depth") {
                int maxDepth = stoi(tokens[2]);
                bestMove = search.getBestMove(board, maxDepth);
            } else {
                int whiteTime = 60 * 1000;
                int blackTime = 60 * 1000;
                int whiteInc = 0;
                int blackInc = 0;

                for (int i = 1; i + 1 < (int)tokens.size(); i++) {
                    if      (tokens[i] == "wtime") whiteTime = stoi(tokens[++i]);
                    else if (tokens[i] == "btime") blackTime = stoi(tokens[++i]);
                    else if (tokens[i] == "winc")  whiteInc  = stoi(tokens[++i]);
                    else if (tokens[i] == "binc")  blackInc  = stoi(tokens[++i]);
                }
                if (!fromFen && !board.hasMoves()) {
                    bestMove = openingsWhite[rand() % openingsWhite.size()];
                } else if (!fromFen && board.moveCount() == 1) {
                    string firstMove = board.getMoveStr(0);
                    if (openingsBlack.find(firstMove) != openingsBlack.end()) {
                        bestMove = openingsBlack[firstMove][rand() % openingsBlack[firstMove].size()];
                    } else {
                        bestMove = search.getBestMove(board, whiteTime, blackTime, whiteInc, blackInc);
                    }
                } else {
                    bestMove = search.getBestMove(board, whiteTime, blackTime, whiteInc, blackInc);
                }
            }

            if (datagen) {
                // eval is from side-to-move perspective, convert to white's perspective
                int eval = search.lastEval;
                if (board.turn == BoardType::BLACK) eval = -eval;
                datagenFile << board.getFen() << " | " << eval << "\n";
                datagenFile.flush();
            }

            board.processMove(bestMove);
            moves++;

            cout << "bestmove " << bestMove << endl;
        } else if (tokens[0] == "undo") {
            if (board.getLastMoveStr() == "null") {
                board.undoNullMove();
            } else {
                board.undoMove();
            }
            moves--;
        } else if (tokens[0] == "eval") {
            if (tokens.size() > 1) {
                BoardType boardCpy = board;
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
            MoveList legalMoveList;
            board.getLegalMoves(legalMoveList);
            for(auto& m : legalMoveList) {
                cout << format("mv:{} mp:{} cp:{} icp:{} ics:{} ipm:{}", moveToUci(m.move), m.movePiece, m.capturePiece, m.isCapture, m.isCastle, m.isPromotion) << endl;
            }

            cout << "----ordered moves----" << endl;
            uint16_t noMove = MOVE_NONE;
            search.setBoard(board);
            search.reorderMoves(legalMoveList, noMove, noMove, noMove);
            for(auto& m : legalMoveList) {
                cout << format("mv:{} mp:{} cp:{} icp:{} ics:{} ipm:{}", moveToUci(m.move), m.movePiece, m.capturePiece, m.isCapture, m.isCastle, m.isPromotion) << endl;
            }

            cout << endl;
        } else if (tokens[0] == "fen") {
            cout << board.getFen() << endl;
        } else if (tokens[0] == "print") {
            cout << board.printBoard() << endl;
        } else if (tokens[0] == "lmr") {
            for (int d = 1; d < 64; d++) {
                for (int m = 1; m < 64; m++) {
                    cout << lmrTable[d][m] << ",";
                }
                cout << "\n";
            }
        }

        if(isManual()) {
            cout << board.printBoard() << endl;
            cout << board.getHash() << endl;
        }
    }
};