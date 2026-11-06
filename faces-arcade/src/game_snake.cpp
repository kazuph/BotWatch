// Snake written for FACES ARCADE.
#include "common.h"

namespace {
const int kCols = 20, kRows = 13, kCell = 16, kTop = 32;
const char* const kKey = "snake";
const uint16_t kBoardA = rgb(24, 44, 40), kBoardB = rgb(30, 54, 48);

enum State { TITLE, PLAY, PAUSE, OVER };
State state;
struct P { int8_t x, y; };
P body[kCols * kRows];  // ring buffer, head at index headIdx
int headIdx, length;
int8_t dirX, dirY;
int8_t queueX[2], queueY[2];
int queued;
P apple;
int score;
uint32_t lastStep, overAt;

P at(int fromHead) { return body[(headIdx - fromHead + kCols * kRows) % (kCols * kRows)]; }

bool onSnake(int x, int y) {
    for (int i = 0; i < length; ++i) {
        const P p = at(i);
        if (p.x == x && p.y == y) return true;
    }
    return false;
}

void drawGround(int x, int y) {
    M5.Display.fillRect(x * kCell, kTop + y * kCell, kCell, kCell, ((x + y) & 1) ? kBoardA : kBoardB);
}

uint16_t bodyColor(int i) {
    const int t = min(i * 6, 110);
    return rgb(90, 225 - t, 110 - t / 2);
}

void drawBody(P p, int i) {
    M5.Display.fillRoundRect(p.x * kCell + 1, kTop + p.y * kCell + 1, kCell - 2, kCell - 2, 4, bodyColor(i));
}

void drawHead() {
    auto& d       = M5.Display;
    const P p     = at(0);
    const int x   = p.x * kCell, y = kTop + p.y * kCell;
    d.fillRoundRect(x, y, kCell, kCell, 5, rgb(150, 240, 130));
    const int cx = x + 8 + dirX * 3, cy = y + 8 + dirY * 3;
    const int ox = dirY ? 4 : 0, oy = dirX ? 4 : 0;
    d.fillCircle(cx - ox, cy - oy, 2, TFT_WHITE);
    d.fillCircle(cx + ox, cy + oy, 2, TFT_WHITE);
    d.drawPixel(cx - ox + dirX, cy - oy + dirY, TFT_BLACK);
    d.drawPixel(cx + ox + dirX, cy + oy + dirY, TFT_BLACK);
}

void drawApple() {
    auto& d     = M5.Display;
    const int x = apple.x * kCell, y = kTop + apple.y * kCell;
    d.fillCircle(x + 8, y + 9, 6, pal::kRed);
    d.fillCircle(x + 6, y + 7, 1, rgb(255, 190, 190));
    d.drawFastVLine(x + 8, y + 1, 3, rgb(120, 80, 40));
    d.fillRect(x + 9, y + 1, 3, 2, pal::kGreen);
}

void placeApple() {
    do {
        apple.x = arcadeRandom(kCols);
        apple.y = arcadeRandom(kRows);
    } while (onSnake(apple.x, apple.y));
    drawApple();
}

void drawHud() {
    auto& d = M5.Display;
    d.fillRect(0, 0, 320, kTop, pal::kPanel);
    d.drawFastHLine(0, kTop - 1, 320, pal::kLine);
    char b[24];
    snprintf(b, sizeof(b), "点数 %d", score);
    text(d, b, 8, 4, pal::kText, F24);
    snprintf(b, sizeof(b), "最高 %d", max(bestScore(kKey), score));
    text(d, b, 312, 4, pal::kCyan, F24, top_right);
}

void drawAll() {
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kCols; ++x) drawGround(x, y);
    for (int i = length - 1; i > 0; --i) drawBody(at(i), i);
    drawHead();
    drawApple();
    drawHud();
}

void startGame() {
    score    = 0;
    length   = 3;
    headIdx  = 2;
    body[0]  = {7, 6};
    body[1]  = {8, 6};
    body[2]  = {9, 6};
    dirX     = 1;
    dirY     = 0;
    queued   = 0;
    state    = PLAY;
    lastStep = lgfx::millis();
    do {
        apple.x = arcadeRandom(kCols);
        apple.y = arcadeRandom(kRows);
    } while (onSnake(apple.x, apple.y));
    drawAll();
    puts("SNAKE start");
}

void gameOver() {
    state   = OVER;
    overAt  = lgfx::millis();
    const bool best = submitScore(kKey, score);
    char b[24];
    snprintf(b, sizeof(b), "点数 %d", score);
    dialog("ゲームオーバー", pal::kRed, b, best ? "さいこう更新！" : nullptr, "A:もう一度");
    printf("SNAKE gameover score=%d\n", score);
}

void queueDir(int x, int y) {
    const int lastX = queued ? queueX[queued - 1] : dirX;
    const int lastY = queued ? queueY[queued - 1] : dirY;
    if ((x == lastX && y == lastY) || (x == -lastX && y == -lastY) || queued == 2) return;
    queueX[queued] = x;
    queueY[queued] = y;
    ++queued;
}

void step() {
    if (queued) {
        dirX = queueX[0];
        dirY = queueY[0];
        queueX[0] = queueX[1];
        queueY[0] = queueY[1];
        --queued;
    }
    const P head = at(0);
    const P next = {int8_t(head.x + dirX), int8_t(head.y + dirY)};
    if (next.x < 0 || next.x >= kCols || next.y < 0 || next.y >= kRows) { gameOver(); return; }
    const bool eating = next.x == apple.x && next.y == apple.y;
    const P tail      = at(length - 1);
    // The tail cell empties on this step, so moving into it is allowed.
    for (int i = 0; i < length - (eating ? 0 : 1); ++i) {
        const P p = at(i);
        if (p.x == next.x && p.y == next.y) { gameOver(); return; }
    }
    if (!eating) {
        drawGround(tail.x, tail.y);
    }
    headIdx = (headIdx + 1) % (kCols * kRows);
    body[headIdx] = next;
    if (eating) {
        ++length;
        ++score;
        drawHud();
        printf("SNAKE score=%d\n", score);
        placeApple();
    }
    // Recolor the segments right behind the head; the rest keep a close shade.
    for (int i = min(length - 1, 3); i > 0; --i) drawBody(at(i), i);
    drawHead();
}

uint32_t stepMs() { return max(75, 170 - score * 4); }

void enter() {
    state = TITLE;
    auto& d = M5.Display;
    d.fillScreen(kBoardA);
    for (int i = 0; i < 18; ++i) {
        const int x = 16 + i * 15;
        const int y = 16 + int(sin(i * 0.7f) * 8);
        d.fillRoundRect(x, y, 15, 15, 4, bodyColor(18 - i));
    }
    d.fillCircle(296, 26, 7, pal::kRed);
    text(d, "へび", 160, 40, pal::kGreen, F40, top_center);
    char b[20];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(d, b, 160, 88, pal::kCyan, F32, top_center);
    text(d, "A:はじめる", 160, 128, pal::kText, F32, top_center);
    text(d, "十字キーで動かす", 160, 172, pal::kDim, F24, top_center);
    text(d, "SELECT:もどる", 160, 206, pal::kDim, F24, top_center);
}

void tick() {
    switch (state) {
        case TITLE:
            if ((pad_just & (kA | kStart)) || tap_just) startGame();
            break;
        case PAUSE:
            if ((pad_just & kStart) || tap_just) {
                state = PLAY;
                lastStep = lgfx::millis();
                drawAll();
            }
            break;
        case OVER:
            if (((pad_just & (kA | kStart)) || tap_just) && lgfx::millis() - overAt > 500) startGame();
            break;
        case PLAY:
            if (pad_just & kStart) {
                state = PAUSE;
                dialog("一時停止", pal::kAccent, "START:つづける", "SELECT:もどる");
                break;
            }
            if (pad_just & kUp) queueDir(0, -1);
            if (pad_just & kDown) queueDir(0, 1);
            if (pad_just & kLeft) queueDir(-1, 0);
            if (pad_just & kRight) queueDir(1, 0);
            if (lgfx::millis() - lastStep >= stepMs()) {
                lastStep = lgfx::millis();
                step();
            }
            break;
    }
}

void icon(int x, int y) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, 40, 40, 6, kBoardA);
    d.fillRoundRect(x + 5, y + 24, 8, 8, 2, bodyColor(6));
    d.fillRoundRect(x + 13, y + 24, 8, 8, 2, bodyColor(4));
    d.fillRoundRect(x + 13, y + 16, 8, 8, 2, bodyColor(2));
    d.fillRoundRect(x + 21, y + 16, 8, 8, 3, rgb(150, 240, 130));
    d.fillCircle(x + 33, y + 9, 4, pal::kRed);
}
}  // namespace

const Game kSnake = {"へび", kKey, enter, tick, icon, nullptr};
