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
    vector<TTEntry> ttable;
    vector<TTEntry> qttable;

    int nodes = 0;
    int qNodes = 0;
    int cutOff = 0;
    int pvsSuccess = 0;
    int pvsFailure = 0;
    int wrongEntry = 0;
    int cacheHit= 0;
    int qcacheHit= 0;
    int cacheFutileHit= 0;
    int qcacheFutileHit= 0;
    int cacheSave= 0;
    int qcacheSave= 0;
    int cacheSaveSuccess= 0;
    int qcacheSaveSuccess= 0;
    int QSEARCH_MAX_DEPTH = 10;
    int START_DEPTH = 1;
    int NULL_MOVE_REDUCTION = 2;
    high_resolution_clock::time_point startTime;
    long hardTimeLimitMs{};
    vector<string> orderedMovesLastRound;
    ofstream ofile;

    struct Node {
        int eval{0};
        string moves;
        Node(){};

        Node(int eval, const string& moves) {
            this->eval = eval;
            this->moves = moves;
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
//        ofile.open("log.txt");
        ttable.reserve(TTSize);
        qttable.reserve(TTSize);
        for(int i=0;i<TTSize;i++) {
            ttable.emplace_back();
            qttable.emplace_back();
        }
    }

    void logMembers() {
        cout << "orderedMovesLastRound: " << orderedMovesLastRound.size() << "  " << orderedMovesLastRound.capacity() << endl;
        cout << "TTable: " << ttable.size() << "  " << ttable.capacity() << endl;
    }

    string getBestMove(Board& currentBoard, int whiteTimeMs, int blackTimeMs, int whiteIncMs, int blackIncMs) {
        this->board = &currentBoard;
        nodes = 0;
        qNodes = 0;
        cutOff = 0;
        pvsSuccess = 0;
        pvsFailure = 0;
        wrongEntry = 0;
        cacheHit = 0;
        cacheFutileHit = 0;
        cacheSave = 0;
        cacheSaveSuccess = 0;
        qcacheHit = 0;
        qcacheFutileHit = 0;
        qcacheSave = 0;
        qcacheSaveSuccess = 0;
        orderedMovesLastRound.clear();

        int myTimeLeft = (board->turn == Board::WHITE) ? whiteTimeMs : blackTimeMs;

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

        for(int depth = START_DEPTH; depth < 30 ; depth++) {
            // if timer > softThreshold => quit loop
            auto currentTime = high_resolution_clock::now();
            auto elapsedTime = duration_cast<milliseconds>(currentTime - startTime).count();
            if (elapsedTime >= softTimeLimitMs) {
                break;
            }

            auto result = negamax(NEGATIVE_NUM, POSITIVE_NUM, depth, depth, false);

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

            // if we find checkmate, there is no need to search deeper
            if (result.eval >= board->checkmateEval) {
                break;
            }
        }

        auto stopTime = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(stopTime - startTime);

        cout << "info depth " << depthEvaluated << " nodes " << nodes << " time " << duration.count() << " score cp " << bestMoveEval << " pv " << bestMoveLine << endl;
        cout << "info qnodes " << qNodes << " nullCutoff " << cutOff << endl;
        cout << "info pvs " << pvsSuccess << " " << pvsFailure << endl;
        cout << "info cache " << "save " << cacheSave << " " << cacheSaveSuccess << " " << (cacheSaveSuccess*100)/cacheSave << " hit " << cacheHit << " " << cacheHit - cacheFutileHit
             << " " << (100*(cacheHit - cacheFutileHit))/cacheHit << endl;
        cout << "info qcache " << "save " << qcacheSave << " " << qcacheSaveSuccess << " " << (qcacheSaveSuccess*100)/qcacheSave << " hit " << qcacheHit << " " << qcacheHit - qcacheFutileHit
             << " " << (100*(qcacheHit - qcacheFutileHit))/qcacheHit << endl;
        cout << "info wrongentry " << wrongEntry << endl;

        return bestMove;
    }

    Node negamax(int alpha, int beta, int depth, int maxDepth, bool nullAllowed) {
        nodes++;

        if (nullAllowed && board->isPositionRepeated()) {
            // give three-fold repetition the eval 0, so we go for it in worse positions and avoid it in good positions.
            return {0, ""};
        }

        // Check transposition table for cached result
        // We don't use TTable when we are doing full search as it causes issues.
        const TTEntry& ttEntry = ttable[board -> getHash() % TTSize];
        string ttMove;
        if (ttEntry.hash == board -> getHash()) {
            if (alpha == beta -1 ) {
                cacheHit++;
                if (ttEntry.depth >= depth) {
                    if (ttEntry.flag == TTFlagExact) {
                        return {ttEntry.eval, ttEntry.move};
                    } else if (ttEntry.flag == TTFlagBeta && ttEntry.eval >= beta) {
                        return {beta, ""};
                    } else if (ttEntry.flag == TTFlagAlpha && ttEntry.eval <= alpha) {
                        return {alpha, ""};
                    }
                }

                ttMove = ttEntry.move;
                cacheFutileHit++;
            }
        } else if (!board -> isKingPresent()){
            return {-(Board::checkmateEval + depth), ""};
        } else if (depth > 3){
            // internal iterative deepening
            depth -= 1;
        }

        if (depth == 0) {
            const Node& result = quiescenceSearch(alpha, beta, QSEARCH_MAX_DEPTH, maxDepth);
            saveInTT(result.moves, result.eval, depth, TTFlagExact);
            return result;
        }

        if (shouldQuit()) {
            // only evaluate if we are just starting
            return {maxDepth == START_DEPTH ? board->getBoardEval(): 0, ""};
        }

        vector<string> legalMoves;
        legalMoves.reserve(50);
        // use last round's move as starting point in search
        if (depth == maxDepth && !orderedMovesLastRound.empty()) {
            legalMoves = orderedMovesLastRound;
        } else {
            board->getLegalMoves(false, legalMoves);
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

//        string prefix;
//        for(int i=0;i<maxDepth - depth;i++) {
//            prefix += "  ";
//        }

        if (!ttMove.empty()) {
            reorderMoves(legalMoves, ttMove);
        }


        vector<pair<int, string>> resultList;
        string bestMoves;
        int index = 0;
        int ttflag = TTFlagAlpha;
        int maxEval = NEGATIVE_NUM;
        for(const auto& move: legalMoves) {
            // logmsg(format("{}m {} md {} d {}", prefix, move, maxDepth, depth));
            board->processMove(move);
            Node result;
            if (index == 0) {
                result = negamax(-beta, -alpha, depth - 1, maxDepth, true);
                result.eval = -result.eval;
            } else {
                result = negamax(-alpha - 1, -alpha, depth - 1, maxDepth, true);
                result.eval = -result.eval;
                if (result.eval > alpha && result.eval < beta) {
                    // pvs failed, do full search
                    // logmsg(format("{}m {} md {} d {} search failed", prefix, move, maxDepth, depth));
                    pvsFailure++;
                    result = negamax(-beta, -alpha, depth - 1, maxDepth, true);
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

            if (result.eval > alpha) {
                alpha = max(alpha, result.eval);
                ttflag = TTFlagExact;
            }

            if (depth == maxDepth) {
                resultList.emplace_back(result.eval, move);
            }

            if (beta <= alpha) {
                ttflag = TTFlagBeta;
                break;
            }
            index++;
        }


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

        saveInTT(bestMoves, maxEval, depth, ttflag);

        return {maxEval, bestMoves};
    }


    Node quiescenceSearch(int alpha, int beta, int depth, int minmaxDepth) {
        qNodes++;

        // Check transposition table for cached result
        const TTEntry& ttEntry = qttable[board -> getHash() % TTSize];
        string ttMove;
        if (ttEntry.hash == board -> getHash() && alpha == beta - 1) {
            qcacheHit++;
            if (ttEntry.depth >= depth) {
                if (ttEntry.flag == TTFlagExact) {
                    return {ttEntry.eval, ttEntry.move};
                } else if (ttEntry.flag == TTFlagBeta && ttEntry.eval >= beta) {
                    return {beta, ""};
                } else if (ttEntry.flag == TTFlagAlpha && ttEntry.eval <= alpha) {
                    return {alpha, ""};
                }
            }

            ttMove = ttEntry.move;
            qcacheFutileHit++;
        }


        if (!board->isKingPresent()) {
            return { -(Board::checkmateEval + depth), ""};
        }

        if (depth == 0) {
            return {board->getBoardEval(), ""};
        }

        int boardEval = board->getBoardEval();
        if (boardEval >= beta) {
            return {boardEval, ""};
        }
        alpha = max(alpha, boardEval);


        vector<string> legalMoves;
        legalMoves.reserve(50);
        board->getLegalMoves(true, legalMoves);
        if (legalMoves.empty()) {
            // not perfect
            return {board->getBoardEval(), ""};
        }

//        string prefix;
//        for(int i=0;i<minmaxDepth + (QSEARCH_MAX_DEPTH - depth);i++) {
//            prefix += "  ";
//        }

        if (!ttMove.empty()) {
            reorderMoves(legalMoves, ttMove);
        }


        string bestMoves;
        int maxEval = alpha;
        int ttflag = TTFlagAlpha;
        for(const auto& move: legalMoves) {
            // logmsg(format("{}qSearch m {} md {} d {}", prefix, move, minmaxDepth, depth));
//            logMsg(format("{}m {} h {}", prefix, move, board->getHash()));
            board->processMove(move);
            auto result = quiescenceSearch(-beta, -alpha, depth - 1, minmaxDepth);
            result.eval = -result.eval;
            board->undoMove();
//            logMsg(format("{}undo m {} h {}", prefix, move, board->getHash()));

            if (result.eval > maxEval) {
                maxEval = result.eval;
                bestMoves = move + " " + result.moves;
            }
            // logmsg(format("{}qSearch m {} md {} d {} cp {}", prefix, move, minmaxDepth, depth, result.eval));

            if (result.eval > alpha) {
                alpha = max(alpha, result.eval);
                ttflag = TTFlagExact;
            }

            if (beta <= alpha) {
                ttflag = TTFlagBeta;
                break;
            }
        }

        saveInQTT(bestMoves, maxEval, depth, ttflag);

        return {maxEval, bestMoves};
    }

    void saveInTT(const string& move, int eval, int depth, int flag) {
        cacheSave++;
        int key = board->getHash() % TTSize;

        auto entry = &ttable[key];

        if (board->getHash() == entry->hash && entry->depth > depth) {
            return;
        }

        cacheSaveSuccess++;
        ttable[key].update(board->getHash(), move, eval, depth, flag);
    }

    void saveInQTT(const string& move, int eval, int depth, int flag) {
        qcacheSave++;
        int key = board->getHash() % TTSize;

        auto entry = &qttable[key];

        if (board->getHash() == entry->hash && entry->depth > depth) {
            return;
        }

        qcacheSaveSuccess++;

        qttable[key].update(board->getHash(), move, eval, depth, flag);
    }

    void reorderMoves(vector<string> &legalMoves, string& ttMoves) {
        return;
//        string ttMove = getFirstMove(ttMoves);
//
//        bool wasPresent = erase(legalMoves, ttMove);
//        if (wasPresent)
//            legalMoves.insert(legalMoves.begin(), ttMove);
//        else {
//            cout << board -> printBoard() << endl;
//            cout << "turn: " << board->turn << endl;
//            cout << "move: " << ttMove << endl;
//            cout << "legalMove: ";
//            for(auto& move: legalMoves) {
//                cout << move << ", ";
//            }
//            cout << endl;
//            cout << "prevMove: " << board->prevMoves[board->prevMoves.size() - 1] << endl;
//            cout << "EpCol: " << board->enPassantCol << endl;
//
//            cout << "-------------------" << endl;
//            wrongEntry++;
//        }

    }

    static string getFirstMove(string& pv) {
        string ans;
        stringstream ss(pv);
        ss >> ans;
        return ans;
    }
};