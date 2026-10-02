#pragma once
// Turns a RenderSnapshot into audio. One instance is driven by the device
// callback (live); export and tests use a separate instance (offline).
//
// MIDI tracks: the renderer turns the snapshot's notes into note events for the
// track's processors. It remembers which notes it started and when each ends,
// so every note-on gets its note-off even if the arrangement changes while the
// note sounds; stopping, locating and loop wraps release all sounding notes.
//
// Tracks and the master are strips (processStrip): devices, then fader and
// meter. A track's signal leaves on its edges (Routing.h): its output, into a
// bus (another track's input: a group) or the master, and its sends, into
// returns, tapped after its fader or before it. The snapshot lists the tracks so
// that each comes after what feeds it, so one pass renders them all.
//
// Mute and solo follow the routing. A muted track is silent on every edge, its
// pre-fader sends too. Solo works on edges: an edge plays if what it leaves is
// downstream of a solo (soloed, or fed by something soloed: a soloed track is
// heard through its groups and the returns it sends to, "solo in place"), or if
// where it goes is upstream of one (soloed, or feeding something soloed: what
// goes into a soloed group or return keeps going into it). The rest are silent,
// and so is a track none of whose edges play. Worked out once per chunk.
//
// Processors see continuous stretches of the timeline: a block the loop wraps
// around in is processed in two parts. Delay compensation is per edge: an edge
// that arrives earlier than the latest one going into the same place (a bus, a
// return, the master) is delayed to line up with it, at every level; a track
// sending to two returns of different latency is delayed differently on each.
// The master's devices add their latency after that. The metronome is delayed
// as much, so the output lags the timeline by the snapshot's outputLatency().
//
// Automation: before each stretch a processor processes, the renderer hands it
// its automated parameters' values over the stretch (Processor::automate): the
// value at the start, then wherever it changes, every kAutomationStep samples
// along a slope and at each breakpoint. Automated volume and pan are followed
// sample by sample, as are a send's level where its edge is summed. While
// stopped, automated values follow the playhead.
//
// Input: a monitored track (MonitorMode) plays the device's input instead of
// its clips, through its strip like any audio, but isn't delay-compensated
// (compensationFor()). While recording, the input of the tracks recorded goes
// to the RecordingSession's rings, tagged with the timeline position it was
// taken at; the loop doesn't wrap then. A count-in clicks before the playhead
// moves (the metronome, even if it is off).
//
// MIDI input (MidiInput.h): messages come stamped with the device sample they
// play at; each chunk takes those due in it. A track whose MIDI input accepts
// them hears them (while monitored: In, or Auto and armed), beside its clips'
// notes and preview notes; with In its clips' notes don't play. The renderer
// remembers the live notes it started, like the clips' notes, so each gets its
// note-off even if the track stops hearing its input, stops existing, or the
// transport stops while the key is held. A MIDI track being recorded sends what
// it hears (monitored or not) to its MidiRecordingTake.
//
// Threads: a chunk has three parts. A serial prologue works out everything the
// tracks share: the timeline's stretches, solo, the recording's input, every
// track's note events (clips, preview, MIDI input, MIDI recording) and the
// stretchers its clips play through, into the track's own TrackBuffers. Then
// the graph: each track renders from its own state (its inputs, clips, strip)
// into its own buffer (and the edges' own buffers that need one: pre-fader taps,
// delayed edges), on whichever thread the Scheduler gives it, with that thread's
// WorkerScratch; a bus starts by summing its incoming edges in a fixed order,
// each at its level. Last, serially: the master sums its incoming edges in
// order, then its strip and the metronome. Nothing a track computes depends on the thread it runs on, so
// renders are bit-identical with and without workers. Each track's render is
// timed; the graph starts the tracks with the most work hanging off them first
// (TaskGraph::orderRoots()), so a heavy track doesn't start last.

#include <array>
#include <cstdint>
#include <vector>

#include "AudioDevice.h"
#include "Metronome.h"
#include "Recorder.h"
#include "Scheduler.h"
#include "Snapshot.h"
#include "Transport.h"

namespace gil {

class Renderer {
public:
    static constexpr int kMaxBlock = 1024;

    // Non-real-time: allocates buffers. Keeps the transport position.
    void prepare(double sampleRate);
    // The threads tracks render on (null: all on the rendering thread). Non-real-
    // time, while the renderer doesn't run: allocates scratch for each thread.
    // The scheduler must outlive the rendering.
    void setScheduler(Scheduler* scheduler);
    // Below this much work (tracks worth a thread, times frames) a chunk renders
    // serially: waking workers would cost more than they save.
    static constexpr int kMinParallelWork = 256;
    // Whether the graph starts the tracks that cost the most first (else in
    // snapshot order). Any thread; on by default.
    void setCostOrdering(bool on) noexcept { costOrdering_.store(on, std::memory_order_relaxed); }
    bool costOrdering() const noexcept { return costOrdering_.load(std::memory_order_relaxed); }

    // Real-time. Applies transport commands, renders tracks, metronome and the
    // browser preview, and publishes position/meters. The master goes to the
    // first two outputs (mixed to mono if there is only one); any others are silent.
    // Monitored tracks hear the inputs, audio and MIDI; `recording` (if any) takes them.
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
    // Likewise delay-compensation lines, one per snapshot edge (null: none needed).
    void setDelayLines(const std::vector<std::shared_ptr<DelayLine>>* lines) noexcept { delayOverride_ = lines; }

private:
    static constexpr int kAutomationStep = 64;  // samples between a slope's values for processors
    static constexpr float kCostSmoothing = 0.1f;  // how much of a track's cost each chunk's measurement makes

    // A rendering thread's scratch: what a track needs only while it renders.
    struct WorkerScratch {
        std::vector<float> warpLeft, warpRight;  // one warped clip's audio, before gain and fades
        std::vector<float> autoGain, autoPanLeft, autoPanRight;  // automated fader, per sample
        std::vector<float> audible;   // the fader's mute (and solo) ramp, for pre-fader taps
        std::vector<float> edgeGain;  // an automated send level, per sample
    };

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
    struct InputEvent {  // a MIDI input message due in this chunk
        int offset;
        uint16_t port;
        uint8_t status;
        uint8_t data1;
        uint8_t data2;
    };
    struct LiveNote {  // a note a track's MIDI input started, not released yet
        uint32_t trackId;
        uint16_t port;
        uint8_t channel;
        uint8_t key;
    };
    static constexpr int kMaxSegments = 16;
    static constexpr int kMaxTicks = 64;
    static constexpr int kMaxPendingTicks = 256;
    static constexpr int kMaxActiveNotes = 512;  // across all tracks
    static constexpr int kMaxPreviewNotes = 256;
    static constexpr int kMaxPendingInput = 2048;  // MIDI input not due yet
    static constexpr int kMaxInputEvents = 512;    // MIDI input in one chunk
    static constexpr int kMaxLiveNotes = 512;      // across all tracks

    void renderChunk(const RenderSnapshot& snap, int frames, ChunkFlags flags) noexcept;
    // The graph's job: track `node` of the chunk's snapshot, on thread `worker`.
    static void renderNode(void* self, int node, int worker) noexcept;
    // A track, from its TrackBuffers (prepared by the prologue) and its incoming
    // edges, into its own signal and the own buffers of its edges that need one.
    void renderTrack(const RenderSnapshot& snap, int t, WorkerScratch& scratch) noexcept;
    // Prologue: who solo lets through (Renderer.h), into the tracks' and edges' state.
    void workOutSolo(const RenderSnapshot& snap) noexcept;
    // Adds an edge's signal at its level to a destination's left/right.
    void sumEdge(const RenderSnapshot& snap, const EdgeRender& edge, float* left, float* right, int frames,
                 WorkerScratch& scratch) noexcept;
    // A strip's body, in place: inserts (with its note events) -> fader and
    // meter. Its input is in left/right already. `audibleOut` (if any) gets the
    // fader's mute ramp, per sample.
    void processStrip(const RenderSnapshot& snap, const StripRender& strip, ProcessContext& context,
                      ProcessEvent* events, int numEvents, float* left, float* right, int frames, bool audible,
                      ChunkFlags flags, WorkerScratch& scratch, float* audibleOut = nullptr) noexcept;
    // How much an edge leaving a track is delayed to line up with the latest one
    // going into the same place.
    static int compensationFor(const EdgeRender& edge, bool monitored) noexcept;
    bool isMonitored(const TrackRender& track, ChunkFlags flags) const noexcept;
    const float* inputChannel(int index, int offset) const noexcept;
    void readInput(const InputEdge& input, float* left, float* right, int frames) const noexcept;
    void recordInput() noexcept;
    void scheduleCountIn(const RenderSnapshot& snap, int length) noexcept;
    void processInserts(const StripRender& strip, ProcessContext& context, ProcessEvent* events, int numEvents,
                        float* left, float* right, int frames, double samplesPerBeat) noexcept;
    void automateInsert(const AutomationRender& lane, int64_t position, int length, bool moving) noexcept;
    // Volume and pan (automated or not), in place; live renders also smooth and
    // meter. `audibleOut` (if any) gets the mute ramp, per sample.
    void applyFader(const RenderSnapshot& snap, TrackParams& params, const AutomationRender& volume,
                    const AutomationRender& pan, bool audible, float* left, float* right, int frames, bool live,
                    WorkerScratch& scratch, float* audibleOut) noexcept;
    void fillLane(const AutomationRender& lane, int frames, float* out) const noexcept;
    static int64_t automationTime(int64_t t, int latency) noexcept { return t > latency ? t - latency : 0; }
    // Adds the clips' audio over the segment to left/right (the chunk's buffers).
    // Stretched clips play through the voices the prologue gave them (from
    // `nextVoice` on, in the order the clips play).
    void renderClips(const TrackRender& track, const Segment& segment, int64_t clipFade, const TrackBuffers& buffers,
                     int& nextVoice, WorkerScratch& scratch, float* left, float* right) noexcept;
    // Prologue: the stretchers the track's clips play through in this chunk.
    void assignVoices(const TrackRender& track, const WarpVoiceSet& voices, TrackBuffers& buffers) noexcept;
    WarpVoice* acquireVoice(const WarpVoiceSet& voices, const ClipRender& clip, bool& continuing) noexcept;
    void scheduleTicks(const RenderSnapshot& snap, int64_t position, int length, int offset) noexcept;
    void renderTicks(int frames) noexcept;
    void mixPreview(SharedState& shared, int frames) noexcept;
    // A track's note events for this chunk, into `out`: preview notes, its MIDI
    // input (if `hearsInput`, or to `take`), then (if `clipNotes`) its clips' notes.
    void buildNoteEvents(const TrackRender& track, TrackBuffers& out, bool hearsInput, bool clipNotes,
                         MidiRecordingTake* take) noexcept;
    void releaseNotes(uint32_t trackId, int offset, TrackBuffers& out) noexcept;
    void drainMidiInput(SharedState& shared) noexcept;
    void gatherMidiInput(int frames) noexcept;
    bool hearsMidiInput(const TrackRender& track, ChunkFlags flags) const noexcept;
    MidiRecordingTake* midiTake(uint32_t trackId) const noexcept;
    void routeMidiInput(const TrackRender& track, bool hears, MidiRecordingTake* take, TrackBuffers& out) noexcept;
    void recordMidi(MidiRecordingTake* take, int offset, uint8_t channel, uint8_t key, uint8_t velocity) noexcept;
    int findLiveNote(uint32_t trackId, uint16_t port, uint8_t channel, uint8_t key) const noexcept;
    void forgetNotesOfRemovedTracks(const RenderSnapshot& snap) noexcept;

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

    std::vector<float> masterLeft_, masterRight_;
    Scheduler* scheduler_ = nullptr;
    std::atomic<bool> costOrdering_{true};
    std::vector<WorkerScratch> scratch_;  // one per thread: [0] the rendering thread's
    // The chunk the graph renders (set by the prologue, read by every thread).
    const RenderSnapshot* chunkSnap_ = nullptr;
    int chunkFrames_ = 0;
    ChunkFlags chunkFlags_{};
    ProcessContext chunkContext_;
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
    std::vector<PreviewNote> previewNotes_;
    int numPreviewNotes_ = 0;

    // MIDI input (live renders only).
    std::vector<MidiInputEvent> pendingInput_;  // arrived, not due yet; in arrival order
    int numPendingInput_ = 0;
    std::vector<InputEvent> inputEvents_;  // due in this chunk
    int numInputEvents_ = 0;
    std::vector<LiveNote> liveNotes_;
    int numLiveNotes_ = 0;
    int64_t deviceTime_ = 0;          // the chunk's first sample on the device's clock
    bool releaseLiveNotes_ = false;   // the transport stopped: held keys are released
    bool wasPlaying_ = false;

    Metronome metronome_;

    uint32_t previewSerial_ = 0;
    const AudioSource* previewSource_ = nullptr;
    int64_t previewPosition_ = 0;
};

}  // namespace gil
