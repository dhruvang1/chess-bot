// Trains the K+B+N vs K evaluation net (kbnEval.hpp) and writes kbnNetWeights.hpp.
//
//   kbn_train [out=kbnNetWeights.hpp] [--members 4] [--hidden 48] [--epochs 40] [--seed 1]
//
// Teacher: the exact mate distance from kbn_oracle.hpp (retrograde analysis, ~4 s). Student: the one-hidden-layer
// net of kbnEval.hpp. Recipe, and why each part is there (all measured with test/kbn_verify.cpp):
//   * Regression on mate distance in plies, but weighted toward the part of the game that decides the 50-move
//     race: full weight up to 40 plies from mate, 0.15 beyond.
//   * Positions with the defender king next to a wrong-colour corner (and 20-55 plies from mate) weigh x10. That
//     is the W-manoeuvre stage: nets without the extra weight stall there for 10-20 moves.
//   * Several members trained from different seeds are averaged. Equal RMSE does not mean equal play -- single nets
//     converted anywhere from 82% to 99% of games, each stalling in a different place; the average does not.
//     Averaging N nets of width H is exactly one net of width N*H (output weights /N), which is what is written.
#define KBN_TRAINING
#include "../kbnEval.hpp"
#include "kbn_oracle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace KbnEval;

namespace {

struct Sample {
    int16_t rows[ACTIVE_ROWS];
    uint8_t dtm;
    float weight;
};

struct Net {
    int H = 0;
    std::vector<float> E, b1, w2;  // E[NUM_ROWS][H]
    float b2 = 0.f;

    void init(int hidden, unsigned seed) {
        H = hidden;
        std::mt19937 g(seed);
        std::normal_distribution<float> nd(0.f, 1.f);
        E.resize((size_t)NUM_ROWS * H); for (auto& x : E) x = nd(g) * 0.15f;
        b1.assign(H, 0.f);
        w2.resize(H); for (auto& x : w2) x = nd(g) * 0.09f;
        b2 = 48.f;
    }
    float forward(const int16_t* rows, float* h) const {
        for (int j = 0; j < H; j++) h[j] = b1[j];
        for (int k = 0; k < ACTIVE_ROWS; k++) {
            const float* e = &E[(size_t)rows[k] * H];
            for (int j = 0; j < H; j++) h[j] += e[j];
        }
        float o = b2;
        for (int j = 0; j < H; j++) { if (h[j] < 0.f) h[j] = 0.f; o += w2[j] * h[j]; }
        return o;
    }
};

void trainMember(Net& net, const std::vector<Sample>& data, int epochs, unsigned seed, int id) {
    const int H = net.H, BATCH = 128;
    const float B1 = 0.9f, B2 = 0.999f, EPS = 1e-8f;
    net.init(H, seed);
    std::vector<float> mE(net.E.size(), 0.f), vE(net.E.size(), 0.f), gE(net.E.size(), 0.f);
    std::vector<float> mb(H, 0.f), vb(H, 0.f), mw(H, 0.f), vw(H, 0.f), gb(H), gw(H), h(H);
    float mb2 = 0.f, vb2 = 0.f;
    std::vector<uint32_t> order(data.size());
    for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
    std::vector<int> touched; std::vector<char> mark(NUM_ROWS, 0);
    std::mt19937 rng(seed * 31 + 1);
    long step = 0;
    for (int ep = 0; ep < epochs; ep++) {
        std::shuffle(order.begin(), order.end(), rng);
        const float lr = 0.003f * std::pow(0.93f, (float)ep);
        for (size_t i = 0; i + BATCH <= order.size(); i += BATCH) {
            std::fill(gb.begin(), gb.end(), 0.f); std::fill(gw.begin(), gw.end(), 0.f);
            float gb2 = 0.f; touched.clear();
            for (int s = 0; s < BATCH; s++) {
                const Sample& sm = data[order[i + s]];
                const float o = net.forward(sm.rows, h.data());
                const float d = (o - sm.dtm) * sm.weight * (2.f / BATCH);
                gb2 += d;
                for (int j = 0; j < H; j++) {
                    gw[j] += d * h[j];
                    const float dh = h[j] > 0.f ? d * net.w2[j] : 0.f;
                    gb[j] += dh; h[j] = dh;
                }
                for (int k = 0; k < ACTIVE_ROWS; k++) {
                    const int r = sm.rows[k];
                    float* g = &gE[(size_t)r * H];
                    if (!mark[r]) { mark[r] = 1; touched.push_back(r); std::fill(g, g + H, 0.f); }
                    for (int j = 0; j < H; j++) g[j] += h[j];
                }
            }
            step++;
            const float a = lr * std::sqrt(1.f - std::pow(B2, (float)step)) / (1.f - std::pow(B1, (float)step));
            auto adam = [&](float& p, float g, float& m, float& v) {
                m = B1 * m + (1.f - B1) * g; v = B2 * v + (1.f - B2) * g * g;
                p -= a * m / (std::sqrt(v) + EPS);
            };
            for (int r : touched) {
                mark[r] = 0;
                const size_t o = (size_t)r * H;
                for (int j = 0; j < H; j++) adam(net.E[o + j], gE[o + j], mE[o + j], vE[o + j]);
            }
            for (int j = 0; j < H; j++) { adam(net.b1[j], gb[j], mb[j], vb[j]); adam(net.w2[j], gw[j], mw[j], vw[j]); }
            adam(net.b2, gb2, mb2, vb2);
        }
        if (ep % 10 == 9) { fprintf(stderr, "  member %d: epoch %d/%d\n", id, ep + 1, epochs); }
    }
}

int pow2Scale(float maxAbs, float limit) {
    int s = 1;
    while (maxAbs * (s * 2) <= limit && s < (1 << 14)) s *= 2;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = "kbnNetWeights.hpp";
    int members = 4, hidden = 48, epochs = 40, seed = 1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--members" && i + 1 < argc) members = atoi(argv[++i]);
        else if (a == "--hidden" && i + 1 < argc) hidden = atoi(argv[++i]);
        else if (a == "--epochs" && i + 1 < argc) epochs = atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = atoi(argv[++i]);
        else if (a[0] != '-') out = a;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }

    auto t0 = std::chrono::steady_clock::now();
    kbn::Table t = kbn::solve();
    fprintf(stderr, "teacher solved (longest mate %d plies)\n", t.maxA);

    // One representative per symmetry class: dark bishop, defender king in the wedge. 1 in 50 is held out.
    std::vector<Sample> train, holdout;
    std::mt19937 split(12345);
    for (int stm = 0; stm < 2; stm++) {
        const std::vector<uint8_t>& T = stm == 0 ? t.A : t.D;
        for (uint32_t i = 0; i < kbn::TABLE_SIZE; i++) {
            if (T[i] == kbn::UNRES) continue;
            const kbn::Pos p = kbn::unpack(i);
            if (!isDark(p.b) || WEDGE[p.bk] < 0) continue;
            Sample s;
            int rows[ACTIVE_ROWS];
            featureRows(p.wk, p.bk, p.n, p.b, stm == 0, rows);
            for (int k = 0; k < ACTIVE_ROWS; k++) s.rows[k] = (int16_t)rows[k];
            s.dtm = T[i];
            s.weight = T[i] <= 40 ? 1.f : 0.15f;
            const int wrongCornerDist = std::max(std::abs((p.bk & 7) - 0), std::abs((p.bk >> 3) - 7));  // a8 in the folded frame
            if (wrongCornerDist <= 1 && T[i] >= 20 && T[i] <= 55) s.weight *= 10.f;
            (split() % 50 == 0 ? holdout : train).push_back(s);
        }
    }
    fprintf(stderr, "%zu training / %zu held-out positions; training %d members x %d hidden units, %d epochs\n",
            train.size(), holdout.size(), members, hidden, epochs);

    std::vector<Net> nets(members);
    for (auto& n : nets) n.H = hidden;
    std::vector<std::thread> pool;
    for (int m = 0; m < members; m++)
        pool.emplace_back(trainMember, std::ref(nets[m]), std::cref(train), epochs, (unsigned)(seed * 1000 + m + 1), m);
    for (auto& th : pool) th.join();

    // Average == one wide net: concatenate hidden units, divide output weights by the member count.
    const int HT = members * hidden;
    std::vector<float> E((size_t)NUM_ROWS * HT), b1(HT), w2(HT);
    float b2 = 0.f;
    for (int m = 0; m < members; m++) {
        for (int r = 0; r < NUM_ROWS; r++)
            for (int j = 0; j < hidden; j++) E[(size_t)r * HT + m * hidden + j] = nets[m].E[(size_t)r * hidden + j];
        for (int j = 0; j < hidden; j++) { b1[m * hidden + j] = nets[m].b1[j]; w2[m * hidden + j] = nets[m].w2[j] / members; }
        b2 += nets[m].b2 / members;
    }
    // Scales: powers of two as fine as possible subject to (1) every stored weight fits int16 and (2) the int32
    // accumulator in predictFixed() provably cannot overflow: |hidden| <= |bias| + the five largest |row weight|
    // of that unit, and |sum| <= max|hidden| * sum|out weights|.
    float maxE = 0.f, maxW = 0.f, maxHidden = 0.f, sumW = 0.f;
    for (float x : E) maxE = std::max(maxE, std::fabs(x));
    for (float x : b1) maxE = std::max(maxE, std::fabs(x));
    for (float x : w2) { maxW = std::max(maxW, std::fabs(x)); sumW += std::fabs(x); }
    for (int j = 0; j < HT; j++) {
        std::vector<float> col(NUM_ROWS);
        for (int r = 0; r < NUM_ROWS; r++) col[r] = std::fabs(E[(size_t)r * HT + j]);
        std::partial_sort(col.begin(), col.begin() + ACTIVE_ROWS, col.end(), std::greater<float>());
        float h = std::fabs(b1[j]);
        for (int k = 0; k < ACTIVE_ROWS; k++) h += col[k];
        maxHidden = std::max(maxHidden, h);
    }
    int eScale = pow2Scale(maxE, 30000.f), wScale = pow2Scale(maxW, 30000.f);
    while ((double)eScale * wScale * maxHidden * sumW > 2147483647.0 / 1.05 && (eScale > 1 || wScale > 1)) {
        if (wScale >= eScale) wScale /= 2; else eScale /= 2;
    }
    auto q16 = [](float x, int s) { return (int16_t)std::lrintf(x * s); };
    std::vector<int16_t> qE(E.size()), qB(HT), qW(HT);
    for (size_t i = 0; i < E.size(); i++) qE[i] = q16(E[i], eScale);
    for (int j = 0; j < HT; j++) { qB[j] = q16(b1[j], eScale); qW[j] = q16(w2[j], wScale); }
    const int32_t qOut = (int32_t)std::lrintf(b2 * PRED_ONE);

    // Held-out error of the quantised net -- the thing that actually ships.
    double se = 0, se45 = 0; long n45 = 0;
    for (const Sample& s : holdout) {
        int rows[ACTIVE_ROWS]; for (int k = 0; k < ACTIVE_ROWS; k++) rows[k] = s.rows[k];
        const double pred = (double)predictFixed(qE.data(), qB.data(), qW.data(), qOut, HT, eScale, wScale, rows) / PRED_ONE;
        const double e = pred - s.dtm;
        se += e * e; if (s.dtm <= 45) { se45 += e * e; n45++; }
    }
    fprintf(stderr, "held-out RMSE of the shipped (quantised) net: %.2f plies overall, %.2f within 45 plies of mate\n",
            std::sqrt(se / holdout.size()), std::sqrt(se45 / std::max(1L, n45)));

    FILE* f = fopen(out.c_str(), "w");
    if (!f) { perror(out.c_str()); return 1; }
    fprintf(f, "#pragma once\n// GENERATED by test/kbn_train.cpp -- do not edit. Regenerate: build/kbn_train kbnNetWeights.hpp");
    fprintf(f, " --members %d --hidden %d --epochs %d --seed %d\n", members, hidden, epochs, seed);
    fprintf(f, "// %d independently trained nets of %d hidden units, averaged into one net of %d units; teacher = exact K+B+N vs K mate distance.\n", members, hidden, HT);
    fprintf(f, "// Held-out RMSE (quantised): %.2f plies overall, %.2f within 45 plies of mate.\n#include <cstdint>\n\nnamespace KbnEval {\n\n",
            std::sqrt(se / holdout.size()), std::sqrt(se45 / std::max(1L, n45)));
    fprintf(f, "constexpr int HIDDEN  = %d;\nconstexpr int E_SCALE = %d;   // ROW_WEIGHTS and HIDDEN_BIAS are stored x E_SCALE\nconstexpr int W_SCALE = %d;   // OUT_WEIGHTS are stored x W_SCALE\n", HT, eScale, wScale);
    fprintf(f, "constexpr int32_t OUT_BIAS = %d;  // predicted plies x %d\n\n", qOut, PRED_ONE);
    fprintf(f, "constexpr int16_t HIDDEN_BIAS[HIDDEN] = {");
    for (int j = 0; j < HT; j++) fprintf(f, "%s%d,", j % 16 == 0 ? "\n    " : " ", qB[j]);
    fprintf(f, "\n};\n\nconstexpr int16_t OUT_WEIGHTS[HIDDEN] = {");
    for (int j = 0; j < HT; j++) fprintf(f, "%s%d,", j % 16 == 0 ? "\n    " : " ", qW[j]);
    fprintf(f, "\n};\n\nconstexpr int16_t ROW_WEIGHTS[%d][HIDDEN] = {", NUM_ROWS);
    for (int r = 0; r < NUM_ROWS; r++) {
        fprintf(f, "\n    {");
        for (int j = 0; j < HT; j++) fprintf(f, "%s%d,", j % 24 == 0 ? "\n     " : " ", qE[(size_t)r * HT + j]);
        fprintf(f, "\n    },");
    }
    fprintf(f, "\n};\n\n}  // namespace KbnEval\n");
    fclose(f);
    fprintf(stderr, "wrote %s (%.0f s)\n", out.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
