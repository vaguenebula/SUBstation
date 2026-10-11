#pragma once
// MIDI 2.0's Universal MIDI Packets (UMP), the form MIDI 2.0 devices (and
// Windows MIDI Services, for MIDI 1.0 devices too) hand messages over in: a
// packet is one to four 32-bit words, the first word's top four bits its
// message type. What the engine takes from them is the channel voice
// messages, MIDI 1.0's (type 2) and MIDI 2.0's (type 4), as MidiInputEvents
// (MidiInput.h): MIDI 2.0's at MIDI 1.0's resolution (16-bit velocities, 32-bit
// controllers and pitch bends scaled down), except its per-note pitch bend,
// which MIDI 1.0 has no message for: that bends the one note it names, as a
// note's drawn bend does (NoteBend.h), over kMaxBendSemitones either way.
// Groups are merged (a track hears the 16 channels of every group alike).
//
// Header-only and allocation-free: Engine::sendUmp() decodes on the caller's
// thread, as Engine::midiInput() takes MIDI 1.0 bytes.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "MidiInput.h"
#include "NoteBend.h"

namespace sub::ump {

// A per-note pitch bend of none.
inline constexpr uint32_t kBendCenter = 0x80000000u;
// MIDI 2.0's channel voice opcodes (the high nibble of a type 4 packet's second byte).
inline constexpr uint8_t kPerNoteBend = 0x6;
inline constexpr uint8_t kPerNoteManagement = 0xF;
// Per-note management's flags: reset the note's controllers (its bend: none), detach them.
inline constexpr uint8_t kResetControllers = 0x01;
inline constexpr uint8_t kDetachControllers = 0x02;

// Words in the packet that starts with `word`, by its message type.
inline int packetWords(uint32_t word) noexcept {
    switch (word >> 28) {
        case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
        case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
        case 0xB: case 0xC: return 3;
        default: return 4;
    }
}

// A per-note pitch bend's 32 bits as semitones (`range` either way) and back.
inline double bendSemitones(uint32_t value, double range = kMaxBendSemitones) noexcept {
    return (static_cast<double>(value) - static_cast<double>(kBendCenter)) / static_cast<double>(kBendCenter) * range;
}
inline uint32_t bendValue(double semitones, double range = kMaxBendSemitones) noexcept {
    const double x = std::clamp(semitones / range, -1.0, 1.0) * static_cast<double>(kBendCenter);
    return static_cast<uint32_t>(std::clamp(std::llround(static_cast<double>(kBendCenter) + x), 0ll, 0xFFFFFFFFll));
}

// One packet (`count` words, at least packetWords(words[0])) into `event`'s
// message fields; false if it holds nothing a track hears (another type,
// a system message, or a controller MIDI 1.0 has no form for).
inline bool decode(const uint32_t* words, int count, MidiInputEvent& event) noexcept {
    if (count < 1) return false;
    const uint32_t first = words[0];
    const auto type = static_cast<uint8_t>(first >> 28);
    if (type == 0x2) {  // MIDI 1.0 channel voice: the bytes as they are
        event.kind = MidiInputEvent::Kind::Message;
        event.value = 0;
        event.status = static_cast<uint8_t>(first >> 16);
        event.data1 = static_cast<uint8_t>((first >> 8) & 0x7F);
        event.data2 = static_cast<uint8_t>(first & 0x7F);
        return (event.status & 0xF0) >= 0x80 && (event.status & 0xF0) < 0xF0;
    }
    if (type != 0x4 || count < 2) return false;
    const auto opcode = static_cast<uint8_t>((first >> 20) & 0xF);
    const auto channel = static_cast<uint8_t>((first >> 16) & 0xF);
    const auto index = static_cast<uint8_t>((first >> 8) & 0x7F);  // a note, or a controller
    const uint32_t data = words[1];
    event.kind = MidiInputEvent::Kind::Message;
    event.data1 = index;
    event.data2 = 0;
    event.value = 0;
    switch (opcode) {
        case 0x8:  // note off, a 16-bit velocity
            event.status = static_cast<uint8_t>(0x80 | channel);
            event.data2 = static_cast<uint8_t>(data >> 25);
            return true;
        case 0x9: {  // note on: velocity 0 is a note too, in MIDI 2.0 (the softest)
            event.status = static_cast<uint8_t>(0x90 | channel);
            event.data2 = static_cast<uint8_t>(std::max<uint32_t>(1, data >> 25));
            return true;
        }
        case 0xA:  // poly pressure, 32 bits
            event.status = static_cast<uint8_t>(0xA0 | channel);
            event.data2 = static_cast<uint8_t>(data >> 25);
            return true;
        case 0xB:  // control change, 32 bits
            event.status = static_cast<uint8_t>(0xB0 | channel);
            event.data2 = static_cast<uint8_t>(data >> 25);
            return true;
        case 0xC:  // program change: the program in the data word's top byte
            event.status = static_cast<uint8_t>(0xC0 | channel);
            event.data1 = static_cast<uint8_t>((data >> 24) & 0x7F);
            return true;
        case 0xD:  // channel pressure, 32 bits
            event.status = static_cast<uint8_t>(0xD0 | channel);
            event.data1 = static_cast<uint8_t>(data >> 25);
            return true;
        case 0xE: {  // pitch bend, 32 bits into 14
            const uint32_t bend = data >> 18;
            event.status = static_cast<uint8_t>(0xE0 | channel);
            event.data1 = static_cast<uint8_t>(bend & 0x7F);
            event.data2 = static_cast<uint8_t>((bend >> 7) & 0x7F);
            return true;
        }
        case kPerNoteBend:
            event.kind = MidiInputEvent::Kind::NoteBend;
            event.status = static_cast<uint8_t>((kPerNoteBend << 4) | channel);
            event.value = data;
            return true;
        case kPerNoteManagement:  // resetting its controllers takes its bend back to none
            if (!(first & kResetControllers)) return false;
            event.kind = MidiInputEvent::Kind::NoteBend;
            event.status = static_cast<uint8_t>((kPerNoteBend << 4) | channel);
            event.value = kBendCenter;
            return true;
        default:
            return false;  // per-note and registered/assignable controllers: no MIDI 1.0 form
    }
}

// MIDI 2.0 channel voice packets (group 0), for whatever sends them: tests, a computer keyboard.
inline std::array<uint32_t, 2> message(uint8_t opcode, int channel, int index, int low, uint32_t data) noexcept {
    return {0x40000000u | (static_cast<uint32_t>(opcode & 0xF) << 20) | (static_cast<uint32_t>(channel & 0xF) << 16) |
                (static_cast<uint32_t>(index & 0x7F) << 8) | static_cast<uint32_t>(low & 0xFF),
            data};
}
inline std::array<uint32_t, 2> noteOn(int channel, int key, uint16_t velocity) noexcept {
    return message(0x9, channel, key, 0, static_cast<uint32_t>(velocity) << 16);
}
inline std::array<uint32_t, 2> noteOff(int channel, int key, uint16_t velocity = 0) noexcept {
    return message(0x8, channel, key, 0, static_cast<uint32_t>(velocity) << 16);
}
inline std::array<uint32_t, 2> perNoteBend(int channel, int key, double semitones) noexcept {
    return message(kPerNoteBend, channel, key, 0, bendValue(semitones));
}
inline std::array<uint32_t, 2> perNoteManagement(int channel, int key, uint8_t flags) noexcept {
    return message(kPerNoteManagement, channel, key, flags, 0);
}
inline std::array<uint32_t, 2> controlChange(int channel, int controller, uint32_t value) noexcept {
    return message(0xB, channel, controller, 0, value);
}

}  // namespace sub::ump
