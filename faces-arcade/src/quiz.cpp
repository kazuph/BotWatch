// Listening quizzes: note (absolute or relative pitch), chord and kana (spoken with TTS).
// Absolute pitch plays only the note to guess. Relative pitch plays "do" first as a reference.
// Four choices in a 2x2 grid. Tap answers directly; the D-pad moves and A answers.
// Ten correct answers in a row finish the quiz and return to the menu.
#include "common.h"
#include "ui_icons.h"

namespace {
enum Kind { NOTE, CHORD, KANA };
enum State { MODE_SELECT, ASK, RIGHT, WRONG, DONE };
const int kGoal = 10;
const int kBarH = 42;
const uint32_t kWrongShowMs = 2600;  // long enough to hear the buzzer and then the question again
const uint32_t kRightShowMs = 1000;  // long enough for the chime to ring out

Kind kind;
State state;
int streak, selected, answer, choiceCount;
int kanaMode;  // 0: hiragana, 1: katakana, 2: mixed
bool absolutePitch;  // note quiz only: no reference "do" before the question
uint32_t feedbackAt;
bool replayPending;  // WRONG: replay the question once the buzzer has finished
const uint32_t kReplayAfterMs = 900;
bool colorsInitialized = false, colorsAnimating = false;
bool renderedThisTick = false;
uint32_t colorsAt;
uint16_t fillsFrom[4], fillsTo[4], bordersFrom[4], bordersTo[4];
uint16_t textFrom[4], textTo[4], subFrom[4], subTo[4];
uint16_t dotsFrom[10], dotsTo[10];
char choiceLabel[4][16];
char choiceSub[4][16];
int notes[3];
char speakText[8];
const char* scoreKey;

// ---------- Note quiz: white keys of the C major scale, one octave above middle C.
const char* const kNoteNames[7] = {"ド", "レ", "ミ", "ファ", "ソ", "ラ", "シ"};
const int kNoteMidi[7]          = {72, 74, 76, 77, 79, 81, 83};

// ---------- Chord quiz: diatonic triads of C major.
struct Chord {
    const char* name;
    const char* sol;
    int midi[3];
};
const Chord kChords[6] = {
    {"C", "ドミソ", {72, 76, 79}}, {"Dm", "レファラ", {74, 77, 81}}, {"Em", "ミソシ", {76, 79, 83}},
    {"F", "ファラド", {77, 81, 84}}, {"G", "ソシレ", {79, 83, 86}}, {"Am", "ラドミ", {81, 84, 88}},
};

// ---------- Kana quiz. ぢ and づ are left out because they sound the same as じ and ず.
struct Kana {
    const char* hira;
    const char* kata;
};
const Kana kKana[] = {
    {"あ", "ア"}, {"い", "イ"}, {"う", "ウ"}, {"え", "エ"}, {"お", "オ"},
    {"か", "カ"}, {"き", "キ"}, {"く", "ク"}, {"け", "ケ"}, {"こ", "コ"},
    {"さ", "サ"}, {"し", "シ"}, {"す", "ス"}, {"せ", "セ"}, {"そ", "ソ"},
    {"た", "タ"}, {"ち", "チ"}, {"つ", "ツ"}, {"て", "テ"}, {"と", "ト"},
    {"な", "ナ"}, {"に", "ニ"}, {"ぬ", "ヌ"}, {"ね", "ネ"}, {"の", "ノ"},
    {"は", "ハ"}, {"ひ", "ヒ"}, {"ふ", "フ"}, {"へ", "ヘ"}, {"ほ", "ホ"},
    {"ま", "マ"}, {"み", "ミ"}, {"む", "ム"}, {"め", "メ"}, {"も", "モ"},
    {"や", "ヤ"}, {"ゆ", "ユ"}, {"よ", "ヨ"},
    {"ら", "ラ"}, {"り", "リ"}, {"る", "ル"}, {"れ", "レ"}, {"ろ", "ロ"},
    {"わ", "ワ"}, {"を", "ヲ"}, {"ん", "ン"},
    {"が", "ガ"}, {"ぎ", "ギ"}, {"ぐ", "グ"}, {"げ", "ゲ"}, {"ご", "ゴ"},
    {"ざ", "ザ"}, {"じ", "ジ"}, {"ず", "ズ"}, {"ぜ", "ゼ"}, {"ぞ", "ゾ"},
    {"だ", "ダ"}, {"で", "デ"}, {"ど", "ド"},
    {"ば", "バ"}, {"び", "ビ"}, {"ぶ", "ブ"}, {"べ", "ベ"}, {"ぼ", "ボ"},
    {"ぱ", "パ"}, {"ぴ", "ピ"}, {"ぷ", "プ"}, {"ぺ", "ペ"}, {"ぽ", "ポ"},
};
const int kKanaCount = sizeof(kKana) / sizeof(kKana[0]);
const int kKanaO = 4, kKanaWo = 44;  // お and を are spoken the same

bool soundsSame(int a, int b) {
    if (a == b) return true;
    return kind == KANA && ((a == kKanaO && b == kKanaWo) || (a == kKanaWo && b == kKanaO));
}

void pickChoices(int correct, int range, int n, int* out) {
    out[0] = correct;
    for (int i = 1; i < n; ++i) {
        bool dup;
        do {
            out[i] = arcadeRandom(range);
            dup    = false;
            for (int j = 0; j < i; ++j) dup |= soundsSame(out[j], out[i]);
        } while (dup);
    }
    for (int i = n - 1; i > 0; --i) {
        const int j = arcadeRandom(i + 1);
        const int t = out[i];
        out[i]      = out[j];
        out[j]      = t;
    }
}

void cellRect(int i, int& x, int& y, int& w, int& h) {
    w = 148;
    h = 88;
    x = 8 + (i % 2) * 156;
    y = 46 + (i / 2) * 96;
}

int cellAt(int px, int py) {
    for (int i = 0; i < choiceCount; ++i) {
        int x, y, w, h;
        cellRect(i, x, y, w, h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

void drawLargeLabel(const char* label, int x, int y, uint16_t color, float scale) {
    uiGlyph.fillSprite(TFT_BLACK);
    text(uiGlyph, label, 96, 24, color, F40, middle_center);
    uiGlyph.pushRotateZoomWithAA(&uiFrame, x, y, 0, scale, scale, TFT_BLACK);
}

void drawCell(int i, uint16_t fill, uint16_t border, uint16_t foreground, uint16_t subColor) {
    auto& d = uiFrame;
    int x, y, w, h;
    cellRect(i, x, y, w, h);
    d.fillSmoothRoundRect(x, y, w, h, 14, border);
    d.fillSmoothRoundRect(x + 2, y + 2, w - 4, h - 4, 12, fill);
    const int cx = x + w / 2, cy = y + h / 2;
    if (state == MODE_SELECT) {
        text(d, choiceLabel[i], cx, cy, foreground, F32, middle_center);
    } else if (kind == KANA) {
        drawLargeLabel(choiceLabel[i], cx, cy, foreground, 2.0f);
    } else if (kind == NOTE) {
        drawLargeLabel(choiceLabel[i], cx, cy, foreground, 1.5f);
    } else {
        text(d, choiceLabel[i], cx, y + 25, foreground, F40, middle_center);
        text(d, choiceSub[i], cx, y + 64, subColor, F24, middle_center);
    }
}

uint16_t blend565(uint16_t from, uint16_t to, uint32_t elapsed) {
    elapsed = min(elapsed, uint32_t(300));
    const auto mix = [elapsed](int a, int b) { return (a * (300 - elapsed) + b * elapsed) / 300; };
    return (mix((from >> 11) & 31, (to >> 11) & 31) << 11) |
           (mix((from >> 5) & 63, (to >> 5) & 63) << 5) | mix(from & 31, to & 31);
}

void drawAskBar(uint32_t elapsed);
void drawModeBar();

void renderCells(uint32_t now) {
    const uint32_t elapsed = now - colorsAt;
    uiRadialBackground(rgb(15, 47, 68), rgb(7, 9, 18));
    if (state == MODE_SELECT) drawModeBar();
    else drawAskBar(elapsed);
    for (int i = 0; i < choiceCount; ++i)
        drawCell(i, blend565(fillsFrom[i], fillsTo[i], elapsed),
                 blend565(bordersFrom[i], bordersTo[i], elapsed),
                 blend565(textFrom[i], textTo[i], elapsed), blend565(subFrom[i], subTo[i], elapsed));
    uiFrame.pushSprite(0, 0);
    renderedThisTick = true;
    colorsAnimating = elapsed < 300;
}

void transitionCells() {
    const uint32_t now = lgfx::millis();
    const uint32_t elapsed = now - colorsAt;
    bool changed = false;
    for (int i = 0; i < choiceCount; ++i) {
        const bool on = i == selected;
        const bool back = state == MODE_SELECT && i == 3 && !on;
        uint16_t fill = on ? rgb(79, 209, 255) : back ? rgb(14, 18, 38) : rgb(20, 50, 74);
        uint16_t border = on ? rgb(79, 209, 255) : back ? rgb(58, 65, 128) : rgb(44, 90, 122);
        if (state == RIGHT || state == WRONG) {
            if (i == answer) fill = border = rgb(91, 227, 138);
            else if (on && state == WRONG) fill = border = rgb(255, 157, 70);
        }
        const bool bright = on || ((state == RIGHT || state == WRONG) && i == answer);
        const uint16_t foreground = back ? pal::kDim : bright ? rgb(3, 32, 43) : pal::kText;
        const uint16_t subColor = bright ? rgb(3, 48, 74) : pal::kAccent;
        fillsFrom[i] = colorsInitialized ? blend565(fillsFrom[i], fillsTo[i], elapsed) : fill;
        bordersFrom[i] = colorsInitialized ? blend565(bordersFrom[i], bordersTo[i], elapsed) : border;
        textFrom[i] = colorsInitialized ? blend565(textFrom[i], textTo[i], elapsed) : foreground;
        subFrom[i] = colorsInitialized ? blend565(subFrom[i], subTo[i], elapsed) : subColor;
        fillsTo[i] = fill;
        bordersTo[i] = border;
        textTo[i] = foreground;
        subTo[i] = subColor;
        changed |= fillsFrom[i] != fillsTo[i] || bordersFrom[i] != bordersTo[i] ||
                   textFrom[i] != textTo[i] || subFrom[i] != subTo[i];
    }
    for (int i = 0; i < kGoal; ++i) {
        const uint16_t target = i < streak ? pal::kCyan : rgb(38, 57, 90);
        dotsFrom[i] = colorsInitialized ? blend565(dotsFrom[i], dotsTo[i], elapsed) : target;
        dotsTo[i] = target;
        changed |= dotsFrom[i] != dotsTo[i];
    }
    colorsAt = now;
    colorsInitialized = true;
    renderCells(now);
    colorsAnimating = changed;
}

void drawBar(uint16_t fill) {
    uiFrame.fillRect(0, 0, 320, kBarH, fill);
}

void drawAskBar(uint32_t elapsed) {
    drawBar(rgb(7, 9, 18));
    const char* title = kind == NOTE ? (absolutePitch ? "絶対音感" : "相対音感") : kind == CHORD ? "和音あて" : "かな読み";
    const int badgeWidth = 120;
    uiFrame.fillSmoothRoundRect(6, 6, badgeWidth, 32, 16, pal::kCyan);
    uiFrame.fillSmoothRoundRect(7, 7, badgeWidth - 2, 30, 15, rgb(7, 9, 18));
    text(uiFrame, title, 6 + badgeWidth / 2, 10, pal::kCyan, F24, top_center);
    if (state == RIGHT) {
        text(uiFrame, "せいかい！", 314, 8, rgb(91, 227, 138), F24, top_right);
        return;
    }
    if (state == WRONG) {
        text(uiFrame, "ざんねん", 314, 8, rgb(255, 157, 70), F24, top_right);
        return;
    }
    for (int i = 0; i < kGoal; ++i)
        uiFrame.fillSmoothCircle(176 + i * 14, 22, 5, blend565(dotsFrom[i], dotsTo[i], elapsed));
}

void drawModeBar() {
    drawBar(rgb(7, 9, 18));
    uiFrame.fillSmoothRoundRect(6, 6, 120, 32, 16, pal::kCyan);
    uiFrame.fillSmoothRoundRect(7, 7, 118, 30, 15, rgb(7, 9, 18));
    text(uiFrame, "かな読み", 66, 10, pal::kCyan, F24, top_center);
    text(uiFrame, "どれにする？", 314, 8, pal::kDim, F24, top_right);
}

void playQuestion() {
    if (kind == KANA) {
        ttsSay(speakText);
        return;
    }
    soundStop();
    if (kind == NOTE) {
        if (absolutePitch) {
            soundPlay(0, 800, notes[0]);    // the note to guess, with nothing to compare against
        } else {
            soundPlay(0, 500, 72);          // reference "do"
            soundPlay(700, 800, notes[0]);  // the note to guess
        }
    } else {
        soundPlay(0, 1400, notes[0], notes[1], notes[2]);
    }
}

void askQuestion() {
    int picks[4];
    memset(choiceSub, 0, sizeof(choiceSub));
    selected    = -1;  // nothing is highlighted until the player moves the cursor
    choiceCount = 4;
    if (kind == NOTE) {
        const int target = arcadeRandom(7);
        pickChoices(target, 7, 4, picks);
        for (int i = 0; i < 4; ++i) {
            strlcpy(choiceLabel[i], kNoteNames[picks[i]], sizeof(choiceLabel[i]));
            if (picks[i] == target) answer = i;
        }
        notes[0] = kNoteMidi[target];
    } else if (kind == CHORD) {
        const int target = arcadeRandom(6);
        pickChoices(target, 6, 4, picks);
        for (int i = 0; i < 4; ++i) {
            strlcpy(choiceLabel[i], kChords[picks[i]].name, sizeof(choiceLabel[i]));
            strlcpy(choiceSub[i], kChords[picks[i]].sol, sizeof(choiceSub[i]));
            if (picks[i] == target) answer = i;
        }
        memcpy(notes, kChords[target].midi, sizeof(notes));
    } else {
        const int target = arcadeRandom(kKanaCount);
        pickChoices(target, kKanaCount, 4, picks);
        const bool useKata = kanaMode == 1 || (kanaMode == 2 && arcadeRandom(2));
        for (int i = 0; i < 4; ++i) {
            strlcpy(choiceLabel[i], useKata ? kKana[picks[i]].kata : kKana[picks[i]].hira, sizeof(choiceLabel[i]));
            if (picks[i] == target) answer = i;
        }
        strlcpy(speakText, target == kKanaWo ? "お" : kKana[target].hira, sizeof(speakText));
    }
    state = ASK;
    transitionCells();
    printf("QUIZ streak=%d answer=%d\n", streak, answer);
    playQuestion();
}

void drawDone() {
    auto& d = uiFrame;
    state      = DONE;
    feedbackAt = lgfx::millis();
    d.fillScreen(rgb(7, 9, 18));
    uiGradientCard(24, 24, 272, 192, 20, rgb(79, 209, 255), rgb(20, 82, 122));
    ui_icon::star(d, 160, 80, 38, pal::kAccent);
    text(d, "クリア！", 160, 120, pal::kText, F40, top_center);
    text(d, "10問 れんぞく正解", 160, 171, pal::kText, F24, top_center);
    uiFrame.pushSprite(0, 0);
    puts("QUIZ done streak=10");
    soundStop();
    if (kind != KANA) {
        soundPlay(0, 180, 72);
        soundPlay(200, 180, 76);
        soundPlay(400, 180, 79);
        soundPlay(600, 500, 84);
    }
}

void answerNow() {
    const bool ok = selected == answer;
    soundStop();
    replayPending = false;
    if (ok) {
        ++streak;
        submitScore(scoreKey, streak);
        state = RIGHT;
        soundCorrect();  // "pin-poon" chime shared by every quiz
    } else {
        streak = 0;
        state = WRONG;
        soundWrong();    // "bu-buu" buzzer shared by every quiz
        replayPending = true;       // then hear the right answer again
    }
    feedbackAt = lgfx::millis();
    transitionCells();
    printf("QUIZ picked=%d ok=%d streak=%d\n", selected, ok, streak);
}

void drawModeSelect() {
    state = MODE_SELECT;
    choiceCount = 4;
    memset(choiceSub, 0, sizeof(choiceSub));
    strlcpy(choiceLabel[0], "ひらがな", sizeof(choiceLabel[0]));
    strlcpy(choiceLabel[1], "カタカナ", sizeof(choiceLabel[1]));
    strlcpy(choiceLabel[2], "まぜる", sizeof(choiceLabel[2]));
    strlcpy(choiceLabel[3], "もどる", sizeof(choiceLabel[3]));
    selected = kanaMode;
    transitionCells();
}

void moveCursor() {
    if (selected < 0) {  // the first D-pad press only shows the cursor
        if (!(pad_just & (kLeft | kRight | kUp | kDown))) return;
        selected = 0;
        transitionCells();
        return;
    }
    int next = selected;
    if (pad_just & kLeft) next = selected % 2 ? selected - 1 : selected + 1;
    if (pad_just & kRight) next = selected % 2 ? selected - 1 : selected + 1;
    if (pad_just & (kUp | kDown)) next = (selected + 2) % 4;
    if (next == selected) return;
    selected = next;
    transitionCells();
}

void chooseMode() {
    if (selected == 3) {
        request_menu = true;
        return;
    }
    kanaMode = selected;
    streak   = 0;
    askQuestion();
}

void tick() {
    renderedThisTick = false;
    switch (state) {
        case MODE_SELECT: {
            moveCursor();
            const int hit = tap_just ? cellAt(tap_x, tap_y) : -1;
            if (hit >= 0) selected = hit;
            if (hit >= 0 || (pad_just & (kA | kStart))) chooseMode();
            break;
        }
        case ASK: {
            moveCursor();
            if ((pad_just & kB) || (tap_just && tap_y < kBarH)) playQuestion();
            const int hit = tap_just ? cellAt(tap_x, tap_y) : -1;
            if (hit >= 0) {
                if (hit != selected) {
                    selected = hit;
                }
            }
            if (hit >= 0 || ((pad_just & kA) && selected >= 0)) answerNow();
            break;
        }
        case RIGHT:
            if (lgfx::millis() - feedbackAt >= kRightShowMs) {
                if (streak >= kGoal) drawDone();
                else askQuestion();
            }
            break;
        case WRONG:
            if (replayPending && lgfx::millis() - feedbackAt >= kReplayAfterMs) {
                replayPending = false;
                playQuestion();
            }
            if (lgfx::millis() - feedbackAt >= kWrongShowMs ||
                (((pad_just & kA) || tap_just) && lgfx::millis() - feedbackAt >= 250)) {
                askQuestion();
            }
            break;
        case DONE:
            if (lgfx::millis() - feedbackAt >= 4000 || ((pad_just || tap_just) && lgfx::millis() - feedbackAt >= 800)) request_menu = true;
            break;
    }
    if (state != DONE && colorsAnimating && !renderedThisTick) renderCells(lgfx::millis());
}

void enterKind(Kind k, const char* key) {
    kind     = k;
    colorsInitialized = false;
    colorsAnimating = false;
    scoreKey = key;
    streak   = 0;
    soundBegin();
    if (k == KANA) {
        if (!ttsBegin()) {
            uiFrame.fillScreen(pal::kBg);
            text(uiFrame, "音声が使えません", 160, 100, pal::kRed, F32, top_center);
            uiFrame.pushSprite(0, 0);
            state = DONE;
            feedbackAt = lgfx::millis();
            return;
        }
        drawModeSelect();
    } else {
        askQuestion();
    }
}

// The relative quiz keeps the "note" key: records saved before the split were played with the reference "do".
void enterNoteAbsolute() { absolutePitch = true; enterKind(NOTE, "note_abs"); }
void enterNoteRelative() { absolutePitch = false; enterKind(NOTE, "note"); }
void enterChord() { enterKind(CHORD, "chord"); }
void enterKana() { enterKind(KANA, "kana"); }
void leave() { soundEnd(); }

void iconNote(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    d.fillEllipse(x + 15, y + 30, 7, 5, pal::kCyan);
    d.fillRect(x + 20, y + 7, 3, 23, pal::kCyan);
    d.fillRect(x + 20, y + 7, 12, 4, pal::kCyan);
}

void iconChord(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    d.fillEllipse(x + 19, y + 32, 7, 4, pal::kAccent);
    d.fillEllipse(x + 19, y + 23, 7, 4, pal::kAccent);
    d.fillEllipse(x + 19, y + 14, 7, 4, pal::kAccent);
    d.fillRect(x + 25, y + 4, 2, 28, pal::kAccent);
}

void iconKana(int x, int y) {
    uiGlyph.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    text(uiGlyph, "あ", x + 4, y + 4, pal::kText, F32);
}
}  // namespace

const Game kAbsolutePitchQuiz = {"絶対音感", "note_abs", enterNoteAbsolute, tick, iconNote, leave, GameCategory::learn, GameFooter::score};
const Game kNoteQuiz  = {"相対音感", "note", enterNoteRelative, tick, iconNote, leave, GameCategory::learn, GameFooter::score};
const Game kChordQuiz = {"和音あて", "chord", enterChord, tick, iconChord, leave, GameCategory::learn, GameFooter::score};
const Game kKanaQuiz  = {"かな読み", "kana", enterKana, tick, iconKana, leave, GameCategory::learn, GameFooter::score};
