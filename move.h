#pragma once

#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>

using namespace std;

static constexpr uint16_t MOVE_NONE = 0;

static inline uint16_t encodeMove(int from, int to) {
    return (uint16_t)(from | (to << 6));
}

static inline uint16_t encodePromo(int from, int to, char piece) {
    uint16_t promo = 0;
    switch (piece) {
        case 'n': promo = 0; break;
        case 'b': promo = 1; break;
        case 'r': promo = 2; break;
        case 'q': promo = 3; break;
    }
    return (uint16_t)(from | (to << 6) | (promo << 12) | (1 << 14));
}

static inline int fromSq(uint16_t m) { return m & 0x3F; }
static inline int toSq(uint16_t m) { return (m >> 6) & 0x3F; }
static inline bool isPromoMove(uint16_t m) { return (m >> 14) & 1; }
static inline char promoChar(uint16_t m) {
    static const char pieces[] = {'n', 'b', 'r', 'q'};
    return pieces[(m >> 12) & 3];
}

static inline string moveToUci(uint16_t m) {
    if (m == MOVE_NONE) return "";
    int from = fromSq(m);
    int to = toSq(m);
    string s;
    s += (char)('a' + (from & 7));
    s += (char)('1' + (from >> 3));
    s += (char)('a' + (to & 7));
    s += (char)('1' + (to >> 3));
    if (isPromoMove(m)) s += promoChar(m);
    return s;
}

static inline uint16_t uciToMove(const string& uci) {
    if (uci.length() < 4) return MOVE_NONE;
    int from = (uci[0] - 'a') + (uci[1] - '1') * 8;
    int to = (uci[2] - 'a') + (uci[3] - '1') * 8;
    if (uci.length() == 5) {
        return encodePromo(from, to, uci[4]);
    }
    return encodeMove(from, to);
}

struct Move {
    uint16_t move = MOVE_NONE;
    char movePiece = ' ';
    char capturePiece = ' ';
    bool isCapture = false;
    bool isPromotion = false;
    bool isCastle = false;
    bool isLosingCapture = false;
    int32_t score = 0;

    Move() = default;

    Move(uint16_t move, char movePiece)
        : move(move), movePiece(movePiece) {}

    Move(uint16_t move, char movePiece, bool isCastle, bool isPromotion)
        : move(move), movePiece(movePiece), isPromotion(isPromotion), isCastle(isCastle) {}

    Move(uint16_t move, char movePiece, char capturePiece)
        : move(move), movePiece(movePiece), capturePiece(capturePiece), isCapture(true) {}

    // Compatibility constructors for board.cpp (string-based move generation)
    Move(const string& uci, char movePiece)
        : move(uciToMove(uci)), movePiece(movePiece) {}

    Move(const string& uci, char movePiece, bool isCastle, bool isPromotion)
        : move(uciToMove(uci)), movePiece(movePiece), isPromotion(isPromotion), isCastle(isCastle) {}

    Move(const string& uci, char movePiece, char capturePiece)
        : move(uciToMove(uci)), movePiece(movePiece), capturePiece(capturePiece), isCapture(true) {}
};

struct MoveList {
    static constexpr int MAX_MOVES = 256;
    Move moves[MAX_MOVES];
    int count = 0;

    void add(const Move& m) { moves[count++] = m; }

    template<typename... Args>
    void emplace_back(Args&&... args) { moves[count++] = Move(std::forward<Args>(args)...); }

    Move& operator[](int i) { return moves[i]; }
    const Move& operator[](int i) const { return moves[i]; }
    int size() const { return count; }
    bool empty() const { return count == 0; }
    void clear() { count = 0; }
    Move* begin() { return moves; }
    Move* end() { return moves + count; }
    const Move* begin() const { return moves; }
    const Move* end() const { return moves + count; }
    void push_back(const Move& m) { moves[count++] = m; }
};
