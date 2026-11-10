// 3D space shooter for FACES ARCADE (own graphics).
// Cockpit view over a scrolling neon grid. Flat-shaded 3D enemies fly in waves; every 4th wave is a boss.
// D-pad aims (the ship banks and slides with the sight), A fires the laser,
// hold B to sweep lock-ons over up to 6 targets and release B to launch homing missiles (bigger chains, bigger bonus).
// Enemy shots can be dodged by moving or shot down. Blue capsules restore a shield.
#include <algorithm>
#include "common.h"
#include "esp_timer.h"

namespace {
const int kW = 320, kH = 240, kCx = kW / 2, kCy = kH / 2 + 4;
const float kFocal = 220.0f, kNear = 22.0f, kFar = 460.0f;
const int kHitRadius = 48;  // dots: an enemy shot hurts when it arrives this close to the middle of the screen
const uint32_t kStepMs = 33;  // about 30 steps per second; drawing is about 25 fps
const char* const kKey = "space3d";
const int kStars = 64, kEnemies = 9, kBits = 64, kShots = 10, kMissiles = 6, kRings = 6, kPops = 3;
const int kMaxShields = 5, kStartShields = 3, kMaxLocks = 6;

enum State { TITLE, PLAY, OVER };
enum Kind { OCTA, DART, CUBE, CAPSULE, POD };
State state;
struct V3 { float x, y, z; };
struct Star { float x, y, z; };
struct Enemy { bool alive; Kind kind; int hp, locked, fireIn; float x, y, z, vx, vy, ax, ay, sa, sb; };
struct Bit { bool alive; float x, y, vx, vy; int life; uint16_t color; };
struct Shot { bool alive; int locked; float x, y, z, vx, vy, vz; };
struct Missile { bool alive; bool shot; int target, age, chain; float x, y, cx, cy; };  // shot: target is shots[target]
struct Ring { bool alive; float x, y, r; int life; uint16_t color; };
struct Pop { int life; float x, y; char text[12]; uint16_t color; };
Star stars[kStars];
Enemy enemies[kEnemies];
Bit bits[kBits];
Shot shots[kShots];
Missile missiles[kMissiles];
Ring rings[kRings];
Pop pops[kPops];

float sightX, sightY, camX, camY, roll, rollC = 1, rollS = 0, gridPhase, warp;
int score, shields, laserFrames, hurtFrames, shake, wave, toSpawn, bannerFrames, combo, comboFrames;
int lockCount, titleFrames;
bool bossWave, bossAlive, paused;
float bossX, bossY, bossZ, bossA, bossT;
int bossHp, bossMaxHp, bossFireIn, bossHitFrames;
uint32_t lastStep, overAt, spawnAt, frames;
int ox, oy;  // screen shake offset (moves the picture, never changes its colours)

// ---- meshes (triangles, flat shaded) ----
const V3 kOctaV[6] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
const uint8_t kOctaF[8][3] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1}, {5, 2, 1}, {5, 3, 2}, {5, 4, 3}, {5, 1, 4}};
const V3 kDartV[5] = {{0, 0, -1.6f}, {-1.4f, 0.3f, 0.7f}, {1.4f, 0.3f, 0.7f}, {0, -0.55f, 0.6f}, {0, 0.45f, 0.5f}};
const uint8_t kDartF[6][3] = {{0, 1, 3}, {0, 3, 2}, {0, 2, 4}, {0, 4, 1}, {1, 2, 3}, {1, 4, 2}};
const V3 kCubeV[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
const uint8_t kCubeF[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
                               {3, 2, 6}, {3, 6, 7}, {0, 3, 7}, {0, 7, 4}, {1, 5, 6}, {1, 6, 2}};
struct Mesh { const V3* v; int nv; const uint8_t (*f)[3]; int nf; };
const Mesh kMeshes[5] = {{kOctaV, 6, kOctaF, 8}, {kDartV, 5, kDartF, 6}, {kCubeV, 8, kCubeF, 12},
                         {kOctaV, 6, kOctaF, 8}, {kOctaV, 6, kOctaF, 8}};
const float kSizes[5] = {15, 17, 13, 10, 12};
const uint8_t kBaseRGB[5][3] = {{90, 255, 160}, {255, 90, 190}, {255, 170, 60}, {120, 200, 255}, {255, 230, 90}};

uint16_t shade(const uint8_t* c, float t) {
    t = min(1.0f, max(0.0f, t));
    return rgb(int(c[0] * t), int(c[1] * t), int(c[2] * t));
}
uint16_t kindColor(Kind k) { return shade(kBaseRGB[k], 1.0f); }

// World -> screen with camera slide, bank and shake.
bool project(float x, float y, float z, int& sx, int& sy) {
    if (z < 1.0f) return false;
    const float rx = x - camX, ry = y - camY;
    const float px = rx * rollC - ry * rollS, py = rx * rollS + ry * rollC;
    sx = kCx + ox + int(px * kFocal / z);
    sy = kCy + oy + int(py * kFocal / z);
    return true;
}

void resetStar(Star& s, bool anywhere) {
    s.x = arcadeRandom(-500, 500);
    s.y = arcadeRandom(-360, 200);
    s.z = anywhere ? arcadeRandom(30, int(kFar)) : kFar + arcadeRandom(60);
}

void burst(int sx, int sy, uint16_t color, int count) {
    int made = 0;
    for (auto& b : bits) {
        if (b.alive) continue;
        const float a = arcadeRandom(628) / 100.0f, sp = 1.5f + arcadeRandom(50) / 10.0f;
        b = {true, float(sx), float(sy), cosf(a) * sp, sinf(a) * sp, 10 + int(arcadeRandom(12)), color};
        if (++made >= count) break;
    }
    for (auto& r : rings)
        if (!r.alive) { r = {true, float(sx), float(sy), 4, 12, color}; break; }
}

void pop(int sx, int sy, const char* s, uint16_t color) {
    Pop* slot = &pops[0];
    for (auto& p : pops) if (p.life < slot->life) slot = &p;
    slot->life = 24;
    slot->x = sx;
    slot->y = sy;
    slot->color = color;
    snprintf(slot->text, sizeof(slot->text), "%s", s);
}

Enemy* spawn(Kind k) {
    for (auto& e : enemies) {
        if (e.alive) continue;
        e = {};
        e.alive = true;
        e.kind = k;
        e.hp = k == CUBE ? 3 : 1;
        e.x = arcadeRandom(-120, 121);
        e.y = arcadeRandom(-80, 50);
        e.z = kFar;
        e.vx = arcadeRandom(-12, 13) / 30.0f;
        e.vy = arcadeRandom(-8, 9) / 30.0f;
        e.sa = (arcadeRandom(2) ? 1 : -1) * (0.04f + arcadeRandom(6) / 100.0f);
        e.sb = (arcadeRandom(2) ? 1 : -1) * (0.02f + arcadeRandom(4) / 100.0f);
        e.fireIn = 40 + arcadeRandom(60);
        return &e;
    }
    return nullptr;
}

void startWave(int w) {
    wave = w;
    bossWave = (w % 4) == 0;
    bannerFrames = 50;
    warp = 1.0f;
    if (bossWave) {
        toSpawn = 0;
        bossAlive = true;
        bossX = 0;
        bossY = -20;
        bossZ = kFar + 80;
        bossA = bossT = 0;
        bossMaxHp = bossHp = 24 + w * 4;
        bossFireIn = 60;
        for (int i = 0; i < 4; ++i) {
            Enemy* p = spawn(POD);
            if (p) { p->hp = 3 + w / 4; p->sa = i * PI / 2; p->vx = p->vy = 0; }
        }
        soundPlayVolume(0.6f, 0, 300, 40, 47);
        soundPlayVolume(0.6f, 320, 500, 39, 46);
    } else {
        toSpawn = 6 + w * 2;
        spawnAt = lgfx::millis() + 1200;
        soundPlayTimbre(Timbre::bell, 0, 140, 72);
        soundPlayTimbre(Timbre::bell, 150, 140, 76);
        soundPlayTimbre(Timbre::bell, 300, 260, 79);
    }
    printf("SPACE wave=%d boss=%d\n", w, bossWave ? 1 : 0);
}

void resetRound() {
    for (auto& s : stars) resetStar(s, true);
    for (auto& e : enemies) e.alive = false;
    for (auto& b : bits) b.alive = false;
    for (auto& s : shots) s.alive = false, s.locked = 0;
    for (auto& m : missiles) m.alive = false;
    for (auto& r : rings) r.alive = false;
    for (auto& p : pops) p.life = 0;
    sightX = kCx;
    sightY = kCy;
    camX = camY = roll = 0;
    rollC = 1;
    rollS = 0;
    score = 0;
    shields = kStartShields;
    laserFrames = hurtFrames = shake = combo = comboFrames = lockCount = 0;
    bossAlive = paused = false;
    ox = oy = 0;
    startWave(1);
}

// ---- drawing ----
void drawBackground() {
    auto& d = uiFrame;
    // Deep space gradient above the horizon, dark floor below.
    const int horizon = kCy + oy + int(90 * kFocal / kFar) - int(camY * kFocal / kFar);
    for (int y = 0; y < kH; y += 8) {
        int r, g, b;
        if (y < horizon) {
            const float t = float(y) / max(1, horizon);
            r = 8 + int(40 * t * t);
            g = 6 + int(10 * t);
            b = 22 + int(48 * t);
        } else {
            r = 6; g = 4; b = 16;
        }
        d.fillRect(0, y, kW, 8, rgb(r, g, b));
    }
    // A big ringed planet that drifts with the camera (far away, so it moves little).
    const int px = 236 + ox - int(camX * 0.08f), py = 60 + oy - int(camY * 0.08f);
    d.fillCircle(px, py, 30, rgb(60, 40, 110));
    d.fillCircle(px - 6, py - 5, 25, rgb(110, 70, 170));
    d.fillCircle(px - 11, py - 9, 16, rgb(160, 110, 210));
    d.drawEllipse(px, py, 50, 9, rgb(210, 170, 120));
    d.drawEllipse(px, py, 46, 7, rgb(150, 110, 80));
    // Stars streak more while warping.
    const float streak = 6.0f + warp * 60.0f;
    for (const auto& s : stars) {
        int sx, sy, tx, ty;
        if (!project(s.x, s.y, s.z, sx, sy)) continue;
        const int bright = 255 - int(s.z * 190 / (kFar + 60));
        const uint16_t c = rgb(bright, bright, min(255, bright + 40));
        if (warp > 0.05f && project(s.x, s.y, s.z + streak, tx, ty)) d.drawLine(sx, sy, tx, ty, c);
        else if (s.z < 110) d.fillRect(sx, sy, 2, 2, c);
        else d.drawPixel(sx, sy, c);
    }
    // Neon floor grid 90 units below the ship.
    const float gy = 90;
    const uint16_t gridFar = rgb(70, 20, 110), gridNear = rgb(200, 60, 220);
    for (int i = 0; i < 9; ++i) {
        const float z = 40 + i * 50 - gridPhase;
        if (z < 24) continue;
        int x0, y0, x1, y1;
        if (project(-600, gy, z, x0, y0) && project(600, gy, z, x1, y1))
            d.drawLine(x0, y0, x1, y1, z < 200 ? gridNear : gridFar);
    }
    for (int i = -6; i <= 6; ++i) {
        int x0, y0, x1, y1;
        if (project(i * 90.0f, gy, 30, x0, y0) && project(i * 90.0f, gy, kFar, x1, y1))
            d.drawLine(x0, y0, x1, y1, gridFar);
    }
}

void drawMesh(Kind k, float cx, float cy, float cz, float size, float a, float b, const uint8_t* col, float glow) {
    const Mesh& m = kMeshes[k];
    V3 w[8];
    int px[8], py[8];
    bool ok[8];
    const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    for (int i = 0; i < m.nv; ++i) {
        const V3 v = m.v[i];
        const float x1 = v.x * ca - v.z * sa, z1 = v.x * sa + v.z * ca;
        const float y2 = v.y * cb - z1 * sb, z2 = v.y * sb + z1 * cb;
        w[i] = {cx + x1 * size, cy + y2 * size, cz + z2 * size};
        ok[i] = project(w[i].x, w[i].y, w[i].z, px[i], py[i]);
    }
    // Painter's order inside the object: far faces first.
    int order[12];
    float depth[12];
    for (int i = 0; i < m.nf; ++i) {
        order[i] = i;
        depth[i] = w[m.f[i][0]].z + w[m.f[i][1]].z + w[m.f[i][2]].z;
    }
    for (int i = 1; i < m.nf; ++i)
        for (int j = i; j > 0 && depth[order[j]] > depth[order[j - 1]]; --j) std::swap(order[j], order[j - 1]);
    const float fog = 1.0f - min(1.0f, max(0.0f, (cz - kNear) / (kFar - kNear))) * 0.65f;
    for (int n = 0; n < m.nf; ++n) {
        const uint8_t* f = m.f[order[n]];
        if (!ok[f[0]] || !ok[f[1]] || !ok[f[2]]) continue;
        const V3 &p0 = w[f[0]], &p1 = w[f[1]], &p2 = w[f[2]];
        const float ux = p1.x - p0.x, uy = p1.y - p0.y, uz = p1.z - p0.z;
        const float vx = p2.x - p0.x, vy = p2.y - p0.y, vz = p2.z - p0.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float len = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-4f;
        nx /= len; ny /= len; nz /= len;
        const float light = fabsf(nx * -0.45f + ny * -0.6f + nz * -0.66f);
        const uint16_t c = shade(col, (0.28f + 0.72f * light) * fog + glow);
        uiFrame.fillTriangle(px[f[0]], py[f[0]], px[f[1]], py[f[1]], px[f[2]], py[f[2]], c);
    }
    // Bright edges on the near silhouette make the shape crisp.
    const uint16_t edge = shade(col, fog * 1.1f + 0.15f);
    for (int i = 0; i < m.nf; ++i) {
        const uint8_t* f = m.f[i];
        if (ok[f[0]] && ok[f[1]] && depth[i] < 3 * cz) uiFrame.drawLine(px[f[0]], py[f[0]], px[f[1]], py[f[1]], edge);
    }
}

void podPos(const Enemy& e, float& x, float& y, float& z) {
    x = bossX + cosf(e.sa + bossA) * 70;
    y = bossY + sinf(e.sa + bossA) * 50;
    z = bossZ;
}

void enemyScreen(const Enemy& e, float& x, float& y, float& z) {
    if (e.kind == POD) podPos(e, x, y, z);
    else { x = e.x; y = e.y; z = e.z; }
}

void drawLockMark(int sx, int sy, int n, float z) {
    auto& d = uiFrame;
    const int r = 8 + int(10 * kFocal / max(z, 30.0f) * 0.3f) + 6;
    const uint16_t c = rgb(255, 80, 80);
    const int k = (frames * 2) % 6;  // corner brackets slide slowly inward (motion, not flashing)
    const int rr = r + 6 - k;
    d.drawFastHLine(sx - rr, sy - rr, 6, c); d.drawFastVLine(sx - rr, sy - rr, 6, c);
    d.drawFastHLine(sx + rr - 5, sy - rr, 6, c); d.drawFastVLine(sx + rr, sy - rr, 6, c);
    d.drawFastHLine(sx - rr, sy + rr, 6, c); d.drawFastVLine(sx - rr, sy + rr - 5, 6, c);
    d.drawFastHLine(sx + rr - 5, sy + rr, 6, c); d.drawFastVLine(sx + rr, sy + rr - 5, 6, c);
    (void)n;
}

void drawHud() {
    auto& d = uiFrame;
    char b[24];
    snprintf(b, sizeof(b), "%d", score);
    text(d, b, 8, 2, pal::kText, F24, top_left);
    for (int i = 0; i < kMaxShields; ++i) {
        if (i >= max(shields, kStartShields)) break;
        d.fillRoundRect(314 - (i + 1) * 22, 8, 18, 12, 4, i < shields ? pal::kCyan : rgb(38, 57, 90));
    }
    if (combo >= 2) {
        snprintf(b, sizeof(b), "x%d", 1 + min(combo / 4, 4));
        text(d, b, 8, 28, pal::kAccent, F24, top_left);
        d.fillRect(8, 54, comboFrames * 50 / 60, 3, pal::kAccent);
    }
    if (bossAlive) {
        const int w = 160 * bossHp / max(1, bossMaxHp);
        d.drawRect(80, 34, 162, 8, rgb(255, 120, 120));
        d.fillRect(81, 35, w, 6, rgb(240, 70, 80));
    }
    // Radar on the dashboard: enemies as dots by x and depth.
    const int rx = kCx, ry = kH - 14;
    d.fillRoundRect(rx - 34, ry - 18, 68, 30, 6, rgb(10, 24, 30));
    d.drawRoundRect(rx - 34, ry - 18, 68, 30, 6, rgb(40, 120, 120));
    for (const auto& e : enemies) {
        if (!e.alive) continue;
        float x, y, z;
        enemyScreen(e, x, y, z);
        const int dx = rx + int(max(-30.0f, min(30.0f, (x - camX) / 6))), dy = ry + 9 - int(min(1.0f, z / kFar) * 24);
        d.fillRect(dx - 1, dy - 1, 3, 3, kindColor(e.kind));
    }
    d.fillRect(rx - 1, ry + 9, 3, 3, TFT_WHITE);
    // Lock-on gauge.
    for (int i = 0; i < kMaxLocks; ++i)
        d.fillRect(kCx - 33 + i * 11, ry - 24, 9, 4, i < lockCount ? rgb(255, 80, 80) : rgb(50, 40, 60));
}

void render(bool showSight = true, bool push = true) {
    auto& d = uiFrame;
    drawBackground();
    // Collect drawable objects and draw far ones first.
    struct Item { float z; int kind; int idx; };
    Item items[kEnemies + 1];
    int n = 0;
    for (int i = 0; i < kEnemies; ++i)
        if (enemies[i].alive) {
            float x, y, z;
            enemyScreen(enemies[i], x, y, z);
            items[n++] = {z, 0, i};
        }
    if (bossAlive) items[n++] = {bossZ, 1, 0};
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && items[j].z > items[j - 1].z; --j) std::swap(items[j], items[j - 1]);
    for (int i = 0; i < n; ++i) {
        if (items[i].kind == 1) {
            static const uint8_t core[3] = {150, 160, 200}, hot[3] = {255, 150, 90};
            int podsLeft = 0;
            for (const auto& e : enemies) if (e.alive && e.kind == POD) ++podsLeft;
            drawMesh(CUBE, bossX, bossY, bossZ, 34, bossA * 0.7f, bossA * 0.4f, podsLeft ? core : hot, bossHitFrames > 0 ? 0.25f : 0);
            continue;
        }
        const auto& e = enemies[items[i].idx];
        float x, y, z;
        enemyScreen(e, x, y, z);
        drawMesh(e.kind, x, y, z, kSizes[e.kind], e.ax, e.ay, kBaseRGB[e.kind], 0);
    }
    // Enemy shots: glowing orbs that grow as they come close.
    for (const auto& s : shots) {
        int sx, sy;
        if (!s.alive || !project(s.x, s.y, s.z, sx, sy)) continue;
        const int r = max(2, int(7 * kFocal / s.z));
        d.fillCircle(sx, sy, r + 2, rgb(150, 30, 20));
        d.fillCircle(sx, sy, r, rgb(255, 110, 50));
        d.fillCircle(sx, sy, max(1, r / 2), rgb(255, 230, 170));
    }
    for (const auto& r : rings)
        if (r.alive) d.drawCircle(int(r.x), int(r.y), int(r.r), r.color);
    for (const auto& b : bits)
        if (b.alive) d.fillRect(int(b.x), int(b.y), b.life > 8 ? 3 : 2, b.life > 8 ? 3 : 2, b.color);
    // Lock marks, and a steady red ring on enemies about to reach the cockpit.
    for (const auto& e : enemies) {
        if (!e.alive) continue;
        float x, y, z;
        int sx, sy;
        enemyScreen(e, x, y, z);
        if (!project(x, y, z, sx, sy)) continue;
        if (e.locked) drawLockMark(sx, sy, e.locked, z);
        else if (e.kind != CAPSULE && e.kind != POD && z < 130) d.drawCircle(sx, sy, int(kSizes[e.kind] * kFocal / z) + 6, rgb(255, 70, 60));
    }
    for (const auto& sh : shots) {
        int sx, sy;
        if (sh.alive && sh.locked && project(sh.x, sh.y, sh.z, sx, sy)) drawLockMark(sx, sy, sh.locked, sh.z);
    }
    // Missiles: bright heads with short trails on a curved path.
    for (const auto& m : missiles) {
        if (!m.alive) continue;
        d.fillCircle(int(m.x), int(m.y), 3, rgb(255, 255, 200));
        d.drawLine(int(m.x), int(m.y), int(m.cx), int(m.cy), rgb(120, 200, 255));
    }
    if (laserFrames > 0) {
        const uint16_t c = rgb(255, 230, 120), c2 = rgb(255, 150, 60);
        d.drawLine(24, kH - 4, int(sightX), int(sightY), c);
        d.drawLine(kW - 24, kH - 4, int(sightX), int(sightY), c);
        d.drawLine(25, kH - 4, int(sightX) + 1, int(sightY), c2);
        d.drawLine(kW - 25, kH - 4, int(sightX) - 1, int(sightY), c2);
    }
    for (const auto& p : pops)
        if (p.life > 0) text(d, p.text, int(p.x), int(p.y), p.color, F24, middle_center);
    if (showSight) {
        const int x = int(sightX), y = int(sightY);
        const bool locking = pad_now & kB;
        const uint16_t sc = locking ? rgb(255, 100, 100) : rgb(255, 210, 90);
        d.drawCircle(x, y, locking ? 16 : 12, sc);
        d.drawFastHLine(x - 22, y, 10, sc);
        d.drawFastHLine(x + 13, y, 10, sc);
        d.drawFastVLine(x, y - 22, 10, sc);
        d.drawFastVLine(x, y + 13, 10, sc);
        d.drawPixel(x, y, sc);
    }
    // Cockpit frame. A hit tints it orange that fades out slowly (no flashing).
    const float h = hurtFrames / 24.0f;
    const uint16_t frameColor = rgb(30 + int(170 * h), 50 + int(40 * h), 80 - int(40 * h));
    d.fillTriangle(0, kH, 0, kH - 46, 78, kH, frameColor);
    d.fillTriangle(kW, kH, kW, kH - 46, kW - 78, kH, frameColor);
    d.drawLine(0, kH - 46, 78, kH, rgb(80, 120, 170));
    d.drawLine(kW, kH - 46, kW - 78, kH, rgb(80, 120, 170));
    drawHud();
    if (bannerFrames > 0) {
        char b[24];
        if (bossWave) snprintf(b, sizeof(b), "ボス せっきん");
        else snprintf(b, sizeof(b), "WAVE %d", wave);
        text(d, b, kCx, 92, bossWave ? rgb(255, 120, 120) : pal::kCyan, F32, middle_center);
    }
    if (paused) {
        darkenRoundRect(60, 80, 200, 80, 14);
        text(d, "ポーズ", kCx, 104, pal::kText, F32, middle_center);
        text(d, "START:つづける", kCx, 140, pal::kDim, F24, middle_center);
    }
    if (push) d.pushSprite(0, 0);  // title and game-over draw their text first, then push once
}

// ---- gameplay ----
int comboMul() { return 1 + min(combo / 4, 4); }

void kill(Enemy& e, int bonusMul) {
    float x, y, z;
    int sx, sy;
    enemyScreen(e, x, y, z);
    if (!project(x, y, z, sx, sy)) { sx = kCx; sy = kCy; }
    burst(sx, sy, kindColor(e.kind), e.kind == CUBE ? 22 : 14);
    if (e.locked && lockCount > 0 && (pad_now & kB)) --lockCount;
    e.locked = 0;
    e.alive = false;
    if (e.kind == CAPSULE) {
        shields = min(kMaxShields, shields + 1);
        pop(sx, sy, "SHIELD+", pal::kCyan);
        soundPlayTimbre(Timbre::bell, 0, 200, 84, 88);
        return;
    }
    ++combo;
    comboFrames = 60;
    const int base = (e.kind == CUBE ? 30 : e.kind == POD ? 50 : 10) + int((kFar - z) / 40);
    const int pts = base * comboMul() * bonusMul;
    score += pts;
    char b[12];
    snprintf(b, sizeof(b), "%d", pts);
    pop(sx, sy - 14, b, bonusMul > 1 ? rgb(255, 120, 120) : pal::kAccent);
    soundPlayVolume(0.7f, 20, 150, 50 + min(combo, 12), 57 + min(combo, 12));
    printf("SPACE hit score=%d combo=%d\n", score, combo);
}

void damageBoss(int dmg, int sx, int sy) {
    bossHp -= dmg;
    bossHitFrames = 4;
    burst(sx, sy, rgb(255, 170, 90), 6);
    if (bossHp > 0) { soundPlayVolume(0.5f, 0, 60, 45); return; }
    bossAlive = false;
    for (int i = 0; i < 4; ++i) burst(sx + arcadeRandom(-40, 41), sy + arcadeRandom(-30, 31), rgb(255, 200, 120), 16);
    const int pts = 1000 * (wave / 4);
    score += pts;
    char b[12];
    snprintf(b, sizeof(b), "%d", pts);
    pop(sx, sy, b, rgb(255, 120, 120));
    shake = 16;
    soundPlay(0, 300, 36, 43);
    soundPlayTimbre(Timbre::bell, 350, 200, 72, 76);
    soundPlayTimbre(Timbre::bell, 560, 500, 79, 84);
    printf("SPACE boss down score=%d\n", score);
}

// Returns true if (sx,sy) on screen is within the target circle of an object at depth z with world radius r.
bool underSight(float x, float y, float z, float r, float margin) {
    int sx, sy;
    if (!project(x, y, z, sx, sy)) return false;
    const float rr = r * kFocal / z + margin;
    const float dx = sx - sightX, dy = sy - sightY;
    return dx * dx + dy * dy <= rr * rr;
}

// Removes an enemy shot (its intercept missile, if any, stops on the next step).
void dropShot(Shot& s) {
    s.locked = 0;
    s.alive = false;
}

// rapid: a repeat while A is held, which keeps the beam lit and plays a quieter tick.
void fire(bool rapid = false) {
    laserFrames = rapid ? 4 : 3;
    soundPlayVolume(rapid ? 0.2f : 0.4f, 0, 50, 96);  // the shot plays often; low amplitude keeps it from being loud
    // Nearest thing under the sight: enemy shot, enemy, pod or boss.
    int best = -1, bestShot = -1;
    float bestZ = 1e9f;
    for (int i = 0; i < kShots; ++i) {
        const auto& s = shots[i];
        if (s.alive && s.z < bestZ && underSight(s.x, s.y, s.z, 8, 8)) { bestShot = i; bestZ = s.z; }
    }
    for (int i = 0; i < kEnemies; ++i) {
        const auto& e = enemies[i];
        if (!e.alive) continue;
        float x, y, z;
        enemyScreen(e, x, y, z);
        if (z < bestZ && underSight(x, y, z, kSizes[e.kind], 7)) { best = i; bestShot = -1; bestZ = z; }
    }
    if (bestShot >= 0) {
        int sx, sy;
        auto& s = shots[bestShot];
        project(s.x, s.y, s.z, sx, sy);
        dropShot(s);
        burst(sx, sy, rgb(255, 140, 60), 8);
        score += 5;
        soundPlayVolume(0.5f, 0, 70, 79);
        return;
    }
    if (best >= 0) {
        auto& e = enemies[best];
        if (--e.hp <= 0) kill(e, 1);
        else {
            float x, y, z;
            int sx, sy;
            enemyScreen(e, x, y, z);
            if (project(x, y, z, sx, sy)) burst(sx, sy, kindColor(e.kind), 4);
            soundPlayVolume(0.4f, 0, 50, 67);
        }
        return;
    }
    if (bossAlive && underSight(bossX, bossY, bossZ, 40, 0)) {
        int sx, sy;
        project(bossX, bossY, bossZ, sx, sy);
        int podsLeft = 0;
        for (const auto& e : enemies) if (e.alive && e.kind == POD) ++podsLeft;
        if (podsLeft) { soundPlayVolume(0.4f, 0, 40, 100); burst(sx, sy, rgb(160, 170, 210), 3); }  // shielded: a dull "ting"
        else damageBoss(1, sx, sy);
    }
}

void updateLocks() {
    if (!(pad_now & kB) || lockCount >= kMaxLocks) return;
    // During the boss fight, sweeping B over its shots intercepts them: a missile leaves at once
    // (waiting for the release would let the shot arrive first). These do not use up the enemy locks.
    if (bossAlive)
        for (int i = 0; i < kShots; ++i) {
            auto& sh = shots[i];
            if (!sh.alive || sh.locked || !underSight(sh.x, sh.y, sh.z, 8, 14)) continue;
            Missile* m = nullptr;
            for (auto& c : missiles) if (!c.alive) { m = &c; break; }
            if (!m) break;
            sh.locked = 1;
            const float fromX = (i % 2) ? kW - 40 : 40;
            *m = {true, true, i, 0, 1, fromX, float(kH - 20), fromX, float(kH - 20)};
            soundPlayVolume(0.45f, 0, 70, 88);
            printf("SPACE intercept shot=%d\n", i);
        }
    for (auto& e : enemies) {
        if (!e.alive || e.locked || e.kind == CAPSULE) continue;
        float x, y, z;
        enemyScreen(e, x, y, z);
        if (!underSight(x, y, z, kSizes[e.kind], 14)) continue;
        e.locked = ++lockCount;
        soundPlayVolume(0.45f, 0, 70, 76 + lockCount * 2);
        if (lockCount >= kMaxLocks) break;
    }
}

void releaseLocks() {
    if (lockCount == 0) return;
    // Free missile slots only, so intercept missiles already in flight keep going.
    Missile* launched[kMissiles];
    int n = 0, slot = 0;
    for (int i = 0; i < kEnemies; ++i) {
        if (!enemies[i].alive || !enemies[i].locked) continue;
        while (slot < kMissiles && missiles[slot].alive) ++slot;
        if (slot >= kMissiles) { enemies[i].locked = 0; continue; }
        auto& m = missiles[slot];
        m = {true, false, i, n * -2, 1, float(n % 2 ? kW - 40 : 40), float(kH - 20), float(n % 2 ? kW - 40 : 40), float(kH - 20)};
        launched[n++] = &m;
    }
    for (int i = 0; i < n; ++i) launched[i]->chain = n;
    printf("SPACE missiles=%d\n", n);
    for (int i = 0; i < min(n, 3); ++i) soundPlayVolume(0.5f, i * 70, 90, 84 + i * 3);
    lockCount = 0;
}

void stepShotMissile(Missile& m) {
    auto& sh = shots[m.target];
    int sx, sy;
    if (!sh.alive || !project(sh.x, sh.y, sh.z, sx, sy)) { m.alive = false; sh.locked = 0; return; }
    m.cx = m.x;
    m.cy = m.y;
    // A shot is already on its way in, so this missile is quicker than one sent at an enemy: it lands within 4 steps.
    const float t = min(1.0f, m.age / 3.0f);
    m.x += (sx - m.x) * (0.4f + 0.6f * t);
    m.y += (sy - m.y) * (0.4f + 0.6f * t);
    if (m.age >= 4 || (fabsf(m.x - sx) < 6 && fabsf(m.y - sy) < 6)) {
        m.alive = false;
        sh.locked = 0;
        sh.alive = false;
        burst(sx, sy, rgb(255, 140, 60), 8);
        const int pts = 5 * (m.chain > 1 ? 1 << min(m.chain - 1, 5) : 1);
        score += pts;
        char b[12];
        snprintf(b, sizeof(b), "%d", pts);
        pop(sx, sy - 10, b, rgb(255, 160, 90));
        soundPlayVolume(0.5f, 0, 70, 79);
        printf("SPACE shot down by missile score=%d\n", score);
    }
}

void stepMissiles() {
    int hits = 0;
    for (auto& m : missiles) {
        if (!m.alive) continue;
        ++m.age;
        if (m.age < 0) continue;
        if (m.shot) { stepShotMissile(m); continue; }
        auto& e = enemies[m.target];
        if (!e.alive) { m.alive = false; continue; }
        float x, y, z;
        int sx, sy;
        enemyScreen(e, x, y, z);
        if (!project(x, y, z, sx, sy)) { m.alive = false; e.locked = 0; continue; }
        m.cx = m.x;
        m.cy = m.y;
        // Curve outward first, then home in.
        const float t = min(1.0f, m.age / 9.0f);
        const float side = (m.x < kCx ? -1 : 1) * 30.0f * (1 - t);
        m.x += (sx - m.x) * (0.18f + 0.5f * t) + side * 0.3f;
        m.y += (sy - m.y) * (0.18f + 0.5f * t) - 6 * (1 - t);
        if (m.age >= 10 || (fabsf(m.x - sx) < 6 && fabsf(m.y - sy) < 6)) {
            m.alive = false;
            e.locked = 0;
            ++hits;
            if (e.kind == CUBE) e.hp = 1;
            if (--e.hp <= 0) kill(e, m.chain > 1 ? 1 << min(m.chain - 1, 5) : 1);  // bigger chains multiply the bonus: 2,4,8,16,32
        }
    }
    (void)hits;
}

void hurt() {
    --shields;
    hurtFrames = 24;
    shake = 10;
    combo = 0;
    soundPlay(0, 260, 40, 41);
    printf("SPACE hurt shields=%d\n", shields);
}

void enemyFire(float x, float y, float z, float spread) {
    for (auto& s : shots) {
        if (s.alive) continue;
        s.alive = true;
        s.locked = 0;
        s.x = x; s.y = y; s.z = z;
        // Aim at where the ship is now; moving away dodges it.
        const float steps = (z - kNear) / 7.0f;
        s.vx = (camX - x) / steps + spread;
        s.vy = (camY - y) / steps;
        s.vz = -7.0f;
        soundPlayVolume(0.3f, 0, 60, 62);
        return;
    }
}

void gameOver() {
    state = OVER;
    overAt = lgfx::millis();
    const bool best = submitScore(kKey, score);
    ox = oy = 0;
    render(false, false);
    auto& d = uiFrame;
    d.fillSmoothRoundRect(40, 52, 240, 136, 18, rgb(12, 18, 40));
    d.drawRoundRect(40, 52, 240, 136, 18, pal::kCyan);
    text(d, "ゲームオーバー", 160, 62, pal::kText, F32, top_center);
    char b[32];
    snprintf(b, sizeof(b), best ? "NEW BEST %d" : "スコア %d", score);
    text(d, b, 160, 100, best ? pal::kAccent : pal::kText, F24, top_center);
    snprintf(b, sizeof(b), "WAVE %d まで", wave);
    text(d, b, 160, 126, pal::kDim, F24, top_center);
    text(d, "A:もういちど", 160, 154, pal::kCyan, F24, top_center);
    d.pushSprite(0, 0);
    soundPlay(0, 200, 64);
    soundPlay(220, 200, 60);
    soundPlay(440, 400, 55);
    printf("SPACE over score=%d wave=%d\n", score, wave);
}

void step() {
    ++frames;
    warp = max(0.0f, warp - 0.025f);
    const float speed = 2.6f + wave * 0.35f + warp * 14.0f;
    for (auto& s : stars) {
        s.z -= speed * 2.0f;
        if (s.z < 4) resetStar(s, false);
    }
    gridPhase += speed * 1.5f;
    if (gridPhase >= 50) gridPhase -= 50;
    const float move = 5.5f * 1.15f;  // 1.15x after play testing
    if (pad_now & kLeft) sightX -= move;
    if (pad_now & kRight) sightX += move;
    if (pad_now & kUp) sightY -= move;
    if (pad_now & kDown) sightY += move;
    sightX = std::min(std::max(sightX, 16.0f), kW - 16.0f);
    sightY = std::min(std::max(sightY, 30.0f), kH - 30.0f);
    // The ship slides and banks toward the sight.
    camX += ((sightX - kCx) * 0.55f - camX) * 0.15f;
    camY += ((sightY - kCy) * 0.45f - camY) * 0.15f;
    roll += (-(sightX - kCx) / kCx * 0.18f - roll) * 0.12f;
    rollC = cosf(roll);
    rollS = sinf(roll);
    if (shake > 0) { --shake; ox = arcadeRandom(-shake, shake + 1) / 2; oy = arcadeRandom(-shake, shake + 1) / 2; }
    else ox = oy = 0;
    if (laserFrames > 0) --laserFrames;
    if (hurtFrames > 0) --hurtFrames;
    if (bannerFrames > 0) --bannerFrames;
    if (bossHitFrames > 0) --bossHitFrames;
    if (comboFrames > 0 && --comboFrames == 0) combo = 0;
    updateLocks();
    // Boss motion and attacks.
    if (bossAlive) {
        bossT += 0.03f;
        bossA += 0.03f;
        bossZ += (210 - bossZ) * 0.02f;
        bossX = sinf(bossT) * 80;
        bossY = -20 + sinf(bossT * 1.7f) * 40;
        if (--bossFireIn <= 0) {
            int pods = 0;
            for (const auto& e : enemies) if (e.alive && e.kind == POD) ++pods;
            const int n = pods ? 1 : 3;
            for (int i = 0; i < n; ++i) enemyFire(bossX, bossY, bossZ - 30, (i - (n - 1) / 2.0f) * 0.6f);
            bossFireIn = max(20, (pods ? 50 : 34) - wave * 2);
        }
    }
    for (auto& e : enemies) {
        if (!e.alive) continue;
        e.ax += e.sa * (e.kind == POD ? 0 : 1) + (e.kind == POD ? 0.08f : 0);
        e.ay += e.sb;
        if (e.kind == POD) continue;
        e.z -= speed * (e.kind == DART ? 1.25f : e.kind == CAPSULE ? 0.8f : 1.0f);
        e.x += e.vx;
        e.y += e.vy;
        if (e.kind == DART) e.x += sinf(frames * 0.08f + e.sa * 20) * 1.2f;  // darts weave
        if (e.kind == DART && e.z < kFar - 60 && e.z > 120 && --e.fireIn <= 0) {
            enemyFire(e.x, e.y, e.z, 0);
            e.fireIn = max(40, 110 - wave * 6);
        }
        if (e.z < kNear) {
            if (e.locked && lockCount > 0 && (pad_now & kB)) --lockCount;
            e.alive = false;
            if (e.kind != CAPSULE) hurt();
        }
    }
    for (auto& s : shots) {
        if (!s.alive) continue;
        s.x += s.vx;
        s.y += s.vy;
        s.z += s.vz;
        if (s.z < kNear) {
            dropShot(s);
            // It hurts only when its center reaches a 48-dot circle around the middle of the screen
            // (the orb is drawn about 60 dots wide there, so it visibly covers the middle).
            int sx, sy;
            const bool project_ok = project(s.x, s.y, kNear, sx, sy);
            const int dx = sx - (kCx + ox), dy = sy - (kCy + oy);
            if (project_ok && dx * dx + dy * dy <= kHitRadius * kHitRadius) hurt();
            else soundPlayVolume(0.25f, 0, 80, 70, 74);  // a near miss whooshes past
        }
    }
    stepMissiles();
    for (auto& b : bits) {
        if (!b.alive) continue;
        b.x += b.vx;
        b.y += b.vy;
        b.vx *= 0.95f;
        b.vy *= 0.95f;
        if (--b.life <= 0) b.alive = false;
    }
    for (auto& r : rings) {
        if (!r.alive) continue;
        r.r += 3.5f;
        if (--r.life <= 0) r.alive = false;
    }
    for (auto& p : pops)
        if (p.life > 0) { --p.life; p.y -= 1.0f; }
    // Spawning and wave progress.
    const uint32_t now = lgfx::millis();
    if (toSpawn > 0 && int32_t(now - spawnAt) >= 0) {
        const int dice = arcadeRandom(100);
        Kind k = OCTA;
        if (wave >= 2 && dice < 30) k = DART;
        else if (wave >= 3 && dice < 45) k = CUBE;
        else if (dice >= 94) k = CAPSULE;
        if (spawn(k)) --toSpawn;
        // Occasionally a second enemy arrives in formation.
        if (toSpawn > 0 && wave >= 2 && arcadeRandom(3) == 0) {
            Enemy* e = spawn(OCTA);
            if (e) --toSpawn;
        }
        spawnAt = now + max(420, 1300 - wave * 110);
    }
    bool any = false;
    for (const auto& e : enemies) if (e.alive) any = true;
    if (!any && toSpawn == 0 && !bossAlive && bannerFrames == 0) startWave(wave + 1);
}

void renderTitle() {
    auto& d = uiFrame;
    ++titleFrames;
    for (auto& s : stars) { s.z -= 6; if (s.z < 4) resetStar(s, false); }
    gridPhase += 4; if (gridPhase >= 50) gridPhase -= 50;
    warp = 0.3f;
    drawBackground();
    static const uint8_t c1[3] = {255, 90, 190}, c2[3] = {90, 255, 160};
    drawMesh(DART, -40, -8, 150, 22, titleFrames * 0.05f, 0.3f, c1, 0);
    drawMesh(OCTA, 46, 4, 170, 18, -titleFrames * 0.04f, titleFrames * 0.03f, c2, 0);
    text(d, "3D宇宙", 160, 28, pal::kText, F40, top_center);
    char b[24];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(d, b, 160, 78, pal::kAccent, F24, top_center);
    text(d, "十字:ねらう  A:うつ", 160, 114, pal::kText, F24, top_center);
    text(d, "Bながおし:ロックオン", 160, 146, pal::kText, F24, top_center);
    text(d, "A:はじめる", 160, 186, pal::kCyan, F24, top_center);
    d.pushSprite(0, 0);
}

void enter() {
    soundBegin();
    resetRound();
    state = TITLE;
    titleFrames = 0;
    lastStep = lgfx::millis();
    renderTitle();
}

void tick() {
    const uint32_t now = lgfx::millis();
    if (state == TITLE || state == OVER) {
        if (((pad_just & (kA | kStart)) || tap_just) && (state == TITLE || now - overAt > 600)) {
            resetRound();
            state = PLAY;
            lastStep = now;
            puts("SPACE start");
            return;
        }
        if (state == TITLE && now - lastStep >= 50) { lastStep = now; renderTitle(); }
        return;
    }
    if (pad_just & kStart) {
        paused = !paused;
        render();
        lastStep = now;
        return;
    }
    if (paused) return;
    // Holding A keeps the laser firing: one shot on the press, then one every 100 ms after a short hold.
    static uint32_t aDownAt = 0, lastRapid = 0;
    if (pad_just & kA) { fire(); aDownAt = lastRapid = now; }
    else if ((pad_now & kA) && now - aDownAt >= 250 && now - lastRapid >= 100) { lastRapid = now; fire(true); }
    if (tap_just) {  // tapping aims and fires at once
        sightX = tap_x;
        sightY = tap_y;
        fire();
    }
    static bool bWasDown = false;
    const bool bDown = pad_now & kB;
    if (bWasDown && !bDown) releaseLocks();
    bWasDown = bDown;
    if (now - lastStep < kStepMs) return;
    int steps = min<uint32_t>((now - lastStep) / kStepMs, 3);
    lastStep = now;
    while (steps-- > 0) step();
    if (shields <= 0) {
        gameOver();
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    render(true, false);
    const int64_t t1 = esp_timer_get_time();
    uiFrame.pushSprite(0, 0);
    static uint32_t lastLog = 0;
    if (now - lastLog > 3000) {
        lastLog = now;
        printf("SPACE draw_us=%d push_us=%d wave=%d score=%d\n", int(t1 - t0), int(esp_timer_get_time() - t1), wave, score);
    }
}

void leave() { soundEnd(); }

void icon(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, rgb(4, 6, 16));
    d.drawPixel(x + 6, y + 8, TFT_WHITE);
    d.drawPixel(x + 33, y + 6, TFT_WHITE);
    d.drawPixel(x + 30, y + 33, TFT_WHITE);
    // Neon floor lines.
    const uint16_t g = rgb(160, 50, 200);
    d.drawLine(x + 2, y + 36, x + 38, y + 36, g);
    d.drawLine(x + 8, y + 31, x + 32, y + 31, g);
    d.drawLine(x + 20, y + 28, x + 4, y + 39, g);
    d.drawLine(x + 20, y + 28, x + 36, y + 39, g);
    // A small shaded octahedron.
    const int cx = x + 20, cy = y + 16;
    d.fillTriangle(cx, cy - 11, cx - 10, cy, cx + 3, cy + 3, rgb(90, 255, 160));
    d.fillTriangle(cx, cy - 11, cx + 10, cy, cx + 3, cy + 3, rgb(60, 170, 110));
    d.fillTriangle(cx, cy + 11, cx - 10, cy, cx + 3, cy + 3, rgb(40, 120, 80));
    d.fillTriangle(cx, cy + 11, cx + 10, cy, cx + 3, cy + 3, rgb(30, 90, 60));
    d.drawCircle(cx, cy, 15, rgb(255, 210, 90));
}
}  // namespace

const Game kSpace3D = {"3D宇宙", kKey, enter, tick, icon, leave, GameCategory::play, GameFooter::score};
