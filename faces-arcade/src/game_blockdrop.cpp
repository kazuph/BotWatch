// Falling-block puzzle written for FACES ARCADE (original graphics, no third-party artwork).
#include "common.h"

namespace {
const int kCols = 10, kRows = 20, kCell = 11;
const int kFieldX = (320 - kCols * kCell) / 2, kFieldY = 10;
const char* const kKey = "blocks";

// 4x4 masks (bit 15 = top-left) for each piece and rotation.
const uint16_t kShapes[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, {0x8E00, 0x6440, 0x0E20, 0x44C0},
    {0x2E00, 0x4460, 0x0E80, 0xC440}, {0x6600, 0x6600, 0x6600, 0x6600},
    {0x6C00, 0x4620, 0x06C0, 0x8C40}, {0x4E00, 0x4640, 0x0E40, 0x4C40},
    {0xC600, 0x2640, 0x0C60, 0x4C80}};
const uint16_t kFill[8]  = {0,
                            rgb(60, 210, 235), rgb(70, 110, 240), rgb(245, 150, 50), rgb(245, 215, 60),
                            rgb(80, 210, 100), rgb(180, 90, 230), rgb(235, 70, 80)};
const uint16_t kLight[8] = {0,
                            rgb(170, 245, 255), rgb(160, 185, 255), rgb(255, 205, 140), rgb(255, 245, 165),
                            rgb(170, 245, 180), rgb(225, 175, 255), rgb(255, 165, 170)};
const uint16_t kDark[8]  = {0,
                            rgb(25, 130, 150), rgb(35, 60, 150), rgb(160, 90, 25), rgb(165, 140, 25),
                            rgb(40, 130, 60), rgb(110, 50, 150), rgb(150, 35, 45)};

enum State { TITLE, PLAY, PAUSE, CLEARING, OVER };
State state;
int8_t board[kRows][kCols];
int piece, rot, px, py, nextPiece;
uint8_t bag[7];
int bagLeft;
int score, lines, level, lockMoves;
uint32_t lastFall, lockAt, clearAt, moveAt, overAt;
bool newBest;
bool fullRow[kRows];
M5Canvas field(&M5.Display);
bool fieldDirty, hudDirty;

inline bool cellOf(uint16_t mask, int cx, int cy) { return mask & (0x8000 >> (cy * 4 + cx)); }

bool fits(int p, int r, int x, int y) {
    const uint16_t m = kShapes[p][r];
    for (int cy = 0; cy < 4; ++cy)
        for (int cx = 0; cx < 4; ++cx) {
            if (!cellOf(m, cx, cy)) continue;
            const int bx = x + cx, by = y + cy;
            if (bx < 0 || bx >= kCols || by >= kRows) return false;
            if (by >= 0 && board[by][bx]) return false;
        }
    return true;
}

int takeFromBag() {
    if (bagLeft == 0) {
        for (int i = 0; i < 7; ++i) bag[i] = i;
        for (int i = 6; i > 0; --i) {
            const int j = arcadeRandom(i + 1);
            const uint8_t t = bag[i];
            bag[i]          = bag[j];
            bag[j]          = t;
        }
        bagLeft = 7;
    }
    return bag[--bagLeft];
}

uint32_t fallMs() { return max(70, 800 - (level - 1) * 75); }

template <typename D>
void drawBlock(D& d, int x, int y, int size, int color) {
    d.fillRect(x, y, size, size, kFill[color]);
    d.drawFastHLine(x, y, size, kLight[color]);
    d.drawFastVLine(x, y, size, kLight[color]);
    d.drawFastHLine(x, y + size - 1, size, kDark[color]);
    d.drawFastVLine(x + size - 1, y, size, kDark[color]);
}

void renderField() {
    field.fillSprite(rgb(10, 12, 26));
    for (int x = 1; x < kCols; ++x) field.drawFastVLine(x * kCell, 0, kRows * kCell, rgb(20, 24, 46));
    for (int y = 1; y < kRows; ++y) field.drawFastHLine(0, y * kCell, kCols * kCell, rgb(20, 24, 46));
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kCols; ++x) {
            if (state == CLEARING && fullRow[y]) {
                field.fillRect(x * kCell, y * kCell, kCell, kCell, TFT_WHITE);
            } else if (board[y][x]) {
                drawBlock(field, x * kCell, y * kCell, kCell, board[y][x]);
            }
        }
    if (state == PLAY || state == PAUSE) {
        int gy = py;
        while (fits(piece, rot, px, gy + 1)) ++gy;
        const uint16_t m = kShapes[piece][rot];
        for (int cy = 0; cy < 4; ++cy)
            for (int cx = 0; cx < 4; ++cx) {
                if (!cellOf(m, cx, cy)) continue;
                const int sx = (px + cx) * kCell;
                if (gy + cy >= 0) field.drawRect(sx + 1, (gy + cy) * kCell + 1, kCell - 2, kCell - 2, kDark[piece + 1]);
                if (py + cy >= 0) drawBlock(field, sx, (py + cy) * kCell, kCell, piece + 1);
            }
    }
    field.pushSprite(kFieldX, kFieldY);
}

void stat(int y, const char* label, int value, uint16_t color) {
    auto& d = M5.Display;
    d.fillRect(2, y, 96, 56, pal::kBg);
    text(d, label, 4, y, pal::kDim, F24);
    char b[12];
    snprintf(b, sizeof(b), "%d", value);
    text(d, b, 4, y + 24, color, F32);
}

void drawNext() {
    auto& d = M5.Display;
    const int x = 226, y = 36, w = 88, h = 66;
    d.fillRoundRect(x, y, w, h, 6, pal::kPanel);
    const uint16_t m = kShapes[nextPiece][0];
    const int sz     = 14;
    int minX = 4, maxX = 0, minY = 4, maxY = 0;
    for (int cy = 0; cy < 4; ++cy)
        for (int cx = 0; cx < 4; ++cx)
            if (cellOf(m, cx, cy)) {
                minX = min(minX, cx); maxX = max(maxX, cx);
                minY = min(minY, cy); maxY = max(maxY, cy);
            }
    const int ox = x + (w - (maxX - minX + 1) * sz) / 2 - minX * sz;
    const int oy = y + (h - (maxY - minY + 1) * sz) / 2 - minY * sz;
    for (int cy = 0; cy < 4; ++cy)
        for (int cx = 0; cx < 4; ++cx)
            if (cellOf(m, cx, cy)) drawBlock(d, ox + cx * sz, oy + cy * sz, sz, nextPiece + 1);
}

void drawHud() {
    stat(4, "点数", score, pal::kText);
    stat(62, "ライン", lines, pal::kText);
    stat(120, "レベル", level, pal::kAccent);
    stat(178, "最高", max(bestScore(kKey), score), pal::kCyan);
    drawNext();
}

void drawFrame() {
    auto& d = M5.Display;
    d.fillScreen(pal::kBg);
    d.drawRoundRect(kFieldX - 3, kFieldY - 3, kCols * kCell + 6, kRows * kCell + 6, 3, pal::kLine);
    d.drawRoundRect(kFieldX - 2, kFieldY - 2, kCols * kCell + 4, kRows * kCell + 4, 2, pal::kLine);
    text(d, "つぎ", 226, 8, pal::kDim, F24);
    text(d, "A:回す", 226, 116, pal::kDim, F24);
    text(d, "上:落下", 226, 148, pal::kDim, F24);
    text(d, "START", 226, 180, pal::kDim, F24);
    text(d, ":止める", 226, 208, pal::kDim, F24);
    drawHud();
    fieldDirty = true;
}

void gameOver() {
    state   = OVER;
    overAt  = lgfx::millis();
    newBest = submitScore(kKey, score);
    renderField();
    char b[24];
    snprintf(b, sizeof(b), "点数 %d", score);
    dialog("ゲームオーバー", pal::kRed, b, newBest ? "さいこう更新！" : nullptr, "A:もう一度");
    printf("BLOCKS gameover score=%d lines=%d level=%d\n", score, lines, level);
}

void spawn() {
    piece     = nextPiece;
    nextPiece = takeFromBag();
    rot       = 0;
    px        = 3;
    py        = -1;
    lockMoves = 0;
    lockAt    = 0;
    lastFall  = lgfx::millis();
    fieldDirty = hudDirty = true;
    if (!fits(piece, rot, px, py)) gameOver();
}

void lockPiece() {
    const uint16_t m = kShapes[piece][rot];
    for (int cy = 0; cy < 4; ++cy)
        for (int cx = 0; cx < 4; ++cx)
            if (cellOf(m, cx, cy) && py + cy >= 0) board[py + cy][px + cx] = piece + 1;
    int cleared = 0;
    for (int y = 0; y < kRows; ++y) {
        fullRow[y] = true;
        for (int x = 0; x < kCols; ++x)
            if (!board[y][x]) fullRow[y] = false;
        if (fullRow[y]) ++cleared;
    }
    if (cleared) {
        static const int kPoints[5] = {0, 100, 300, 500, 800};
        score += kPoints[cleared] * level;
        lines += cleared;
        level = lines / 10 + 1;
        state = CLEARING;
        clearAt = lgfx::millis();
        fieldDirty = hudDirty = true;
        printf("BLOCKS cleared=%d score=%d lines=%d\n", cleared, score, lines);
    } else {
        spawn();
    }
}

void collapse() {
    int w = kRows - 1;
    for (int y = kRows - 1; y >= 0; --y) {
        if (fullRow[y]) continue;
        if (w != y) memcpy(board[w], board[y], sizeof(board[0]));
        --w;
    }
    for (; w >= 0; --w) memset(board[w], 0, sizeof(board[0]));
    memset(fullRow, 0, sizeof(fullRow));
    state = PLAY;
    spawn();
}

void tryMove(int dx) {
    if (fits(piece, rot, px + dx, py)) {
        px += dx;
        fieldDirty = true;
        if (lockAt && lockMoves++ < 12) lockAt = lgfx::millis();
    }
}

void tryRotate(int dir) {
    const int nr = (rot + dir + 4) % 4;
    static const int8_t kicks[][2] = {{0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, -1}, {-1, -1}, {1, -1}};
    for (auto& k : kicks)
        if (fits(piece, nr, px + k[0], py + k[1])) {
            rot = nr;
            px += k[0];
            py += k[1];
            fieldDirty = true;
            if (lockAt && lockMoves++ < 12) lockAt = lgfx::millis();
            return;
        }
}

void startGame() {
    memset(board, 0, sizeof(board));
    memset(fullRow, 0, sizeof(fullRow));
    score = lines = 0;
    level   = 1;
    bagLeft = 0;
    nextPiece = takeFromBag();
    state = PLAY;
    drawFrame();
    spawn();
    puts("BLOCKS start");
}

void tickPlay() {
    const uint32_t now = lgfx::millis();
    if (pad_just & kStart) {
        state = PAUSE;
        fieldDirty = true;
        renderField();
        dialog("一時停止", pal::kAccent, "START:つづける", "SELECT:もどる");
        return;
    }
    if (pad_just & kLeft) { tryMove(-1); moveAt = now + 160; }
    if (pad_just & kRight) { tryMove(1); moveAt = now + 160; }
    if ((pad_now & (kLeft | kRight)) && !(pad_just & (kLeft | kRight)) && now >= moveAt) {
        tryMove((pad_now & kLeft) ? -1 : 1);
        moveAt = now + 45;
    }
    if (pad_just & kA) tryRotate(1);
    if (pad_just & kB) tryRotate(-1);
    if (pad_just & kUp) {
        while (fits(piece, rot, px, py + 1)) { ++py; score += 2; }
        hudDirty = true;
        lockPiece();
        return;
    }
    const bool grounded = !fits(piece, rot, px, py + 1);
    if (grounded) {
        if (!lockAt) lockAt = now;
        if (now - lockAt >= 400) { lockPiece(); return; }
    } else {
        lockAt = 0;
        const bool soft = pad_now & kDown;
        if (now - lastFall >= (soft ? 40 : fallMs())) {
            ++py;
            lastFall = now;
            if (soft) { ++score; hudDirty = true; }
            fieldDirty = true;
        }
    }
}

void enter() {
    if (!field.getBuffer()) {
        field.setColorDepth(16);
        field.createSprite(kCols * kCell, kRows * kCell);
    }
    state = TITLE;
    auto& d = M5.Display;
    d.fillScreen(pal::kBg);
    text(d, "ブロック", 160, 6, pal::kAccent, F40, top_center);
    char b[20];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(d, b, 160, 54, pal::kCyan, F32, top_center);
    text(d, "A:はじめる", 160, 96, pal::kText, F32, top_center);
    text(d, "左右:移動 下:速く", 160, 142, pal::kDim, F24, top_center);
    text(d, "A/B:回す 上:一気に", 160, 172, pal::kDim, F24, top_center);
    text(d, "SELECT:もどる", 160, 206, pal::kDim, F24, top_center);
}

void tick() {
    switch (state) {
        case TITLE:
            if ((pad_just & (kA | kStart)) || tap_just) startGame();
            return;
        case PAUSE:
            if ((pad_just & kStart) || tap_just) {
                state = PLAY;
                lastFall = lgfx::millis();
                drawFrame();
            }
            return;
        case OVER:
            if (((pad_just & (kA | kStart)) || tap_just) && lgfx::millis() - overAt > 500) startGame();
            return;
        case CLEARING:
            if (fieldDirty) { renderField(); fieldDirty = false; }
            if (hudDirty) { drawHud(); hudDirty = false; }
            if (lgfx::millis() - clearAt >= 180) collapse();
            return;
        case PLAY:
            tickPlay();
            break;
    }
    if (state == PLAY || state == CLEARING) {
        if (fieldDirty) { renderField(); fieldDirty = false; }
        if (hudDirty) { drawHud(); hudDirty = false; }
    }
}

void icon(int x, int y) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    static const int8_t cells[][3] = {{1, 1, 6}, {2, 1, 6}, {3, 1, 6}, {2, 2, 6}, {0, 3, 1}, {1, 3, 1}, {2, 3, 1}, {3, 3, 1}, {0, 2, 3}};
    for (auto& c : cells) drawBlock(d, x + 2 + c[0] * 9, y + 2 + c[1] * 9, 9, c[2]);
}
}  // namespace

const Game kBlockDrop = {"ブロック", kKey, enter, tick, icon, nullptr};
