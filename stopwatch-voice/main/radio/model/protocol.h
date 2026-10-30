/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>

namespace transceiver {

using MacAddress = std::array<uint8_t, 6>;

enum class Phase : uint8_t {
    Off,
    Discovering,
    Calling,
    Incoming,
    Answering,
    Connected,
    Error,
};

enum class Role : uint8_t {
    None,
    Parent,
    Child,
};

enum class ConversationMode : uint8_t {
    Ptt,
};

class Protocol {
public:
    explicit Protocol(MacAddress localMac) : _local_mac(localMac)
    {
    }

    void startDiscovery()
    {
        if (_phase == Phase::Off) {
            _phase = Phase::Discovering;
            clearFloor();
        }
    }

    void startCall(uint32_t session)
    {
        if (_phase != Phase::Off && _phase != Phase::Discovering) {
            return;
        }
        _phase   = Phase::Calling;
        _role    = Role::Parent;
        _session = session;
        _peer.fill(0);
        clearFloor();
    }

    void receiveCall(const MacAddress& sender, uint32_t session)
    {
        if (sender == _retired_peer && session == _retired_session) {
            return;
        }
        if (_phase == Phase::Off || _phase == Phase::Discovering) {
            becomeIncoming(sender, session);
            return;
        }

        if (_phase == Phase::Calling && sender != _local_mac) {
            // Simultaneous ON has no shared clock. The lower station MAC is the deterministic
            // tie-breaker, so both devices converge on one parent and one child.
            if (sender < _local_mac) {
                becomeIncoming(sender, session);
            } else {
                retire(sender, session);
            }
        }
    }

    bool answer()
    {
        if (_phase != Phase::Incoming) {
            return false;
        }
        _phase = Phase::Answering;
        return true;
    }

    void receiveAnswer(const MacAddress& sender, uint32_t session)
    {
        if (_phase == Phase::Calling && _session == session) {
            _peer  = sender;
            _phase = Phase::Connected;
        }
    }

    void receiveConfirm(const MacAddress& sender, uint32_t session)
    {
        if (_phase == Phase::Answering && _peer == sender && _session == session) {
            _phase = Phase::Connected;
        }
    }

    void hangup()
    {
        retireCurrentSession();
        reset();
    }

    void setError()
    {
        reset();
        _phase = Phase::Error;
    }

    void receiveHangup(const MacAddress& sender, uint32_t session)
    {
        if ((_phase == Phase::Incoming || _phase == Phase::Answering || _phase == Phase::Connected) &&
            _peer == sender && _session == session) {
            retireCurrentSession();
            reset();
        }
    }

    bool receivePeerRestart(const MacAddress& sender)
    {
        if (_phase != Phase::Connected || _peer != sender) {
            return false;
        }
        retireCurrentSession();
        reset();
        _phase = Phase::Discovering;
        return true;
    }

    bool beginLocalTalk()
    {
        if (_phase != Phase::Connected || _conversation_mode != ConversationMode::Ptt) {
            return false;
        }
        if (_remote_talking && _role != Role::Parent) {
            return false;
        }
        _local_talking  = true;
        _remote_talking = false;
        return true;
    }

    void endLocalTalk()
    {
        _local_talking = false;
    }

    void receiveTalkStart(const MacAddress& sender, uint32_t session, Role talker)
    {
        if (!matches(sender, session) || talker == Role::None || talker == _role) {
            return;
        }
        if (_local_talking && _role == Role::Parent) {
            return;
        }
        _local_talking  = false;
        _remote_talking = true;
    }

    void receiveTalkStop(const MacAddress& sender, uint32_t session, Role talker)
    {
        if (matches(sender, session) && talker != _role) {
            _remote_talking = false;
        }
    }

    bool acceptAudio(const MacAddress& sender, uint32_t session, Role talker)
    {
        if (!matches(sender, session) || talker == Role::None || talker == _role) {
            return false;
        }
        receiveTalkStart(sender, session, talker);
        return _remote_talking;
    }

    bool setConversationMode(ConversationMode mode)
    {
        if (_phase != Phase::Connected || mode != ConversationMode::Ptt) {
            return false;
        }
        _conversation_mode = mode;
        clearFloor();
        return true;
    }

    Phase phase() const
    {
        return _phase;
    }
    Role role() const
    {
        return _role;
    }
    uint32_t session() const
    {
        return _session;
    }
    const MacAddress& peer() const
    {
        return _peer;
    }
    bool hasPeer() const
    {
        return _peer != MacAddress{};
    }
    bool matchesPeerSession(const MacAddress& sender, uint32_t session) const
    {
        return matches(sender, session);
    }
    bool localTalking() const
    {
        return _local_talking;
    }
    bool remoteTalking() const
    {
        return _remote_talking;
    }
    ConversationMode conversationMode() const
    {
        return _conversation_mode;
    }

private:
    void becomeIncoming(const MacAddress& sender, uint32_t session)
    {
        _phase   = Phase::Incoming;
        _role    = Role::Child;
        _session = session;
        _peer    = sender;
        clearFloor();
    }

    bool matches(const MacAddress& sender, uint32_t session) const
    {
        return _phase == Phase::Connected && _peer == sender && _session == session;
    }

    void clearFloor()
    {
        _local_talking  = false;
        _remote_talking = false;
    }

    void reset()
    {
        _phase   = Phase::Off;
        _role    = Role::None;
        _session = 0;
        _peer.fill(0);
        _conversation_mode = ConversationMode::Ptt;
        clearFloor();
    }

    void retireCurrentSession()
    {
        if (hasPeer()) {
            retire(_peer, _session);
        }
    }

    void retire(const MacAddress& peer, uint32_t session)
    {
        _retired_peer    = peer;
        _retired_session = session;
    }

    MacAddress _local_mac;
    MacAddress _peer                    = {};
    MacAddress _retired_peer            = {};
    Phase _phase                        = Phase::Off;
    Role _role                          = Role::None;
    uint32_t _session                   = 0;
    uint32_t _retired_session           = 0;
    bool _local_talking                 = false;
    bool _remote_talking                = false;
    ConversationMode _conversation_mode = ConversationMode::Ptt;
};

}  // namespace transceiver
