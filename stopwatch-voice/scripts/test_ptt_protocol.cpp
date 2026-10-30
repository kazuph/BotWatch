#include "radio/model/protocol.h"
#include "radio/transport/g711.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace transceiver;
int main() {
 MacAddress a={1,2,3,4,5,6},b={6,5,4,3,2,1},alien={9,9,9,9,9,9};
 Protocol parent(a),child(b);parent.startDiscovery();child.startDiscovery();parent.startCall(123);child.startCall(456);
 parent.receiveCall(b,456);child.receiveCall(a,123);assert(parent.phase()==Phase::Calling && child.phase()==Phase::Incoming);
 assert(child.answer());parent.receiveAnswer(b,123);child.receiveConfirm(a,123);assert(parent.phase()==Phase::Connected && child.phase()==Phase::Connected);
 assert(parent.beginLocalTalk());assert(child.beginLocalTalk());parent.receiveTalkStart(b,123,Role::Child);child.receiveTalkStart(a,123,Role::Parent);
 assert(parent.localTalking() && !parent.remoteTalking() && !child.localTalking() && child.remoteTalking());
 assert(!parent.acceptAudio(b,123,Role::Child));assert(child.acceptAudio(a,123,Role::Parent));
 assert(!child.acceptAudio(alien,123,Role::Parent));assert(!child.acceptAudio(a,456,Role::Parent));
 parent.endLocalTalk();child.receiveTalkStop(a,123,Role::Parent);assert(child.beginLocalTalk());parent.receiveTalkStart(b,123,Role::Child);assert(parent.remoteTalking());
 parent.receiveHangup(b,456);assert(parent.phase()==Phase::Connected);parent.receiveHangup(b,123);assert(parent.phase()==Phase::Off);parent.receiveCall(b,123);assert(parent.phase()==Phase::Off);
 assert(child.receivePeerRestart(a));assert(child.phase()==Phase::Discovering && !child.localTalking() && !child.remoteTalking());
 assert(!child.setConversationMode(static_cast<ConversationMode>(1)));
 std::array<int16_t,320> pcm;for(unsigned i=0;i<pcm.size();++i)pcm[i]=4000*std::sin(2*3.141592653589793*440*i/16000);
 const auto encoded=g711::encodeFrame<16000>(pcm);std::array<int16_t,882> decoded;g711::decodeFrameInto<44100,882>(encoded,decoded.data());
 unsigned nonzero=0;for(auto s:decoded)nonzero+=s!=0;assert(nonzero>800);
 printf("PASS PTT connect/collision/single floor/alien/stale session/hangup/restart and G711 real codec\n");
}
