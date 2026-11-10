// 3D space shooter "3D宇宙F" for FACES ARCADE (own graphics).
// Cockpit view with flat-shaded solid 3D ships, a ringed planet, waves with a boss, and warp between waves.
// The D-pad moves the sight and steers the ship (the camera follows the sight, so plasma and rocks can be dodged).
// A fires the laser (hold for rapid fire, watch the heat gauge), B drops a bomb. Kills in a row raise the multiplier.
// Nothing flashes: hits show as sparks, a steady tint that fades, and a small shake.
#include <algorithm>
#include "common.h"

namespace {
const int kW = 320, kH = 240, kCx = kW / 2, kCy = 108;
const float kFocal = 220.0f, kNear = 18.0f, kFar = 460.0f;
const uint32_t kStepMs = 33;  // about 30 steps per second; drawing is about 25 fps
const char* const kKey = "space3d_fable";
const int kStars = 80, kObjs = 14, kBits = 64, kRings = 8, kBolts = 4, kStartShields = 3, kMaxShields = 5, kMaxBombs = 3;

enum State { TITLE, PLAY, OVER };
enum Phase { FIGHT, BOSS_FIGHT, WARP };
enum Kind { DRONE, FIGHTER, ROCK, BOSS, PLASMA, CAPSULE };

struct V3 { float x, y, z; };
struct Star { float x, y, z; int tint; };
struct Obj { bool alive; int kind, hp, timer, count; float x, y, z, vx, vy, vz, angle, spin, phase; };
struct Bit { bool alive; float x, y, vx, vy; int life; uint16_t color; };
struct Ring { int life; float x, y, r, grow; uint16_t color; };
struct Bolt { int life; float tx, ty; };

State state;
Phase phase;
Star stars[kStars];
Obj objs[kObjs];
Bit bits[kBits];
Ring rings[kRings];
Bolt bolts[kBolts];
float sightX, sightY, camX, camY, warpF, warpScroll, titleAngle;
int viewX = kCx, viewY = kCy;
int score, shields, wave, kills, quota, chain, chainTimer, heat, bombs, fireCool, hurtFrames, shake;
int bannerFrames, phaseTimer, spawnTimer, target, beat, bossMaxHp, totalKills, drawMs, drawCount;
bool overheated, fireLatch, bombLatch, paused;
char banner[32];
uint32_t lastStep, overAt, frames, statAt;

// Solid models: vertices and triangles. Faces are sorted by depth and shaded by their normal.
struct Model { const V3* v; int nv; const uint8_t (*f)[3]; int nf; };
const V3 kOctaV[6] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
const uint8_t kOctaF[8][3] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1}, {5, 1, 2}, {5, 2, 3}, {5, 3, 4}, {5, 4, 1}};
const V3 kDartV[5] = {{0, 0, -1.6f}, {-1.5f, 0.25f, 0.7f}, {1.5f, 0.25f, 0.7f}, {0, -0.55f, 0.5f}, {0, 0.3f, 0.6f}};
const uint8_t kDartF[6][3] = {{0, 1, 3}, {0, 3, 2}, {0, 2, 4}, {0, 4, 1}, {1, 3, 4}, {3, 2, 4}};
const V3 kRockV[6] = {{0.1f, -1.1f, 0.2f}, {1.2f, 0.1f, -0.2f}, {0.1f, 0.25f, 1.0f},
                      {-0.9f, -0.2f, 0.1f}, {-0.1f, 0.1f, -1.2f}, {0.2f, 0.9f, 0}};
const V3 kSaucerV[8] = {{0, -0.55f, 0}, {0, 0.4f, 0}, {1.6f, 0, 0}, {0.8f, 0, 1.386f},
                        {-0.8f, 0, 1.386f}, {-1.6f, 0, 0}, {-0.8f, 0, -1.386f}, {0.8f, 0, -1.386f}};
const uint8_t kSaucerF[12][3] = {{0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 6}, {0, 6, 7}, {0, 7, 2},
                                 {1, 2, 3}, {1, 3, 4}, {1, 4, 5}, {1, 5, 6}, {1, 6, 7}, {1, 7, 2}};
const Model kModels[4] = {{kOctaV, 6, kOctaF, 8}, {kDartV, 5, kDartF, 6}, {kRockV, 6, kOctaF, 8}, {kSaucerV, 8, kSaucerF, 12}};
const float kSize[6] = {14.0f, 15.0f, 20.0f, 40.0f, 7.0f, 10.0f};  // hit size per kind (world units)
const int kBgm[16] = {45, 45, 57, 45, 48, 48, 60, 48, 43, 43, 55, 43, 50, 50, 62, 52};

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
uint16_t shadeRgb(int r, int g, int b, float s) {
    return rgb(uint8_t(clampi(int(r * s), 0, 255)), uint8_t(clampi(int(g * s), 0, 255)), uint8_t(clampi(int(b * s), 0, 255)));
}

// World to screen. The camera sits at (camX, camY, 0) and looks along +z.
bool project(float x, float y, float z, int& sx, int& sy) {
    if (z < 2.0f) return false;
    sx = viewX + int((x - camX) * kFocal / z);
    sy = viewY + int((y - camY) * kFocal / z);
    return true;
}

void resetStar(Star& s, bool anywhere) {
    s.x = float(arcadeRandom(-420, 420));
    s.y = float(arcadeRandom(-320, 320));
    s.z = anywhere ? float(arcadeRandom(20, int(kFar))) : kFar;
    s.tint = int(arcadeRandom(5));
}

Obj* freeObj() {
    for (auto& o : objs)
        if (!o.alive) return &o;
    return nullptr;
}

void setBanner(const char* s, int framesShown) {
    snprintf(banner, sizeof(banner), "%s", s);
    bannerFrames = framesShown;
}

void addRing(float x, float y, float r, float grow, int life, uint16_t color) {
    for (auto& g : rings)
        if (g.life <= 0) {
            g = {life, x, y, r, grow, color};
            return;
        }
}

void burst(int sx, int sy, uint16_t color, int count, float power) {
    int made = 0;
    for (auto& b : bits) {
        if (b.alive) continue;
        const float a = arcadeRandom(628) / 100.0f, sp = power * (0.35f + arcadeRandom(100) / 100.0f);
        b = {true, float(sx), float(sy), cosf(a) * sp, sinf(a) * sp, 9 + int(arcadeRandom(12)), color};
        if (++made >= count) break;
    }
}

void explode(int sx, int sy, uint16_t color, float scale) {
    burst(sx, sy, color, int(8 * scale) + 4, 2.5f + scale * 1.5f);
    burst(sx, sy, rgb(255, 235, 150), 5, 2.0f + scale);
    addRing(float(sx), float(sy), 4.0f, 2.0f + scale * 1.6f, 9, color);
}

uint16_t kindColor(int kind) {
    switch (kind) {
        case DRONE: return rgb(110, 255, 160);
        case FIGHTER: return rgb(255, 110, 200);
        case ROCK: return rgb(210, 180, 140);
        case BOSS: return rgb(255, 170, 80);
        case PLASMA: return rgb(255, 150, 60);
        default: return rgb(120, 255, 140);
    }
}

float baseSpeed() { return min(7.0f, 3.2f + wave * 0.45f); }

void spawnEnemy(int forceKind = -1) {
    Obj* o = freeObj();
    if (!o) return;
    *o = {};
    o->alive = true;
    const int roll = int(arcadeRandom(100));
    const int fighterChance = min(45, 5 + wave * 10);
    o->kind = forceKind >= 0 ? forceKind : (roll < 20 ? ROCK : (roll < 20 + fighterChance ? FIGHTER : DRONE));
    o->x = camX + float(arcadeRandom(-120, 121));
    o->y = camY + float(arcadeRandom(-75, 76));
    o->z = kFar;
    o->phase = arcadeRandom(628) / 100.0f;
    o->spin = (arcadeRandom(2) ? 1 : -1) * (0.05f + arcadeRandom(6) / 100.0f);
    o->hp = 1;
    o->vz = baseSpeed();
    if (o->kind == ROCK) {
        o->hp = wave >= 3 ? 3 : 2;
        o->vz *= 1.25f;
        // Rocks fly straight at a point near the ship: steer away or shoot them.
        const float steps = kFar / o->vz;
        o->vx = (camX + float(arcadeRandom(-28, 29)) - o->x) / steps;
        o->vy = (camY + float(arcadeRandom(-22, 23)) - o->y) / steps;
    } else if (o->kind == FIGHTER) {
        o->vz *= 1.1f;
        o->timer = 30 + int(arcadeRandom(30));
    }
}

void firePlasma(const Obj& src) {
    Obj* p = freeObj();
    if (!p) return;
    *p = {};
    p->alive = true;
    p->kind = PLASMA;
    p->hp = 1;
    p->x = src.x;
    p->y = src.y;
    p->z = src.z - 10.0f;
    p->vz = min(12.0f, 8.0f + wave * 0.5f);
    const float steps = p->z / p->vz;
    p->vx = (camX - p->x) / steps;  // aimed at where the ship is now: move to dodge
    p->vy = (camY - p->y) / steps;
    soundPlayVolume(0.3f, 0, 80, 67);
}

void startBoss() {
    phase = BOSS_FIGHT;
    Obj* o = freeObj();
    if (!o) o = &objs[0];
    *o = {};
    o->alive = true;
    o->kind = BOSS;
    o->hp = bossMaxHp = 10 + wave * 5;
    o->z = kFar;
    o->timer = 70;
    setBanner("ボス せっきん!", 60);
    soundPlay(0, 300, 36, 43);
    soundPlay(320, 300, 35, 42);
    soundPlay(640, 500, 34, 41);
    printf("SPACE boss wave=%d hp=%d\n", wave, o->hp);
}

void startWarp() {
    phase = WARP;
    phaseTimer = 85;
    for (auto& o : objs) {
        if (!o.alive) continue;
        int sx, sy;
        if (project(o.x, o.y, o.z, sx, sy)) explode(sx, sy, kindColor(o.kind), 1.0f);
        o.alive = false;
    }
    shields = min(kMaxShields, shields + 1);
    bombs = min(kMaxBombs, bombs + 1);
    char b[32];
    snprintf(b, sizeof(b), "WAVE %d クリア!", wave);
    setBanner(b, 70);
    soundPlay(0, 120, 60);
    soundPlay(130, 120, 64);
    soundPlay(260, 120, 67);
    soundPlayTimbre(Timbre::bell, 390, 500, 72, 76, 79);
    printf("SPACE warp wave=%d score=%d\n", wave, score);
}

void hurt() {
    --shields;
    hurtFrames = 16;
    shake = 8;
    chain = 0;
    chainTimer = 0;
    soundPlay(0, 250, 40, 41);
    printf("SPACE hurt shields=%d\n", shields);
}

int multiplier() { return 1 + min(chain, 14) / 2; }

// The player destroyed an object: effects, score and wave progress.
void destroy(Obj& o) {
    int sx = kCx, sy = kCy;
    project(o.x, o.y, o.z, sx, sy);
    const int kind = o.kind;
    const float near = 1.0f - clampf(o.z / kFar, 0.0f, 1.0f);
    o.alive = false;
    if (kind == CAPSULE) {
        if (shields < kMaxShields) ++shields;
        else if (bombs < kMaxBombs) ++bombs;
        else score += 200;
        burst(sx, sy, kindColor(kind), 12, 3.0f);
        addRing(float(sx), float(sy), 6.0f, 3.0f, 10, rgb(120, 255, 140));
        soundPlayTimbre(Timbre::bell, 0, 300, 84, 88);
        return;
    }
    explode(sx, sy, kindColor(kind), kind == BOSS ? 3.0f : 0.6f + near * 1.6f);
    if (kind != PLASMA) {
        ++chain;
        chainTimer = 80;
    }
    const int base = kind == DRONE ? 10 : kind == FIGHTER ? 20 : kind == ROCK ? 30 : kind == PLASMA ? 5 : 300 + wave * 100;
    score += (base + int(near * 10)) * multiplier();
    if (kind == BOSS) {
        for (int i = 0; i < 4; ++i)
            explode(sx + int(arcadeRandom(-40, 41)), sy + int(arcadeRandom(-30, 31)), rgb(255, 200, 90), 2.0f);
        startWarp();
    } else if (kind != PLASMA) {
        ++kills;
        ++totalKills;
        soundPlayVolume(0.7f, 0, 140, 50, 57);
        if (chain > 1) soundPlayVolume(0.45f, 40, 110, 70 + 2 * min(chain, 9));
        if (totalKills % 12 == 0) {  // a repair capsule drifts in now and then
            Obj* c = freeObj();
            if (c) {
                *c = {};
                c->alive = true;
                c->kind = CAPSULE;
                c->hp = 1;
                c->x = camX + float(arcadeRandom(-60, 61));
                c->y = camY + float(arcadeRandom(-40, 41));
                c->z = 300.0f;
                c->vz = 2.6f;
            }
        }
    } else {
        soundPlayVolume(0.5f, 0, 60, 79);
    }
    printf("SPACE hit score=%d chain=%d\n", score, chain);
}

void damage(Obj& o, int amount) {
    o.hp -= amount;
    if (o.hp <= 0) {
        destroy(o);
        return;
    }
    burst(int(sightX), int(sightY), rgb(255, 235, 150), 5, 2.5f);  // sparks where the laser lands
    soundPlayVolume(0.45f, 0, 40, 76);
}

// The nearest object under the sight, or -1.
int findTarget() {
    int best = -1;
    float bestZ = 1e9f;
    for (int i = 0; i < kObjs; ++i) {
        const auto& o = objs[i];
        int sx, sy;
        if (!o.alive || o.z > kFar || !project(o.x, o.y, o.z, sx, sy)) continue;
        const float r = kSize[o.kind] * kFocal / o.z + 8.0f;
        const float dx = sx - sightX, dy = sy - sightY;
        if (dx * dx + dy * dy <= r * r && o.z < bestZ) {
            best = i;
            bestZ = o.z;
        }
    }
    return best;
}

void fire() {
    if (overheated) return;
    fireCool = 5;
    heat += 10;
    if (heat >= 100) {
        heat = 100;
        overheated = true;
        soundPlayVolume(0.5f, 0, 220, 45);
    }
    for (auto& b : bolts)
        if (b.life <= 0) {
            b = {3, sightX, sightY};
            break;
        }
    soundPlayVolume(0.3f, 0, 50, 93);  // plays often, so it stays quiet
    const int hit = findTarget();
    if (hit >= 0) damage(objs[hit], 1);
}

void bomb() {
    if (bombs <= 0) return;
    --bombs;
    addRing(float(kCx), float(kCy), 10.0f, 13.0f, 18, rgb(120, 230, 255));
    addRing(float(kCx), float(kCy), 4.0f, 9.0f, 18, rgb(255, 235, 150));
    soundPlay(0, 500, 31, 38);
    soundPlayVolume(0.6f, 80, 300, 43);
    for (auto& o : objs) {
        if (!o.alive || o.kind == CAPSULE) continue;
        if (o.kind == BOSS) damage(o, 6);
        else destroy(o);
    }
    puts("SPACE bomb");
}

void resetRound() {
    for (auto& s : stars) resetStar(s, true);
    for (auto& o : objs) o.alive = false;
    for (auto& b : bits) b.alive = false;
    for (auto& g : rings) g.life = 0;
    for (auto& b : bolts) b.life = 0;
    sightX = kCx;
    sightY = kCy;
    camX = camY = 0;
    warpF = 1.0f;
    warpScroll = 0;
    score = chain = chainTimer = heat = fireCool = hurtFrames = shake = kills = totalKills = beat = 0;
    shields = kStartShields;
    bombs = 2;
    wave = 1;
    quota = 10;
    phase = FIGHT;
    spawnTimer = 40;
    target = -1;
    overheated = fireLatch = bombLatch = paused = false;
    bannerFrames = 0;
    drawMs = drawCount = 0;
}

void moveStars(float speed) {
    for (auto& s : stars) {
        s.z -= speed;
        if (s.z < 4) resetStar(s, false);
    }
}

void step() {
    ++frames;
    const float move = 5.0f;
    if (pad_now & kLeft) sightX -= move;
    if (pad_now & kRight) sightX += move;
    if (pad_now & kUp) sightY -= move;
    if (pad_now & kDown) sightY += move;
    sightX = clampf(sightX, 14.0f, kW - 14.0f);
    sightY = clampf(sightY, 30.0f, kH - 44.0f);
    // The ship drifts toward where the sight points, so aiming also steers.
    camX += ((sightX - kCx) * 0.38f - camX) * 0.2f;
    camY += ((sightY - kCy) * 0.38f - camY) * 0.2f;

    if (fireCool > 0) --fireCool;
    const bool held = pad_now & kA;
    if (fireLatch || (held && fireCool == 0)) fire();
    fireLatch = false;
    heat = max(0, heat - (held && !overheated ? 1 : 3));
    if (overheated && heat <= 30) overheated = false;
    if (bombLatch) bomb();
    bombLatch = false;
    if (hurtFrames > 0) --hurtFrames;
    if (shake > 0) --shake;
    if (bannerFrames > 0) --bannerFrames;
    if (chainTimer > 0 && --chainTimer == 0) chain = 0;

    warpF += ((phase == WARP ? 5.0f : 1.0f) - warpF) * 0.12f;
    moveStars(baseSpeed() * 2.2f * warpF);

    for (auto& o : objs) {
        if (!o.alive) continue;
        switch (o.kind) {
            case DRONE:
            case FIGHTER: {
                const float k = o.vz / max(o.z, 30.0f);  // closes on the ship: has to be shot
                o.x += (camX - o.x) * k;
                o.y += (camY - o.y) * k;
                if (o.kind == FIGHTER) {
                    o.phase += 0.12f;
                    o.x += cosf(o.phase) * 2.6f;
                    if (--o.timer <= 0 && o.z > 150.0f && o.z < 400.0f) {
                        firePlasma(o);
                        o.timer = 55;
                    }
                } else {
                    o.phase += 0.08f;
                    o.x += cosf(o.phase) * 1.2f;
                    o.y += sinf(o.phase) * 1.2f;
                }
                o.angle += o.spin;
                o.z -= o.vz;
                if (o.z < kNear) {
                    o.alive = false;
                    hurt();
                }
                break;
            }
            case ROCK:
            case PLASMA: {
                o.x += o.vx;
                o.y += o.vy;
                o.angle += o.spin;
                o.z -= o.vz;
                if (o.z < kNear) {
                    o.alive = false;
                    const float rx = o.kind == ROCK ? 24.0f : 16.0f, ry = o.kind == ROCK ? 20.0f : 14.0f;
                    if (fabsf(o.x - camX) < rx && fabsf(o.y - camY) < ry) hurt();
                    else soundPlayVolume(0.25f, 0, 90, 55);  // it whooshes past
                }
                break;
            }
            case CAPSULE:
                o.angle += 0.1f;
                o.z -= o.vz;
                if (o.z < kNear) o.alive = false;
                break;
            case BOSS:
                o.phase += 0.035f;
                o.angle += 0.05f;
                if (o.z > 175.0f) o.z -= 4.0f;
                o.x = sinf(o.phase) * 85.0f;
                o.y = sinf(o.phase * 1.7f) * 38.0f - 5.0f;
                if (--o.timer <= 0 && o.z <= 180.0f) {
                    firePlasma(o);
                    o.timer = max(20, 48 - wave * 4);
                    if (++o.count % 3 == 0) spawnEnemy(wave >= 2 ? FIGHTER : DRONE);
                }
                break;
        }
    }
    for (auto& b : bits) {
        if (!b.alive) continue;
        b.x += b.vx;
        b.y += b.vy;
        if (--b.life <= 0) b.alive = false;
    }
    for (auto& g : rings)
        if (g.life > 0) {
            --g.life;
            g.r += g.grow;
        }
    for (auto& b : bolts)
        if (b.life > 0) --b.life;

    if (phase == FIGHT) {
        if (kills >= quota) startBoss();
        else if (--spawnTimer <= 0) {
            spawnEnemy();
            spawnTimer = max(11, 40 - wave * 4) + int(arcadeRandom(8));
        }
    } else if (phase == WARP) {
        warpScroll += 22.0f;
        if (--phaseTimer <= 0) {
            ++wave;
            kills = 0;
            quota = 8 + wave * 2;
            phase = FIGHT;
            spawnTimer = 25;
            char b[24];
            snprintf(b, sizeof(b), "WAVE %d", wave);
            setBanner(b, 50);
            soundPlayVolume(0.6f, 0, 120, 67);
            soundPlayVolume(0.6f, 130, 120, 72);
            soundPlayVolume(0.6f, 260, 240, 79);
        }
    }
    target = findTarget();

    // A quiet bass line keeps the pace; it speeds up for the boss and climbs during the warp.
    const int every = phase == BOSS_FIGHT ? 6 : 8;
    if (frames % every == 0) {
        const int note = phase == WARP ? 57 + (beat % 8) * 2 : kBgm[beat % 16];
        soundPlayVolume(0.16f, 0, 110, note);
        ++beat;
    }
}

void drawModel(const Model& m, float ox, float oy, float oz, float size, float yaw, float pitch, float roll,
               int cr, int cg, int cb, float fog) {
    auto& d = uiFrame;
    V3 w[8];
    int px[8], py[8];
    const float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), crl = cosf(roll), srl = sinf(roll);
    for (int i = 0; i < m.nv; ++i) {
        const float x1 = m.v[i].x * cyw - m.v[i].z * syw, z1 = m.v[i].x * syw + m.v[i].z * cyw;
        const float y2 = m.v[i].y * cp - z1 * sp, z2 = m.v[i].y * sp + z1 * cp;
        const float x3 = x1 * crl - y2 * srl, y3 = x1 * srl + y2 * crl;
        w[i] = {ox + x3 * size, oy + y3 * size, oz + z2 * size};
        if (w[i].z < 4.0f) return;
        px[i] = viewX + int(w[i].x * kFocal / w[i].z);
        py[i] = viewY + int(w[i].y * kFocal / w[i].z);
    }
    int order[12];
    float depth[12], shade[12];
    for (int i = 0; i < m.nf; ++i) {
        const V3 &a = w[m.f[i][0]], &b = w[m.f[i][1]], &c = w[m.f[i][2]];
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float gx = (a.x + b.x + c.x) / 3, gy = (a.y + b.y + c.y) / 3, gz = (a.z + b.z + c.z) / 3;
        if (nx * gx + ny * gy + nz * gz > 0) {  // make the normal face the camera
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }
        const float len = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f;
        const float lit = (nx * -0.40f + ny * -0.70f + nz * -0.59f) / len;  // light from the upper left, behind the camera
        shade[i] = (0.28f + 0.72f * max(0.0f, lit)) * fog;
        depth[i] = gz;
        order[i] = i;
    }
    for (int i = 1; i < m.nf; ++i)  // far faces first
        for (int j = i; j > 0 && depth[order[j]] > depth[order[j - 1]]; --j) std::swap(order[j], order[j - 1]);
    for (int k = 0; k < m.nf; ++k) {
        const int i = order[k];
        const int a = m.f[i][0], b = m.f[i][1], c = m.f[i][2];
        d.fillTriangle(px[a], py[a], px[b], py[b], px[c], py[c], shadeRgb(cr, cg, cb, shade[i]));
        d.drawTriangle(px[a], py[a], px[b], py[b], px[c], py[c], shadeRgb(cr, cg, cb, shade[i] * 1.2f + 0.12f * fog));
    }
}

void drawObj(const Obj& o) {
    auto& d = uiFrame;
    const float x = o.x - camX, y = o.y - camY;
    const float fog = 1.0f - clampf((o.z - kNear) / (kFar - kNear), 0.0f, 1.0f) * 0.65f;
    int sx, sy;
    switch (o.kind) {
        case DRONE:
            drawModel(kModels[0], x, y, o.z, 14.0f, o.angle, 0.4f, 0, 110, 255, 160, fog);
            break;
        case FIGHTER:
            drawModel(kModels[1], x, y, o.z, 13.0f, sinf(o.phase) * 0.5f, 0.25f, cosf(o.phase) * 0.6f, 255, 110, 200, fog);
            break;
        case ROCK:
            drawModel(kModels[2], x, y, o.z, 20.0f, o.angle, o.angle * 0.7f, 0, 210, 180, 140, fog);
            break;
        case BOSS: {
            // The hull slowly turns from steel blue to red as it takes damage.
            const float hurtT = 1.0f - float(o.hp) / float(max(1, bossMaxHp));
            drawModel(kModels[3], x, y, o.z, 34.0f, o.angle, 0.5f + sinf(o.phase * 2.0f) * 0.15f, 0,
                      int(150 + 105 * hurtT), int(170 - 80 * hurtT), int(230 - 150 * hurtT), fog);
            if (project(o.x, o.y, o.z, sx, sy)) {
                const int r = max(3, int(11.0f * kFocal / o.z));
                d.fillCircle(sx, sy, r, rgb(255, 150, 60));
                d.fillCircle(sx, sy, r / 2, rgb(255, 240, 180));
            }
            break;
        }
        case PLASMA:
            if (project(o.x, o.y, o.z, sx, sy)) {
                const int r = max(2, int(6.0f * kFocal / o.z));
                d.fillCircle(sx, sy, r, rgb(255, 130, 40));
                d.fillCircle(sx, sy, max(1, r / 2), rgb(255, 240, 170));
                // A steady red ring marks plasma that will hit unless the ship moves.
                const float steps = (o.z - kNear) / o.vz;
                if (fabsf(o.x + o.vx * steps - camX) < 16.0f && fabsf(o.y + o.vy * steps - camY) < 14.0f)
                    d.drawCircle(sx, sy, r + 4, rgb(255, 70, 70));
            }
            break;
        default:
            if (project(o.x, o.y, o.z, sx, sy)) {
                const int r = max(4, int(9.0f * kFocal / o.z));
                d.fillCircle(sx, sy, r, rgb(40, 150, 80));
                d.drawCircle(sx, sy, r, rgb(160, 255, 180));
                const int a = max(1, r / 4), l = max(3, r * 2 / 3);
                d.fillRect(sx - a, sy - l, a * 2 + 1, l * 2 + 1, TFT_WHITE);
                d.fillRect(sx - l, sy - a, l * 2 + 1, a * 2 + 1, TFT_WHITE);
            }
            break;
    }
}

// A ringed gas giant far away. Its colours change with the wave.
void drawPlanetRing(int px, int py, int R, bool front, uint16_t color) {
    auto& d = uiFrame;
    const float ct = cosf(-0.35f), st = sinf(-0.35f);
    for (int pass = 0; pass < 2; ++pass) {
        const float a = R * (pass ? 1.65f : 1.95f), b = R * (pass ? 0.38f : 0.46f);
        int lx = 0, ly = 0;
        for (int i = 0; i <= 32; ++i) {
            const float t = i * (2 * PI / 32);
            const float ex = cosf(t) * a, ey = sinf(t) * b;
            const int x = px + int(ex * ct - ey * st), y = py + int(ex * st + ey * ct);
            const bool isFront = sinf(t - PI / 32) > 0;
            if (i > 0 && isFront == front) d.drawLine(lx, ly, x, y, color);
            lx = x;
            ly = y;
        }
    }
}

void drawPlanet() {
    auto& d = uiFrame;
    static const uint8_t kPal[3][3] = {{230, 150, 90}, {110, 170, 240}, {190, 120, 230}};
    const uint8_t* p = kPal[(wave - 1) % 3];
    const int R = 34, px = 238 - int(camX * 0.12f) + (viewX - kCx), py = 66 - int(camY * 0.12f) + (viewY - kCy);
    const uint16_t ringColor = shadeRgb(p[0], p[1], p[2], 0.7f);
    drawPlanetRing(px, py, R, false, ringColor);
    for (int dy = -R; dy <= R; ++dy) {
        const int w = int(sqrtf(float(R * R - dy * dy)));
        const float band = ((dy + R) / 7) & 1 ? 0.82f : 1.0f;
        d.drawFastHLine(px - w, py + dy, 2 * w + 1, shadeRgb(p[0], p[1], p[2], 0.22f * band));
        d.drawFastHLine(px - w, py + dy, int(w * 1.3f), shadeRgb(p[0], p[1], p[2], 0.62f * band));
        d.drawFastHLine(px - w, py + dy, int(w * 0.7f), shadeRgb(p[0], p[1], p[2], 0.8f * band));
    }
    drawPlanetRing(px, py, R, true, ringColor);
}

// Hexagon gates rush past during the warp between waves.
void drawTunnel() {
    auto& d = uiFrame;
    const float scroll = fmodf(warpScroll, 420.0f);
    const float strength = clampf((warpF - 1.3f) / 2.0f, 0.0f, 1.0f);
    for (int i = 0; i < 6; ++i) {
        const float z = 30.0f + fmodf(i * 70.0f + 420.0f - scroll, 420.0f);
        const float fog = (1.0f - z / 460.0f) * strength;
        const uint16_t c = shadeRgb(120, 200, 255, fog);
        int lx = 0, ly = 0;
        for (int k = 0; k <= 6; ++k) {
            const float a = k * (PI / 3) + z * 0.004f;
            const int x = viewX + int(cosf(a) * 95.0f * kFocal / z), y = viewY + int(sinf(a) * 95.0f * kFocal / z);
            if (k > 0) d.drawLine(lx, ly, x, y, c);
            lx = x;
            ly = y;
        }
    }
}

void drawCockpit() {
    auto& d = uiFrame;
    // A hit tints the frame orange and the tint fades out smoothly (no flashing).
    const float t = hurtFrames / 16.0f;
    const uint16_t fc = rgb(uint8_t(30 + 170 * t), uint8_t(50 + 40 * t), uint8_t(80 - 40 * t));
    const uint16_t edge = rgb(uint8_t(70 + 150 * t), uint8_t(110 + 20 * t), uint8_t(160 - 80 * t));
    d.fillTriangle(0, kH, 0, kH - 46, 84, kH, fc);
    d.fillTriangle(kW, kH, kW, kH - 46, kW - 84, kH, fc);
    d.drawLine(0, kH - 46, 84, kH, edge);
    d.drawLine(kW, kH - 46, kW - 84, kH, edge);
    d.fillTriangle(98, kH, 114, kH - 32, 206, kH - 32, fc);
    d.fillTriangle(98, kH, 206, kH - 32, 222, kH, fc);
    d.drawLine(98, kH, 114, kH - 32, edge);
    d.drawLine(114, kH - 32, 206, kH - 32, edge);
    d.drawLine(206, kH - 32, 222, kH, edge);
    // Radar: left-right is the bearing, up is far away.
    d.fillRect(120, kH - 28, 80, 26, rgb(6, 20, 26));
    d.drawRect(120, kH - 28, 80, 26, rgb(40, 110, 120));
    d.drawFastVLine(160, kH - 27, 24, rgb(24, 60, 70));
    for (const auto& o : objs) {
        if (!o.alive) continue;
        const int rx = clampi(160 + int((o.x - camX) * 0.28f), 122, 197);
        const int ry = clampi(kH - 5 - int(o.z / kFar * 20.0f), kH - 26, kH - 5);
        if (o.kind == BOSS) d.fillRect(rx - 3, ry - 2, 7, 4, kindColor(o.kind));
        else d.fillRect(rx - 1, ry - 1, 3, 3, kindColor(o.kind));
    }
    // Laser heat on the left, bombs on the right.
    d.fillRect(6, kH - 11, 52, 7, rgb(10, 20, 36));
    const uint16_t hc = overheated ? rgb(240, 70, 60) : heat > 70 ? rgb(255, 170, 60) : rgb(70, 220, 240);
    d.fillRect(7, kH - 10, heat / 2, 5, hc);
    for (int i = 0; i < kMaxBombs; ++i)
        d.fillCircle(kW - 12 - i * 15, kH - 8, 5, i < bombs ? rgb(255, 208, 70) : rgb(16, 28, 48));
}

void drawHud() {
    auto& d = uiFrame;
    char b[24];
    snprintf(b, sizeof(b), "%d", score);
    text(d, b, 8, 4, pal::kText, F24, top_left);
    if (multiplier() > 1) {
        snprintf(b, sizeof(b), "x%d", multiplier());
        text(d, b, 8, 30, pal::kAccent, F24, top_left);
        d.fillRect(8, 56, chainTimer / 2, 3, pal::kAccent);
    }
    const int cells = max(kStartShields, shields);
    for (int i = 0; i < cells; ++i)
        d.fillSmoothRoundRect(316 - (cells - i) * 22, 10, 17, 12, 4, i < shields ? pal::kCyan : rgb(38, 57, 90));
    for (const auto& o : objs)
        if (o.alive && o.kind == BOSS) {
            d.fillRect(110, 30, 100, 8, rgb(40, 20, 30));
            d.fillRect(111, 31, 98 * max(0, o.hp) / max(1, bossMaxHp), 6, rgb(255, 120, 70));
            d.drawRect(110, 30, 100, 8, rgb(200, 200, 220));
        }
    if (bannerFrames > 0) text(d, banner, kCx, 62, pal::kAccent, F32, top_center);
}

void render(bool showSight, bool cockpit, bool push) {
    auto& d = uiFrame;
    viewX = kCx + (shake > 0 ? ((frames & 1) ? shake / 2 : -shake / 2) : 0);
    viewY = kCy + (shake > 0 ? ((frames & 2) ? shake / 3 : -shake / 3) : 0);
    d.fillScreen(rgb(4, 6, 16));
    drawPlanet();
    const float streak = baseSpeed() * 2.2f * warpF * 1.6f;
    for (const auto& s : stars) {
        int sx, sy;
        if (!project(s.x, s.y, s.z, sx, sy)) continue;
        const int bright = 255 - int(s.z * 190 / kFar);
        const uint16_t c = s.tint == 0 ? rgb(uint8_t(bright), uint8_t(bright * 3 / 4), uint8_t(bright / 2))
                         : s.tint == 1 ? rgb(uint8_t(bright / 2), uint8_t(bright * 7 / 8), uint8_t(bright))
                                       : rgb(uint8_t(bright), uint8_t(bright), uint8_t(min(255, bright + 30)));
        int ex, ey;
        if (warpF > 1.3f && project(s.x, s.y, s.z + streak, ex, ey)) d.drawLine(sx, sy, ex, ey, c);
        else if (s.z < 90) d.fillRect(sx, sy, 2, 2, c);
        else d.drawPixel(sx, sy, c);
    }
    if (warpF > 1.3f) drawTunnel();
    // Far objects first so near ones are on top.
    int order[kObjs], n = 0;
    for (int i = 0; i < kObjs; ++i)
        if (objs[i].alive) order[n++] = i;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && objs[order[j]].z > objs[order[j - 1]].z; --j) std::swap(order[j], order[j - 1]);
    for (int i = 0; i < n; ++i) drawObj(objs[order[i]]);
    for (const auto& g : rings)
        if (g.life > 0) {
            d.drawCircle(int(g.x), int(g.y), int(g.r), g.color);
            if (g.r > 6) d.drawCircle(int(g.x), int(g.y), int(g.r) - 2, g.color);
        }
    for (const auto& b : bits)
        if (b.alive) {
            const int sz = b.life > 8 ? 3 : 2;
            d.fillRect(int(b.x), int(b.y), sz, sz, b.color);
        }
    // Laser bolts travel from the two cannons to the sight.
    for (const auto& b : bolts) {
        if (b.life <= 0) continue;
        const float t0 = (3 - b.life) * 0.3f, t1 = t0 + 0.4f;
        const uint16_t c = rgb(130, 255, 220);
        for (int side = 0; side < 2; ++side) {
            const float gx = side ? kW - 30.0f : 30.0f, gy = kH - 24.0f;
            const int x0 = int(gx + (b.tx - gx) * t0), y0 = int(gy + (b.ty - gy) * t0);
            const int x1 = int(gx + (b.tx - gx) * t1), y1 = int(gy + (b.ty - gy) * t1);
            d.drawLine(x0, y0, x1, y1, c);
            d.drawLine(x0 + 1, y0, x1 + 1, y1, c);
            d.drawLine(x0, y0 + 1, x1, y1 + 1, TFT_WHITE);
        }
    }
    if (showSight) {
        const int x = int(sightX), y = int(sightY);
        const uint16_t sc = overheated ? rgb(130, 130, 140) : target >= 0 ? rgb(255, 110, 90) : rgb(255, 210, 90);
        d.drawCircle(x, y, 12, sc);
        d.drawFastHLine(x - 20, y, 10, sc);
        d.drawFastHLine(x + 11, y, 10, sc);
        d.drawFastVLine(x, y - 20, 10, sc);
        d.drawFastVLine(x, y + 11, 10, sc);
        d.fillRect(x - 1, y - 1, 3, 3, sc);
        if (target >= 0 && !overheated) {  // lock-on brackets
            for (int sxn = -1; sxn <= 1; sxn += 2)
                for (int syn = -1; syn <= 1; syn += 2) {
                    d.drawFastHLine(sxn > 0 ? x + 13 : x - 19, y + syn * 19, 7, sc);
                    d.drawFastVLine(x + sxn * 19, syn > 0 ? y + 13 : y - 19, 7, sc);
                }
        }
    }
    if (cockpit) {
        drawCockpit();
        drawHud();
    }
    if (push) d.pushSprite(0, 0);  // title, pause and game-over draw their text first, then push once
}

void renderTitle() {
    render(false, false, false);
    auto& d = uiFrame;
    drawModel(kModels[0], -72, 6, 130, 13.0f, titleAngle * 1.3f, 0.4f, 0, 110, 255, 160, 1.0f);
    drawModel(kModels[1], 0, 6, 120, 13.0f, titleAngle, 0.3f, sinf(titleAngle * 0.7f) * 0.5f, 255, 110, 200, 1.0f);
    drawModel(kModels[3], 72, 6, 130, 10.0f, -titleAngle, 0.6f, 0, 150, 170, 230, 1.0f);
    text(d, "3D宇宙F", 160, 12, pal::kText, F40, top_center);
    char b[24];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(d, b, 160, 56, pal::kAccent, F24, top_center);
    text(d, "十字:ねらう・よける", 160, 146, pal::kText, F24, top_center);
    text(d, "A:レーザー B:ボム", 160, 174, pal::kText, F24, top_center);
    text(d, "A:はじめる", 160, 208, pal::kCyan, F24, top_center);
    d.pushSprite(0, 0);
}

void gameOver() {
    state = OVER;
    overAt = lgfx::millis();
    const bool best = submitScore(kKey, score);
    shake = 0;
    render(false, true, false);
    auto& d = uiFrame;
    d.fillSmoothRoundRect(40, 44, 240, 148, 18, rgb(12, 18, 40));
    d.drawRoundRect(40, 44, 240, 148, 18, pal::kCyan);
    text(d, "ゲームオーバー", 160, 54, pal::kText, F32, top_center);
    char b[48];
    snprintf(b, sizeof(b), best ? "NEW BEST %d" : "スコア %d", score);
    text(d, b, 160, 94, best ? pal::kAccent : pal::kText, F24, top_center);
    snprintf(b, sizeof(b), "WAVE %d  げきは %d", wave, totalKills);
    text(d, b, 160, 124, pal::kDim, F24, top_center);
    text(d, "A:もういちど", 160, 156, pal::kCyan, F24, top_center);
    d.pushSprite(0, 0);
    soundPlay(0, 200, 64);
    soundPlay(220, 200, 60);
    soundPlay(440, 400, 55);
    printf("SPACE over score=%d wave=%d\n", score, wave);
}

void enter() {
    soundBegin();
    resetRound();
    state = TITLE;
    titleAngle = 0;
    lastStep = lgfx::millis();
    renderTitle();
}

void tick() {
    const uint32_t now = lgfx::millis();
    if (state != PLAY) {
        if (((pad_just & (kA | kStart)) || tap_just) && (state == TITLE || now - overAt > 700)) {
            resetRound();
            state = PLAY;
            lastStep = statAt = now;
            setBanner("WAVE 1", 50);
            soundPlayVolume(0.6f, 0, 120, 67);
            soundPlayVolume(0.6f, 130, 120, 72);
            soundPlayVolume(0.6f, 260, 240, 79);
            puts("SPACE start");
        } else if (state == TITLE && now - lastStep >= kStepMs) {
            lastStep = now;
            ++frames;
            titleAngle += 0.05f;
            moveStars(7.0f);
            renderTitle();
        }
        return;
    }
    if (pad_just & kStart) {
        paused = !paused;
        lastStep = now;
        if (paused) {
            render(false, true, false);
            auto& d = uiFrame;
            d.fillSmoothRoundRect(60, 76, 200, 84, 16, rgb(12, 18, 40));
            d.drawRoundRect(60, 76, 200, 84, 16, pal::kCyan);
            text(d, "ポーズ", 160, 84, pal::kText, F32, top_center);
            text(d, "START:つづける", 160, 124, pal::kDim, F24, top_center);
            d.pushSprite(0, 0);
        }
        return;
    }
    if (paused) return;
    if (pad_just & kA) fireLatch = true;
    if (pad_just & kB) bombLatch = true;
    if (tap_just) {  // tapping aims and fires at once
        sightX = clampf(float(tap_x), 14.0f, kW - 14.0f);
        sightY = clampf(float(tap_y), 30.0f, kH - 44.0f);
        fireLatch = true;
    }
    if (now - lastStep < kStepMs) return;
    int steps = int(min<uint32_t>((now - lastStep) / kStepMs, 3));
    lastStep = now;
    while (steps-- > 0 && shields > 0) step();
    if (shields <= 0) {
        gameOver();
        return;
    }
    const uint32_t t0 = lgfx::millis();
    render(true, true, true);
    drawMs += int(lgfx::millis() - t0);
    if (++drawCount >= 100) {
        const uint32_t t1 = lgfx::millis();
        printf("SPACE fps=%d frame=%dms wave=%d\n", int(100000 / max<uint32_t>(1, t1 - statAt)), drawMs / 100, wave);
        statAt = t1;
        drawMs = drawCount = 0;
    }
}

void leave() { soundEnd(); }

void icon(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, rgb(4, 6, 16));
    d.drawPixel(x + 6, y + 8, TFT_WHITE);
    d.drawPixel(x + 33, y + 6, TFT_WHITE);
    d.drawPixel(x + 30, y + 33, TFT_WHITE);
    // A small solid octahedron inside the sight.
    const int cx = x + 20, cy = y + 19;
    d.fillTriangle(cx, cy - 12, cx - 11, cy, cx + 3, cy + 4, rgb(110, 255, 160));
    d.fillTriangle(cx, cy - 12, cx + 11, cy, cx + 3, cy + 4, rgb(60, 170, 100));
    d.fillTriangle(cx, cy + 12, cx - 11, cy, cx + 3, cy + 4, rgb(50, 130, 80));
    d.fillTriangle(cx, cy + 12, cx + 11, cy, cx + 3, cy + 4, rgb(30, 80, 50));
    d.drawCircle(x + 20, y + 19, 16, rgb(255, 210, 90));
}
}  // namespace

const Game kSpace3DFable = {"3D宇宙F", kKey, enter, tick, icon, leave, GameCategory::play, GameFooter::score};
