#include "common.h"
#include "ruby_ble.h"
#include "ruby_service.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <csetjmp>
#include <string>
#include "mrubyc.h"

namespace {
struct Program { uint8_t* bytes; size_t size; };
struct Note { int midi; uint32_t duration; };
QueueHandle_t programs, notes;
SemaphoreHandle_t taskDone, submitLock, runStopped;
bool vmBusy = false;
bool bleStarted = false;
jmp_buf oomRecovery;
std::atomic<TaskHandle_t> oomOwner{nullptr};
TaskHandle_t taskHandle;
esp_timer_handle_t vmTimer;
std::atomic<bool> exiting{false}, replacement{false}, gameActive{false};
std::atomic<uint8_t> held{0}, pressed{0};
portMUX_TYPE vmLock = portMUX_INITIALIZER_UNLOCKED;
mrbc_vm* activeVm = nullptr;
// The VM and the next incoming bytecode must coexist in PSRAM.
constexpr size_t kConcurrentPsramUsers = 2;

void wakeVm() {
    portENTER_CRITICAL(&vmLock);
    if (activeVm) activeVm->flag_preemption = 1;
    portEXIT_CRITICAL(&vmLock);
}
void timerWake(void*) { wakeVm(); }
bool args(mrbc_vm* vm, mrbc_value* v, int argc, int count, int stringArg = -1, int symbolArg = -1) {
    if (argc == count) {
        bool ok = true;
        for (int i = 1; i <= argc; ++i) {
            auto type = i == stringArg ? MRBC_TT_STRING : i == symbolArg ? MRBC_TT_SYMBOL : MRBC_TT_INTEGER;
            if (mrbc_type(v[i]) != type) ok = false;
        }
        if (ok) return true;
    }
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "invalid arguments"); return false;
}
void clear(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,1)) uiFrame.fillScreen(v[1].i); }
void rect(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,5)) uiFrame.drawRect(v[1].i,v[2].i,v[3].i,v[4].i,v[5].i); }
void fillRect(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,5)) uiFrame.fillRect(v[1].i,v[2].i,v[3].i,v[4].i,v[5].i); }
void circle(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,4)) uiFrame.drawCircle(v[1].i,v[2].i,v[3].i,v[4].i); }
void line(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,5)) uiFrame.drawLine(v[1].i,v[2].i,v[3].i,v[4].i,v[5].i); }
void textMethod(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,4,1)) text(uiFrame,mrbc_string_cstr(&v[1]),v[2].i,v[3].i,v[4].i,F24); }
void show(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,0)) uiFrame.pushSprite(0,0); }
void color(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,3)) SET_INT_RETURN(rgb(v[1].i,v[2].i,v[3].i)); }
uint8_t button(const char* s) {
    if (!strcmp(s,"up")) return kUp;
    if (!strcmp(s,"down")) return kDown;
    if (!strcmp(s,"left")) return kLeft;
    if (!strcmp(s,"right")) return kRight;
    if (!strcmp(s,"a")) return kA;
    if (!strcmp(s,"b")) return kB;
    if (!strcmp(s,"start")) return kStart;
    return 0;
}
void down(mrbc_vm* vm, mrbc_value* v, int n) {
    if (!args(vm,v,n,1,-1,1)) return;
    auto bit = button(mrbc_symbol_cstr(&v[1]));
    if (!bit) { mrbc_raise(vm, MRBC_CLASS(ArgumentError), "unknown button"); return; }
    SET_BOOL_RETURN(held.load() & bit);
}
void just(mrbc_vm* vm, mrbc_value* v, int n) {
    if (!args(vm,v,n,1,-1,1)) return;
    auto bit = button(mrbc_symbol_cstr(&v[1]));
    if (!bit) { mrbc_raise(vm, MRBC_CLASS(ArgumentError), "unknown button"); return; }
    SET_BOOL_RETURN(pressed.fetch_and(uint8_t(~bit)) & bit);
}
void play(mrbc_vm* vm, mrbc_value* v, int n) {
    if (!args(vm,v,n,2)) return;
    if (v[1].i < 0 || v[1].i > 127 || v[2].i < 0) { mrbc_raise(vm,MRBC_CLASS(ArgumentError),"invalid note"); return; }
    Note note{int(v[1].i),uint32_t(v[2].i)}; xQueueSend(notes,&note,0);
}
void ms(mrbc_vm* vm, mrbc_value* v, int n) { if (args(vm,v,n,0)) SET_INT_RETURN(lgfx::millis()); }
void randomMethod(mrbc_vm* vm, mrbc_value* v, int n) {
    if (!args(vm,v,n,1)) return;
    if (v[1].i <= 0) { mrbc_raise(vm,MRBC_CLASS(ArgumentError),"rand requires a positive integer"); return; }
    SET_INT_RETURN(arcadeRandom(v[1].i));
}
void sleepMethod(mrbc_vm* vm, mrbc_value* v, int n) {
    if (!args(vm,v,n,1)) return;
    if (v[1].i < 0) { mrbc_raise(vm,MRBC_CLASS(ArgumentError),"negative sleep"); return; }
    uint32_t remaining = v[1].i;
    while (remaining && !exiting.load() && !replacement.load()) {
        uint32_t part = min(remaining, uint32_t(portTICK_PERIOD_MS));
        vTaskDelay(pdMS_TO_TICKS(part) ? pdMS_TO_TICKS(part) : 1); remaining -= part;
    }
}
void bind() {
    auto screen = mrbc_define_class(nullptr,"Screen",MRBC_CLASS(Object));
    mrbc_define_method(nullptr,screen,"clear",clear); mrbc_define_method(nullptr,screen,"rect",rect);
    mrbc_define_method(nullptr,screen,"fill_rect",fillRect); mrbc_define_method(nullptr,screen,"circle",circle);
    mrbc_define_method(nullptr,screen,"line",line); mrbc_define_method(nullptr,screen,"text",textMethod);
    mrbc_define_method(nullptr,screen,"show",show); mrbc_define_method(nullptr,screen,"rgb",color);
    auto pad = mrbc_define_class(nullptr,"Pad",MRBC_CLASS(Object));
    mrbc_define_method(nullptr,pad,"down?",down); mrbc_define_method(nullptr,pad,"pressed?",just);
    auto sound = mrbc_define_class(nullptr,"Sound",MRBC_CLASS(Object)); mrbc_define_method(nullptr,sound,"play",play);
    auto sys = mrbc_define_class(nullptr,"Sys",MRBC_CLASS(Object)); mrbc_define_method(nullptr,sys,"ms",ms);
    mrbc_define_method(nullptr,nullptr,"rand",randomMethod); mrbc_define_method(nullptr,nullptr,"sleep_ms",sleepMethod);
}
void showError(const char* message) {
    uiFrame.fillScreen(pal::kBg);
    text(uiFrame,"Ruby エラー",8,8,pal::kRed,F24);
    std::string line;
    int y = 40;
    const std::string full(message);
    for (size_t pos = 0; pos < full.size();) {
        unsigned char c = full[pos];
        if (c == '\n') {
            if (y < 240) text(uiFrame,line.c_str(),8,y,pal::kRed,F24);
            y += 24; line.clear(); ++pos; continue;
        }
        size_t count = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        count = min(count,full.size() - pos);
        std::string character = full.substr(pos,count);
        if (!line.empty() && uiFrame.textWidth((line + character).c_str(),F24) > 304) {
            if (y < 240) text(uiFrame,line.c_str(),8,y,pal::kRed,F24);
            y += 24; line.clear();
        }
        line += character; pos += count;
    }
    if (!line.empty() && y < 240) text(uiFrame,line.c_str(),8,y,pal::kRed,F24);
    uiFrame.pushSprite(0,0);
    std::string out = "error "; out += full; rubyBleNotify(out.c_str());
}
// This scope calls mruby/c without holding submitLock or vmLock. Native Ruby
// bindings do not retain either lock across calls that can allocate in mruby/c.
void runProgram(Program program, void* heap, size_t heapSize) {
    oomOwner = xTaskGetCurrentTaskHandle();
    if (setjmp(oomRecovery) != 0) {
        oomOwner = nullptr;
        portENTER_CRITICAL(&vmLock); activeVm = nullptr; portEXIT_CRITICAL(&vmLock);
        // The allocator and all VM-owned objects are discarded together.
        mrbc_cleanup();
        showError("メモリ不足で止まりました");
        return;
    }
    mrbc_init(heap,heapSize); bind();
    mrbc_vm* vm = mrbc_vm_open(nullptr);
    if (!vm || mrbc_load_mrb(vm,program.bytes)) {
        if (vm) mrbc_vm_close(vm);
        oomOwner = nullptr; mrbc_cleanup(); showError("バイトコードを読めません"); return;
    }
    mrbc_vm_begin(vm);
    portENTER_CRITICAL(&vmLock); activeVm = vm; portEXIT_CRITICAL(&vmLock);
    rubyBleNotify("running");
    int result = 0;
    while (!exiting.load() && !replacement.load() && result == 0) {
        portENTER_CRITICAL(&vmLock);
        bool stopping = exiting.load() || replacement.load();
        if (!stopping) vm->flag_preemption = 0;
        portEXIT_CRITICAL(&vmLock);
        if (stopping) break;
        result = mrbc_vm_run(vm);
        if (result == 0) vTaskDelay(1);
    }
    portENTER_CRITICAL(&vmLock); activeVm = nullptr; portEXIT_CRITICAL(&vmLock);
    if (result == 2 && mrbc_israised(vm)) {
        auto exc = vm->exception.exception;
        std::string message = mrbc_symid_to_str(exc->cls->sym_id);
        message += ": ";
        if (exc->message) {
            const char* detail = reinterpret_cast<const char*>(exc->message);
            message.append(detail, exc->message_size ? exc->message_size : strlen(detail));
        }
        showError(message.c_str());
    }
    mrbc_vm_end(vm); mrbc_vm_close(vm);
    oomOwner = nullptr;
    mrbc_cleanup();
}
void vmTask(void*) {
    for (;;) {
        Program program{};
        xSemaphoreTake(submitLock,portMAX_DELAY);
        const bool stop = exiting.load();
        const bool got = !stop && gameActive.load() && xQueueReceive(programs,&program,0) == pdTRUE;
        if (got) {
            replacement.store(uxQueueMessagesWaiting(programs) != 0);
            xSemaphoreTake(runStopped,0);
            vmBusy = true;
        }
        xSemaphoreGive(submitLock);
        if (stop) break;
        if (!got) { ulTaskNotifyTake(pdTRUE,portMAX_DELAY); continue; }
        const size_t vmHeapSize = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / kConcurrentPsramUsers;
        void* heap = heap_caps_malloc(vmHeapSize,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!heap) showError("メモリ不足で止まりました");
        else { runProgram(program,heap,vmHeapSize); free(heap); }
        free(program.bytes);
        xSemaphoreTake(submitLock,portMAX_DELAY);
        vmBusy = false;
        if (!gameActive.load()) xSemaphoreGive(runStopped);
        xSemaphoreGive(submitLock);
    }
    xSemaphoreGive(taskDone); vTaskDelete(nullptr);
}
bool beginService() {
    if (submitLock) return taskHandle != nullptr;
    exiting.store(false); replacement.store(false); gameActive.store(false);
    held.store(0); pressed.store(0); bleStarted = false; vmBusy = false;
    programs = xQueueCreate(1,sizeof(Program)); notes = xQueueCreate(8,sizeof(Note));
    taskDone = xSemaphoreCreateBinary(); submitLock = xSemaphoreCreateMutex(); runStopped = xSemaphoreCreateBinary();
    if (!programs || !notes || !taskDone || !submitLock || !runStopped) return false;
    esp_timer_create_args_t timerArgs{}; timerArgs.callback = timerWake; timerArgs.name = "ruby_vm";
    if (esp_timer_create(&timerArgs,&vmTimer) != ESP_OK) return false;
    if (xTaskCreate(vmTask,"ruby_vm",8192,nullptr,5,&taskHandle) != pdPASS) { taskHandle = nullptr; return false; }
    bleStarted = rubyBleStart();
    return bleStarted;
}
void stopService() {
    if (submitLock) { xSemaphoreTake(submitLock,portMAX_DELAY); exiting.store(true); xSemaphoreGive(submitLock); }
    else exiting.store(true);
    wakeVm();
    if (taskHandle) { xTaskNotifyGive(taskHandle); xSemaphoreTake(taskDone,portMAX_DELAY); taskHandle = nullptr; }
    if (bleStarted) { rubyBleStop(); bleStarted = false; }
    Program pending{};
    if (programs) while (xQueueReceive(programs,&pending,0) == pdTRUE) free(pending.bytes);
    if (vmTimer) { esp_timer_stop(vmTimer); esp_timer_delete(vmTimer); vmTimer = nullptr; }
    if (programs) vQueueDelete(programs);
    if (notes) vQueueDelete(notes);
    if (taskDone) vSemaphoreDelete(taskDone);
    if (submitLock) vSemaphoreDelete(submitLock);
    if (runStopped) vSemaphoreDelete(runStopped);
    programs = notes = nullptr; taskDone = submitLock = runStopped = nullptr; gameActive.store(false);
}
void drawWaiting() {
    uiFrame.fillScreen(pal::kBg); text(uiFrame,"Ruby",160,52,pal::kText,F32,top_center);
    text(uiFrame,"Bluetooth で待機中",160,104,pal::kCyan,F24,top_center);
    text(uiFrame,"SELECT でもどる",160,164,pal::kDim,F24,top_center); uiFrame.pushSprite(0,0);
}
void enter() {
    if (!beginService()) { stopService(); showError("タスクを開始できません"); return; }
    soundBegin();
    drawWaiting();
    if (esp_timer_start_periodic(vmTimer,uint64_t(portTICK_PERIOD_MS)*1000) != ESP_OK) {
        showError("タイマーを開始できません"); return;
    }
    xSemaphoreTake(submitLock,portMAX_DELAY);
    gameActive.store(true);
    xSemaphoreGive(submitLock);
    xTaskNotifyGive(taskHandle);
}
void tick() {
    held.store(pad_now); pressed.fetch_or(pad_just & uint8_t(~kSelect));
    wakeVm();
    Note note; while (notes && xQueueReceive(notes,&note,0) == pdTRUE) soundPlay(0,note.duration,note.midi);
}
void leave() {
    if (submitLock) {
        xSemaphoreTake(submitLock,portMAX_DELAY);
        gameActive.store(false);
        replacement.store(true);
        const bool wait = vmBusy;
        xSemaphoreGive(submitLock);
        wakeVm();
        if (wait) xSemaphoreTake(runStopped,portMAX_DELAY);
    }
    if (vmTimer) esp_timer_stop(vmTimer);
    soundEnd();
}
void icon(int x, int y) { text(uiGlyph,"R",x,y,pal::kText,F32); }
}

bool rubyServiceBegin() {
    if (beginService()) return true;
    stopService(); return false;
}
bool rubyServiceProgramReady() {
    if (!submitLock) return false;
    xSemaphoreTake(submitLock,portMAX_DELAY);
    bool ready = !exiting.load() && programs && uxQueueMessagesWaiting(programs) != 0;
    xSemaphoreGive(submitLock);
    return ready;
}

bool rubySubmit(uint8_t* data, size_t size) {
    if (!submitLock) return false;
    Program program{data,size};
    xSemaphoreTake(submitLock,portMAX_DELAY);
    bool sent = false;
    if (!exiting.load() && programs) {
        Program superseded{};
        if (xQueueReceive(programs,&superseded,0) == pdTRUE) free(superseded.bytes);
        sent = xQueueSend(programs,&program,0) == pdTRUE;
        if (sent) replacement.store(true);
    }
    xSemaphoreGive(submitLock);
    if (sent) { wakeVm(); if (taskHandle) xTaskNotifyGive(taskHandle); }
    return sent;
}
extern "C" int mrbc_hal_write(int, const void* data, int n) {
    if (n > 0) rubyBleLog(data, static_cast<size_t>(n));
    return n;
}
extern "C" void mrbc_hal_abort(const char* message) {
    if (oomOwner.load() && xTaskGetCurrentTaskHandle() == oomOwner.load()) longjmp(oomRecovery, 1);
    printf("mruby/c abort: %s\n",message ? message : ""); abort();
}
const Game kRuby = {"Ruby",nullptr,enter,tick,icon,leave,GameCategory::play,GameFooter::sound};
