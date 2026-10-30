#include "radio_app.h"
#include "radio/transport/esp_now_radio.h"
#include "radio/transport/g711.h"
#include "tts.h"
#include "audio_buffer.h"
#include <new>
#include <M5Unified.h>
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
namespace radio_app {
namespace {
using namespace transceiver;
EspNowRadio radio;
std::unique_ptr<Protocol> protocol;
MacAddress target{};
std::atomic<bool> capture_active{false},capture_on{false},capture_failed{false};
std::atomic<uint64_t> written{0},requested{0};
std::atomic<unsigned> callbacks{0};
audio_memory::Buffer<int16_t> capture_ring;
uint64_t processed=0;
audio_memory::Buffer<AudioPacket> playback_queue;
size_t queue_read=0,queue_write=0,queue_count=0;
audio_memory::Buffer<int16_t> decoded[3];
unsigned decoded_index=0;
bool playback_started=false,talk_ended=true,injected=false,diagnostic_key=false,physical_was_down=false;
uint32_t sequence=0,last_audio_sequence=0;
bool have_sequence=false;
uint32_t tx=0,rx=0,played=0,drops=0,decode_hash=2166136261U;
int current_volume=80;
constexpr unsigned talkStopRetries=2; // Parent-approved: at most two retries.
constexpr unsigned remoteIdleFrames=30; // Parent-approved: 30 * 20 ms = 600 ms.
constexpr int64_t remoteIdleUs=int64_t(remoteIdleFrames)*captureDurationMs*1000;
int64_t last_remote_audio_us=0;
bool diagnostic_drop_talk_stop=false,memory_failed=false;
void memory(const char *stage) {
 printf("RADIO_MEM stage=%s internal=%u largest=%u psram=%u psram_largest=%u\n",stage,unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));fflush(stdout);
}
bool control(PacketType type,bool broadcast=false) {
 ControlPacket packet;packet.header.type=type;packet.header.session=protocol->session();packet.header.sequence=++sequence;packet.role=protocol->role();
 // Serialize with the driver's completion callback before queueing another frame.
 while(!radio.isSendReady())vTaskDelay(1);
 return broadcast?radio.sendBroadcast(&packet,sizeof(packet)):radio.sendToBlocking(protocol->hasPeer()?protocol->peer():target,&packet,sizeof(packet));
}
bool send_talk_stop() {
 for(unsigned retry=0;retry<=talkStopRetries;++retry) {
  const bool ok=control(PacketType::TalkStop);
  printf("RADIO_TALK_STOP attempt=%u success=%d\n",retry+1,ok);
  if(ok)return true;
 }
 return false;
}
void stop_capture() {
 capture_on=false;
 while(M5.Mic.isRecording() || (capture_active && (written.load()<requested.load() || callbacks.load())))vTaskDelay(1);
 capture_active=false;M5.Mic.end();injected=false;
}
void clear_playback() {
 queue_read=queue_write=queue_count=0;playback_started=false;talk_ended=true;have_sequence=false;last_remote_audio_us=0;
 while(M5.Speaker.isPlaying())vTaskDelay(1);
 M5.Speaker.end();
}
void reset_stream() {
 stop_capture();clear_playback();diagnostic_key=false;
}
void discover() {
 reset_stream();protocol->hangup();protocol->startDiscovery();control(PacketType::Discover,true);
}
void transmit(const int16_t *samples) {
 AudioPacket packet;packet.header.type=PacketType::Audio;packet.header.session=protocol->session();packet.header.sequence=++sequence;packet.talker=protocol->role();
 std::array<int16_t,sourceSamplesPerPacket> source;std::copy_n(samples,source.size(),source.begin());
 packet.samples=g711::encodeFrame<captureSampleRate>(source);
 while(!radio.isSendReady())vTaskDelay(1);
 if(radio.sendToBlocking(protocol->peer(),&packet,sizeof(packet)))++tx;
 else {printf("RADIO_DISCONNECT reason=send_failed\n");discover();}
}
void begin_capture() {
 clear_playback();control(PacketType::TalkStart);
 written=0;requested=sourceSamplesPerPacket;processed=0;capture_failed=false;capture_active=true;capture_on=true;
 if(!M5.Mic.begin() || !M5.Mic.record(capture_ring.data(),sourceSamplesPerPacket,captureSampleRate)) {
  requested=0;capture_on=false;capture_failed=true;printf("ERROR RADIO microphone_begin_or_record\n");
 }
}
void receive() {
 EspNowRadio::ReceivedPacket received;
 while(radio.receive(received)) {
  if(received.length<sizeof(PacketHeader))continue;
  PacketHeader header;std::memcpy(&header,received.payload.data(),sizeof(header));if(!validHeader(header))continue;
  if(header.type==PacketType::Audio) {
   if(received.length!=sizeof(AudioPacket))continue;
   AudioPacket packet;std::memcpy(&packet,received.payload.data(),sizeof(packet));
   if(packet.sampleCount!=audioSamplesPerPacket || !protocol->acceptAudio(received.sender,header.session,packet.talker))continue;
   if(capture_active)stop_capture();
   if(have_sequence && int32_t(header.sequence-last_audio_sequence)<=0)continue;
   have_sequence=true;last_audio_sequence=header.sequence;talk_ended=false;last_remote_audio_us=esp_timer_get_time();++rx;
   if(queue_count==playback_queue.size()){++drops;continue;}
   playback_queue[queue_write]=packet;queue_write=(queue_write+1)%playback_queue.size();++queue_count;
   continue;
  }
  if(received.length!=sizeof(ControlPacket))continue;
  ControlPacket packet;std::memcpy(&packet,received.payload.data(),sizeof(packet));if(!validConversationMode(packet.conversationMode))continue;
  switch(header.type) {
  case PacketType::Discover:
   if(protocol->receivePeerRestart(received.sender)){reset_stream();printf("RADIO_PEER_RESTART\n");control(PacketType::Discover,true);}
   if(protocol->phase()==Phase::Discovering) {
    target=received.sender;uint32_t session;do{session=esp_random();}while(!session);protocol->startCall(session);control(PacketType::Call);
   } else if(protocol->phase()==Phase::Calling)control(PacketType::Call);
   break;
  case PacketType::Call:
   protocol->receiveCall(received.sender,header.session);
   if(protocol->phase()==Phase::Incoming){reset_stream();protocol->answer();control(PacketType::Answer);}
   else if(protocol->phase()==Phase::Answering && protocol->peer()==received.sender && protocol->session()==header.session)control(PacketType::Answer);
   else if(protocol->phase()==Phase::Calling)control(PacketType::Call);
   break;
  case PacketType::Answer:
   protocol->receiveAnswer(received.sender,header.session);
   if(protocol->matchesPeerSession(received.sender,header.session))control(PacketType::Confirm);
   break;
  case PacketType::Confirm:protocol->receiveConfirm(received.sender,header.session);break;
  case PacketType::Hangup:
   if(protocol->matchesPeerSession(received.sender,header.session)){printf("RADIO_DISCONNECT reason=hangup\n");discover();}
   break;
  case PacketType::TalkStart:
   if(protocol->matchesPeerSession(received.sender,header.session)) {
    protocol->receiveTalkStart(received.sender,header.session,packet.role);
    if(protocol->remoteTalking()){stop_capture();clear_playback();talk_ended=false;last_remote_audio_us=esp_timer_get_time();}
   }break;
  case PacketType::TalkStop:
   if(protocol->matchesPeerSession(received.sender,header.session)) {
    if(diagnostic_drop_talk_stop){diagnostic_drop_talk_stop=false;printf("RADIO_DIAGNOSTIC dropped_talk_stop sequence=%u\n",unsigned(header.sequence));break;}
    protocol->receiveTalkStop(received.sender,header.session,packet.role);talk_ended=true;
   }
   break;
  default:break;
  }
 }
}
void playback() {
 if(!queue_count)return;
 if(!playback_started && !talk_ended && queue_count<playbackPrebufferFrames)return;
 if(!tts::is_muted() && M5.Speaker.isPlaying(0)>=2)return;
 if(!playback_started){
  if(!tts::is_muted() && !M5.Speaker.begin()){printf("ERROR RADIO speaker_begin\n");++drops;return;}
  M5.Speaker.setVolume(current_volume*255/100);playback_started=true;
 }
 auto &buffer=decoded[decoded_index++%3];
 g711::decodeFrameInto<playbackSampleRate,playbackSamplesPerPacket>(playback_queue[queue_read].samples,buffer.data());
 for(size_t i=0;i<buffer.size();++i){const auto value=buffer[i];decode_hash=(decode_hash^uint8_t(value))*16777619U;decode_hash=(decode_hash^uint8_t(value>>8))*16777619U;}
 if(!tts::is_muted() && !M5.Speaker.playRaw(buffer.data(),buffer.size(),playbackSampleRate,false,1,0)){printf("ERROR RADIO play\n");++drops;}
 ++played;queue_read=(queue_read+1)%playback_queue.size();--queue_count;
 if(!queue_count && talk_ended)playback_started=false;
}
}
size_t audio_memory_bytes() {
 return transportBufferFrames*(sourceSamplesPerPacket*sizeof(int16_t)+sizeof(AudioPacket))+3*playbackSamplesPerPacket*sizeof(int16_t);
}
bool start() {
 memory("before");memory_failed=false;MacAddress local{};esp_read_mac(local.data(),ESP_MAC_WIFI_STA);protocol.reset(new(std::nothrow)Protocol(local));
 if(!protocol){memory_failed=true;printf("ERROR RADIO protocol_memory\n");return false;}
 bool allocated=capture_ring.allocate(transportBufferFrames*sourceSamplesPerPacket) && playback_queue.allocate(transportBufferFrames);
 for(auto &buffer:decoded)if(!buffer.allocate(playbackSamplesPerPacket))allocated=false;
 if(!allocated){memory_failed=true;protocol->setError();memory("audio_memory_failed");printf("ERROR RADIO audio_memory\n");return false;}
 memory("audio_allocated");
 if(!radio.start()){protocol->setError();radio.stop();memory("start_failed");printf("ERROR RADIO start\n");return false;}
 tx=rx=played=drops=0;decode_hash=2166136261U;sequence=0;physical_was_down=false;diagnostic_drop_talk_stop=false;
 protocol->startDiscovery();control(PacketType::Discover,true);memory("ready");stats();return true;
}
void stop() {
 if(!protocol)return;
 if(radio.isStarted() && protocol->hasPeer())control(PacketType::Hangup);
 reset_stream();radio.stop();protocol.reset();capture_ring.reset();playback_queue.reset();
 for(auto &buffer:decoded)buffer.reset();
 memory("stopped");
}
bool microphone_released(void *data,size_t samples) {
 if(!capture_active)return false;
 auto *p=static_cast<int16_t*>(data);if(p<capture_ring.data() || p>=capture_ring.data()+capture_ring.size())return false;
 ++callbacks;const uint64_t limit=written.load()+samples;written=limit;
 if(capture_on){requested.fetch_add(sourceSamplesPerPacket);if(!M5.Mic.record(capture_ring.data()+limit%capture_ring.size(),sourceSamplesPerPacket,captureSampleRate)){requested.fetch_sub(sourceSamplesPerPacket);capture_failed=true;}}
 --callbacks;return true;
}
void drop_next_talk_stop() {diagnostic_drop_talk_stop=true;printf("RADIO_DIAGNOSTIC armed_drop_talk_stop=1\n");}
void key(bool pressed) {
 diagnostic_key=pressed;printf("RADIO_KEY pressed=%d\n",pressed);
}
void inject(const int16_t *samples,size_t count) {
 if(!protocol || !protocol->localTalking() || count!=sourceSamplesPerPacket){printf("ERROR RADIO inject_requires_ptt_320\n");return;}
 if(!injected){stop_capture();injected=true;}
 transmit(samples);printf("RADIO_PCM tx=%u\n",unsigned(tx));
}
bool update(bool pressed,int volume) {
 if(!protocol || !radio.isStarted())return false;
 const auto before=protocol->phase();const bool local=protocol->localTalking(),remote=protocol->remoteTalking();
 receive();current_volume=volume;
 if(protocol->remoteTalking() && esp_timer_get_time()-last_remote_audio_us>=remoteIdleUs) {
  const auto remoteRole=protocol->role()==Role::Parent?Role::Child:Role::Parent;
  protocol->receiveTalkStop(protocol->peer(),protocol->session(),remoteRole);talk_ended=true;
  printf("RADIO_REMOTE_TIMEOUT idle_us=%lld frames=%u\n",(long long)(esp_timer_get_time()-last_remote_audio_us),remoteIdleFrames);
 }
 const bool down=pressed||diagnostic_key;
 if(down && !physical_was_down && protocol->beginLocalTalk())begin_capture();
 if(!down && physical_was_down && protocol->localTalking()){stop_capture();protocol->endLocalTalk();send_talk_stop();stats();}
 physical_was_down=down;
 if(capture_active) {
  const auto limit=written.load();
  if(capture_failed || limit-processed>=capture_ring.size()){printf("ERROR RADIO capture_overrun\n");stop_capture();protocol->endLocalTalk();send_talk_stop();}
  else while(processed+sourceSamplesPerPacket<=limit && protocol->localTalking()){transmit(capture_ring.data()+processed%capture_ring.size());processed+=sourceSamplesPerPacket;}
 }
 playback();
 bool changed=before!=protocol->phase() || local!=protocol->localTalking() || remote!=protocol->remoteTalking();
 if(changed)stats();
 return changed;
}
const char *status() {
 if(memory_failed)return "メモリがたりません";
 if(!protocol || protocol->phase()==Phase::Error)return "むせんをはじめられません";
 if(protocol->phase()!=Phase::Connected)return "さがしています";
 if(protocol->localTalking())return "はなしています";
 if(protocol->remoteTalking())return "きいています";
 return "つながりました";
}
void stats() {
 printf("RADIO_STAT phase=%d role=%d session=%u local=%d remote=%d tx=%u rx=%u decoded=%u hash=%u drops=%u send_failures=%u receive_drops=%u muted=%d\n",protocol?int(protocol->phase()):-1,protocol?int(protocol->role()):0,protocol?unsigned(protocol->session()):0,protocol&&protocol->localTalking(),protocol&&protocol->remoteTalking(),unsigned(tx),unsigned(rx),unsigned(played),unsigned(decode_hash),unsigned(drops),unsigned(radio.sendFailureCount()),unsigned(radio.receiveDropCount()),tts::is_muted());fflush(stdout);
}
}
