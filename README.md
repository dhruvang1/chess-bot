# Chess Engine

A chess engine written in C++20 with UCI protocol support. Designed to run with [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) or any UCI-compatible GUI.

## Architecture

| File | Purpose |
|---|---|
| `board.cpp` | 8x8 array board representation, move generation, evaluation |
| `magicBoard.cpp` | Bitboard-based board representation (in progress) |
| `search.cpp` | Alpha-beta search with iterative deepening |
| `uci.cpp` | UCI protocol handler |
| `hash.cpp` | Zobrist hashing |
| `transposition.cpp` | Transposition table |
| `staticEvals.cpp` | Piece-square tables (PeSTO) |
| `move.h` | Move struct |

## Search

- Alpha-beta with principal variation search (PVS)
- Iterative deepening
- Quiescence search
- Transposition table with exact/alpha/beta flags
- Null move pruning
- Late move reductions (LMR)
- Killer move heuristic
- Move ordering (TT move, captures by MVV-LVA, killers, quiets)

## Evaluation

- Tapered eval blending middlegame and endgame piece-square tables
- Bishop pair bonus
- Pawn structure: doubled, isolated, and passed pawn evaluation
- Game phase detection (piece-weighted)

## Build

Requires CMake 3.27+ and a C++20 compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The engine binary is `build/chess_engine`.

## Tests

```bash
cmake -B build
cmake --build build --target test_board
./build/test_board
```

## Usage

The engine communicates over stdin/stdout using the UCI protocol:

```
$ ./build/chess_engine
uci
isready
position startpos moves e2e4 e7e5
go movetime 5000
```

Set the `manual` environment variable to `1` for manual/debug mode:

```bash
manual=1 ./build/chess_engine
```
