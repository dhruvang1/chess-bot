# Chess Engine - Simple Bot

A chess engine written in C++20 with UCI protocol support. Designed to run with [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) or any UCI-compatible GUI. It plays under the handle [simple-bot](https://lichess.org/@/simple-bot) on lichess.

Simple Bot prides itself on using the original self-generated dataset for its neural network. No other engine evaluations or dataset (Stockfish, Leela, etc.) were used. 
Early iterations used Lichess games to generate diversity.

## Architecture

| File | Purpose |
|---|---|
| `magicBoard.cpp` | Bitboard board representation, move generation, incremental eval, NNUE accumulator |
| `search.cpp` | Alpha-beta search with iterative deepening |
| `uci.cpp` | UCI protocol handler |
| `hash.cpp` | Zobrist hashing |
| `transposition.cpp` | Transposition table |
| `nnue.h` | NNUE weight structs, accumulator ops, forward pass |
| `kbnEval.hpp` / `kbnNetWeights.hpp` | K+B+N vs K evaluation: tiny net distilled from the exact mate distance (weights are generated) |
| `magic_constants.h` | Magic numbers and slider table init |
| `move.h` | Move encoding |

## Search

**Core**
- Alpha-beta with principal variation search (PVS)
- Iterative deepening with aspiration windows
- Quiescence search (captures + promotions)
- Transposition table — 2-slot buckets with age-based replacement, exact/alpha/beta flags

**Pruning**
- Null move pruning with variable reduction
- Reverse futility pruning
- Futility pruning
- Late move pruning (LMP) with history-gated extension
- Probcut

**Extensions & Reductions**
- Singular extensions — TT move searched alone at reduced depth; extended if clearly best
- Check extension
- Late move reductions (LMR) — logarithmic table, adjusted by history, captures, killers, PV node
- Internal iterative reduction (IIR)

**Move Ordering**
- TT move → promotions → good captures (SEE ≥ 0) → killer 1/2 → countermove → quiets → losing captures
- Good/losing captures ordered by MVV-LVA + capture history
- History heuristic with bonus on beta cutoff and malus on all tried quiets that failed
- 1-ply continuation history — conditions quiet move scores on the previous move's piece/square

## Evaluation

**NNUE** (HalfKAv2-HM, 12288→1024→8, SCReLU, dual perspective, 8 output buckets)
- Feature set: HalfKAv2 with horizontal mirroring — 768 features × 16 mirrored king buckets = 12,288 inputs per perspective
- King mirroring: when king is on files e–h all piece squares are flipped, halving the king-square dimension from 64→32; 32 post-mirror squares mapped to 16 buckets
- 8 material-count output buckets
- Accumulators updated incrementally on make; reversed incrementally on undo (no copy overhead)
- Network loaded via `setoption name NNUEPath value <path>`
- Trained with [bullet](https://github.com/jw1912/bullet)

**K+B+N vs K** (`kbnEval.hpp`)
- NNUE has no data for this ending and hand-written terms (distance to the right corner, king proximity, tropism, per-piece square tables) cannot express the 33-move manoeuvre needed against a defender sitting in the wrong-colour corner: they converted ~45% of random positions inside the 50-move rule against best defence.
- Replaced by a 192-unit ReLU net over five one-hot features (defender king, the three attackers, side to move, folded by the board's symmetries) that predicts the mate distance; `eval = 1500 + (70 - predicted plies) * 20`. Integer inference, ~35k int16 weights, no probing of any table.
- These scores are mate distances, so the search treats them like mate scores: they are shifted by ply (`kbnAdj` in `search.cpp`) and stored in the TT relative to the node. Without that, an entry from a deeper search (any entry carried over from an earlier move) outscores a fresh shallow one and the engine keeps repeating its old plan in the wrong-corner phase.

## Build

Requires CMake 3.27+ and a C++20 compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Tests

Runs `MagicBoard` through test positions checking board state, attack maps, legal moves, evaluation, and Zobrist hashes.

```bash
cmake --build build --target test_board
./build/test_board
```

### K+B+N vs K conversion

`test/kbn_oracle.hpp` solves K+B+N vs K exactly (retrograde analysis, ~4 s) and is used only as a *test oracle and training teacher* -- the engine never links it. `kbn_verify` plays the real engine over UCI as the attacker against a defender that plays the exact optimal defence (or "sits": heads for the wrong-colour corner and shuffles), and reports how many games end in mate inside the 50-move rule.

```bash
cmake --build build --target kbn_verify
./build/kbn_verify --engine ./build/chess_magic --nnue nnue/quantised.bin \
    --set random --count 1000 --go "nodes 5000" --defender optimal      # fixed budget
./build/kbn_verify ... --set corner --defender sitter --tc 2+0.01       # defender starts in the wrong corner, real clock
./build/kbn_verify ... --set file:book/kbn_vs_k.epd                      # your own positions
./build/kbn_verify --set selftest                                       # validates the oracle itself
```

Sets: `random`, `hard` (>= 57 plies from mate), `corner`, `file:<epd>`, `fen:<fen>`. Add `--trace` to see the true mate distance after every move of a failed game, `--min-rate` to turn it into a pass/fail check.

To regenerate the net (deterministic, ~1 min): `cmake --build build --target kbn_train && ./build/kbn_train kbnNetWeights.hpp`. A stale `kbnNetWeights.hpp` fails to compile (`static_assert` on the feature layout).

## Usage

The engine communicates over stdin/stdout using the UCI protocol:

```
$ ./build/chess_magic
uci
isready
position startpos moves e2e4 e7e5
go wtime 60000 btime 60000
```

Supported `go` limits: `go wtime <ms> btime <ms> [winc <ms>] [binc <ms>]` (time control),
`go depth <n>` (fixed depth), and `go nodes <n>` (fixed node budget — searches to the
default max depth but stops once ~`<n>` nodes have been visited; the count overshoots
slightly because the in-progress iterative-deepening iteration is always finished).

Set the `manual` environment variable to `1` for debug mode (prints board and hash after each command):

```bash
manual=1 ./build/chess_magic
```

## Benchmark

`bench` runs a fixed 41-position suite to a fixed depth on one thread with a
freshly-zeroed TT, and prints a deterministic `<nodes> nodes <nps> nps` line. The
node count is identical on every run/machine (a build fingerprint); NPS is the
metric to compare between builds.

```bash
./build/chess_magic --nnue nnue/quantised.bin bench      # default depth 10
./build/chess_magic --nnue nnue/quantised.bin bench 13   # deeper, less noisy NPS
```

For an A/B NPS comparison, build both revisions and interleave runs
(`A B A B …`, not `A A A B B B`), then take the median.

## Engine vs Engine

Compatible GUIs: [BanksiaGUI](https://banksiagui.com/), [cutechess](https://github.com/cutechess/cutechess), or via CLI: