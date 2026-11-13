// Math drills: addition, factoring (fill in the integers) and square roots.
// A number keypad sits at the bottom: tap a key, or move with the D-pad and press A.
// Ten correct answers in a row finish the drill and return to the menu.
#include "common.h"
#include "ui_icons.h"

namespace {
enum State { MODE_SELECT, ASK, RIGHT, WRONG, DONE };
enum PartType : uint8_t { P_TEXT, P_SUP, P_BLANK, P_ROOT_START, P_ROOT_END, P_NEWLINE, P_MINUS };
enum Mode { M_ADD, M_FACTOR, M_ROOT, M_MIX };
const int kGoal = 10;
const int kBarH = 40;
const uint32_t kWrongShowMs = 2600;
const uint32_t kRightShowMs = 1000;
const int kKeyCount = 12;
const char* const kModeNames[4] = {"たし算", "因数分解", "ルート", "まぜる"};
const char* const kModeLog[4]   = {"add", "factor", "root", "mix"};
const char* const kTimes        = "\xC3\x97";      // U+00D7
const uint16_t kBarBg           = rgb(7, 9, 18);
const uint16_t kKeyFill         = rgb(20, 50, 74);
const uint16_t kKeyBorder       = rgb(44, 90, 122);
const uint16_t kSelFill         = rgb(79, 209, 255);
const uint16_t kSelText         = rgb(3, 32, 43);
const uint16_t kOkGreen         = rgb(91, 227, 138);
const uint16_t kNgOrange        = rgb(255, 157, 70);

struct Part {
    PartType type;
    char s[24];
    uint8_t idx;  // blank number
    int16_t x, w;
    uint8_t line;
};
struct Blank {
    char buf[5];
    int len, maxLen;
    int x, y, w, h;
};

State state;
int mode = M_ADD, lastMode = 0, curMode, curType, curLevel;
int streak, selected, keyCursor;
uint32_t feedbackAt;
bool dirty;
Part parts[16];
int nParts, nLines;
Blank blanks[2];
int nBlanks, active;
int answers[2], nAnswers;
bool swappable;
const lgfx::IFont* exprFont;
int exprFh;
int lineCy[3];
int rootX0[3], rootX1[3], rootLine[3], nRoots;

// ---------- layout helpers
int textW(const char* s, const lgfx::IFont* f) {
    uiFrame.setFont(f);
    return uiFrame.textWidth(s);
}

void addPart(PartType t, const char* s = "", int idx = 0) {
    if (nParts >= 16) return;
    Part& p = parts[nParts++];
    p.type  = t;
    p.idx   = idx;
    p.x = p.w = 0;
    p.line  = 0;
    strlcpy(p.s, s, sizeof(p.s));
}
void addText(const char* s) { addPart(P_TEXT, s); }
// The font has no U+2212, so the minus sign is drawn as a line.
void addMinus() { addPart(P_MINUS); }
void addSign(bool minus) {
    if (minus) addMinus();
    else addText("+");
}
void addBlank(int maxLen) {
    blanks[nBlanks] = {};
    blanks[nBlanks].maxLen = maxLen;
    addPart(P_BLANK, "", nBlanks);
    ++nBlanks;
}

// Positions every part for the font `f`; returns the widest line.
int layoutWith(const lgfx::IFont* f) {
    uiFrame.setFont(f);
    const int fh = uiFrame.fontHeight(), digit = uiFrame.textWidth("0");
    int lineW[3] = {0, 0, 0};
    int line = 0;
    for (int i = 0; i < nParts; ++i) {
        Part& p = parts[i];
        p.line  = line;
        switch (p.type) {
            case P_TEXT: p.w = textW(p.s, f); break;
            case P_SUP: p.w = textW(p.s, F24); break;
            case P_BLANK: p.w = digit * blanks[p.idx].maxLen + 16; break;
            case P_ROOT_START: p.w = 15; break;
            case P_ROOT_END: p.w = 4; break;
            case P_MINUS: p.w = textW("+", f); break;
            case P_NEWLINE: p.w = 0; ++line; continue;
        }
        p.x = lineW[line];
        lineW[line] += p.w;
    }
    nLines   = line + 1;
    int maxW = 0;
    for (int l = 0; l < nLines; ++l) maxW = max(maxW, lineW[l]);
    for (int i = 0; i < nParts; ++i)
        if (parts[i].type != P_NEWLINE) parts[i].x += (320 - lineW[parts[i].line]) / 2;
    exprFont = f;
    exprFh   = fh;
    const int lineH = fh + 8, total = (nLines - 1) * lineH + fh;
    for (int l = 0; l < nLines; ++l) lineCy[l] = 92 - total / 2 + fh / 2 + l * lineH;
    nRoots = 0;
    int open = -1;
    for (int i = 0; i < nParts; ++i) {
        if (parts[i].type == P_ROOT_START) open = i;
        if (parts[i].type == P_ROOT_END && open >= 0 && nRoots < 3) {
            rootX0[nRoots]   = parts[open].x;
            rootX1[nRoots]   = parts[i].x + 3;
            rootLine[nRoots] = parts[open].line;
            ++nRoots;
            open = -1;
        }
    }
    for (int i = 0; i < nParts; ++i) {
        if (parts[i].type != P_BLANK) continue;
        Blank& b = blanks[parts[i].idx];
        b.x = parts[i].x;
        b.w = parts[i].w;
        b.h = fh + 4;
        b.y = lineCy[parts[i].line] - b.h / 2;
    }
    return maxW;
}

void layout() {
    if (layoutWith(F40) > 304) layoutWith(F32);
}

// ---------- question generation
void addExpressionAdd(int type) {
    int a, b;
    if (type == 0) {
        a = arcadeRandom(10, 100);
        b = arcadeRandom(10, 100);
    } else if (type == 1) {
        a = arcadeRandom(100, 1000);
        b = arcadeRandom(10, 100);
        if (arcadeRandom(2)) std::swap(a, b);
    } else {
        a = arcadeRandom(100, 1000);
        b = arcadeRandom(100, 1000);
    }
    char s[24];
    snprintf(s, sizeof(s), "%d + %d = ", a, b);
    addText(s);
    addBlank(4);
    answers[0] = a + b;
    nAnswers   = 1;
}

void addExpressionFactor(int type) {
    int p = arcadeRandom(1, 10), q = arcadeRandom(1, 10);
    if (type == 2)
        while (q == p) q = arcadeRandom(1, 10);
    const int coef = type == 2 ? abs(p - q) : p + q;
    const bool midMinus = type == 1 || (type == 2 && p < q);
    char s[24];
    addText("x");
    addPart(P_SUP, "2");
    addSign(midMinus);
    if (coef != 1) {
        snprintf(s, sizeof(s), "%d", coef);
        addText(s);
    }
    addText("x");
    addSign(type == 2);
    snprintf(s, sizeof(s), "%d", p * q);
    addText(s);
    addPart(P_NEWLINE);
    addText("=(x");
    addSign(type == 1);
    addBlank(1);
    addText(")(x");
    addSign(type != 0);
    addBlank(1);
    addText(")");
    answers[0] = p;
    answers[1] = q;
    nAnswers   = 2;
    swappable  = type != 2;
}

const int kRootB[5] = {2, 3, 5, 6, 7};

void addExpressionRoot(int type) {
    char s[24];
    if (type == 0) {
        const int n = arcadeRandom(2, 16);
        snprintf(s, sizeof(s), "%d", n * n);
        addPart(P_ROOT_START);
        addText(s);
        addPart(P_ROOT_END);
        addText(" = ");
        addBlank(2);
        answers[0] = n;
        nAnswers   = 1;
    } else if (type == 1) {
        const int b = kRootB[arcadeRandom(5)], m = arcadeRandom(1, 5), n = arcadeRandom(1, 5);
        addPart(P_ROOT_START);
        snprintf(s, sizeof(s), "%d", m * m * b);
        addText(s);
        addPart(P_ROOT_END);
        addText(kTimes);
        addPart(P_ROOT_START);
        snprintf(s, sizeof(s), "%d", n * n * b);
        addText(s);
        addPart(P_ROOT_END);
        addText(" = ");
        addBlank(3);
        answers[0] = m * n * b;
        nAnswers   = 1;
    } else {
        int a, b;
        do {
            a = arcadeRandom(2, 7);
            b = kRootB[arcadeRandom(5)];
        } while (a * a * b > 252);
        snprintf(s, sizeof(s), "%d", a * a * b);
        addPart(P_ROOT_START);
        addText(s);
        addPart(P_ROOT_END);
        addText(" = ");
        addBlank(1);
        addPart(P_ROOT_START);
        addBlank(1);
        addPart(P_ROOT_END);
        answers[0] = a;
        answers[1] = b;
        nAnswers   = 2;
    }
}

void askQuestion() {
    curLevel = streak < 4 ? 0 : streak < 7 ? 1 : 2;
    curMode  = mode == M_MIX ? arcadeRandom(3) : mode;
    curType  = arcadeRandom(curLevel + 1);
    nParts = nBlanks = nLines = 0;
    active    = 0;
    swappable = false;
    if (curMode == M_ADD) addExpressionAdd(curType);
    else if (curMode == M_FACTOR) addExpressionFactor(curType);
    else addExpressionRoot(curType);
    layout();
    state = ASK;
    dirty = true;
    printf("MATH ask mode=%s type=%d level=%d answer=%d", kModeLog[curMode], curType, curLevel, answers[0]);
    if (nAnswers > 1) printf(",%d", answers[1]);
    printf("\n");
}

// ---------- drawing
void cellRect(int i, int& x, int& y, int& w, int& h) {
    w = 148;
    h = 88;
    x = 8 + (i % 2) * 156;
    y = 46 + (i / 2) * 96;
}

int cellAt(int px, int py) {
    for (int i = 0; i < 4; ++i) {
        int x, y, w, h;
        cellRect(i, x, y, w, h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

void keyRect(int i, int& x, int& y, int& w, int& h) {
    x = 4 + (i % 6) * 53;
    y = i < 6 ? 146 : 194;
    w = 50;
    h = 44;
}

int keyAt(int px, int py) {
    for (int i = 0; i < kKeyCount; ++i) {
        int x, y, w, h;
        keyRect(i, x, y, w, h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

void drawBadge(const char* title) {
    uiFrame.fillRect(0, 0, 320, kBarH, kBarBg);
    uiFrame.fillSmoothRoundRect(6, 6, 120, 32, 16, pal::kCyan);
    uiFrame.fillSmoothRoundRect(7, 7, 118, 30, 15, kBarBg);
    text(uiFrame, title, 66, 10, pal::kCyan, F24, top_center);
}

void drawModeSelect() {
    uiRadialBackground(rgb(15, 47, 68), rgb(7, 9, 18));
    drawBadge("計算");
    text(uiFrame, "どれにする？", 314, 8, pal::kDim, F24, top_right);
    for (int i = 0; i < 4; ++i) {
        int x, y, w, h;
        cellRect(i, x, y, w, h);
        const bool on = i == selected;
        uiFrame.fillSmoothRoundRect(x, y, w, h, 14, on ? kSelFill : kKeyBorder);
        uiFrame.fillSmoothRoundRect(x + 2, y + 2, w - 4, h - 4, 12, on ? kSelFill : kKeyFill);
        text(uiFrame, kModeNames[i], x + w / 2, y + h / 2, on ? kSelText : pal::kText, F32, middle_center);
    }
    uiFrame.pushSprite(0, 0);
}

void drawRoot(int x0, int x1, int cy) {
    auto& d        = uiFrame;
    const int yt   = cy - exprFh / 2 - 3;
    const int yb   = cy + exprFh / 2 - 3;
    const int ym   = cy + 2;
    d.drawWideLine(x0 + 1, ym + 1, x0 + 4, ym - 2, 1.0f, pal::kText);
    d.drawWideLine(x0 + 4, ym - 2, x0 + 8, yb, 1.0f, pal::kText);
    d.drawWideLine(x0 + 8, yb, x0 + 13, yt, 1.0f, pal::kText);
    d.drawWideLine(x0 + 13, yt, x1, yt, 1.0f, pal::kText);
}

void drawBlank(int i, bool done) {
    auto& d       = uiFrame;
    const Blank& b = blanks[i];
    const bool act = !done && i == active && nBlanks > 1;
    const uint16_t border = done ? kOkGreen : act ? pal::kCyan : pal::kLine;
    d.fillSmoothRoundRect(b.x, b.y, b.w, b.h, 6, border);
    const int t = act ? 2 : 1;
    d.fillSmoothRoundRect(b.x + t, b.y + t, b.w - 2 * t, b.h - 2 * t, 5, done ? border : rgb(14, 18, 38));
    char s[6];
    memcpy(s, b.buf, 5);
    s[4] = 0;
    s[b.len] = 0;
    text(d, s, b.x + b.w / 2, b.y + b.h / 2, done ? kSelText : pal::kText, exprFont, middle_center);
}

void drawAskBar() {
    drawBadge(kModeNames[mode]);
    if (state == RIGHT) {
        text(uiFrame, "せいかい！", 314, 8, kOkGreen, F24, top_right);
        return;
    }
    if (state == WRONG) {
        text(uiFrame, "ざんねん", 314, 8, kNgOrange, F24, top_right);
        return;
    }
    for (int i = 0; i < kGoal; ++i)
        uiFrame.fillSmoothCircle(176 + i * 14, 22, 5, i < streak ? pal::kCyan : rgb(38, 57, 90));
}

void drawKeys() {
    auto& d = uiFrame;
    static const char* const labels[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    for (int i = 0; i < kKeyCount; ++i) {
        int x, y, w, h;
        keyRect(i, x, y, w, h);
        const bool on     = i == keyCursor;
        const uint16_t fg = on ? kSelText : pal::kText;
        d.fillSmoothRoundRect(x, y, w, h, 8, on ? kSelFill : i == 11 ? pal::kAccent : kKeyBorder);
        d.fillSmoothRoundRect(x + 2, y + 2, w - 4, h - 4, 6, on ? kSelFill : kKeyFill);
        const int cx = x + w / 2, cy = y + h / 2;
        if (i < 10) text(d, labels[i], cx, cy, fg, F32, middle_center);
        else if (i == 11) text(d, "OK", cx, cy, fg, F32, middle_center);
        else {
            d.drawWideLine(cx - 11, cy, cx + 11, cy, 1.5f, fg);
            d.drawWideLine(cx - 11, cy, cx - 3, cy - 8, 1.5f, fg);
            d.drawWideLine(cx - 11, cy, cx - 3, cy + 8, 1.5f, fg);
        }
    }
}

void renderQuestion() {
    auto& d = uiFrame;
    uiRadialBackground(rgb(15, 47, 68), rgb(7, 9, 18));
    drawAskBar();
    const bool done = state == RIGHT || state == WRONG;
    for (int i = 0; i < nRoots; ++i) drawRoot(rootX0[i], rootX1[i], lineCy[rootLine[i]]);
    for (int i = 0; i < nParts; ++i) {
        const Part& p = parts[i];
        const int cy  = lineCy[p.line];
        if (p.type == P_TEXT) text(d, p.s, p.x, cy, pal::kText, exprFont, middle_left);
        else if (p.type == P_SUP) text(d, p.s, p.x, cy - exprFh / 2 - 2, pal::kText, F24, top_left);
        else if (p.type == P_BLANK) drawBlank(p.idx, done);
        else if (p.type == P_MINUS) d.drawWideLine(p.x + 3, cy + 1, p.x + p.w - 3, cy + 1, exprFh >= 40 ? 1.5f : 1.2f, pal::kText);
    }
    drawKeys();
    d.pushSprite(0, 0);
    dirty = false;
}

void drawDone() {
    auto& d    = uiFrame;
    state      = DONE;
    feedbackAt = lgfx::millis();
    d.fillScreen(kBarBg);
    uiGradientCard(24, 24, 272, 192, 20, rgb(79, 209, 255), rgb(20, 82, 122));
    ui_icon::star(d, 160, 80, 38, pal::kAccent);
    text(d, "クリア！", 160, 120, pal::kText, F40, top_center);
    text(d, "10問 れんぞく正解", 160, 171, pal::kText, F24, top_center);
    d.pushSprite(0, 0);
    puts("MATH done streak=10");
    soundStop();
    soundPlay(0, 180, 72);
    soundPlay(200, 180, 76);
    soundPlay(400, 180, 79);
    soundPlay(600, 500, 84);
}

// ---------- input
void typeDigit(char c) {
    Blank& b = blanks[active];
    if (b.len == 1 && b.buf[0] == '0') b.len = 0;  // a lone 0 is replaced, so "057" can never be entered
    if (b.len >= b.maxLen) return;
    b.buf[b.len++] = c;
    if (b.len == b.maxLen) {
        for (int j = active + 1; j < nBlanks; ++j)
            if (blanks[j].len == 0) {
                active = j;
                break;
            }
    }
}

void backspace() {
    if (blanks[active].len == 0 && active > 0) --active;
    if (blanks[active].len > 0) --blanks[active].len;
}

int valueOf(int i) {
    char s[6];
    memcpy(s, blanks[i].buf, 5);
    s[blanks[i].len] = 0;
    return atoi(s);
}

void submit() {
    int v[2] = {valueOf(0), nBlanks > 1 ? valueOf(1) : 0};
    bool ok;
    if (nAnswers == 1) ok = v[0] == answers[0];
    else if (swappable) ok = (v[0] == answers[0] && v[1] == answers[1]) || (v[0] == answers[1] && v[1] == answers[0]);
    else ok = v[0] == answers[0] && v[1] == answers[1];
    soundStop();
    if (ok) {
        ++streak;
        submitScore("math", streak);
        state = RIGHT;
        soundCorrect();
    } else {
        streak = 0;
        state  = WRONG;
        for (int i = 0; i < nBlanks; ++i) {
            blanks[i].len = snprintf(blanks[i].buf, sizeof(blanks[i].buf), "%d", answers[i]);
        }
        soundWrong();
    }
    feedbackAt = lgfx::millis();
    dirty      = true;
    if (nBlanks > 1) printf("MATH submit input=%d,%d ok=%d streak=%d\n", v[0], v[1], ok, streak);
    else printf("MATH submit input=%d ok=%d streak=%d\n", v[0], ok, streak);
}

void pressOk() {
    if (blanks[active].len == 0) return;
    for (int j = 0; j < nBlanks; ++j)
        if (blanks[j].len == 0) {
            active = j;
            return;
        }
    submit();
}

void pressKey(int k) {
    if (k < 10) {
        static const char digits[10] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
        typeDigit(digits[k]);
        soundPlay(0, 40, 84);
    } else if (k == 10) {
        backspace();
    } else {
        pressOk();
    }
}

void moveKeyCursor() {
    int next = keyCursor;
    const int row = keyCursor / 6, col = keyCursor % 6;
    if (pad_just & kLeft) next = row * 6 + (col + 5) % 6;
    if (pad_just & kRight) next = row * 6 + (col + 1) % 6;
    if (pad_just & (kUp | kDown)) next = (1 - row) * 6 + col;
    if (next != keyCursor) {
        keyCursor = next;
        dirty     = true;
    }
}

void tickModeSelect() {
    int next = selected;
    if (pad_just & (kLeft | kRight)) next = selected ^ 1;
    if (pad_just & (kUp | kDown)) next = selected ^ 2;
    const int hit = tap_just ? cellAt(tap_x, tap_y) : -1;
    if (hit >= 0) next = hit;
    if (next != selected) {
        selected = next;
        drawModeSelect();
    }
    if (hit >= 0 || (pad_just & (kA | kStart))) {
        mode = lastMode = selected;
        streak          = 0;
        printf("MATH mode=%s\n", kModeLog[mode]);
        askQuestion();
    }
}

void tickAsk() {
    moveKeyCursor();
    if (pad_just & kA) pressKey(keyCursor), dirty = true;
    if (pad_just & kB) backspace(), dirty = true;
    if (pad_just & kStart) pressOk(), dirty = true;
    if (tap_just) {
        const int k = keyAt(tap_x, tap_y);
        if (k >= 0) {
            keyCursor = k;
            pressKey(k);
            dirty = true;
        } else {
            for (int i = 0; i < nBlanks; ++i) {
                const Blank& b = blanks[i];
                if (tap_x >= b.x && tap_x < b.x + b.w && tap_y >= b.y && tap_y < b.y + b.h) {
                    active = i;
                    dirty  = true;
                }
            }
        }
    }
    if (dirty) renderQuestion();
}

void tick() {
    switch (state) {
        case MODE_SELECT: tickModeSelect(); break;
        case ASK: tickAsk(); break;
        case RIGHT:
            if (dirty) renderQuestion();
            if (lgfx::millis() - feedbackAt >= kRightShowMs) {
                if (streak >= kGoal) drawDone();
                else askQuestion();
            }
            break;
        case WRONG:
            if (dirty) renderQuestion();
            if (lgfx::millis() - feedbackAt >= kWrongShowMs ||
                (((pad_just & kA) || tap_just) && lgfx::millis() - feedbackAt >= 250)) {
                askQuestion();
            }
            break;
        case DONE:
            if (lgfx::millis() - feedbackAt >= 4000 ||
                ((pad_just || tap_just) && lgfx::millis() - feedbackAt >= 800))
                request_menu = true;
            break;
    }
}

void enter() {
    soundBegin();
    streak    = 0;
    keyCursor = 0;
    state     = MODE_SELECT;
    selected  = lastMode;
    drawModeSelect();
}

void leave() { soundEnd(); }

void icon(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    d.drawWideLine(x + 5, y + 22, x + 8, y + 19, 1.0f, pal::kCyan);
    d.drawWideLine(x + 8, y + 19, x + 13, y + 33, 1.0f, pal::kCyan);
    d.drawWideLine(x + 13, y + 33, x + 20, y + 8, 1.0f, pal::kCyan);
    d.drawWideLine(x + 20, y + 8, x + 36, y + 8, 1.0f, pal::kCyan);
    d.fillRect(x + 21, y + 24, 11, 3, pal::kAccent);
    d.fillRect(x + 25, y + 20, 3, 11, pal::kAccent);
}
}  // namespace

const Game kMath = {"計算", "math", enter, tick, icon, leave, GameCategory::learn, GameFooter::score};
