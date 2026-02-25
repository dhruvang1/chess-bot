# Chess Engine

A chess engine written in C++20 with UCI protocol support. Designed to run with [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) or any UCI-compatible GUI.

## Architecture

| File | Purpose |
|---|---|
| `board.cpp` | 8x8 array board representation, move generation, evaluation |
| `magicBoard.cpp` | Bitboard-based board with magic bitboards for sliding pieces |
| `search.cpp` | Alpha-beta search with iterative deepening |
| `uci.cpp` | UCI protocol handler |
| `hash.cpp` | Zobrist hashing |
| `transposition.cpp` | Transposition table |
| `staticEvals.cpp` | Piece-square tables (PeSTO) |
| `magic_constants.h` | Magic numbers and slow attack generators for slider table init |
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
- Bishop pair bonus (scales from 5% to 30% of pawn value toward endgame)
- Pawn structure: doubled, isolated, and passed pawn evaluation
- King safety: pawn shield bonus for castled king positions
- Game phase detection (piece-weighted, max 24)

## Build

Requires CMake 3.27+ and a C++20 compiler.

The project builds two engine binaries from the same codebase:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

| Binary | Board representation |
|---|---|
| `build/chess_old` | 8x8 array (`Board`) |
| `build/chess_magic` | Bitboards with magic bitboards (`MagicBoard`) |

To build only one:

```bash
cmake --build build --target chess_old
cmake --build build --target chess_magic
```

## Tests

Runs both `Board` and `MagicBoard` through the same positions and compares board state, attack maps, legal moves, evaluation, and Zobrist hashes.

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

Build both binaries and use a UCI GUI to run matches between them:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Compatible GUIs: [BanksiaGUI](https://banksiagui.com/), [cutechess](https://github.com/cutechess/cutechess), or via CLI: