// 3D space shooter for FACES ARCADE (own graphics).
// Cockpit view: stars and wireframe enemies fly toward the player in perspective.
// The D-pad moves the sight, A fires. Shots use energy, so timing matters.
// Enemies shoot back: an enemy bullet hits only if the sight is on it when it arrives,
// so moving the sight away dodges it. Close kills and kill chains multiply the score.
// Every 12 kills a boss appears; beating it restores one shield.
#include <algorithm>
#include "common.h"

namespace {
const int kW = 320, kH = 240, kCx = kW / 2, kCy = kH / 2 + 8;
const float kFocal = 220.0f, kNear = 18.0f, kFar = 420.0f;
const uint32_t kStepMs = 33;  // about 30 steps per second; drawing is about 25 fps
const char* const kKey = "space3d";
const int kStars = 70, kEnemies = 6, kBits = 48, kShields = 3, kShots = 10, kPops = 4;
const int kWaveKills = 12;
const float kEnergyMax = 100.0f, kShotCost = 22.0f, kEnergyRegen = 1.1f;
const float kCloseZ = 130.0f;  // kills nearer than this are "close" kills

enum State { TITLE, PLAY, OVER };
enum Kind { NORMAL, DODGER, ARMOR, BOSS };
State state;
struct Star { float x, y, z; };
struct Enemy {
    bool alive;
    Kind kind;
    int hp, hitFrames;
    float x, y, z, vx, vy, spin, angle, phase;
    uint32_t fireAt;
};
struct Bit { bool alive; float x, y, vx, vy; int life; uint16_t color; };
struct Shot { bool alive; float x, y, z, vx, vy, vz; };
struct Pop { bool alive; int x, y, life; uint16_t color; char text[24]; };
Star stars[kStars];
Enemy enemies[kEnemies];
Bit bits[kBits];
Shot shots[kShots];
Pop pops[kPops];
float sightX, sightY, energy;
int score, shields, laserFrames, hurtFrames, dryFrames, level, kills, wave, chain, bestChain, shake;
bool bossActive;
uint32_t lastStep, overAt, spawnAt, frames, chainAt;

// Wireframe shapes: octahedron (normal), dart (dodger), cube (armor), and a spiky boss.
struct V3 { float x, y, z; };
const V3 kOcta[6] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
const uint8_t kOctaEdges[12][2] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {1, 2}, {2, 3}, {3, 4}, {4, 1}};
const V3 kDart[5] = {{0, 0, -1.4f}, {-1.2f, 0.4f, 0.6f}, {1.2f, 0.4f, 0.6f}, {0, -0.6f, 0.6f}, {0, 0.5f, 0.2f}};
const uint8_t kDartEdges[8][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {2, 3}, {3, 1}, {1, 4}, {2, 4}};
const V3 kCube[8] = {{-.8f, -.8f, -.8f}, {.8f, -.8f, -.8f}, {.8f, .8f, -.8f}, {-.8f, .8f, -.8f},
                     {-.8f, -.8f, .8f},  {.8f, -.8f, .8f},  {.8f, .8f, .8f},  {-.8f, .8f, .8f}};
const uint8_t kCubeEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
// Boss: an octahedron core with six long spikes.
const V3 kBoss[12] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0, 1, 0},
                      {0, -2.2f, 0}, {2.2f, 0, 0}, {0, 0, 2.2f}, {-2.2f, 0, 0}, {0, 0, -2.2f}, {0, 2.2f, 0}};
const uint8_t kBossEdges[18][2] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {1, 2},
                                   {2, 3}, {3, 4}, {4, 1}, {0, 6}, {1, 7}, {2, 8}, {3, 9}, {4, 10}, {5, 11}};

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

float enemySize(const Enemy& e) { return e.kind == BOSS ? 22.0f : e.kind == DODGER ? 16.0f : 14.0f; }

uint16_t enemyColor(const Enemy& e) {
    switch (e.kind) {
        case DODGER: return rgb(255, 120, 200);
        case ARMOR: return e.hp > 1 ? rgb(120, 170, 255) : rgb(190, 215, 255);
        case BOSS: return rgb(255, 150, 60);
        default: return rgb(120, 255, 170);
    }
}

void spawnEnemy() {
    for (auto& e : enemies) {
        if (e.alive) continue;
        const int roll = arcadeRandom(100);
        // New kinds join as the waves go on.
        Kind kind = NORMAL;
        if (wave >= 1 && roll < 30) kind = DODGER;
        else if (wave >= 2 && roll < 50) kind = ARMOR;
        e = {};
        e.alive = true;
        e.kind = kind;
        e.hp = kind == ARMOR ? 2 : 1;
        e.x = arcadeRandom(-90, 90);
        e.y = arcadeRandom(-60, 60);
        e.z = kFar;
        e.vx = (arcadeRandom(-10, 11)) / 30.0f;
        e.vy = (arcadeRandom(-10, 11)) / 30.0f;
        e.spin = (arcadeRandom(2) ? 1 : -1) * (0.05f + arcadeRandom(6) / 100.0f);
        e.phase = arcadeRandom(628) / 100.0f;
        e.fireAt = lgfx::millis() + 900 + arcadeRandom(1600);
        return;
    }
}

void spawnBoss() {
    for (auto& e : enemies) e.alive = false;
    auto& b = enemies[0];
    b = {};
    b.alive = true;
    b.kind = BOSS;
    b.hp = 10 + wave * 3;
    b.x = 0;
    b.y = -10;
    b.z = kFar;
    b.spin = 0.03f;
    b.fireAt = lgfx::millis() + 2000;
    bossActive = true;
    soundPlay(0, 180, 45);
    soundPlay(200, 180, 44);
    soundPlay(400, 420, 43);
    printf("SPACE boss wave=%d hp=%d\n", wave, b.hp);
}

void burst(int sx, int sy, uint16_t color, int count = 14) {
    int made = 0;
    for (auto& b : bits) {
        if (b.alive) continue;
        const float a = arcadeRandom(628) / 100.0f, sp = 2.0f + arcadeRandom(40) / 10.0f;
        b = {true, float(sx), float(sy), cosf(a) * sp, sinf(a) * sp, 12 + int(arcadeRandom(8)), color};
        if (++made >= count) break;
    }
}

void popup(int x, int y, uint16_t color, const char* s) {
    Pop* slot = &pops[0];
    for (auto& p : pops)
        if (!p.alive || p.life < slot->life) slot = &p;
    slot->alive = true;
    slot->x = std::min(std::max(x, 40), kW - 40);
    slot->y = std::min(std::max(y - 30, 34), kH - 60);
    slot->life = 24;
    slot->color = color;
    strlcpy(slot->text, s, sizeof(slot->text));
}

// Enemy bullet aimed at where the sight is now. It hits only if the sight is still close when it arrives.
void enemyFire(const Enemy& e, float spread) {
    for (auto& s : shots) {
        if (s.alive) continue;
        const float tx = (sightX - kCx) * kNear / kFocal + spread, ty = (sightY - kCy) * kNear / kFocal;
        const float speed = 4.2f + level * 0.35f;
        const float t = (e.z - kNear) / speed;
        s = {true, e.x, e.y, e.z, (tx - e.x) / t, (ty - e.y) / t, -speed};
        return;
    }
}

void resetRound() {
    for (auto& s : stars) resetStar(s, true);
    for (auto& e : enemies) e.alive = false;
    for (auto& b : bits) b.alive = false;
    for (auto& s : shots) s.alive = false;
    for (auto& p : pops) p.alive = false;
    sightX = kCx;
    sightY = kCy;
    energy = kEnergyMax;
    score = 0;
    shields = kShields;
    level = 1;
    kills = wave = chain = bestChain = 0;
    laserFrames = hurtFrames = dryFrames = shake = 0;
    bossActive = false;
    spawnAt = lgfx::millis() + 800;
}

void drawShape(const Enemy& e, const V3* verts, int nv, const uint8_t (*edges)[2], int ne, uint16_t col) {
    const float size = enemySize(e);
    const float c = cosf(e.angle), s = sinf(e.angle);
    int px[12], py[12];
    bool ok[12];
    for (int i = 0; i < nv; ++i) {
        // Spin around the vertical axis, then tilt a little around the horizontal axis.
        const float x = verts[i].x * c - verts[i].z * s, z0 = verts[i].x * s + verts[i].z * c;
        const float y = verts[i].y * 0.92f - z0 * 0.38f, z = verts[i].y * 0.38f + z0 * 0.92f;
        ok[i] = project(e.x + x * size, e.y + y * size, e.z + z * size, px[i], py[i]);
    }
    for (int i = 0; i < ne; ++i) {
        const int a = edges[i][0], b = edges[i][1];
        if (ok[a] && ok[b]) uiFrame.drawLine(px[a] + shake, py[a], px[b] + shake, py[b], col);
    }
}

void drawEnemy(const Enemy& e) {
    // Farther enemies are dimmer, so depth is easy to read. A hit enemy turns white for a moment.
    const float t = 1.0f - min(1.0f, (e.z - kNear) / (kFar - kNear)) * 0.7f;
    const uint16_t base = e.hitFrames > 0 ? rgb(255, 255, 255) : enemyColor(e);
    const uint16_t col = rgb(int(((base >> 11) & 31) * 8 * t), int(((base >> 5) & 63) * 4 * t), int((base & 31) * 8 * t));
    switch (e.kind) {
        case DODGER: drawShape(e, kDart, 5, kDartEdges, 8, col); break;
        case ARMOR: drawShape(e, kCube, 8, kCubeEdges, 12, col); break;
        case BOSS: drawShape(e, kBoss, 12, kBossEdges, 18, col); break;
        default: drawShape(e, kOcta, 6, kOctaEdges, 12, col); break;
    }
}

void drawHud() {
    auto& d = uiFrame;
    char b[24];
    snprintf(b, sizeof(b), "%d", score);
    text(d, b, 8, 4, pal::kText, F24, top_left);
    if (chain >= 2) {
        snprintf(b, sizeof(b), "x%d", min(chain, 8));
        text(d, b, 8, 30, pal::kAccent, F24, top_left);
    }
    for (int i = 0; i < kShields; ++i)
        d.fillSmoothRoundRect(316 - (kShields - i) * 26, 10, 20, 12, 4, i < shields ? pal::kCyan : rgb(38, 57, 90));
    // Energy bar between the cockpit frames. It turns orange while too low to fire.
    const int w = int(140 * energy / kEnergyMax);
    d.fillRoundRect(90, 226, 140, 8, 3, rgb(24, 32, 56));
    d.fillRoundRect(90, 226, max(w, 2), 8, 3, energy >= kShotCost ? pal::kCyan : rgb(230, 130, 40));
    if (bossActive && enemies[0].alive && enemies[0].kind == BOSS) {
        const int maxHp = 10 + wave * 3;
        d.drawRoundRect(100, 12, 120, 10, 3, rgb(255, 150, 60));
        d.fillRoundRect(102, 14, 116 * enemies[0].hp / maxHp, 6, 2, rgb(255, 150, 60));
    }
}

void render(bool showSight = true, bool push = true) {
    auto& d = uiFrame;
    d.fillScreen(rgb(4, 6, 16));
    for (const auto& s : stars) {
        int sx, sy;
        if (!project(s.x, s.y, s.z, sx, sy)) continue;
        const int bright = 255 - int(s.z * 200 / kFar);
        const uint16_t c = rgb(bright, bright, min(255, bright + 30));
        if (s.z < 90) d.fillRect(sx + shake, sy, 2, 2, c);
        else d.drawPixel(sx + shake, sy, c);
    }
    // Draw far enemies first so near ones are on top.
    int order[kEnemies];
    for (int i = 0; i < kEnemies; ++i) order[i] = i;
    for (int i = 0; i < kEnemies; ++i)
        for (int j = i + 1; j < kEnemies; ++j)
            if (enemies[order[j]].z > enemies[order[i]].z) std::swap(order[i], order[j]);
    for (int i : order)
        if (enemies[i].alive) drawEnemy(enemies[i]);
    // Enemy bullets: a glowing ball that grows as it comes closer.
    for (const auto& s : shots) {
        int sx, sy;
        if (!s.alive || !project(s.x, s.y, s.z, sx, sy)) continue;
        const int r = std::min(14, 2 + int(5.0f * kFocal / s.z / 4));
        d.fillCircle(sx + shake, sy, r + 2, rgb(120, 30, 20));
        d.fillCircle(sx + shake, sy, r, rgb(255, 110, 60));
        d.fillCircle(sx + shake, sy, max(1, r / 2), rgb(255, 230, 180));
    }
    for (const auto& b : bits)
        if (b.alive) d.fillRect(int(b.x) + shake, int(b.y), 3, 3, b.color);
    if (laserFrames > 0) {
        const uint16_t c = rgb(255, 230, 120);
        d.drawLine(20, kH - 4, int(sightX), int(sightY), c);
        d.drawLine(kW - 20, kH - 4, int(sightX), int(sightY), c);
        d.drawLine(21, kH - 4, int(sightX) + 1, int(sightY), c);
        d.drawLine(kW - 21, kH - 4, int(sightX) - 1, int(sightY), c);
    }
    for (const auto& p : pops)
        if (p.alive) text(d, p.text, p.x, p.y - (24 - p.life), p.color, F24, top_center);
    if (showSight) {
        const int x = int(sightX), y = int(sightY);
        // The sight dims while there is not enough energy to fire.
        const uint16_t sc = energy >= kShotCost ? rgb(255, 210, 90) : rgb(120, 100, 60);
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

void killEnemy(Enemy& e, int sx, int sy) {
    burst(sx, sy, enemyColor(e), e.kind == BOSS ? 40 : 14);
    e.alive = false;
    const uint32_t now = lgfx::millis();
    chain = (now - chainAt < 2500) ? chain + 1 : 1;
    chainAt = now;
    bestChain = max(bestChain, chain);
    const bool close = e.z < kCloseZ;
    int base = e.kind == BOSS ? 500 : e.kind == ARMOR ? 30 : e.kind == DODGER ? 20 : 10;
    base += int((kFar - e.z) / 40);
    int mult = min(chain, 8) * (close ? 3 : 1);
    const int gained = base * mult;
    score += gained;
    char b[24];
    if (e.kind == BOSS) {
        bossActive = false;
        ++wave;
        shields = min(kShields, shields + 1);
        spawnAt = now + 1500;
        snprintf(b, sizeof(b), "+%d", gained);
        popup(sx, sy, pal::kAccent, b);
        soundPlay(0, 120, 72, 76);
        soundPlay(130, 120, 74, 79);
        soundPlay(260, 400, 79, 84);
        printf("SPACE boss down wave=%d score=%d\n", wave, score);
    } else {
        ++kills;
        if (close) snprintf(b, sizeof(b), "接近x%d", mult);
        else snprintf(b, sizeof(b), "+%d", gained);
        popup(sx, sy, close ? pal::kAccent : pal::kText, b);
        soundPlayVolume(0.7f, 30, 160, 52 + min(chain, 8), 59 + min(chain, 8));
        if (kills % kWaveKills == 0) spawnBoss();
    }
    level = 1 + wave + score / 400;
    printf("SPACE hit kind=%d z=%d chain=%d score=%d\n", int(e.kind), int(e.z), chain, score);
}

void fire() {
    if (energy < kShotCost) {
        dryFrames = 6;
        soundPlayVolume(0.3f, 0, 40, 40);
        return;
    }
    energy -= kShotCost;
    laserFrames = 3;
    soundPlayVolume(0.5f, 0, 60, 96);  // the shot plays often; half amplitude keeps it from being loud
    // Bullets can be shot down too (small reward, and it saves a shield).
    for (auto& s : shots) {
        int sx, sy;
        if (!s.alive || !project(s.x, s.y, s.z, sx, sy)) continue;
        const float r = 6.0f * kFocal / s.z + 8.0f;
        const float dx = sx - sightX, dy = sy - sightY;
        if (dx * dx + dy * dy <= r * r) {
            s.alive = false;
            burst(sx, sy, rgb(255, 150, 80), 6);
            score += 5;
            return;
        }
    }
    int best = -1;
    float bestZ = 1e9f;
    for (int i = 0; i < kEnemies; ++i) {
        const auto& e = enemies[i];
        int sx, sy;
        if (!e.alive || !project(e.x, e.y, e.z, sx, sy)) continue;
        const float r = enemySize(e) * (e.kind == BOSS ? 1.6f : 1.0f) * kFocal / e.z + 8.0f;
        const float dx = sx - sightX, dy = sy - sightY;
        if (dx * dx + dy * dy <= r * r && e.z < bestZ) { best = i; bestZ = e.z; }
    }
    if (best < 0) {
        chain = 0;  // a miss breaks the chain
        return;
    }
    auto& e = enemies[best];
    int sx, sy;
    project(e.x, e.y, e.z, sx, sy);
    if (--e.hp > 0) {
        e.hitFrames = 4;
        burst(sx, sy, rgb(200, 220, 255), 5);
        soundPlayVolume(0.6f, 20, 70, 76);
        return;
    }
    killEnemy(e, sx, sy);
}

void gameOver() {
    state = OVER;
    overAt = lgfx::millis();
    const bool best = submitScore(kKey, score);
    shake = 0;
    render(false, false);
    auto& d = uiFrame;
    d.fillSmoothRoundRect(30, 46, 260, 150, 18, rgb(12, 18, 40));
    d.drawRoundRect(30, 46, 260, 150, 18, pal::kCyan);
    text(d, "ゲームオーバー", 160, 56, pal::kText, F32, top_center);
    char b[40];
    snprintf(b, sizeof(b), best ? "NEW BEST %d" : "スコア %d", score);
    text(d, b, 160, 94, best ? pal::kAccent : pal::kText, F24, top_center);
    snprintf(b, sizeof(b), "ボス%d 連続%d", wave, bestChain);
    text(d, b, 160, 122, pal::kDim, F24, top_center);
    text(d, "A:もういちど", 160, 156, pal::kDim, F24, top_center);
    d.pushSprite(0, 0);
    soundPlay(0, 200, 64);
    soundPlay(220, 200, 60);
    soundPlay(440, 400, 55);
    printf("SPACE over score=%d wave=%d chain=%d\n", score, wave, bestChain);
}

void hurt() {
    --shields;
    hurtFrames = 12;
    shake = 4;
    chain = 0;
    soundPlay(0, 250, 40, 41);
    printf("SPACE hurt shields=%d\n", shields);
}

void step() {
    ++frames;
    const float speed = 3.0f + level * 0.5f;
    for (auto& s : stars) {
        s.z -= speed * 2.2f;
        if (s.z < 4) resetStar(s, false);
    }
    const float move = 5.5f;
    if (pad_now & kLeft) sightX -= move;
    if (pad_now & kRight) sightX += move;
    if (pad_now & kUp) sightY -= move;
    if (pad_now & kDown) sightY += move;
    sightX = std::min(std::max(sightX, 16.0f), kW - 16.0f);
    sightY = std::min(std::max(sightY, 30.0f), kH - 16.0f);
    energy = std::min(kEnergyMax, energy + kEnergyRegen);
    if (laserFrames > 0) --laserFrames;
    if (hurtFrames > 0) --hurtFrames;
    if (dryFrames > 0) --dryFrames;
    // The shake settles over a few steps (it moves the picture, the colours stay steady).
    shake = shake > 0 ? -(shake - 1) : shake < 0 ? -(shake + 1) : 0;
    const uint32_t now = lgfx::millis();
    for (auto& e : enemies) {
        if (!e.alive) continue;
        if (e.hitFrames > 0) --e.hitFrames;
        e.angle += e.spin;
        e.phase += 0.08f;
        if (e.kind == BOSS) {
            // The boss glides in, then circles in a figure eight and fires bursts.
            if (e.z > 230) e.z -= 3.0f;
            e.x = sinf(e.phase * 0.6f) * 70.0f;
            e.y = -10 + sinf(e.phase * 1.2f) * 35.0f;
            if (e.z <= 230 && int32_t(now - e.fireAt) >= 0) {
                for (int k = -1; k <= 1; ++k) enemyFire(e, k * 3.0f);
                soundPlayVolume(0.5f, 0, 90, 50);
                e.fireAt = now + max(900, 1700 - wave * 150);
            }
            continue;
        }
        e.z -= speed;
        e.x += e.vx;
        e.y += e.vy;
        if (e.kind == DODGER) e.x += sinf(e.phase) * 2.2f;  // weaves side to side
        // Enemies in the middle distance sometimes shoot back.
        if (wave >= 1 || level >= 2) {
            if (e.z < 330 && e.z > 120 && int32_t(now - e.fireAt) >= 0) {
                enemyFire(e, 0);
                soundPlayVolume(0.35f, 0, 60, 55);
                e.fireAt = now + 2600 + arcadeRandom(2000);
            }
        }
        if (e.z < kNear) {
            e.alive = false;
            hurt();
        }
    }
    for (auto& s : shots) {
        if (!s.alive) continue;
        s.x += s.vx;
        s.y += s.vy;
        s.z += s.vz;
        if (s.z <= kNear) {
            s.alive = false;
            int sx, sy;
            project(s.x, s.y, kNear, sx, sy);
            const float dx = sx - sightX, dy = sy - sightY;
            if (dx * dx + dy * dy <= 30 * 30) hurt();
            else popup(sx, sy, pal::kCyan, "よけた");
        }
    }
    for (auto& b : bits) {
        if (!b.alive) continue;
        b.x += b.vx;
        b.y += b.vy;
        if (--b.life <= 0) b.alive = false;
    }
    for (auto& p : pops)
        if (p.alive && --p.life <= 0) p.alive = false;
    if (!bossActive && int32_t(now - spawnAt) >= 0) {
        spawnEnemy();
        spawnAt = now + max(450, 1500 - level * 130);
    }
}

void enter() {
    soundBegin();
    resetRound();
    state = TITLE;
    render(false, false);
    auto& d = uiFrame;
    text(d, "3D宇宙", 160, 36, pal::kText, F40, top_center);
    char b[24];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(d, b, 160, 84, pal::kAccent, F24, top_center);
    text(d, "十字:ねらう・よける", 160, 116, pal::kText, F24, top_center);
    text(d, "A:うつ 近くで倒すと3倍", 160, 144, pal::kText, F24, top_center);
    text(d, "A:はじめる", 160, 184, pal::kCyan, F24, top_center);
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
