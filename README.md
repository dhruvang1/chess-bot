# Chess Engine

A chess engine written in C++20 with UCI protocol support. Estimated ~2600 Elo. Designed to run with [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) or any UCI-compatible GUI.

## Architecture

| File | Purpose |
|---|---|
| `magicBoard.cpp` | Bitboard board representation, move generation, incremental eval, NNUE accumulator |
| `search.cpp` | Alpha-beta search with iterative deepening |
| `uci.cpp` | UCI protocol handler |
| `hash.cpp` | Zobrist hashing |
| `transposition.cpp` | Transposition table |
| `nnue.h` | NNUE weight structs, accumulator ops, forward pass |
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

**NNUE** (HalfKAv2 HM, 24576→512→1, SCReLU, dual perspective, 8 output buckets)
- Feature set: HalfKAv2 with horizontal mirroring — 768 features × 32 king buckets = 24,576 inputs per perspective
- King mirroring: when king is on files e–h all piece squares are flipped, halving the king-square dimension from 64→32
- 8 material-count output buckets — `bucket = (popcount(occupied) − 2) / 4`
- Dual perspective: STM + NSTM accumulators concatenated (effectively 1024-wide hidden layer)
- Activation: SCReLU — `clamp(x, 0, QA)² × weight`, computed with int32 arithmetic
- Quantization: QA=255, QB=64, Scale=400
- Inference auto-vectorized with ARM NEON (SIMD) — ~1.3M NPS on Apple M-series
- Accumulators updated incrementally on make; reversed incrementally on undo (no copy overhead)
- Network loaded via `setoption name NNUEPath value <path>`
- Trained with [bullet](https://github.com/jw1912/bullet) on self-play

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

## Usage

The engine communicates over stdin/stdout using the UCI protocol:

```
$ ./build/chess_magic
uci
isready
position startpos moves e2e4 e7e5
go wtime 60000 btime 60000
```

Set the `manual` environment variable to `1` for debug mode (prints board and hash after each command):

```bash
manual=1 ./build/chess_magic
```

## Engine vs Engine

Compatible GUIs: [BanksiaGUI](https://banksiagui.com/), [cutechess](https://github.com/cutechess/cutechess), or via CLI: