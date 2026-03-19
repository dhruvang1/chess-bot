# Chess Engine

A chess engine written in C++20 with UCI protocol support. Estimated ~2100 Elo. Designed to run with [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) or any UCI-compatible GUI.

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

- Alpha-beta with principal variation search (PVS)
- Iterative deepening
- Quiescence search
- Transposition table with exact/alpha/beta flags
- Null move pruning
- Late move reductions (LMR)
- Killer move heuristic
- Countermove heuristic
- Move ordering (TT move, captures by MVV-LVA, killers, countermoves, quiets)

## Evaluation

**NNUE** (768→512→1, SCReLU activation, dual perspective accumulators)
- Feature set: Chess768 — 6 piece types × 2 colors × 64 squares = 768 binary inputs
- Dual perspective: separate STM and NSTM accumulators concatenated (effectively 1024-wide hidden layer)
- Activation: SCReLU — `clamp(x, 0, QA)² × weight`, computed with int32 arithmetic
- Quantization: QA=255, QB=64, Scale=400
- Inference is auto-vectorized with ARM NEON (SIMD) — ~1.17M NPS on Apple M-series
- Accumulators updated incrementally on make; reversed incrementally on undo (no copy overhead)
- Network loaded via `setoption name NNUEPath value <path>`
- Trained on ~48M positions: self-play (depth 7) + Lichess elite games (engine-evaluated at depth 7)

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