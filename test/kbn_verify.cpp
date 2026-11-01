// KBN vs K conversion test: plays the real engine (over UCI) as the attacker against a
// defender that uses the exact solver in kbn_oracle.hpp, and reports whether/how fast it mates.
//
//   kbn_verify --engine ./build/chess_magic --nnue nnue/quantised.bin --set random --count 300 \
//              --go "nodes 20000" --defender optimal
//
// Sets:      random | hard | corner | file:<epd> | fen:<fen>      (+ --min-dtm / --max-dtm filters)
// Defenders: optimal (maximises mate distance) | sitter (heads for the wrong-colour corner and
//            shuffles there; DTM only breaks ties) | random
// Attacker:  engine (default) | oracle (self-test of the harness: must mate in exactly DTM plies)
// Clock:     --go "nodes N" / "depth D" (fixed budget)   or   --tc base+inc (seconds; real clock)
// A game counts as converted only if mate is delivered inside the 50-move rule, counted from the
// start position (halfmove clock 0), without stalemate, lost piece, or 3-fold repetition. Start positions
// that are already theoretical draws (a piece hangs) are skipped. Exit status is non-zero if the conversion
// rate is below --min-rate (default 100).
#include "kbn_oracle.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace kbn;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- engine process
class Engine {
    pid_t pid = -1;
    int toFd = -1, fromFd = -1;
    std::string buf;

public:
    Engine(const std::string& path, const std::string& nnue, int hashMb, int threads) {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) { perror("pipe"); exit(1); }
        pid = fork();
        if (pid == 0) {
            dup2(in[0], 0); dup2(out[1], 1);
            int devnull = open("/dev/null", O_WRONLY);
            dup2(devnull, 2);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            execl(path.c_str(), path.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(in[0]); close(out[1]);
        toFd = in[1]; fromFd = out[0];
        signal(SIGPIPE, SIG_IGN);
        send("uci");
        if (!waitFor("uciok", 10000)) { fprintf(stderr, "engine did not answer uci\n"); exit(1); }
        send("setoption name Hash value " + std::to_string(hashMb));
        send("setoption name Threads value " + std::to_string(threads));
        if (!nnue.empty()) send("setoption name NNUEPath value " + nnue);
        send("isready");
        if (!waitFor("readyok", 20000)) { fprintf(stderr, "engine did not answer isready\n"); exit(1); }
    }
    ~Engine() {
        if (pid > 0) { send("quit"); close(toFd); close(fromFd); kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
    }
    void send(const std::string& s) {
        std::string l = s + "\n";
        if (write(toFd, l.data(), l.size()) < 0) {}
    }
    bool readLine(std::string& line, int timeoutMs) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) { line = buf.substr(0, nl); buf.erase(0, nl + 1); return true; }
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return false;
            pollfd pfd{fromFd, POLLIN, 0};
            if (poll(&pfd, 1, left) <= 0) return false;
            char tmp[4096];
            ssize_t n = read(fromFd, tmp, sizeof tmp);
            if (n <= 0) return false;
            buf.append(tmp, n);
        }
    }
    bool waitFor(const std::string& prefix, int timeoutMs) {
        std::string line;
        while (readLine(line, timeoutMs)) if (line.rfind(prefix, 0) == 0) return true;
        return false;
    }
    void newGame() { send("ucinewgame"); send("isready"); waitFor("readyok", 20000); }
    std::string bestMove(const std::string& positionCmd, const std::string& goCmd, int timeoutMs) {
        send(positionCmd);
        send(goCmd);
        std::string line;
        while (readLine(line, timeoutMs))
            if (line.rfind("bestmove", 0) == 0) {
                std::istringstream is(line); std::string a, m; is >> a >> m; return m;
            }
        return "";
    }
};

// ---------------------------------------------------------------- notation
static std::string sqName(int s) { return std::string{char('a' + fileOf(s)), char('1' + rankOf(s))}; }

// frame: attacker is White in the oracle; if the real attacker is Black the board is mirrored (sq ^ 56).
static int toReal(int s, bool atkWhite) { return atkWhite ? s : s ^ 56; }

static std::string toFen(const Pos& p, bool atkToMove, bool atkWhite, int hmc) {
    char board[64]; memset(board, 0, sizeof board);
    auto put = [&](int sq, char c) { board[toReal(sq, atkWhite)] = c; };
    auto A = [&](char c) { return atkWhite ? c : (char)tolower(c); };
    auto Dn = [&](char c) { return atkWhite ? (char)tolower(c) : c; };
    put(p.wk, A('K')); put(p.n, A('N')); put(p.b, A('B')); put(p.bk, Dn('K'));
    std::string f;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            char ch = board[r * 8 + c];
            if (!ch) { empty++; continue; }
            if (empty) { f += char('0' + empty); empty = 0; }
            f += ch;
        }
        if (empty) f += char('0' + empty);
        if (r) f += '/';
    }
    bool whiteToMove = atkToMove == atkWhite;
    return f + (whiteToMove ? " w" : " b") + " - - " + std::to_string(hmc) + " 1";
}

// Parse a FEN holding exactly K, N, B (one colour) + a bare king of the other colour.
static bool parseFen(const std::string& fen, Pos& p, bool& atkToMove) {
    std::istringstream is(fen);
    std::string place, stm;
    is >> place >> stm;
    int wk = -1, bk = -1, n = -1, b = -1;
    bool atkWhite = place.find('N') != std::string::npos || place.find('B') != std::string::npos;
    int r = 7, c = 0;
    for (char ch : place) {
        if (ch == '/') { r--; c = 0; continue; }
        if (isdigit(ch)) { c += ch - '0'; continue; }
        int sq = r * 8 + c++;
        bool white = isupper(ch);
        char u = (char)toupper(ch);
        if (u == 'K') { if (white == atkWhite) wk = sq; else bk = sq; }
        else if (u == 'N') n = sq;
        else if (u == 'B') b = sq;
        else return false;
    }
    if (wk < 0 || bk < 0 || n < 0 || b < 0) return false;
    if (!atkWhite) { wk ^= 56; bk ^= 56; n ^= 56; b ^= 56; }
    p = {wk, bk, n, b};
    bool whiteToMove = stm == "w";
    atkToMove = whiteToMove == atkWhite;
    return true;
}

// ---------------------------------------------------------------- defender policies
enum class DefPolicy { Optimal, Sitter, Random };

static int wrongCornerDist(const Pos& p, int sq) {
    int d = 99;
    static const int corners[4] = {0, 7, 56, 63};
    for (int c : corners) {
        if (isLightSq(c) == isLightSq(p.b)) continue;  // right corner for this bishop
        d = std::min(d, cheb(sq, c));
    }
    return d;
}

// Returns the defender's destination square, or -1 if a capture of N/B is available (it takes it).
static int chooseDefender(const Table& t, const Pos& p, DefPolicy pol, std::mt19937& rng) {
    DefMoves m = defenderMoves(p);
    if (m.hasCapture) return -1;
    if (m.count == 0) return -2;
    int best[8], nb = 0;
    long bestKey = -1;
    for (int k = 0; k < m.count; k++) {
        Pos q = p; q.bk = m.to[k];
        long dtm = t.A[indexOf(q)];
        long key;
        if (pol == DefPolicy::Optimal) key = dtm;
        else if (pol == DefPolicy::Sitter) key = (99 - wrongCornerDist(p, m.to[k])) * 1000 + dtm;
        else key = 0;
        if (key > bestKey) { bestKey = key; nb = 0; }
        if (key == bestKey) best[nb++] = m.to[k];
    }
    return best[rng() % nb];
}

static int chooseOracleAttacker(const Table& t, const Pos& p, AtkMove& out, std::mt19937& rng) {
    AtkMove mv[64];
    int c = attackerMoves(p, mv);
    int best = UNRES, pick[64], np = 0;
    for (int k = 0; k < c; k++) {
        int v = t.D[indexOf(applyAttacker(p, mv[k]))];
        if (v == UNRES) continue;
        if (v < best) { best = v; np = 0; }
        if (v == best) pick[np++] = k;
    }
    if (!np) return 0;
    out = mv[pick[rng() % np]];
    return 1;
}

// ---------------------------------------------------------------- one game
enum Result { MATE, STALEMATE, LOST_PIECE, REPETITION, FIFTY, FLAGGED, ILLEGAL, ENGINE_FAIL, START_DRAW, NRESULT };
static const char* resultName[NRESULT] = {"mate", "stalemate", "lost-piece", "3-fold", "50-move", "flagged",
                                           "illegal-move", "engine-fail", "start-is-draw"};

struct Config {
    std::string engine, nnue, set = "random", goCmd = "nodes 20000", attacker = "engine";
    DefPolicy defender = DefPolicy::Optimal;
    int count = 200, minDtm = 0, maxDtm = 999, jobs = 8, hash = 16, threads = 1, seed = 1, showFails = 5;
    double tcBase = 0, tcInc = 0;  // seconds; tcBase > 0 => real clock
    int color = 2;                 // 0 white, 1 black, 2 alternate
    double minRate = 100.0;        // exit status is non-zero below this conversion percentage
    bool verbose = false, trace = false;
};

struct GameStats {
    Result res = ENGINE_FAIL;
    int plies = 0, startDtm = 0;
    double msTotal = 0; int engineMoves = 0;
    std::vector<int> trace;           // oracle DTM of every attacker-to-move position reached
    int endDtm = -1, endZone = -1;   // oracle DTM when the game stopped; defender-king zone (see zoneName)
    std::string startFen, moves;
};

// Where the defender king ended up: 0 wrong-colour corner area (<=1 from a wrong corner), 1 right-colour corner area,
// 2 other edge square, 3 inside the board.
static const char* zoneName[4] = {"wrong-corner", "right-corner", "edge", "inside"};
static int zoneOf(const Pos& p) {
    bool wrong = false, right = false;
    for (int c : {0, 7, 56, 63}) {
        if (cheb(p.bk, c) > 1) continue;
        if (isLightSq(c) == isLightSq(p.b)) right = true; else wrong = true;
    }
    if (wrong) return 0;
    if (right) return 1;
    int f = fileOf(p.bk), r = rankOf(p.bk);
    return (f == 0 || f == 7 || r == 0 || r == 7) ? 2 : 3;
}

static GameStats playGame(const Config& cfg, Engine* eng, const Table& t, Pos p, bool atkToMove, bool atkWhite,
                          std::mt19937& rng) {
    GameStats g;
    g.startDtm = atkToMove ? t.A[indexOf(p)] : t.D[indexOf(p)];
    g.startFen = toFen(p, atkToMove, atkWhite, 0);
    if (g.startDtm == UNRES) { g.res = START_DRAW; return g; }
    if (eng) eng->newGame();
    std::map<uint64_t, int> seen;
    auto note = [&](const Pos& q, bool a) { return ++seen[(uint64_t)indexOf(q) * 2 + (a ? 1 : 0)]; };
    note(p, atkToMove);
    double clockMs = cfg.tcBase * 1000;
    std::string moveList;
    for (int ply = 1;; ply++) {
        if (atkToMove) {
            g.trace.push_back(t.A[indexOf(p)]);
            AtkMove mv;
            if (cfg.attacker == "oracle") {
                if (!chooseOracleAttacker(t, p, mv, rng)) { g.res = ILLEGAL; break; }
            } else {
                std::string goCmd = "go " + cfg.goCmd;
                if (cfg.tcBase > 0) {
                    int c = (int)clockMs;
                    int inc = (int)(cfg.tcInc * 1000);
                    goCmd = "go wtime " + std::to_string(c) + " btime " + std::to_string(c) + " winc " +
                            std::to_string(inc) + " binc " + std::to_string(inc);
                }
                auto t0 = Clock::now();
                std::string posCmd = "position fen " + g.startFen + (moveList.empty() ? "" : " moves" + moveList);
                std::string best = eng->bestMove(posCmd, goCmd, 60000);
                double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                g.msTotal += ms; g.engineMoves++;
                if (cfg.tcBase > 0) {
                    clockMs += -ms + cfg.tcInc * 1000;
                    if (clockMs <= 0) { g.res = FLAGGED; break; }
                }
                if (best.size() < 4) { g.res = ENGINE_FAIL; break; }
                int from = (best[0] - 'a') + (best[1] - '1') * 8;
                int to = (best[2] - 'a') + (best[3] - '1') * 8;
                if (!atkWhite) { from ^= 56; to ^= 56; }
                int piece = from == p.wk ? 0 : from == p.n ? 1 : from == p.b ? 2 : -1;
                AtkMove all[64]; int c = attackerMoves(p, all);
                bool ok = false;
                for (int k = 0; k < c; k++) if (all[k].piece == piece && all[k].to == to) ok = true;
                if (!ok) { g.res = ILLEGAL; g.moves = moveList + " [" + best + "]"; break; }
                mv = {piece, to};
            }
            std::string uci = sqName(toReal(mv.piece == 0 ? p.wk : mv.piece == 1 ? p.n : p.b, atkWhite)) +
                              sqName(toReal(mv.to, atkWhite));
            moveList += " " + uci;
            p = applyAttacker(p, mv);
            atkToMove = false;
        } else {
            int to = chooseDefender(t, p, cfg.defender, rng);
            if (to == -1) { g.res = LOST_PIECE; g.plies = ply; break; }
            if (to == -2) {
                g.plies = ply - 1;
                g.res = defenderInCheck(p) ? MATE : STALEMATE;
                break;
            }
            moveList += " " + sqName(toReal(p.bk, atkWhite)) + sqName(toReal(to, atkWhite));
            p.bk = to;
            atkToMove = true;
        }
        g.plies = ply;
        // mate / stalemate detection for the side now to move
        if (!atkToMove) {
            DefMoves m = defenderMoves(p);
            if (!m.hasCapture && m.count == 0) { g.res = defenderInCheck(p) ? MATE : STALEMATE; break; }
        }
        if (note(p, atkToMove) >= 3) { g.res = REPETITION; break; }
        if (ply >= 100) { g.res = FIFTY; break; }
    }
    g.moves = moveList;
    g.endZone = zoneOf(p);
    g.endDtm = atkToMove ? t.A[indexOf(p)] : t.D[indexOf(p)];
    return g;
}

// ---------------------------------------------------------------- start sets
struct Start { Pos p; bool atkToMove; };

static std::vector<Start> buildStarts(const Config& cfg, const Table& t) {
    std::vector<Start> out;
    auto addFen = [&](const std::string& f) { Pos p; bool a; if (parseFen(f, p, a)) out.push_back({p, a}); };
    if (cfg.set.rfind("fen:", 0) == 0) { addFen(cfg.set.substr(4)); return out; }
    if (cfg.set.rfind("file:", 0) == 0) {
        FILE* f = fopen(cfg.set.substr(5).c_str(), "r");
        if (!f) { fprintf(stderr, "cannot open %s\n", cfg.set.c_str()); exit(1); }
        char line[512];
        while (fgets(line, sizeof line, f)) addFen(line);
        fclose(f);
        return out;
    }
    std::vector<uint64_t> cand;  // idx*2 + (atkToMove)
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        Pos p = unpack(i);
        if (!legalShape(p)) continue;
        bool corner = false;
        if (cfg.set == "corner") {
            static const int corners[4] = {0, 7, 56, 63};
            for (int c : corners)
                if (isLightSq(c) != isLightSq(p.b) && cheb(p.bk, c) <= 1) corner = true;
            if (!corner) continue;
        }
        int a = t.A[i], d = t.D[i];
        bool allowD = cfg.set == "random";
        if (a != UNRES && a >= cfg.minDtm && a <= cfg.maxDtm && (cfg.set != "hard" || a >= std::max(cfg.minDtm, 57)))
            cand.push_back((uint64_t)i * 2 + 1);
        if (allowD && d != UNRES && d >= cfg.minDtm && d <= cfg.maxDtm) cand.push_back((uint64_t)i * 2);
    }
    std::mt19937_64 rng(cfg.seed);
    std::shuffle(cand.begin(), cand.end(), rng);
    if ((int)cand.size() > cfg.count) cand.resize(cfg.count);
    for (uint64_t c : cand) out.push_back({unpack((uint32_t)(c >> 1)), (bool)(c & 1)});
    return out;
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(1); } return argv[++i]; };
        if (a == "--engine") cfg.engine = next();
        else if (a == "--nnue") cfg.nnue = next();
        else if (a == "--set") cfg.set = next();
        else if (a == "--count") cfg.count = stoi(next());
        else if (a == "--go") cfg.goCmd = next();
        else if (a == "--tc") { std::string s = next(); cfg.tcBase = stod(s.substr(0, s.find('+'))); cfg.tcInc = s.find('+') == std::string::npos ? 0 : stod(s.substr(s.find('+') + 1)); }
        else if (a == "--jobs") cfg.jobs = stoi(next());
        else if (a == "--hash") cfg.hash = stoi(next());
        else if (a == "--threads") cfg.threads = stoi(next());
        else if (a == "--seed") cfg.seed = stoi(next());
        else if (a == "--min-dtm") cfg.minDtm = stoi(next());
        else if (a == "--max-dtm") cfg.maxDtm = stoi(next());
        else if (a == "--show-fails") cfg.showFails = stoi(next());
        else if (a == "--attacker") cfg.attacker = next();
        else if (a == "--color") { std::string s = next(); cfg.color = s == "white" ? 0 : s == "black" ? 1 : 2; }
        else if (a == "--verbose") cfg.verbose = true;
        else if (a == "--trace") cfg.trace = true;
        else if (a == "--min-rate") cfg.minRate = stod(next());
        else if (a == "--defender") { std::string s = next(); cfg.defender = s == "sitter" ? DefPolicy::Sitter : s == "random" ? DefPolicy::Random : DefPolicy::Optimal; }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    if (cfg.attacker == "engine" && cfg.engine.empty() && cfg.set != "selftest") { fprintf(stderr, "--engine required\n"); return 1; }

    auto t0 = Clock::now();
    Table t = solve();
    fprintf(stderr, "oracle ready (%.1fs), longest mate = %d plies\n",
            std::chrono::duration<double>(Clock::now() - t0).count(), t.maxA);
    if (cfg.set == "selftest") {
        long bad = validate(t);
        printf("oracle validate: %ld inconsistent entries, longest mate %d moves\n", bad, (t.maxA + 1) / 2);
        return bad != 0;
    }

    std::vector<Start> starts = buildStarts(cfg, t);
    std::vector<GameStats> results(starts.size());
    std::atomic<size_t> next{0};
    std::mutex mu;
    auto worker = [&]() {
        std::unique_ptr<Engine> eng;
        if (cfg.attacker == "engine") eng = std::make_unique<Engine>(cfg.engine, cfg.nnue, cfg.hash, cfg.threads);
        for (;;) {
            size_t i = next++;
            if (i >= starts.size()) break;
            std::mt19937 rng(cfg.seed * 7919u + (unsigned)i);
            bool atkWhite = cfg.color == 0 ? true : cfg.color == 1 ? false : (i % 2 == 0);
            results[i] = playGame(cfg, eng.get(), t, starts[i].p, starts[i].atkToMove, atkWhite, rng);
            if (cfg.verbose) {
                std::lock_guard<std::mutex> lk(mu);
                fprintf(stderr, "[%zu] %s plies=%d dtm=%d\n", i, resultName[results[i].res], results[i].plies, results[i].startDtm);
            }
        }
    };
    std::vector<std::thread> pool;
    for (int j = 0; j < std::max(1, cfg.jobs); j++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    int cnt[NRESULT] = {};
    std::vector<int> waste, plies;
    double ms = 0; long mv = 0;
    for (auto& g : results) {
        cnt[g.res]++;
        ms += g.msTotal; mv += g.engineMoves;
        if (g.res == MATE) { waste.push_back(g.plies - g.startDtm); plies.push_back(g.plies); }
    }
    std::sort(waste.begin(), waste.end()); std::sort(plies.begin(), plies.end());
    auto pct = [](const std::vector<int>& v, double q) { return v.empty() ? 0 : v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
    const int skipped = cnt[START_DRAW];            // generator left a piece en prise: no win exists, not an engine failure
    int total = (int)results.size() - skipped;
    printf("set=%s defender=%s go=\"%s\" tc=%g+%g n=%d\n", cfg.set.c_str(),
           cfg.defender == DefPolicy::Optimal ? "optimal" : cfg.defender == DefPolicy::Sitter ? "sitter" : "random",
           cfg.tcBase > 0 ? "clock" : cfg.goCmd.c_str(), cfg.tcBase, cfg.tcInc, total);
    printf("CONVERTED %d/%d (%.1f%%) |", cnt[MATE], total, total ? 100.0 * cnt[MATE] / total : 0.0);
    for (int r = 1; r < NRESULT; r++) if (cnt[r] && r != START_DRAW) printf(" %s=%d", resultName[r], cnt[r]);
    if (skipped) printf(" (%d start positions skipped: already a theoretical draw)", skipped);
    printf("\n");
    if (!plies.empty())
        printf("plies to mate: median %d  p90 %d  max %d | waste over optimal: median %d  p90 %d  max %d | %.1f ms/move\n",
               pct(plies, .5), pct(plies, .9), plies.back(), pct(waste, .5), pct(waste, .9), waste.back(), mv ? ms / mv : 0.0);
    {
        int zones[4] = {}, nf = 0; double endSum = 0; int endN = 0;
        for (auto& g : results) {
            if (g.res != FIFTY && g.res != REPETITION) continue;
            nf++; zones[g.endZone]++;
            if (g.endDtm != UNRES && g.endDtm >= 0) { endSum += g.endDtm; endN++; }
        }
        if (nf) {
            printf("stalled games (50-move/3-fold): %d | defender king at end: ", nf);
            for (int z = 0; z < 4; z++) printf("%s=%d ", zoneName[z], zones[z]);
            printf("| mean DTM left %.1f plies\n", endN ? endSum / endN : 0.0);
        }
    }
    int shown = 0;
    for (size_t i = 0; i < results.size() && shown < cfg.showFails; i++) {
        const GameStats& g = results[i];
        if (g.res == MATE || g.res == START_DRAW) continue;
        shown++;
        printf("FAIL %-12s dtm=%-3d plies=%-3d %s moves:%s\n", resultName[g.res], g.startDtm, g.plies, g.startFen.c_str(), g.moves.c_str());
        if (cfg.trace) { printf("  DTM trace:"); for (int v : g.trace) printf(" %d", v); printf("\n"); }
    }
    return total && 100.0 * cnt[MATE] / total >= cfg.minRate ? 0 : 2;
}
