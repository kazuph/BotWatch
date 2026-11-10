// 3D space shooter for FACES ARCADE (own graphics).
// Cockpit view: stars and wireframe enemies fly toward the player in perspective.
// The D-pad moves the sight, A fires. An enemy that reaches the cockpit costs one shield.
#include <algorithm>
#include "common.h"

namespace {
const int kW = 320, kH = 240, kCx = kW / 2, kCy = kH / 2 + 8;
const float kFocal = 220.0f, kNear = 18.0f, kFar = 420.0f;
const uint32_t kStepMs = 33;  // about 30 steps per second; drawing is about 25 fps
const char* const kKey = "space3d";
const int kStars = 70, kEnemies = 5, kBits = 40, kShields = 3;

enum State { TITLE, PLAY, OVER };
State state;
struct Star { float x, y, z; };
struct Enemy { bool alive; int shape; float x, y, z, vx, vy, spin, angle; };
struct Bit { bool alive; float x, y, vx, vy; int life; uint16_t color; };
Star stars[kStars];
Enemy enemies[kEnemies];
Bit bits[kBits];
float sightX, sightY;
int score, shields, laserFrames, hurtFrames, level;
uint32_t lastStep, overAt, spawnAt, frames;

// Two wireframe shapes: an octahedron and a "dart" (a flattened pyramid).
struct V3 { float x, y, z; };
const V3 kOcta[6] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
const uint8_t kOctaEdges[12][2] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {1, 2}, {2, 3}, {3, 4}, {4, 1}};
const V3 kDart[5] = {{0, 0, -1.4f}, {-1.2f, 0.4f, 0.6f}, {1.2f, 0.4f, 0.6f}, {0, -0.6f, 0.6f}, {0, 0.5f, 0.2f}};
const uint8_t kDartEdges[8][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {2, 3}, {3, 1}, {1, 4}, {2, 4}};

bool project(float x, float y, float z, int& sx, int& sy) {
    if (z < 1.0f) return false;
    sx = kCx + int(x * kFocal / z);
    sy = kCy + int(y * kFocal / z);
    return true;
}

void resetStar(Star& s, bool anywhere) {
    s.x = arcadeRandom(-400, 400);
    s.y = arcadeRandom(-300, 300);
    s.z = anywhere ? arcadeRandom(20, int(kFar)) : kFar;
}

void spawnEnemy() {
    for (auto& e : enemies) {
        if (e.alive) continue;
        e.alive = true;
        e.shape = arcadeRandom(2);
        e.x = arcadeRandom(-90, 90);
        e.y = arcadeRandom(-60, 60);
        e.z = kFar;
        e.vx = (arcadeRandom(-10, 11)) / 30.0f;
        e.vy = (arcadeRandom(-10, 11)) / 30.0f;
        e.spin = (arcadeRandom(2) ? 1 : -1) * (0.05f + arcadeRandom(6) / 100.0f);
        e.angle = 0;
        return;
    }
}

void burst(int sx, int sy, uint16_t color) {
    int made = 0;
    for (auto& b : bits) {
        if (b.alive) continue;
        const float a = arcadeRandom(628) / 100.0f, sp = 2.0f + arcadeRandom(40) / 10.0f;
        b = {true, float(sx), float(sy), cosf(a) * sp, sinf(a) * sp, 12 + int(arcadeRandom(8)), color};
        if (++made >= 14) break;
    }
}

void resetRound() {
    for (auto& s : stars) resetStar(s, true);
    for (auto& e : enemies) e.alive = false;
    for (auto& b : bits) b.alive = false;
    sightX = kCx;
    sightY = kCy;
    score = 0;
    shields = kShields;
    level = 1;
    laserFrames = hurtFrames = 0;
    spawnAt = lgfx::millis() + 800;
}

uint16_t enemyColor(const Enemy& e) { return e.shape ? rgb(255, 120, 200) : rgb(120, 255, 170); }

void drawEnemy(const Enemy& e) {
    const float size = e.shape ? 16.0f : 14.0f;
    const float c = cosf(e.angle), s = sinf(e.angle);
    const V3* verts = e.shape ? kDart : kOcta;
    const int nv = e.shape ? 5 : 6;
    int px[6], py[6];
    bool ok[6];
    for (int i = 0; i < nv; ++i) {
        // Spin around the vertical axis, then tilt a little around the horizontal axis.
        const float x = verts[i].x * c - verts[i].z * s, z0 = verts[i].x * s + verts[i].z * c;
        const float y = verts[i].y * 0.92f - z0 * 0.38f, z = verts[i].y * 0.38f + z0 * 0.92f;
        ok[i] = project(e.x + x * size, e.y + y * size, e.z + z * size, px[i], py[i]);
    }
    // Farther enemies are dimmer, so depth is easy to read.
    const float t = 1.0f - min(1.0f, (e.z - kNear) / (kFar - kNear)) * 0.7f;
    const uint16_t base = enemyColor(e);
    const uint16_t col = rgb(int(((base >> 11) & 31) * 8 * t), int(((base >> 5) & 63) * 4 * t), int((base & 31) * 8 * t));
    const int ne = e.shape ? 8 : 12;
    for (int i = 0; i < ne; ++i) {
        const int a = e.shape ? kDartEdges[i][0] : kOctaEdges[i][0], b = e.shape ? kDartEdges[i][1] : kOctaEdges[i][1];
        if (ok[a] && ok[b]) uiFrame.drawLine(px[a], py[a], px[b], py[b], col);
    }
}

void drawHud() {
    auto& d = uiFrame;
    char b[24];
    snprintf(b, sizeof(b), "%d", score);
    text(d, b, 8, 4, pal::kText, F24, top_left);
    for (int i = 0; i < kShields; ++i)
        d.fillSmoothRoundRect(316 - (kShields - i) * 26, 10, 20, 12, 4, i < shields ? pal::kCyan : rgb(38, 57, 90));
}

void render(bool showSight = true, bool push = true) {
    auto& d = uiFrame;
    d.fillScreen(rgb(4, 6, 16));
    for (const auto& s : stars) {
        int sx, sy;
        if (!project(s.x, s.y, s.z, sx, sy)) continue;
        const int bright = 255 - int(s.z * 200 / kFar);
        const uint16_t c = rgb(bright, bright, min(255, bright + 30));
        if (s.z < 90) d.fillRect(sx, sy, 2, 2, c);
        else d.drawPixel(sx, sy, c);
    }
    // Draw far enemies first so near ones are on top.
    int order[kEnemies];
    for (int i = 0; i < kEnemies; ++i) order[i] = i;
    for (int i = 0; i < kEnemies; ++i)
        for (int j = i + 1; j < kEnemies; ++j)
            if (enemies[order[j]].z > enemies[order[i]].z) std::swap(order[i], order[j]);
    for (int i : order)
        if (enemies[i].alive) drawEnemy(enemies[i]);
    for (const auto& b : bits)
        if (b.alive) d.fillRect(int(b.x), int(b.y), 3, 3, b.color);
    if (laserFrames > 0) {
        const uint16_t c = rgb(255, 230, 120);
        d.drawLine(20, kH - 4, int(sightX), int(sightY), c);
        d.drawLine(kW - 20, kH - 4, int(sightX), int(sightY), c);
        d.drawLine(21, kH - 4, int(sightX) + 1, int(sightY), c);
        d.drawLine(kW - 21, kH - 4, int(sightX) - 1, int(sightY), c);
    }
    if (showSight) {
        const int x = int(sightX), y = int(sightY);
        const uint16_t sc = rgb(255, 210, 90);
        d.drawCircle(x, y, 12, sc);
        d.drawFastHLine(x - 20, y, 10, sc);
        d.drawFastHLine(x + 11, y, 10, sc);
        d.drawFastVLine(x, y - 20, 10, sc);
        d.drawFastVLine(x, y + 11, 10, sc);
    }
    // Cockpit frame. A hit tints it orange for a moment (steady colour, no flashing).
    const uint16_t frameColor = hurtFrames > 0 ? rgb(200, 90, 40) : rgb(30, 50, 80);
    d.fillTriangle(0, kH, 0, kH - 40, 70, kH, frameColor);
    d.fillTriangle(kW, kH, kW, kH - 40, kW - 70, kH, frameColor);
    drawHud();
    if (push) d.pushSprite(0, 0);  // title and game-over draw their text first, then push once
}

void fire() {
    laserFrames = 3;
    soundPlayVolume(0.5f, 0, 60, 96);  // the shot plays often; half amplitude keeps it from being loud
    int best = -1;
    float bestZ = 1e9f;
    for (int i = 0; i < kEnemies; ++i) {
        const auto& e = enemies[i];
        int sx, sy;
        if (!e.alive || !project(e.x, e.y, e.z, sx, sy)) continue;
        const float r = 14.0f * kFocal / e.z + 8.0f;  // enemy size on screen plus a small margin
        const float dx = sx - sightX, dy = sy - sightY;
        if (dx * dx + dy * dy <= r * r && e.z < bestZ) { best = i; bestZ = e.z; }
    }
    if (best < 0) return;
    auto& e = enemies[best];
    int sx, sy;
    project(e.x, e.y, e.z, sx, sy);
    burst(sx, sy, enemyColor(e));
    e.alive = false;
    score += 10 + int((kFar - e.z) / 40);  // farther hits are worth less, closer ones more
    level = 1 + score / 150;
    soundPlay(30, 160, 52, 59);
    printf("SPACE hit score=%d\n", score);
}

void gameOver() {
    state = OVER;
    overAt = lgfx::millis();
    const bool best = submitScore(kKey, score);
    render(false, false);
    auto& d = uiFrame;
    d.fillSmoothRoundRect(40, 60, 240, 120, 18, rgb(12, 18, 40));
    d.drawRoundRect(40, 60, 240, 120, 18, pal::kCyan);
    text(d, "ゲームオーバー", 160, 74, pal::kText, F32, top_center);
    char b[32];
    snprintf(b, sizeof(b), best ? "NEW BEST %d" : "スコア %d", score);
    text(d, b, 160, 114, best ? pal::kAccent : pal::kText, F24, top_center);
    text(d, "A:もういちど", 160, 146, pal::kDim, F24, top_center);
    d.pushSprite(0, 0);
    soundPlay(0, 200, 64);
    soundPlay(220, 200, 60);
    soundPlay(440, 400, 55);
    printf("SPACE over score=%d\n", score);
}

void step() {
    ++frames;
    const float speed = 3.0f + level * 0.6f;
    for (auto& s : stars) {
        s.z -= speed * 2.2f;
        if (s.z < 4) resetStar(s, false);
    }
    const float move = 5.0f;
    if (pad_now & kLeft) sightX -= move;
    if (pad_now & kRight) sightX += move;
    if (pad_now & kUp) sightY -= move;
    if (pad_now & kDown) sightY += move;
    sightX = std::min(std::max(sightX, 16.0f), kW - 16.0f);
    sightY = std::min(std::max(sightY, 30.0f), kH - 16.0f);
    if (laserFrames > 0) --laserFrames;
    if (hurtFrames > 0) --hurtFrames;
    for (auto& e : enemies) {
        if (!e.alive) continue;
        e.z -= speed;
        e.x += e.vx;
        e.y += e.vy;
        e.angle += e.spin;
        if (e.z < kNear) {
            e.alive = false;
            --shields;
            hurtFrames = 12;
            soundPlay(0, 250, 40, 41);
            printf("SPACE hurt shields=%d\n", shields);
        }
    }
    for (auto& b : bits) {
        if (!b.alive) continue;
        b.x += b.vx;
        b.y += b.vy;
        if (--b.life <= 0) b.alive = false;
    }
    const uint32_t now = lgfx::millis();
    if (int32_t(now - spawnAt) >= 0) {
        spawnEnemy();
        spawnAt = now + max(450, 1500 - level * 150);
    }
}

void enter() {
    soundBegin();
    resetRound();
    state = TITLE;
    render(false, false);
    auto& d = uiFrame;
    text(d, "3D宇宙", 160, 50, pal::kText, F40, top_center);
    char b[24];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(d, b, 160, 104, pal::kAccent, F24, top_center);
    text(d, "十字キー:ねらう A:うつ", 160, 140, pal::kText, F24, top_center);
    text(d, "A:はじめる", 160, 178, pal::kCyan, F24, top_center);
    d.pushSprite(0, 0);
}

void tick() {
    if (state == TITLE || state == OVER) {
        if (((pad_just & (kA | kStart)) || tap_just) && (state == TITLE || lgfx::millis() - overAt > 600)) {
            resetRound();
            state = PLAY;
            lastStep = lgfx::millis();
            puts("SPACE start");
        }
        return;
    }
    if (pad_just & kA) fire();
    if (tap_just) {  // tapping aims and fires at once
        sightX = tap_x;
        sightY = tap_y;
        fire();
    }
    const uint32_t now = lgfx::millis();
    if (now - lastStep < kStepMs) return;
    int steps = min<uint32_t>((now - lastStep) / kStepMs, 3);
    lastStep = now;
    while (steps-- > 0) step();
    if (shields <= 0) {
        gameOver();
        return;
    }
    render();
}

void leave() { soundEnd(); }

void icon(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, rgb(4, 6, 16));
    d.drawPixel(x + 6, y + 8, TFT_WHITE);
    d.drawPixel(x + 33, y + 6, TFT_WHITE);
    d.drawPixel(x + 30, y + 33, TFT_WHITE);
    const uint16_t c = rgb(120, 255, 170);
    // A small wireframe octahedron.
    const int cx = x + 20, cy = y + 19;
    d.drawLine(cx, cy - 12, cx - 11, cy, c);
    d.drawLine(cx, cy - 12, cx + 11, cy, c);
    d.drawLine(cx, cy - 12, cx + 3, cy + 4, c);
    d.drawLine(cx, cy + 12, cx - 11, cy, c);
    d.drawLine(cx, cy + 12, cx + 11, cy, c);
    d.drawLine(cx, cy + 12, cx + 3, cy + 4, c);
    d.drawLine(cx - 11, cy, cx + 3, cy + 4, c);
    d.drawLine(cx + 11, cy, cx + 3, cy + 4, c);
    d.drawCircle(x + 20, y + 19, 16, rgb(255, 210, 90));
}
}  // namespace

const Game kSpace3D = {"3D宇宙", kKey, enter, tick, icon, leave, GameCategory::play, GameFooter::score};
