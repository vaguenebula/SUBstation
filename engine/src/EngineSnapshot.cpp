// Engine: building the RenderSnapshot from the edit model, and publishing it.
#include "Engine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <optional>
#include <unordered_map>

#include "Rack.h"
#include "Routing.h"

namespace sub {

// ---------------------------------------------------------------------------
// Snapshot building

int Engine::insertLatency(const Processor& insert) {
    constexpr int kMaxLatency = 1 << 20;
    return insert.isEnabled() ? std::clamp(insert.latencySamples(), 0, kMaxLatency) : 0;
}

std::vector<AutomationNode> Engine::automationNodes(const AutomationLaneDesc& desc, double samplesPerBeat) {
    std::vector<AutomationNode> nodes;
    nodes.reserve(desc.points.size());
    for (const AutomationPoint& point : desc.points) {
        nodes.push_back({std::llround(std::max(0.0, point.beat) * samplesPerBeat), std::clamp(point.value, 0.f, 1.f),
                         std::clamp(point.curve, -1.f, 1.f)});
    }
    std::stable_sort(nodes.begin(), nodes.end(),
                     [](const AutomationNode& a, const AutomationNode& b) { return a.time < b.time; });
    return nodes;
}

namespace {

// A delay line long enough for `samples` (kept if it is: it holds what it delays).
void ensureDelay(std::shared_ptr<DelayLine>& delay, int samples) {
    if (samples > 0 && (!delay || delay->capacity() <= samples)) {
        delay = std::make_shared<DelayLine>(2 * samples + Renderer::kMaxBlock);
    }
}

// A rack chain's fader lane: "chain:<chain id>:volume" (or ":pan") of the rack.
std::string chainLaneParam(uint32_t chainId, const char* control) {
    return "chain:" + std::to_string(chainId) + ":" + control;
}

}  // namespace

void Engine::buildAutomationLocked(const StripBuild& build, int faderLatency, StripRender& strip) {
    for (const AutomationLaneDesc& desc : build.track->automation) {
        if (desc.points.empty() || desc.processorId != 0) continue;
        AutomationRender* target = desc.param == "volume" ? &strip.volume : desc.param == "pan" ? &strip.pan : nullptr;
        if (!target) continue;  // (a send's: its edge's)
        target->nodes = automationNodes(desc, build.samplesPerBeat);
        target->latency = faderLatency;
    }
}

void Engine::buildChainLocked(uint32_t chainId, const StripBuild& build, int depth, RenderSnapshot& snap,
                              StripRender& out) {
    const ChainModel& chain = chains_.at(chainId);
    out.inserts = chain.inserts;
    const std::vector<StripSlot>& slots = *build.slots;
    std::vector<int> sidechains(chain.inserts.size(), -1);
    std::vector<std::shared_ptr<const RackRender>> racks(chain.inserts.size());
    bool anySidechain = false, anyRack = false;
    for (size_t i = 0; i < chain.inserts.size(); ++i) {
        const auto found = build.slotOf.find(chain.inserts[i].get());
        if (found == build.slotOf.end()) continue;
        const auto s = static_cast<size_t>(found->second);
        if (s < build.sidechainOf.size() && build.sidechainOf[s] >= 0) {
            sidechains[i] = build.sidechainOf[s];
            anySidechain = true;
        }
        if (!slots[s].isRack) continue;
        anyRack = true;
        auto rack = std::make_shared<RackRender>();
        rack->depth = depth;
        const std::vector<int>& ends = (*build.chainEnd)[s];
        const std::vector<int>& lineUp = (*build.chainCompensation)[s];
        const ProcessorEntry& entry = processors_.at(slots[s].processorId);
        for (size_t c = 0; c < entry.chains.size(); ++c) {
            ChainModel& model = chains_.at(entry.chains[c]);
            ChainRender render;
            render.id = model.id;
            render.params = model.params;
            render.compensation = c < lineUp.size() ? lineUp[c] : 0;
            ensureDelay(model.delay, render.compensation);
            render.delay = model.delay;
            render.delayIndex = static_cast<int>(snap.chainDelays.size());
            snap.chainDelays.push_back(render.compensation);
            // Its fader hears the timeline as late as the end of its devices.
            const int faderLatency = build.inputLatency + (c < ends.size() ? ends[c] : 0);
            render.latency = c < ends.size() ? ends[c] - (*build.deviceLatency)[s] : 0;
            const std::string volume = chainLaneParam(model.id, "volume"), pan = chainLaneParam(model.id, "pan");
            for (const AutomationLaneDesc& desc : build.track->automation) {
                if (desc.processorId != slots[s].processorId || desc.points.empty()) continue;
                AutomationRender* target = desc.param == volume ? &render.volume : desc.param == pan ? &render.pan : nullptr;
                if (!target) continue;
                target->nodes = automationNodes(desc, build.samplesPerBeat);
                target->latency = faderLatency;
            }
            buildChainLocked(model.id, build, depth + 1, snap, render);
            if (build.chainTaps) {  // the sidechains taken after its devices
                const auto taps = build.chainTaps->find(model.id);
                if (taps != build.chainTaps->end()) render.deviceTaps = taps->second;
            }
            rack->chains.push_back(std::move(render));
        }
        racks[i] = std::move(rack);
    }
    if (anySidechain) out.sidechains = std::move(sidechains);
    if (anyRack) out.racks = std::move(racks);

    // The envelopes of the chain's devices' parameters, each as late as the strip's
    // input plus the latency before its device.
    for (const AutomationLaneDesc& desc : build.track->automation) {
        if (desc.points.empty() || desc.processorId == 0) continue;
        const auto found = processors_.find(desc.processorId);
        if (found == processors_.end() || found->second.chainId != chainId) continue;
        const std::shared_ptr<Processor>& processor = found->second.processor;
        const auto place = std::find(chain.inserts.begin(), chain.inserts.end(), processor);
        if (place == chain.inserts.end()) continue;
        const auto& infos = processor->params();
        const auto info = std::find_if(infos.begin(), infos.end(), [&](const ParamInfo& p) { return p.id == desc.param; });
        if (info == infos.end() || info->readOnly) continue;
        AutomationRender lane;
        lane.processor = processor;
        lane.param = static_cast<int>(info - infos.begin());
        lane.steps = info->stepCount();
        lane.insert = static_cast<int>(place - chain.inserts.begin());
        const auto slot = build.slotOf.find(processor.get());
        lane.latency = build.inputLatency + (slot != build.slotOf.end() ? (*build.deviceLatency)[static_cast<size_t>(slot->second)] : 0);
        lane.nodes = automationNodes(desc, build.samplesPerBeat);
        out.automation.push_back(std::move(lane));
    }
    std::stable_sort(out.automation.begin(), out.automation.end(),
                     [](const AutomationRender& a, const AutomationRender& b) { return a.insert < b.insert; });
}

// ---------------------------------------------------------------------------
// Snapshot publishing

void Engine::rebuildSnapshotLocked() {
    auto snap = std::make_shared<RenderSnapshot>();
    snap->sampleRate = sampleRate_;
    snap->tempo = tempo_;
    snap->timeSigNum = timeSigNum_;
    snap->timeSigDen = timeSigDen_;
    const double spb = snap->samplesPerBeat();
    snap->loopStart = std::llround(loopStartBeat_ * spb);
    snap->loopEnd = std::llround(loopEndBeat_ * spb);
    snap->loopEnabled = loopEnabled_ && snap->loopEnd - snap->loopStart >= 256;

    // Routing: the tracks in an order in which each comes after what feeds it
    // (through outputs, sends, inputs and sidechains alike).
    std::vector<EdgeOrigin> origins;  // per edge: what it is
    std::vector<RouteEdge> edges = routeEdgesLocked(&origins);
    const int count = static_cast<int>(tracks_.size());
    std::vector<int> order = topologicalOrder(count, edges);
    if (static_cast<int>(order.size()) != count) {
        // Every edge that would close a cycle is refused, so there is none; were
        // there one, the tracks would play straight into the master, without
        // sends, inputs from tracks or sidechains.
        assert(false && "the routing graph has a cycle");
        for (TrackModel& track : tracks_) {
            track.output = kMaster;
            track.sends.clear();
            if (track.inputTrack != kMaster) track.inputTrack.reset();
        }
        for (auto& [id, entry] : processors_) entry.sidechain.reset();
        edges = routeEdgesLocked(&origins);
        order = topologicalOrder(count, edges);
    }
    std::vector<int> position(tracks_.size());  // tracks_ index -> snapshot index
    for (size_t i = 0; i < order.size(); ++i) position[order[i]] = static_cast<int>(i);

    // Freezing: a frozen track plays its clips through its fader, without its
    // devices, and doesn't hear what goes into it. A track every edge of which
    // ends at a frozen track (or one like it) isn't rendered (`idle`): what is in
    // a frozen group. Worked out destinations first. Neither has devices here.
    std::vector<char> frozen(tracks_.size(), 0), idle(tracks_.size(), 0), silent(tracks_.size(), 0);
    {
        std::vector<std::vector<int>> destinations(tracks_.size());
        for (const RouteEdge& edge : edges) destinations[static_cast<size_t>(edge.from)].push_back(edge.to);
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const auto t = static_cast<size_t>(*it);
            frozen[t] = tracks_[t].frozen;
            const auto& to = destinations[t];
            idle[t] = !to.empty() && std::all_of(to.begin(), to.end(), [&](int d) {
                return d >= 0 && silent[static_cast<size_t>(d)];
            });
            silent[t] = frozen[t] || idle[t];
        }
    }
    // What goes into a frozen track lines up with nothing: it isn't heard.
    std::vector<RouteEdge> aligning = edges;
    for (RouteEdge& edge : aligning) {
        if (edge.to >= 0 && frozen[static_cast<size_t>(edge.to)]) {
            edge.sums = false;
            edge.device = -1;
        }
    }

    // Plug-in delay compensation at every summing point, per edge: each bus (a
    // group, a return, the master) hears its inputs as late as the latest of
    // them, which the enabled devices before each edge's tap (and those of what
    // feeds them) make; the other edges are delayed to line up with it. Both
    // taps (pre- and post-fader) come after every device of a strip. Input edges
    // aren't summed, so nothing lines up with them. A sidechain lines up with the
    // signal at its device (Routing.h): it is delayed, or that signal is, just
    // before the device.
    // Racks (Routing.h) line up their chains inside the strip they are on.
    const ProcessorIds ids = processorIdsLocked();
    std::vector<std::vector<StripSlot>> slotsOf;  // each track's devices (by tracks_ index), then the master's
    slotsOf.reserve(tracks_.size() + 1);
    for (size_t t = 0; t < tracks_.size(); ++t) {
        TrackModel& track = tracks_[t];
        std::vector<StripSlot> slots = stripSlotsLocked(track, ids);
        // Devices left out start again from silence when they come back (no stale tail).
        if (track.silenced && !silent[t]) {
            for (const StripSlot& slot : slots) slot.processor->requestReset();
        }
        track.silenced = silent[t];
        slotsOf.push_back(silent[t] ? std::vector<StripSlot>{} : std::move(slots));
    }
    slotsOf.push_back(stripSlotsLocked(master_, ids));
    std::vector<std::vector<ChainSlot>> chains;  // the same, as delay compensation sees them
    chains.reserve(slotsOf.size());
    for (const auto& slots : slotsOf) {
        constexpr int kMaxChainLatency = 1 << 20;  // in all
        int total = 0;
        auto& chain = chains.emplace_back();
        for (const StripSlot& slot : slots) {
            ChainSlot device;
            device.rack = slot.rack;
            device.chain = slot.chain;
            device.enabled = slot.enabled;
            if (slot.isRack) {
                device.chains = static_cast<int>(processors_.at(slot.processorId).chains.size());
            } else if (slot.enabled) {
                device.latency = std::min(insertLatency(*slot.processor), kMaxChainLatency - total);
                total += device.latency;
            }
            chain.push_back(device);
        }
    }
    const GraphLatencies aligned = alignGraph(order, aligning, chains);
    snap->maxLatency = aligned.masterInput;
    for (size_t node = 0; node < slotsOf.size(); ++node) {  // what each rack adds, for the UI
        for (size_t s = 0; s < slotsOf[node].size(); ++s) {
            if (!slotsOf[node][s].isRack) continue;
            static_cast<RackProcessor&>(*slotsOf[node][s].processor)
                .setLatency(aligned.deviceOut[node][s] - aligned.deviceLatency[node][s]);
        }
    }

    // The edges in snapshot order (by source; each track's output, then its
    // sends, then the input edges and sidechains it feeds), each node's incoming
    // and outgoing ones (what a bus sums, in that order), the sidechains into
    // each strip's devices and the taps after its devices, and the graph the
    // scheduler runs.
    std::vector<std::vector<int>> edgesOf(tracks_.size());
    for (size_t e = 0; e < edges.size(); ++e) edgesOf[static_cast<size_t>(edges[e].from)].push_back(static_cast<int>(e));
    std::vector<std::vector<int>> incoming(tracks_.size()), outgoing(tracks_.size());  // by snapshot index
    std::vector<std::vector<int>> deviceTaps(tracks_.size());                             // by snapshot index
    std::unordered_map<uint32_t, std::vector<int>> chainTaps;  // rack chain -> the taps after its devices
    std::vector<std::vector<std::pair<int, int>>> sidechains(tracks_.size() + 1);       // (device, edge), the master last
    std::vector<int> inputEdge(tracks_.size(), -1);  // by snapshot index: the input edge it takes, if any
    std::vector<std::pair<int, int>> graphEdges;
    snap->edges.reserve(edges.size());
    for (const int t : order) {
        TrackModel& track = tracks_[static_cast<size_t>(t)];
        for (const int e : edgesOf[static_cast<size_t>(t)]) {
            const EdgeOrigin& origin = origins[static_cast<size_t>(e)];
            const int send = origin.send;
            const RouteEdge& route = edges[static_cast<size_t>(e)];
            const int index = static_cast<int>(snap->edges.size());
            EdgeRender edge;
            edge.from = position[static_cast<size_t>(t)];
            edge.to = route.to >= 0 ? position[static_cast<size_t>(route.to)] : -1;
            edge.compensation = aligned.compensation[static_cast<size_t>(e)];
            if (send == kInputEdge) {  // the destination's input: not summed, nor delayed
                edge.kind = EdgeRender::Kind::Input;
                edge.state = tracks_[static_cast<size_t>(origin.track)].inputState;
                inputEdge[static_cast<size_t>(edge.to)] = index;
            } else if (send == kSidechainEdge) {  // into one of the destination's devices
                SidechainModel& sidechain = *processors_.at(origin.processor).sidechain;
                edge.kind = EdgeRender::Kind::Sidechain;
                edge.state = sidechain.state;
                // After a device (in the source's own chain, or in a rack's chain
                // there: that chain writes it), or before them all (also while the
                // source plays without its devices); else before or after the fader.
                std::optional<uint32_t> tapChain;
                if (route.tap >= 0) {
                    edge.tap = EdgeRender::Tap::AfterDevice;
                    const auto& sourceSlots = slotsOf[static_cast<size_t>(t)];
                    if (route.tap > 0 && static_cast<size_t>(route.tap) <= sourceSlots.size()) {
                        const StripSlot& slot = sourceSlots[static_cast<size_t>(route.tap) - 1];
                        edge.tapDevice = slot.index;
                        if (slot.rack >= 0) tapChain = slot.chainId;
                    }
                } else {
                    edge.tap = sidechain.tap == SidechainTap::PostFader ? EdgeRender::Tap::PostFader
                                                                         : EdgeRender::Tap::PreFader;
                }
                edge.device = origin.device;
                edge.deviceDelay = aligned.deviceDelay[static_cast<size_t>(e)];
                ensureDelay(sidechain.delay, edge.compensation);
                ensureDelay(sidechain.deviceDelay, edge.deviceDelay);
                edge.delay = sidechain.delay;
                edge.deviceDelayLine = sidechain.deviceDelay;
                sidechains[edge.to >= 0 ? static_cast<size_t>(edge.to) : tracks_.size()].emplace_back(edge.device, index);
                if (tapChain) {
                    chainTaps[*tapChain].push_back(index);
                } else if (edge.tap == EdgeRender::Tap::AfterDevice) {
                    deviceTaps[static_cast<size_t>(edge.from)].push_back(index);
                }
            } else {
                std::shared_ptr<DelayLine>& delay =
                    send == kOutputEdge ? track.delay : track.sends[static_cast<size_t>(send)].delay;
                ensureDelay(delay, edge.compensation);
                edge.delay = delay;
            }
            if (send == kOutputEdge) {
                edge.state = track.outputState;
            } else if (send >= 0) {
                const SendModel& model = track.sends[static_cast<size_t>(send)];
                edge.kind = EdgeRender::Kind::Send;
                edge.tap = model.preFader ? EdgeRender::Tap::PreFader : EdgeRender::Tap::PostFader;
                edge.state = model.state;
                // Its level is applied where it is summed: as late as its destination hears its inputs.
                const std::string param = "send:" + std::to_string(model.to);
                for (const AutomationLaneDesc& desc : track.automation) {
                    if (desc.processorId != 0 || desc.param != param || desc.points.empty()) continue;
                    edge.level.nodes = automationNodes(desc, spb);
                    edge.level.latency = aligned.inputLatency[static_cast<size_t>(route.to)];
                }
            }
            outgoing[static_cast<size_t>(edge.from)].push_back(index);
            if (edge.to >= 0) {
                incoming[static_cast<size_t>(edge.to)].push_back(index);
                graphEdges.emplace_back(edge.from, edge.to);
            } else if (edge.sums()) {
                snap->masterInputs.push_back(index);
            }
            snap->edges.push_back(std::move(edge));
        }
    }
    snap->graph = std::make_shared<TaskGraph>(count, graphEdges);
    const auto byTapDevice = [&](std::vector<int>& taps) {
        std::stable_sort(taps.begin(), taps.end(), [&](int a, int b) {
            return snap->edges[static_cast<size_t>(a)].tapDevice < snap->edges[static_cast<size_t>(b)].tapDevice;
        });
    };
    for (auto& taps : deviceTaps) byTapDevice(taps);
    for (auto& [chain, taps] : chainTaps) byTapDevice(taps);
    // What building each strip's devices needs: `node` is its place in slotsOf
    // (and GraphLatencies), `strip` its snapshot index (tracks_.size(): the master).
    const auto stripBuild = [&](const TrackModel& track, size_t node, size_t strip, int inputLatency) {
        StripBuild build;
        build.track = &track;
        build.slots = &slotsOf[node];
        for (size_t s = 0; s < slotsOf[node].size(); ++s) build.slotOf[slotsOf[node][s].processor.get()] = static_cast<int>(s);
        build.inputLatency = inputLatency;
        build.deviceLatency = &aligned.deviceLatency[node];
        build.chainEnd = &aligned.chainEnd[node];
        build.chainCompensation = &aligned.chainCompensation[node];
        build.sidechainOf.assign(slotsOf[node].size(), -1);
        for (const auto& [device, edge] : sidechains[strip]) {
            if (device >= 0 && static_cast<size_t>(device) < build.sidechainOf.size()) {
                build.sidechainOf[static_cast<size_t>(device)] = edge;
            }
        }
        build.samplesPerBeat = spb;
        build.chainTaps = &chainTaps;
        return build;
    };

    // The master: its input is the sum of what goes into it, which comes maxLatency late.
    StripRender& master = snap->master;
    master.params = master_.params;
    master.latency = aligned.deviceLatency[tracks_.size()].back();
    {
        const StripBuild build = stripBuild(master_, tracks_.size(), tracks_.size(), snap->maxLatency);
        buildChainLocked(master_.chainId, build, 0, *snap, master);
        buildAutomationLocked(build, snap->outputLatency(), master);
    }

    const auto rate = static_cast<uint32_t>(sampleRate_);
    const int64_t clipFade = std::llround(clipFadeMs_ * 0.001 * sampleRate_);  // against clicks where clips cut
    std::array<size_t, kNumStretchConfigs> voicesNeeded{};
    snap->tracks.reserve(tracks_.size());
    for (const int t : order) {
        TrackModel& track = tracks_[t];
        TrackRender render;
        render.id = track.id;
        render.params = track.params;
        render.latency = aligned.deviceLatency[t].back();
        render.inputLatency = aligned.inputLatency[t];
        const size_t at = static_cast<size_t>(position[static_cast<size_t>(t)]);
        render.incoming = std::move(incoming[at]);
        render.outgoing = std::move(outgoing[at]);
        render.deviceTaps = std::move(deviceTaps[at]);
        const StripBuild build = stripBuild(track, static_cast<size_t>(t), at, render.inputLatency);
        if (!silent[static_cast<size_t>(t)]) buildChainLocked(track.chainId, build, 0, *snap, render);
        render.frozen = track.frozen;
        render.inputCount = static_cast<int>(render.incoming.size());
        render.buffers = track.buffers;
        render.input = inputEdgeLocked(track);
        render.input.edge = inputEdge[at];
        render.midiInput = track.midiInput;
        render.monitor = track.frozen ? MonitorMode::Off : track.monitor;
        render.armed = track.armed && !track.frozen;
        // Its devices hear the timeline as late as its input; its fader after them
        // (its edges are delayed after the fader, to line up where they go).
        buildAutomationLocked(build, render.inputLatency + render.latency, render);
        static const std::vector<NoteDesc> kNoNotes;
        const std::vector<NoteDesc>& notes = silent[static_cast<size_t>(t)] ? kNoNotes : track.notes;
        render.notes.reserve(notes.size());
        for (const NoteDesc& note : notes) {
            NoteRender nr;
            nr.start = std::max<int64_t>(0, std::llround(note.startBeat * spb));
            nr.end = std::max<int64_t>(nr.start + 1, std::llround((note.startBeat + note.lengthBeats) * spb));
            nr.key = static_cast<uint8_t>(std::clamp(note.key, 0, 127));
            nr.velocity = static_cast<uint8_t>(std::clamp(note.velocity, 1, 127));
            render.notes.push_back(nr);
        }
        std::sort(render.notes.begin(), render.notes.end(), [](const NoteRender& a, const NoteRender& b) {
            return a.start != b.start ? a.start < b.start : a.key < b.key;
        });
        std::array<size_t, kNumStretchConfigs> stretching{};
        for (size_t i = 0; i < (idle[static_cast<size_t>(t)] ? 0 : track.clips.size()); ++i) {  // (idle: no use)
            const ClipDesc& clip = track.clips[i];
            auto it = sources_.find(track.clipKeys[i]);
            if (it == sources_.end() || it->second->sampleRate() != rate) continue;  // still loading
            const auto& source = it->second;
            ClipRender cr;
            cr.source = source;
            cr.start = std::max<int64_t>(0, std::llround(clip.startBeat * spb));
            cr.sourceOffset = std::clamp<int64_t>(std::llround(clip.offsetSec * sampleRate_), 0, source->frames());
            const bool warped = clip.warp && clip.segmentBpm > 0.0;
            if (warped) {
                // Locked to beats: both ends sit on their beats at any tempo.
                cr.rate = tempo_ / clip.segmentBpm;
                const double endBeat = clip.startBeat + clip.durationSec * clip.segmentBpm / 60.0;
                cr.length = std::llround(endBeat * spb) - cr.start;
            } else {
                cr.length = std::llround(clip.durationSec * sampleRate_);
            }
            // Never play past the end of the file.
            const auto available = static_cast<double>(source->frames() - cr.sourceOffset);
            cr.length = std::min<int64_t>(cr.length, static_cast<int64_t>(std::floor(available / cr.rate)));
            if (cr.length <= 0) continue;

            // Its fades: its own, else a short one against clicks where it cuts
            // into the file, but none at the file's own start or end (a one-shot's
            // attack stays as it is). Its own are shortened in proportion where
            // together they would be longer than it; the short ones give way to them.
            const auto fadeLength = [&](double sec) {
                const double samples = warped ? sec * clip.segmentBpm / 60.0 * spb : sec * sampleRate_;
                return std::llround(std::clamp(samples, 0.0, 1e15));
            };
            int64_t ownIn = clip.fadeInSec > 0.0 ? fadeLength(clip.fadeInSec) : 0;
            int64_t ownOut = clip.fadeOutSec > 0.0 ? fadeLength(clip.fadeOutSec) : 0;
            if (ownIn + ownOut > cr.length) {
                const double share = static_cast<double>(ownIn) / static_cast<double>(ownIn + ownOut);
                ownIn = std::llround(share * static_cast<double>(cr.length));
                ownOut = cr.length - ownIn;
            }
            const bool atFileStart = cr.sourceOffset == 0;
            const bool atFileEnd = static_cast<double>(cr.length + 2) * cr.rate >= available;  // (to a sample or two)
            const int64_t declick = std::min(clipFade, cr.length / 2);
            cr.fadeIn = clip.fadeInSec > 0.0 ? ownIn : atFileStart ? 0 : std::min(declick, cr.length - ownOut);
            cr.fadeOut = clip.fadeOutSec > 0.0 ? ownOut : atFileEnd ? 0 : std::min(declick, cr.length - cr.fadeIn);
            cr.fadeInCurve = std::clamp(clip.fadeInSec > 0.0 ? clip.fadeInCurve : 0.f, -1.f, 1.f);
            cr.fadeOutCurve = std::clamp(clip.fadeOutSec > 0.0 ? clip.fadeOutCurve : 0.f, -1.f, 1.f);

            cr.gain = clip.gain;
            balanceGains(clip.pan, cr.panLeft, cr.panRight);
            const bool repitch = warped && clip.warpMode == WarpMode::RePitch;
            const bool speedChanges = std::abs(cr.rate - 1.0) > 1e-9;
            if (repitch) {
                cr.playback = speedChanges ? ClipRender::Playback::Resample : ClipRender::Playback::Direct;
            } else if (speedChanges || clip.transpose != 0.0) {
                cr.playback = ClipRender::Playback::Stretch;
                cr.stretchConfig = stretchConfigFor(clip.warpMode);
                cr.transpose = static_cast<float>(clip.transpose);
                cr.preserveFormants = clip.warpMode == WarpMode::Formants;
                ++stretching[static_cast<size_t>(cr.stretchConfig)];
            }
            const uint64_t identity = clip.id.empty() ? std::hash<size_t>{}(i) : std::hash<std::string>{}(clip.id);
            cr.key = identity ^ (static_cast<uint64_t>(track.id) * 0x9E3779B97F4A7C15ull);

            render.maxClipLength = std::max(render.maxClipLength, cr.length);
            render.clips.push_back(std::move(cr));
        }
        // Clips on a track don't overlap, so only a few play in any one block
        // (more only if they are shorter than a block).
        for (int c = 0; c < kNumStretchConfigs; ++c) voicesNeeded[c] += std::min<size_t>(stretching[c], 3);
        std::sort(render.clips.begin(), render.clips.end(),
                  [](const ClipRender& a, const ClipRender& b) { return a.start < b.start; });
        // Worth a thread of its own: devices to run, or clips to stretch.
        const bool enabledDevice = std::any_of(render.inserts.begin(), render.inserts.end(),
                                               [](const auto& insert) { return insert->isEnabled(); });
        const bool warping = std::any_of(render.clips.begin(), render.clips.end(), [](const ClipRender& clip) {
            return clip.playback != ClipRender::Playback::Direct;
        });
        if (enabledDevice || warping) ++snap->parallelWork;
        snap->tracks.push_back(std::move(render));
    }
    ensureWarpVoicesLocked(voicesNeeded);
    snap->warpVoices = warpVoices_;

    std::shared_ptr<const RenderSnapshot> old = std::move(snapshotHold_);
    snapshotHold_ = snap;
    // Publish, then read the epoch (both seq_cst; see DeferredReleasePool).
    snapshot_.store(snap.get(), std::memory_order_seq_cst);
    releasePool_.retire(std::move(old), audioEpoch_.load(std::memory_order_seq_cst));
    collectGarbageLocked();
    serviceTransportIfIdleLocked();
}

void Engine::ensureWarpVoicesLocked(const std::array<size_t, kNumStretchConfigs>& needed) {
    constexpr size_t kMaxVoices = 64;  // per configuration
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        auto& pool = warpVoices_[c];
        while (pool.size() < std::min(needed[c], kMaxVoices)) {
            pool.push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }
}

}  // namespace sub
