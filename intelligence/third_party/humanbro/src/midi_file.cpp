// Standard MIDI File reader with the same note semantics as midi_io.parse_midi (mido):
// note_on velocity>0 starts a note, note_off / note_on velocity 0 ends it, a re-strike of a
// held key ends the previous instance, unreleased keys end at the end of their track, and
// channel 10 (drums) is skipped. Running status follows mido (meta events do not set it).
#include <algorithm>
#include <fstream>
#include <map>

#include "humanbro/humanbro.hpp"

namespace humanbro {
namespace {

constexpr int kDrumChannel = 9;

class Cursor {
public:
    Cursor(const std::vector<uint8_t>& b, std::size_t pos, std::size_t end) : b_(b), pos_(pos), end_(end) {}
    bool done() const { return pos_ >= end_; }
    std::size_t pos() const { return pos_; }
    uint8_t byte() {
        if (pos_ >= end_) throw Error("unexpected end of MIDI track");
        return b_[pos_++];
    }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t c = byte();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
        throw Error("invalid variable-length quantity");
    }
    void skip(std::size_t n) {
        if (pos_ + n > end_) throw Error("unexpected end of MIDI track");
        pos_ += n;
    }

private:
    const std::vector<uint8_t>& b_;
    std::size_t pos_, end_;
};

uint32_t be32(const std::vector<uint8_t>& b, std::size_t p) {
    if (p + 4 > b.size()) throw Error("truncated MIDI file");
    return (uint32_t(b[p]) << 24) | (uint32_t(b[p + 1]) << 16) | (uint32_t(b[p + 2]) << 8) | b[p + 3];
}
uint16_t be16(const std::vector<uint8_t>& b, std::size_t p) {
    if (p + 2 > b.size()) throw Error("truncated MIDI file");
    return uint16_t((b[p] << 8) | b[p + 1]);
}

int message_size(uint8_t status) {  // including the status byte (mido's spec table)
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
        case 0xC0: case 0xD0: return 2;
        default: break;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        case 0xF6: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: return 1;
        default: throw Error("undefined MIDI status byte");
    }
}

}  // namespace

MidiFile MidiFile::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open MIDI file: " + path);
    MidiFile f;
    f.bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const auto& b = f.bytes_;
    if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd") throw Error("not a MIDI file: " + path);
    const uint32_t header_len = be32(b, 4);
    const uint16_t format = be16(b, 8), n_tracks = be16(b, 10), division = be16(b, 12);
    if (format == 2) throw Error("type-2 (asynchronous) MIDI files are not supported");
    if (division & 0x8000 || division == 0) throw Error("SMPTE / invalid time division is not supported");

    Score& s = f.score_;
    s.ticks_per_quarter = division;
    std::size_t pos = 8 + header_len;
    int64_t end_tick = 0;
    for (uint16_t t = 0; t < n_tracks; ++t) {
        if (pos + 8 > b.size() || std::string(b.begin() + pos, b.begin() + pos + 4) != "MTrk")
            throw Error("no MTrk header at start of track");
        const std::size_t len = be32(b, pos + 4), start = pos + 8, stop = start + len;
        if (stop > b.size()) throw Error("truncated MIDI track");
        Cursor c(b, start, stop);
        int64_t tick = 0;
        int last_status = -1;
        std::map<std::pair<int, int>, std::size_t> open;  // (channel, pitch) -> note index
        while (!c.done()) {
            tick += c.vlq();
            int status = c.byte();
            int peek = -1;
            if (status < 0x80) {
                if (last_status < 0) throw Error("running status without previous status");
                peek = status;
                status = last_status;
            } else if (status != 0xFF) {
                last_status = status;  // meta events do not set running status (as in mido)
            }
            if (status == 0xFF) {
                const uint8_t type = c.byte();
                const uint32_t n = c.vlq();
                const std::size_t data = c.pos();
                c.skip(n);
                if (type == 0x51 && n >= 3) {
                    s.tempo_changes.push_back({tick, double((b[data] << 16) | (b[data + 1] << 8) | b[data + 2])});
                } else if (type == 0x58 && n >= 2) {
                    if (b[data] > 0 && b[data + 1] < 31) s.time_signatures.push_back({tick, b[data], 1 << b[data + 1]});
                }
            } else if (status == 0xF0 || status == 0xF7) {
                c.skip(c.vlq());
            } else {
                const int n_data = message_size(uint8_t(status)) - 1;
                int data[2] = {0, 0};
                std::size_t data_pos[2] = {0, 0};
                for (int k = 0; k < n_data; ++k) {
                    if (k == 0 && peek >= 0) {
                        data[0] = peek;
                        data_pos[0] = c.pos() - 1;
                    } else {
                        data_pos[k] = c.pos();
                        data[k] = c.byte();
                    }
                    if (data[k] > 127) data[k] = 127;  // mido clip=True
                }
                const int kind = status & 0xF0, channel = status & 0x0F;
                if ((kind == 0x90 || kind == 0x80) && channel != kDrumChannel) {
                    const auto key = std::make_pair(channel, data[0]);
                    auto it = open.find(key);
                    if (it != open.end()) {  // note-off, or re-strike of a held key
                        s.notes[it->second].offset_tick = tick;
                        open.erase(it);
                    }
                    if (kind == 0x90 && data[1] > 0) {
                        open[key] = s.notes.size();
                        s.notes.push_back({tick, -1, data[0], data[1]});
                        f.velocity_pos_.push_back(data_pos[1]);
                    }
                }
            }
        }
        for (const auto& [key, idx] : open) s.notes[idx].offset_tick = tick;  // never released
        end_tick = std::max(end_tick, tick);
        pos = stop;
    }
    for (const Note& note : s.notes) end_tick = std::max(end_tick, std::max(note.offset_tick, note.onset_tick));
    s.end_tick = end_tick;
    return f;
}

void MidiFile::save_with_velocities(const std::string& path, const std::vector<int>& velocities) const {
    if (velocities.size() != score_.notes.size())
        throw Error("expected " + std::to_string(score_.notes.size()) + " velocities");
    std::vector<uint8_t> out = bytes_;
    for (std::size_t i = 0; i < velocities.size(); ++i)
        out[velocity_pos_[i]] = uint8_t(std::clamp(velocities[i], 1, 127));  // never 0 (= note-off)
    std::ofstream o(path, std::ios::binary);
    if (!o) throw Error("cannot write " + path);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

}  // namespace humanbro
