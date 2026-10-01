#pragma once
// Turns a RenderSnapshot into audio. One instance is driven by the device
// callback (live); export and tests use a separate instance (offline).
//
// MIDI tracks: the renderer turns the snapshot's notes into note events for the
// track's processors. It remembers which notes it started and when each ends,
// so every note-on gets its note-off even if the arrangement changes while the
// note sounds; stopping, locating and loop wraps release all sounding notes.
//
// Tracks and the master are strips (processStrip): devices, delay compensation,
// fader and meter. The tracks' sum is the master's input.
//
// Processors see continuous stretches of the timeline: a block the loop wraps
// around in is processed in two parts. Tracks whose devices add less latency
// than the slowest track are delayed to line up with it; the master's devices
// add theirs after that. The metronome is delayed as much, so the output lags
// the timeline by the snapshot's outputLatency().
//
// Automation: before each stretch a processor processes, the renderer hands it
// its automated parameters' values over the stretch (Processor::automate): the
// value at the start, then wherever it changes, every kAutomationStep samples
// along a slope and at each breakpoint. Automated volume and pan are followed
// sample by sample. While stopped, automated values follow the playhead.
//
// Input: a monitored track (MonitorMode) plays the device's input instead of
// its clips, through its strip like any audio, but isn't delay-compensated
// (compensationFor()). While recording, the input of the tracks recorded goes
// to the RecordingSession's rings, tagged with the timeline position it was
// taken at; the loop doesn't wrap then. A count-in clicks before the playhead
// moves (the metronome, even if it is off).

#include <array>
#include <cstdint>
#include <vector>

#include "AudioDevice.h"
#include "Metronome.h"
#include "Recorder.h"
#include "Snapshot.h"
#include "Transport.h"

namespace gil {

class Renderer {
public:
    static constexpr int kMaxBlock = 1024;

    // Non-real-time: allocates buffers. Keeps the transport position.
    void prepare(double sampleRate);

    // Real-time. Applies transport commands, renders tracks, metronome and the
    // browser preview, and publishes position/meters. The master goes to the
    // first two outputs (mixed to mono if there is only one); any others are silent.
    // Monitored tracks hear the inputs; `recording` (if any) takes them.
    void processLive(const RenderSnapshot& snap, SharedState& shared, const AudioIO& io,
                     RecordingSession* recording = nullptr) noexcept;

    // Renders the timeline from the current position into interleaved stereo.
    // Never publishes meters or plays the preview; looping and the metronome
    // are opt-in (exports leave them off).
    void renderOffline(const RenderSnapshot& snap, float* outStereo, int64_t frames,
                       bool loop = false, bool metronome = false) noexcept;

    // Transport state. Only one thread drives a renderer at a time: the audio
    // thread while a device runs, otherwise the engine under its edit mutex.
    void syncTempo(const RenderSnapshot& snap) noexcept;
    void drainCommands(SharedState& shared) noexcept;
    void applyCommand(const TransportCommand& command) noexcept;
    void publishTransport(SharedState& shared) const noexcept;

    // Preview notes reach the instruments at the start of the live renderer's
    // next block. With no device running they are thrown away instead.
    void drainPreviewNotes(SharedState& shared) noexcept;
    static void discardPreviewNotes(SharedState& shared) noexcept;

    void setPosition(int64_t samples) noexcept { position_ = samples < 0 ? 0 : samples; }
    int64_t position() const noexcept { return position_; }
    void setPlaying(bool playing) noexcept { playing_ = playing; }
    bool playing() const noexcept { return playing_; }

    // Stretch voices to use instead of the snapshot's (which belong to the live
    // renderer). Offline renders pass a fresh set so they neither disturb live
    // playback nor depend on it. Must outlive the rendering.
    void setWarpVoices(const WarpVoiceSet* voices) noexcept { voiceOverride_ = voices; }
    // Likewise delay-compensation lines, one per snapshot track (null: none needed).
    void setDelayLines(const std::vector<std::shared_ptr<DelayLine>>* lines) noexcept { delayOverride_ = lines; }

private:
    static constexpr int kAutomationStep = 64;  // samples between a slope's values for processors

    struct ChunkFlags {
        bool live;  // smooth parameter changes and publish meters
        bool loop;
        bool metronome;
    };

    struct Segment {
        int64_t position;
        int length;
        int offset;
        bool jump;  // the playhead jumped here (locate, loop wrap): sounding notes stop
        bool chase;  // playback starts here: notes already underway sound
    };
    struct Tick {
        int offset;
        bool accent;
    };
    struct PendingTick {
        int64_t time;  // output sample (the metronome is delayed like the tracks)
        bool accent;
    };
    struct ActiveNote {
        uint32_t trackId;
        uint8_t key;
        int64_t end;  // timeline sample of its note-off
    };
    static constexpr int kMaxSegments = 16;
    static constexpr int kMaxTicks = 64;
    static constexpr int kMaxPendingTicks = 256;
    static constexpr int kMaxActiveNotes = 512;  // across all tracks
    static constexpr int kMaxEvents = 1024;      // per track and block
    static constexpr int kMaxPreviewNotes = 256;

    void renderChunk(const RenderSnapshot& snap, int frames, ChunkFlags flags) noexcept;
    // A strip's body, in place: inserts -> delay compensation -> fader and meter.
    // Its input is in left/right already (and a track's note events in events_).
    void processStrip(const RenderSnapshot& snap, const StripRender& strip, ProcessContext& context, float* left,
                      float* right, int frames, bool audible, ChunkFlags flags, DelayLine* delay,
                      int compensation) noexcept;
    // How much a strip is delayed to line up with the slowest one.
    static int compensationFor(const StripRender& strip, bool monitored) noexcept;
    bool isMonitored(const TrackRender& track, ChunkFlags flags) const noexcept;
    const float* inputChannel(int index, int offset) const noexcept;
    void readInput(const InputEdge& input, float* left, float* right, int frames) const noexcept;
    void recordInput() noexcept;
    void scheduleCountIn(const RenderSnapshot& snap, int length) noexcept;
    void processInserts(const StripRender& strip, ProcessContext& context, float* left, float* right, int frames,
                        double samplesPerBeat) noexcept;
    void automateInsert(const AutomationRender& lane, int64_t position, int length, bool moving) noexcept;
    // Volume and pan (automated or not), in place; live renders also smooth and meter.
    void applyFader(const RenderSnapshot& snap, TrackParams& params, const AutomationRender& volume,
                    const AutomationRender& pan, bool audible, float* left, float* right, int frames,
                    bool live) noexcept;
    void fillLane(const AutomationRender& lane, int frames, float* out) const noexcept;
    static int64_t automationTime(int64_t t, int latency) noexcept { return t > latency ? t - latency : 0; }
    void renderClips(const TrackRender& track, const Segment& segment, int64_t clipFade,
                     const WarpVoiceSet& voices) noexcept;
    WarpVoice* acquireVoice(const WarpVoiceSet& voices, const ClipRender& clip, bool& continuing) noexcept;
    void scheduleTicks(const RenderSnapshot& snap, int64_t position, int length, int offset) noexcept;
    void renderTicks(int frames) noexcept;
    void mixPreview(SharedState& shared, int frames) noexcept;
    void buildNoteEvents(const TrackRender& track) noexcept;
    void releaseNotes(uint32_t trackId, int offset) noexcept;
    void forgetNotesOfRemovedTracks(const RenderSnapshot& snap) noexcept;
    bool pushEvent(const ProcessEvent& event) noexcept;

    double sampleRate_ = 48000.0;
    double samplesPerBeat_ = 0.0;
    int64_t position_ = 0;
    int64_t expectedPosition_ = -1;  // where playback continues if the playhead doesn't jump
    bool playing_ = false;
    bool chasePending_ = false;  // playback just started: its first segment chases notes
    int64_t countIn_ = 0;        // samples of count-in still to come before the playhead moves
    int64_t countInTotal_ = 0;

    // The live callback's inputs, and what records them (live renders only).
    const float* const* inputs_ = nullptr;
    int numInputs_ = 0;
    int inputOffset_ = 0;  // the current chunk's first frame in them
    RecordingSession* recording_ = nullptr;
    std::vector<float> silence_, recordScratch_;

    std::vector<float> trackLeft_, trackRight_, masterLeft_, masterRight_;
    std::vector<float> warpLeft_, warpRight_;  // one warped clip's audio, before gain and fades
    std::vector<float> autoGain_, autoPanLeft_, autoPanRight_;  // automated fader, per sample
    const WarpVoiceSet* voiceOverride_ = nullptr;
    const std::vector<std::shared_ptr<DelayLine>>* delayOverride_ = nullptr;
    uint64_t blockCounter_ = 1;  // stamps voice use; 0 means "never used"
    std::array<Segment, kMaxSegments> segments_{};
    int numSegments_ = 0;
    std::array<Tick, kMaxTicks> ticks_{};  // this chunk's, from pendingTicks_
    int numTicks_ = 0;
    std::array<PendingTick, kMaxPendingTicks> pendingTicks_{};  // ring, in time order
    int pendingTickStart_ = 0;
    int numPendingTicks_ = 0;
    int64_t outputTime_ = 0;  // output samples rendered so far

    // Notes. Sized in prepare(); the counts say how much is in use.
    std::vector<ActiveNote> activeNotes_;
    int numActiveNotes_ = 0;
    std::vector<ProcessEvent> events_;  // the current track's events for this block
    int numEvents_ = 0;
    std::vector<PreviewNote> previewNotes_;
    int numPreviewNotes_ = 0;

    Metronome metronome_;

    uint32_t previewSerial_ = 0;
    const AudioSource* previewSource_ = nullptr;
    int64_t previewPosition_ = 0;
};

}  // namespace gil
