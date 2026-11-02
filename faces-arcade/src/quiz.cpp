// Listening quizzes: note, chord and kana (spoken with TTS).
// Four choices in a 2x2 grid. Tap answers directly; the D-pad moves and A answers.
// Ten correct answers in a row finish the quiz and return to the menu.
#include "common.h"

namespace {
enum Kind { NOTE, CHORD, KANA };
enum State { MODE_SELECT, ASK, RIGHT, WRONG, DONE };
const int kGoal = 10;
const int kBarH = 34;
const uint32_t kWrongShowMs = 2200;  // long enough to hear the question again

Kind kind;
State state;
int streak, selected, answer, choiceCount;
int kanaMode;  // 0: hiragana, 1: katakana, 2: mixed
uint32_t feedbackAt;
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
            out[i] = random(range);
            dup    = false;
            for (int j = 0; j < i; ++j) dup |= soundsSame(out[j], out[i]);
        } while (dup);
    }
    for (int i = n - 1; i > 0; --i) {
        const int j = random(i + 1);
        const int t = out[i];
        out[i]      = out[j];
        out[j]      = t;
    }
}

void cellRect(int i, int& x, int& y, int& w, int& h) {
    w = 154;
    h = 98;
    x = 4 + (i % 2) * 158;
    y = kBarH + 4 + (i / 2) * 102;
}

int cellAt(int px, int py) {
    for (int i = 0; i < choiceCount; ++i) {
        int x, y, w, h;
        cellRect(i, x, y, w, h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

void drawCell(int i, uint16_t fill, uint16_t border) {
    auto& d = M5.Display;
    int x, y, w, h;
    cellRect(i, x, y, w, h);
    d.fillRoundRect(x, y, w, h, 10, fill);
    d.drawRoundRect(x, y, w, h, 10, border);
    d.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, border);
    d.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 8, border);
    const int cx = x + w / 2, cy = y + h / 2;
    if (state == MODE_SELECT) {
        text(d, choiceLabel[i], cx, cy, pal::kText, F32, middle_center);
    } else if (kind == KANA) {
        text(d, choiceLabel[i], cx, cy, pal::kText, F40, middle_center, 2.0f);
    } else if (kind == NOTE) {
        text(d, choiceLabel[i], cx, cy, pal::kText, F40, middle_center, 1.5f);
    } else {
        text(d, choiceLabel[i], cx, y + 32, pal::kText, F40, middle_center);
        text(d, choiceSub[i], cx, y + 74, pal::kAccent, F24, middle_center);
    }
}

void drawCells() {
    for (int i = 0; i < choiceCount; ++i) {
        const bool on = i == selected;
        drawCell(i, on ? rgb(44, 62, 130) : pal::kPanel, on ? pal::kCyan : pal::kLine);
    }
}

void drawBar(uint16_t fill) {
    M5.Display.fillRect(0, 0, 320, kBarH, fill);
}

void drawAskBar() {
    drawBar(pal::kPanel);
    char b[24];
    snprintf(b, sizeof(b), "れんぞく %d/%d", streak, kGoal);
    text(M5.Display, b, 6, 5, streak ? pal::kGreen : pal::kText, F24);
    text(M5.Display, "もう一度", 314, 5, pal::kAccent, F24, top_right);
}

void playQuestion() {
    if (kind == KANA) {
        ttsSay(speakText);
        return;
    }
    soundStop();
    if (kind == NOTE) {
        soundPlay(0, 500, 72);          // reference "do"
        soundPlay(700, 800, notes[0]);  // the note to guess
    } else {
        soundPlay(0, 1400, notes[0], notes[1], notes[2]);
    }
}

void askQuestion() {
    int picks[4];
    memset(choiceSub, 0, sizeof(choiceSub));
    selected    = 0;
    choiceCount = 4;
    if (kind == NOTE) {
        const int target = random(7);
        pickChoices(target, 7, 4, picks);
        for (int i = 0; i < 4; ++i) {
            strlcpy(choiceLabel[i], kNoteNames[picks[i]], sizeof(choiceLabel[i]));
            if (picks[i] == target) answer = i;
        }
        notes[0] = kNoteMidi[target];
    } else if (kind == CHORD) {
        const int target = random(6);
        pickChoices(target, 6, 4, picks);
        for (int i = 0; i < 4; ++i) {
            strlcpy(choiceLabel[i], kChords[picks[i]].name, sizeof(choiceLabel[i]));
            strlcpy(choiceSub[i], kChords[picks[i]].sol, sizeof(choiceSub[i]));
            if (picks[i] == target) answer = i;
        }
        memcpy(notes, kChords[target].midi, sizeof(notes));
    } else {
        const int target = random(kKanaCount);
        pickChoices(target, kKanaCount, 4, picks);
        const bool useKata = kanaMode == 1 || (kanaMode == 2 && random(2));
        for (int i = 0; i < 4; ++i) {
            strlcpy(choiceLabel[i], useKata ? kKana[picks[i]].kata : kKana[picks[i]].hira, sizeof(choiceLabel[i]));
            if (picks[i] == target) answer = i;
        }
        strlcpy(speakText, target == kKanaWo ? "お" : kKana[target].hira, sizeof(speakText));
    }
    state = ASK;
    M5.Display.fillScreen(pal::kBg);
    drawAskBar();
    drawCells();
    Serial.printf("QUIZ streak=%d answer=%d\n", streak, answer);
    playQuestion();
}

void drawDone() {
    auto& d = M5.Display;
    state      = DONE;
    feedbackAt = millis();
    d.fillScreen(pal::kBg);
    for (int i = 0; i < 26; ++i) {
        static const uint16_t kColors[4] = {pal::kAccent, pal::kCyan, pal::kGreen, pal::kRed};
        // Keep the confetti in the strips above and below the text.
        const int y = (i % 2) ? random(0, 22) : random(200, 240);
        d.fillCircle(random(320), y, random(3, 7), kColors[i % 4]);
    }
    text(d, "10もん", 160, 28, pal::kAccent, F40, top_center, 1.5f);
    text(d, "れんぞく", 160, 92, pal::kText, F40, top_center);
    text(d, "せいかい！", 160, 140, pal::kGreen, F40, top_center, 1.25f);
    Serial.println("QUIZ done streak=10");
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
    if (ok) {
        ++streak;
        submitScore(scoreKey, streak);
        drawBar(rgb(30, 110, 60));
        text(M5.Display, "せいかい！", 160, 5, TFT_WHITE, F24, top_center);
        state = RIGHT;
    } else {
        streak = 0;
        drawCell(selected, rgb(130, 34, 44), pal::kRed);
        drawBar(rgb(130, 34, 44));
        text(M5.Display, "ちがうよ", 6, 5, TFT_WHITE, F24);
        text(M5.Display, "こたえは緑", 314, 5, TFT_WHITE, F24, top_right);
        state = WRONG;
    }
    drawCell(answer, rgb(30, 110, 60), pal::kGreen);
    if (!ok) playQuestion();  // Hear the right answer again.
    feedbackAt = millis();
    Serial.printf("QUIZ picked=%d ok=%d streak=%d\n", selected, ok, streak);
}

void drawModeSelect() {
    state = MODE_SELECT;
    M5.Display.fillScreen(pal::kBg);
    drawBar(pal::kPanel);
    text(M5.Display, "どれであそぶ？", 160, 5, pal::kAccent, F24, top_center);
    choiceCount = 4;
    memset(choiceSub, 0, sizeof(choiceSub));
    strlcpy(choiceLabel[0], "ひらがな", sizeof(choiceLabel[0]));
    strlcpy(choiceLabel[1], "カタカナ", sizeof(choiceLabel[1]));
    strlcpy(choiceLabel[2], "まぜる", sizeof(choiceLabel[2]));
    strlcpy(choiceLabel[3], "もどる", sizeof(choiceLabel[3]));
    selected = kanaMode;
    drawCells();
}

void moveCursor() {
    int next = selected;
    if (pad_just & kLeft) next = selected % 2 ? selected - 1 : selected + 1;
    if (pad_just & kRight) next = selected % 2 ? selected - 1 : selected + 1;
    if (pad_just & (kUp | kDown)) next = (selected + 2) % 4;
    if (next == selected) return;
    drawCell(selected, pal::kPanel, pal::kLine);
    selected = next;
    drawCell(selected, rgb(44, 62, 130), pal::kCyan);
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
                    drawCell(selected, pal::kPanel, pal::kLine);
                    selected = hit;
                }
            }
            if (hit >= 0 || (pad_just & kA)) answerNow();
            break;
        }
        case RIGHT:
            if (millis() - feedbackAt >= 800) {
                if (streak >= kGoal) drawDone();
                else askQuestion();
            }
            break;
        case WRONG:
            if (millis() - feedbackAt >= kWrongShowMs ||
                (((pad_just & kA) || tap_just) && millis() - feedbackAt >= 250)) {
                askQuestion();
            }
            break;
        case DONE:
            if (millis() - feedbackAt >= 4000 || ((pad_just || tap_just) && millis() - feedbackAt >= 800)) request_menu = true;
            break;
    }
}

void enterKind(Kind k, const char* key) {
    kind     = k;
    scoreKey = key;
    streak   = 0;
    soundBegin();
    if (k == KANA) {
        if (!ttsBegin()) {
            M5.Display.fillScreen(pal::kBg);
            text(M5.Display, "音声が使えません", 160, 100, pal::kRed, F32, top_center);
            state = DONE;
            feedbackAt = millis();
            return;
        }
        drawModeSelect();
    } else {
        askQuestion();
    }
}

void enterNote() { enterKind(NOTE, "note"); }
void enterChord() { enterKind(CHORD, "chord"); }
void enterKana() { enterKind(KANA, "kana"); }
void leave() { soundEnd(); }

void iconNote(int x, int y) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    d.fillEllipse(x + 15, y + 30, 7, 5, pal::kCyan);
    d.fillRect(x + 20, y + 7, 3, 23, pal::kCyan);
    d.fillRect(x + 20, y + 7, 12, 4, pal::kCyan);
}

void iconChord(int x, int y) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    d.fillEllipse(x + 19, y + 32, 7, 4, pal::kAccent);
    d.fillEllipse(x + 19, y + 23, 7, 4, pal::kAccent);
    d.fillEllipse(x + 19, y + 14, 7, 4, pal::kAccent);
    d.fillRect(x + 25, y + 4, 2, 28, pal::kAccent);
}

void iconKana(int x, int y) {
    M5.Display.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    text(M5.Display, "あ", x + 4, y + 4, pal::kText, F32);
}
}  // namespace

const Game kNoteQuiz  = {"音あて", "note", enterNote, tick, iconNote, leave};
const Game kChordQuiz = {"和音あて", "chord", enterChord, tick, iconChord, leave};
const Game kKanaQuiz  = {"かな読み", "kana", enterKana, tick, iconKana, leave};
