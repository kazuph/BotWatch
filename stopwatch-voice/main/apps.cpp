#include "apps.h"
#include "radio_app.h"
#include "voice_screen.h"
#include "pitch_detector.h"
#include "character_touch.h"
#include "ear_game.h"
#include "audio_spectrum.h"
#include "audio_buffer.h"
#include "tts.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "pomodoro.h"
#include "app_stopwatch.h"
#include "app_volume.h"
#include <ctime>
#include <atomic>
#include <M5Unified.h>
#include <utility/M5IOE1_Class.hpp>
#include "esp_timer.h"
#include "nvs.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef BOTWATCH_RADIO_MEASURE
extern void radio_measure();
#endif
namespace apps {
namespace {
struct AudioCapture {
    // Two analysis windows retain complete 512-sample hops while a changed
    // note is drawn. The producer follows the existing microphone callback.
    audio_memory::Buffer<int16_t> ring;
    std::atomic<uint64_t> written{0},requested{0};
    std::atomic<unsigned> callbacks{0};
    std::atomic<bool> active{false},on{false},failed{false};
    int64_t last_us=0,min_us=INT64_MAX,max_us=0;
} audio_capture;
Stopwatch stopwatch;
model::Pomodoro pomodoro;
esp_timer_handle_t vibration_timer=nullptr;
constexpr uint64_t phase_vibration_us=70*1000; // Original app_pomodoro profile.
constexpr uint8_t phase_vibration_strength=90;
int motor_word() {
    // M5IOE1 PWM1 duty/enable registers used by Power_Class::setVibration.
    uint8_t value[2];
    auto &io=static_cast<m5::M5IOE1_Class&>(M5.getIOExpander(0));
    if(!io.readRegister(0x1b,value,sizeof(value)))return -1;
    return value[0]|int(value[1])<<8;
}
uint32_t now_ms(){return uint32_t(esp_timer_get_time()/1000);}
std::string elapsed_text(uint32_t ms) {
    char text[48];std::snprintf(text,sizeof(text),"%02u:%02u:%02u.%02u",unsigned(ms/3600000),unsigned(ms/60000%60),unsigned(ms/1000%60),unsigned(ms%1000/10));return text;
}
int month_days(int year,int month){const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};return days[month-1]+(month==2 && (year%4==0 && (year%100!=0 || year%400==0)));}
bool valid_clock(const m5::rtc_datetime_t &v){return v.date.year>=2000 && v.date.year<=2099 && v.date.month>=1 && v.date.month<=12 && v.date.date>=1 && v.date.date<=month_days(v.date.year,v.date.month) && v.time.hours>=0 && v.time.hours<24 && v.time.minutes>=0 && v.time.minutes<60 && v.time.seconds>=0 && v.time.seconds<60;}
m5::rtc_datetime_t build_datetime() {
    char month[4];int day,year,hour,minute,second;
    std::sscanf(__DATE__,"%3s %d %d",month,&day,&year);
    std::sscanf(__TIME__,"%d:%d:%d",&hour,&minute,&second);
    const char *months="JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *found=std::strstr(months,month);
    m5::rtc_datetime_t value;
    value.date.year=year;value.date.month=(found-months)/3+1;value.date.date=day;
    value.time.hours=hour;value.time.minutes=minute;value.time.seconds=second;
    return value;
}
bool save_clock(m5::rtc_datetime_t value) {
    if(!valid_clock(value) || !M5.Rtc.isEnabled()){printf("ERROR CLOCK invalid_or_unsupported\n");return false;}
    auto tm=value.get_tm();std::mktime(&tm);value.date.weekDay=tm.tm_wday;
    M5.Rtc.setDateTime(value);
    m5::rtc_datetime_t actual;
    const bool ok=M5.Rtc.getDateTime(&actual) && actual.date.year==value.date.year && actual.date.month==value.date.month && actual.date.date==value.date.date && actual.time.hours==value.time.hours && actual.time.minutes==value.time.minutes;
    printf("CLOCK_SAVE result=%d\n",ok);return ok;
}
void stop_audio() {
    audio_capture.on=false;
    while(M5.Mic.isRecording() || (audio_capture.active && (audio_capture.written.load()<audio_capture.requested.load() || audio_capture.callbacks.load())))vTaskDelay(1);
    if(audio_capture.active)printf("AUDIO_CAPTURE samples=%llu min_hop_us=%lld max_hop_us=%lld failed=%d\n",(unsigned long long)audio_capture.written.load(),(long long)audio_capture.min_us,(long long)audio_capture.max_us,audio_capture.failed.load());
    audio_capture.active=false;
    M5.Mic.end();
    audio_capture.ring.reset();
    while(M5.Speaker.isPlaying())vTaskDelay(1);
    M5.Speaker.end();
}
}
bool microphone_released(void *data,size_t samples) {
    if(!audio_capture.active.load())return false;
    const auto *begin=audio_capture.ring.data();
    if(static_cast<int16_t*>(data)<begin || static_cast<int16_t*>(data)>=begin+audio_capture.ring.size())return false;
    ++audio_capture.callbacks;
    const int64_t now=esp_timer_get_time();
    if(audio_capture.last_us){const auto elapsed=now-audio_capture.last_us;audio_capture.min_us=std::min(audio_capture.min_us,elapsed);audio_capture.max_us=std::max(audio_capture.max_us,elapsed);}
    audio_capture.last_us=now;
    const auto written=audio_capture.written.load()+samples;audio_capture.written=written;
    if(audio_capture.on.load()) {
        audio_capture.requested.fetch_add(tuner::PitchDetector::hopSize);
        if(!M5.Mic.record(audio_capture.ring.data()+written%audio_capture.ring.size(),tuner::PitchDetector::hopSize,tuner::PitchDetector::sampleRate)) {
            audio_capture.requested.fetch_sub(tuner::PitchDetector::hopSize);audio_capture.failed=true;
        }
    }
    --audio_capture.callbacks;
    return true;
}

bool reserve_audio_memory() {
    constexpr size_t analysis_bytes=audio_spectrum::Workspace::requiredBytes(tuner::PitchDetector::frameSize)+3*tuner::PitchDetector::frameSize*sizeof(int16_t);
    constexpr size_t ear_bytes=3*ear_game::ToneStream::sampleRate*ear_game::ToneStream::frameDurationMs/1000*sizeof(int16_t);
    const size_t radio_bytes=radio_app::audio_memory_bytes();
    const size_t bytes=std::max({analysis_bytes,ear_bytes,radio_bytes});
    const bool ok=audio_memory::arena.reserve(bytes);
    printf("AUDIO_ARENA reserved=%u analysis=%u ear=%u radio=%u ok=%d\n",unsigned(audio_memory::arena.capacity()),unsigned(analysis_bytes),unsigned(ear_bytes),unsigned(radio_bytes),ok);
    return ok;
}

void tick() {
    if(!pomodoro.update(now_ms()))return;
    if(!vibration_timer) {
        esp_timer_create_args_t args{};args.callback=[](void*){M5.Power.setVibration(0);printf("POMODORO_VIBRATION stopped=1 motor_word=%d\n",motor_word());fflush(stdout);};args.name="pomodoro-vibration";
        if(esp_timer_create(&args,&vibration_timer)!=ESP_OK){printf("ERROR POMODORO vibration_timer\n");return;}
    }
    M5.Power.setVibration(phase_vibration_strength);
    const auto err=esp_timer_start_once(vibration_timer,phase_vibration_us);
    if(err!=ESP_OK)M5.Power.setVibration(0);
    printf("POMODORO_PHASE phase=%d remaining=%u vibration=%s motor_word=%d\n",int(pomodoro.phase()),unsigned(pomodoro.remainingSeconds()),esp_err_to_name(err),motor_word());fflush(stdout);
}

int run(StreamBufferHandle_t serial, const std::function<bool(uint8_t *, int)> &read,
        const std::function<void()> &capture,bool radio_only) {
    size_t lap_index=0;bool clock_editing=false;int clock_field=0;
    m5::rtc_datetime_t clock_value;
    int64_t last_time_view=-1;
    int rotation=0,current=-1,calibration=tuner::calibrationDefaultHz;
    uint8_t japanese=1;
    int32_t ear_volume=80; // Original Hal::getSpeakerVolume default.
    nvs_handle_t audio_settings;
    if(nvs_open("system",NVS_READONLY,&audio_settings)==ESP_OK){nvs_get_i32(audio_settings,"spk_vol",&ear_volume);nvs_close(audio_settings);}
    ear_volume=std::clamp<int32_t>(ear_volume,0,100);
    nvs_handle_t settings;
    if(nvs_open("tuner",NVS_READONLY,&settings)==ESP_OK) {
        int32_t saved=calibration;nvs_get_i32(settings,"a4_hz",&saved);
        calibration=std::clamp<int>(saved,tuner::calibrationMinHz,tuner::calibrationMaxHz);
        nvs_get_u8(settings,"japanese",&japanese);japanese=japanese?1:0;nvs_close(settings);
    }
    int32_t saved_audio_volume=ear_volume;
    int saved_calibration=calibration;uint8_t saved_japanese=japanese;
    auto save=[&] {
        if(calibration==saved_calibration && japanese==saved_japanese) { printf("TUNER_SAVE writes=0\n");return; }
        esp_err_t err=nvs_open("tuner",NVS_READWRITE,&settings);
        if(err==ESP_OK) {
            if(calibration!=saved_calibration)err=nvs_set_i32(settings,"a4_hz",calibration);
            if(err==ESP_OK && japanese!=saved_japanese)err=nvs_set_u8(settings,"japanese",japanese);
            if(err==ESP_OK)err=nvs_commit(settings);
            nvs_close(settings);
        }
        printf("TUNER_SAVE writes=%d result=%s\n",err==ESP_OK,esp_err_to_name(err));
        if(err==ESP_OK){saved_calibration=calibration;saved_japanese=japanese;}
    };
    tuner::Tracker tracker;
    tuner::PitchResult pitch;
    audio_memory::Buffer<int16_t> window;size_t window_size=0;
    audio_spectrum::Workspace fft_workspace;
    const char *menu_message=nullptr;
    uint64_t capture_scan=0;
    bool recording=false,injected=false;
    std::span<const float> spectrum;
    double peakHz=0;
    ear_game::Question question;
    ear_game::Mode gameMode=ear_game::Mode::Note;
    ear_game::ToneStream tones;
    audio_memory::Buffer<int16_t> playback[3];
    unsigned earFrame=0,earFrames=0;int feedback=-1;
    auto play=[&] {
        while(M5.Speaker.isPlaying())vTaskDelay(1);
        tones.start(question.sounds);earFrame=0;earFrames=question.sounds.size()*ear_game::ToneStream::framesPerBeat;
        M5.Speaker.setVolume(ear_volume*255/100);
        if(ear_volume>0 && !tts::is_muted() && !M5.Speaker.begin())printf("ERROR EAR speaker_begin\n");
        printf("EAR_PLAY frames=%u samples=%u muted=%d\n",earFrames,earFrames*ear_game::ToneStream::sampleRate*ear_game::ToneStream::frameDurationMs/1000,tts::is_muted());
    };
    auto change_volume=[&] {
        ear_volume=transceiver::volume::nextPreset(ear_volume);
        printf("EAR_VOLUME percent=%d pending=%d\n",int(ear_volume),ear_volume!=saved_audio_volume);
        if(ear_volume>0)play();else M5.Speaker.stop();
    };
    auto next_question=[&] {
        question=ear_game::makeQuestion(gameMode,esp_random());feedback=-1;play();
        printf("EAR_QUESTION mode=%d correct=%d choices=%s|%s|%s\n",int(gameMode),question.correctChoice,question.choices[0].c_str(),question.choices[1].c_str(),question.choices[2].c_str());
    };
    characters::TouchGesture gesture;
    int tx=0,ty=0;
    auto draw=[&] {
        if(current==-1)voice_screen::app_menu(rotation,menu_message);
        else if(current==2)voice_screen::app_tuner(pitch,calibration,japanese);
        else if(current==3)voice_screen::app_spectrum(spectrum,peakHz);
        else if(current==4)voice_screen::app_ear(question,feedback,ear_volume);
        else if(current==8)voice_screen::app_radio(radio_app::status(),ear_volume);
        else if(current==5) {
            const auto &laps=stopwatch.laps();std::string detail="ラップなし";
            if(!laps.empty()){lap_index=std::min(lap_index,laps.size()-1);detail="ラップ "+std::to_string(lap_index+1)+"/"+std::to_string(laps.size())+" "+elapsed_text(laps[lap_index]);}
            voice_screen::app_timer("ストップウォッチ",elapsed_text(stopwatch.elapsed(now_ms())),detail,stopwatch.running()?"ラップ":"リセット",stopwatch.running()?"とめる":"はじめる");
        } else if(current==6) {
            const unsigned remaining=pomodoro.remainingSeconds();char value[32];std::snprintf(value,sizeof(value),"%02u:%02u",remaining/60,remaining%60);
            voice_screen::app_timer("ポモドーロ",value,pomodoro.phase()==model::Pomodoro::Phase::Focus?"しゅうちゅう 25ふん":"きゅうけい 5ふん","リセット",pomodoro.state()==model::Pomodoro::State::Running?"とめる":"はじめる");
        } else if(current==7) {
            const bool rtc_valid=clock_editing || M5.Rtc.getDateTime(&clock_value);
            if(!rtc_valid)clock_value=build_datetime();
            char date[48],time[32];std::snprintf(date,sizeof(date),"%04d/%02d/%02d",clock_value.date.year,clock_value.date.month,clock_value.date.date);
            std::snprintf(time,sizeof(time),"%02d:%02d:%02d",clock_value.time.hours,clock_value.time.minutes,clock_value.time.seconds);
            const char *fields[]={"とし","つき","ひ","じ","ふん","びょう"};
            voice_screen::app_clock(date,time,clock_editing?fields[clock_field]:rtc_valid?"":"じこくがみせってい",clock_editing);
        }
    };
    auto leave=[&] {
        if(current==8)radio_app::stop();
        const size_t before_bytes=audio_capture.ring.bytes()+window.bytes()+tracker.bytes()+fft_workspace.bytes()+playback[0].bytes()+playback[1].bytes()+playback[2].bytes();
        tones.stop();earFrames=0;stop_audio();recording=false;
        for(auto &buffer:playback)buffer.reset();
        window.reset();window_size=0;spectrum={};tracker.end();fft_workspace.end();question={};if(current==2 || current==3 || current==4 || current==8)audio_memory::arena.rewind();
        printf("AUDIO_RELEASE app=%d before_bytes=%u capture=%u window=%u spectrum=%u fft=%u playback=%u\n",current,unsigned(before_bytes),unsigned(audio_capture.ring.capacity()),unsigned(window.capacity()),unsigned(spectrum.size()),unsigned(fft_workspace.bytes()+tracker.bytes()),unsigned(playback[0].capacity()+playback[1].capacity()+playback[2].capacity()));
        if((current==4 || current==8) && ear_volume!=saved_audio_volume) {
            auto err=nvs_open("system",NVS_READWRITE,&audio_settings);
            if(err==ESP_OK){err=nvs_set_i32(audio_settings,"spk_vol",ear_volume);if(err==ESP_OK)err=nvs_commit(audio_settings);nvs_close(audio_settings);}
            printf("AUDIO_VOLUME_SAVE percent=%d result=%s\n",int(ear_volume),esp_err_to_name(err));
            if(err==ESP_OK)saved_audio_volume=ear_volume;
        }
        if(current==2)save();
        tracker.reset();pitch={};window_size=0;injected=false;gesture.clear();clock_editing=false;last_time_view=-1;
    };
    auto app_memory=[&](const char *stage) {
        printf("APP_MEMORY stage=%s app=%d internal=%u internal_largest=%u psram=%u psram_largest=%u arena_reserved=%u arena_used=%u\n",stage,current,unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),unsigned(audio_memory::arena.capacity()),unsigned(audio_memory::arena.used()));
    };
    auto select=[&](int index) {
        leave();
        if(index==2 || index==3 || index==4 || index==8)voice_screen::discard_overlay();
        current=index;menu_message=nullptr;app_memory("before_allocate");
        bool allocated=true;
        if(current==2)allocated=tracker.begin();
        if(current==3)allocated=fft_workspace.begin(tuner::PitchDetector::frameSize);
        if(allocated && (current==2 || current==3))allocated=audio_capture.ring.allocate(2*tuner::PitchDetector::frameSize) && window.allocate(tuner::PitchDetector::frameSize);
        if(current==4)for(auto &buffer:playback)if(!buffer.allocate(ear_game::ToneStream::sampleRate*ear_game::ToneStream::frameDurationMs/1000))allocated=false;
        if(!allocated) {
            app_memory("allocation_failed");leave();current=-1;menu_message="メモリがたりません";
            printf("ERROR APP memory\n");
        } else if(current==8 && !radio_app::start()) {
            menu_message=radio_app::status();leave();current=-1;
        }
        if((current==2 || current==3) && !M5.Mic.begin()){printf("ERROR APP microphone_begin\n");leave();current=-1;}
        if(current==4)next_question();
        app_memory("allocated");draw();printf("APP current=%d\n",current);fflush(stdout);
    };
    auto feed=[&](const int16_t *data,size_t size) {
        for(size_t i=0;i<size;) {
            const size_t count=std::min(size-i,tuner::PitchDetector::frameSize-window_size);
            std::copy_n(data+i,count,window.data()+window_size);window_size+=count;i+=count;
            if(window_size==tuner::PitchDetector::frameSize) {
                const int64_t start=esp_timer_get_time();
                if(current==3) {
                    spectrum=fft_workspace.power({window.data(),window_size});
                    const auto index=std::max_element(spectrum.begin()+1,spectrum.end())-spectrum.begin();
                    peakHz=spectrum[index]>0?audio_spectrum::peak(spectrum,index,window_size,tuner::PitchDetector::sampleRate).hz:0;
                    if(injected)printf("SPECTRUM peak_hz=%.6f bins=%u\n",peakHz,unsigned(spectrum.size()));
                } else {
                pitch=tracker.update({window.data(),window_size},calibration);
                if(injected)printf("TUNER valid=%d hz=%.6f cents=%.3f midi=%d octave=%d confidence=%.5f reference=%d us=%lld raw_valid=%d raw_hz=%.6f display_age=%d\n",
                    pitch.valid,pitch.frequencyHz,pitch.cents,pitch.midi,pitch.octave,pitch.confidence,calibration,(long long)(esp_timer_get_time()-start),tracker.latestAnalysis().valid,tracker.latestAnalysis().frequencyHz,tracker.displayAgeSamples());
                }
                const int64_t render_start=esp_timer_get_time();
                draw();
                if(injected)printf("AUDIO_TIMING total_us=%lld render_us=%lld\n",(long long)(esp_timer_get_time()-start),(long long)(esp_timer_get_time()-render_start));
                std::memmove(window.data(),window.data()+tuner::PitchDetector::hopSize,(window_size-tuner::PitchDetector::hopSize)*sizeof(int16_t));window_size-=tuner::PitchDetector::hopSize;
            }
        }
    };
    if(radio_only)select(8);
    else {draw();printf("APP menu\n");fflush(stdout);}
    while(true) {
        M5.update();tick();bool down=false;
        if(M5.Touch.getCount()) {const auto &point=M5.Touch.getDetail(0);down=point.isPressed();tx=point.x;ty=point.y;}
        const auto event=gesture.update(down,tx,ty,uint32_t(esp_timer_get_time()/1000));
        int selected=-1;
        if(event.kind==characters::TouchKind::mode_hold){if(radio_only){leave();return 3;}select(-1);}
        if(current==-1) {
            if(event.kind==characters::TouchKind::swipe_left || event.kind==characters::TouchKind::swipe_right) {
                rotation=(rotation+(event.kind==characters::TouchKind::swipe_left?1:voice_screen::app_count()-1))%voice_screen::app_count();draw();
            } else if(event.kind==characters::TouchKind::tap)selected=voice_screen::app_at(rotation,tx,ty);
        } else if(current==2 && event.kind==characters::TouchKind::tap) {
            const int x=tx-(M5.Display.width()-466)/2,y=ty-(M5.Display.height()-466)/2;
            int next=calibration;
            if(y>=350 && y<395) {if(x<170)--next;else if(x>296)++next;}
            if(y>=395 && y<440) {if(x<190)next=440;else if(x<275)next=442;else japanese=!japanese;}
            next=std::clamp(next,tuner::calibrationMinHz,tuner::calibrationMaxHz);
            if(next!=calibration){calibration=next;tracker.reset();pitch={};}draw();
        }
        if(current==4 && event.kind==characters::TouchKind::tap) {
            const int x=tx-(M5.Display.width()-466)/2,y=ty-(M5.Display.height()-466)/2;
            if(y>=90 && y<140) {gameMode=x<180?ear_game::Mode::Note:x<285?ear_game::Mode::Scale:ear_game::Mode::Chord;next_question();}
            else if(y>=155 && y<310) {const int choice=(y-155)/51;feedback=choice==question.correctChoice;printf("EAR_ANSWER choice=%d correct=%d\n",choice,feedback);}
            else if(y>=380 && y<430) {if(x<233)play();else next_question();}
            draw();
        }
        if(current==5 || current==6) {
            const bool tap=event.kind==characters::TouchKind::tap;
            const int x=tx-(M5.Display.width()-466)/2,y=ty-(M5.Display.height()-466)/2;
            if(M5.BtnA.wasPressed() || (tap && y>=335 && y<405 && x<233)) {
                if(current==5){stopwatch.left(now_ms());if(!stopwatch.laps().empty())lap_index=stopwatch.laps().size()-1;}
                else {pomodoro.reset();}
                draw();
            }
            if(M5.BtnB.wasPressed() || (tap && y>=335 && y<405 && x>=233)) {
                if(current==5)stopwatch.toggle(now_ms());else pomodoro.toggle(now_ms());draw();
            }
            if(current==5 && tap && y>=250 && y<320 && !stopwatch.laps().empty()){lap_index=(lap_index+1)%stopwatch.laps().size();draw();}
        }
        if(current==7 && event.kind==characters::TouchKind::tap) {
            const int x=tx-(M5.Display.width()-466)/2,y=ty-(M5.Display.height()-466)/2;
            if(!clock_editing && y>=345 && y<420){
                if(!M5.Rtc.getDateTime(&clock_value))clock_value=build_datetime();
                clock_editing=true;clock_field=0;
            }
            else if(clock_editing && y>=375 && y<435){if(save_clock(clock_value))clock_editing=false;}
            else if(clock_editing && y>=305 && y<370) {
                if(x>=195 && x<=275)clock_field=(clock_field+1)%6;
                else {
                    const int delta=x<195?-1:1;
                    if(clock_field==0)clock_value.date.year=std::clamp(int(clock_value.date.year)+delta,2000,2099);
                    if(clock_field==1)clock_value.date.month=(clock_value.date.month-1+delta+12)%12+1;
                    if(clock_field==2)clock_value.date.date=(clock_value.date.date-1+delta+month_days(clock_value.date.year,clock_value.date.month))%month_days(clock_value.date.year,clock_value.date.month)+1;
                    if(clock_field==3)clock_value.time.hours=(clock_value.time.hours+delta+24)%24;
                    if(clock_field==4)clock_value.time.minutes=(clock_value.time.minutes+delta+60)%60;
                    if(clock_field==5)clock_value.time.seconds=(clock_value.time.seconds+delta+60)%60;
                    clock_value.date.date=std::min<int>(clock_value.date.date,month_days(clock_value.date.year,clock_value.date.month));
                }
            }draw();
        }
        if(current>=5 && current<=7) {
            const int64_t time_view=current==5?stopwatch.elapsed(now_ms())/10:current==6?pomodoro.remainingSeconds():esp_timer_get_time()/1000000;
            if(time_view!=last_time_view){last_time_view=time_view;draw();}
        }
        if(current==8 && M5.BtnA.wasPressed()){ear_volume=transceiver::volume::nextPreset(ear_volume);M5.Speaker.setVolume(ear_volume*255/100);draw();}
        if(current==8 && radio_app::update(M5.BtnB.isPressed(),ear_volume))draw();
        if(current==4 && M5.BtnA.wasPressed()){change_volume();draw();}
        if(current==4 && M5.BtnB.wasPressed()){play();}
        if(current==4 && earFrame<earFrames && M5.Speaker.isPlaying(0)<2) {
            auto &buffer=playback[earFrame%3];tones.render({buffer.data(),buffer.size()});
            if(ear_volume>0 && !tts::is_muted() && !M5.Speaker.playRaw(buffer.data(),buffer.size(),ear_game::ToneStream::sampleRate,false,1,0)) {printf("ERROR EAR play\n");earFrames=0;}
            ++earFrame;
            if(earFrame==earFrames){tones.stop();printf("EAR_PLAY_DONE frames=%u muted=%d\n",earFrame,tts::is_muted());}
        }
        if((current==2 || current==3) && !injected) {
            if(!recording) {
                audio_capture.written=0;audio_capture.requested=tuner::PitchDetector::hopSize;audio_capture.last_us=0;audio_capture.min_us=INT64_MAX;audio_capture.max_us=0;
                audio_capture.failed=false;audio_capture.active=true;audio_capture.on=true;capture_scan=0;
                recording=M5.Mic.record(audio_capture.ring.data(),tuner::PitchDetector::hopSize,tuner::PitchDetector::sampleRate);
                if(!recording){audio_capture.requested=0;printf("ERROR APP microphone_record\n");select(-1);}
            }
            const auto limit=audio_capture.written.load();
            if(recording && (audio_capture.failed || limit-capture_scan>=audio_capture.ring.size())) {
                printf("ERROR APP audio_capture_overrun_or_chain written=%llu processed=%llu\n",(unsigned long long)limit,(unsigned long long)capture_scan);select(-1);
            } else if(recording)while(capture_scan+tuner::PitchDetector::hopSize<=limit) {
                feed(audio_capture.ring.data()+capture_scan%audio_capture.ring.size(),tuner::PitchDetector::hopSize);
                capture_scan+=tuner::PitchDetector::hopSize;
            }
        }
        uint8_t command[4];
        if(xStreamBufferReceive(serial,command,1,pdMS_TO_TICKS((current>=2 && current<=4) || current==8?1:20))==1 && read(command+1,3)) {
#ifdef BOTWATCH_RADIO_MEASURE
            if(!std::memcmp(command,"RADI",4) && current==-1){radio_measure();continue;}
#endif
            if(!std::memcmp(command,"LFRM",4) || !std::memcmp(command,"SHOT",4))capture();
            else if(!std::memcmp(command,"ASEL",4)) {
                uint8_t index;if(read(&index,1) && index<voice_screen::app_count())selected=index;else printf("ERROR APP index\n");
            } else if(!std::memcmp(command,"ABAK",4)){if(radio_only){leave();return 3;}select(-1);}
            else if(!std::memcmp(command,"INFO",4) && radio_only) {
                printf("BOOT_REPORT_BEGIN\nMODE radio partition=none\nBOOT_REPORT_END\nMEM serving internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n",unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
            }
            else if(!std::memcmp(command,"RDST",4) && current==8){radio_app::drop_next_talk_stop();}
            else if(!std::memcmp(command,"RSTA",4)){radio_app::stats();}
            else if(!std::memcmp(command,"RKEY",4)){uint8_t value;if(read(&value,1) && current==8)radio_app::key(value!=0);else printf("ERROR RADIO app_required\n");}
            else if(!std::memcmp(command,"RPCM",4)){
                uint8_t size[2];if(!read(size,2))continue;const unsigned count=size[0]|unsigned(size[1])<<8;
                if(count>tuner::PitchDetector::frameSize){printf("ERROR RPCM size\n");continue;}
                std::array<int16_t,320> input;
                if(count==input.size()){if(read(reinterpret_cast<uint8_t*>(input.data()),count*sizeof(int16_t)))radio_app::inject(input.data(),count);}
                else {for(size_t i=0;i<count;){const auto n=std::min(count-i,input.size());if(!read(reinterpret_cast<uint8_t*>(input.data()),n*sizeof(int16_t)))break;i+=n;}printf("ERROR RPCM size\n");}
            }
            else if(!std::memcmp(command,"AROT",4)){rotation=(rotation+1)%voice_screen::app_count();draw();}
            else if(!std::memcmp(command,"TPCM",4)) {
                uint8_t size[2];if(!read(size,2))continue;
                const unsigned count=size[0]|unsigned(size[1])<<8;
                if(!count || count>tuner::PitchDetector::frameSize){printf("ERROR TPCM size\n");continue;}
                std::array<int16_t,tuner::PitchDetector::hopSize> input;
                const bool audio_app=current==2 || current==3;
                if(audio_app && !injected){stop_audio();recording=false;window_size=0;tracker.reset();pitch={};injected=true;}
                bool complete=true;
                for(size_t i=0;i<count;){const auto n=std::min(count-i,input.size());if(!read(reinterpret_cast<uint8_t*>(input.data()),n*sizeof(int16_t))){complete=false;break;}if(audio_app)feed(input.data(),n);i+=n;}
                if(!complete)printf("ERROR TPCM short\n");else if(!audio_app)printf("ERROR TPCM audio_app_required\n");else printf("TPCM_ACK %u\n",count);
            } else if(!std::memcmp(command,"EGAM",4)) {
                uint8_t mode;if(read(&mode,1) && current==4 && mode<3){gameMode=ear_game::Mode(mode);next_question();draw();}else printf("ERROR EGAM range_or_app\n");
            } else if(!std::memcmp(command,"EANS",4)) {
                uint8_t choice;if(read(&choice,1) && current==4 && choice<3){feedback=choice==question.correctChoice;draw();printf("EAR_ANSWER choice=%d correct=%d\n",choice,feedback);}else printf("ERROR EANS range_or_app\n");
            } else if(!std::memcmp(command,"EVOL",4) && current==4){change_volume();draw();
            } else if(!std::memcmp(command,"ENXT",4) && current==4){next_question();draw();
            } else if(!std::memcmp(command,"EPLY",4) && current==4){play();
            } else if(!std::memcmp(command,"SWTO",4)){stopwatch.toggle(now_ms());draw();printf("SWTO running=%d\n",stopwatch.running());
            } else if(!std::memcmp(command,"SWLP",4)){stopwatch.left(now_ms());draw();printf("SWLP laps=%u\n",unsigned(stopwatch.laps().size()));
            } else if(!std::memcmp(command,"SWST",4)){printf("SWST elapsed=%u laps=%u running=%d\n",unsigned(stopwatch.elapsed(now_ms())),unsigned(stopwatch.laps().size()),stopwatch.running());
            } else if(!std::memcmp(command,"PMTO",4)){pomodoro.toggle(now_ms());draw();printf("PMTO state=%d\n",int(pomodoro.state()));
            } else if(!std::memcmp(command,"PMRS",4)){pomodoro.reset();draw();printf("PMRS\n");
            } else if(!std::memcmp(command,"PMST",4)){printf("PMST state=%d phase=%d remaining=%u\n",int(pomodoro.state()),int(pomodoro.phase()),unsigned(pomodoro.remainingSeconds()));
            } else if(!std::memcmp(command,"WSET",4)) {
                uint8_t value[7];if(!read(value,7))continue;
                m5::rtc_datetime_t next;next.date.year=value[0]|int(value[1])<<8;next.date.month=value[2];next.date.date=value[3];next.time.hours=value[4];next.time.minutes=value[5];next.time.seconds=value[6];
                save_clock(next);draw();
            } else if(!std::memcmp(command,"WGET",4)) {
                m5::rtc_datetime_t v;if(M5.Rtc.getDateTime(&v))printf("WGET %04d-%02d-%02d %02d:%02d:%02d\n",v.date.year,v.date.month,v.date.date,v.time.hours,v.time.minutes,v.time.seconds);else printf("ERROR WGET rtc\n");
            } else if(!std::memcmp(command,"TCAL",4)) {
                uint8_t value[2];if(!read(value,2))continue;
                const int next=value[0]|int(value[1])<<8;
                if(next<tuner::calibrationMinHz || next>tuner::calibrationMaxHz)printf("ERROR TCAL range\n");
                else {calibration=next;tracker.reset();pitch={};draw();printf("TCAL %d\n",calibration);}
            } else if(!std::memcmp(command,"TNAM",4)) {
                uint8_t value;if(read(&value,1) && value<2){japanese=value;draw();printf("TNAM %u\n",japanese);}else printf("ERROR TNAM range\n");
            } else if(!std::memcmp(command,"TRST",4)){tracker.reset();pitch={};window_size=0;draw();printf("TRST\n");}
            else printf("ERROR APP unsupported_command\n");
        }
        if(selected>=0) {
            if(selected<2){leave();return selected==0?1:0;}
            if(selected==8 && !radio_only){leave();return 2;}
            if(selected>=2 && selected<voice_screen::app_count())select(selected);
            else printf("ERROR APP unsupported=%d\n",selected);
        }
        fflush(stdout);
    }
}
}
