// Python bindings for the engine (module substation._engine).
//
// Long-running calls release the GIL. The audio thread never calls into Python,
// so audio keeps running no matter what the interpreter is doing.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "Engine.h"
#include "plugins/Vst3Format.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/EqDesign.h"

namespace nb = nanobind;
using namespace nb::literals;

using sub::AudioSource;
using sub::Engine;

namespace {

nb::bytes toBytes(const std::vector<uint8_t>& data) {
    return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
}

std::vector<uint8_t> fromBytes(const nb::bytes& data) {
    const auto* begin = static_cast<const uint8_t*>(data.data());
    return {begin, begin + data.size()};
}

// Recorded notes as an int64 array of shape (n, 5): start, end (-1: held), key, velocity, channel.
nb::ndarray<nb::numpy, int64_t, nb::ndim<2>, nb::c_contig> notesArray(const std::vector<sub::RecordedNote>& notes) {
    auto buffer = std::make_unique<std::vector<int64_t>>();
    buffer->reserve(notes.size() * 5);
    for (const sub::RecordedNote& n : notes) buffer->insert(buffer->end(), {n.start, n.end, n.key, n.velocity, n.channel});
    int64_t* data = buffer->data();
    nb::capsule owner(buffer.release(), [](void* p) noexcept { delete static_cast<std::vector<int64_t>*>(p); });
    return {data, {notes.size(), size_t{5}}, owner};
}

}  // namespace

using PeakArray = nb::ndarray<nb::numpy, const float, nb::ndim<3>, nb::c_contig>;
using SampleArray = nb::ndarray<nb::numpy, const float, nb::ndim<2>>;
using StereoArray = nb::ndarray<nb::numpy, float, nb::ndim<2>, nb::c_contig>;

using ReleaseGil = nb::call_guard<nb::gil_scoped_release>;

NB_MODULE(_engine, m) {
    m.doc() = "SUBstation real-time audio engine";
    // Qt/PySide can keep engine objects alive until interpreter teardown; that
    // is harmless, so don't print nanobind's leak report at exit.
    nb::set_leak_warnings(false);
    // Bumped whenever the Python code comes to depend on a change here; the app
    // refuses to start with an engine built from older code (substation.ENGINE_API).
    m.attr("API_VERSION") = 17;
    m.attr("MAX_BLOCK") = sub::Renderer::kMaxBlock;
    m.attr("MASTER") = Engine::kMaster;
    m.attr("MAX_RACK_DEPTH") = Engine::kMaxRackDepth;
    m.attr("PEAK_LEVELS") = AudioSource::kNumPeakLevels;

    nb::class_<sub::AudioFileInfo>(m, "AudioFileInfo")
        .def_ro("frames", &sub::AudioFileInfo::frames)
        .def_ro("channels", &sub::AudioFileInfo::channels)
        .def_ro("sample_rate", &sub::AudioFileInfo::sampleRate)
        .def_ro("duration", &sub::AudioFileInfo::duration)
        .def("__repr__", [](const sub::AudioFileInfo& info) {
            return "AudioFileInfo(frames=" + std::to_string(info.frames) + ", channels=" +
                   std::to_string(info.channels) + ", sample_rate=" + std::to_string(info.sampleRate) + ")";
        });

    m.def("probe_file", &AudioSource::probe, "path"_a, ReleaseGil(),
          "Read an audio file's length and format without decoding it.");

    m.def("driver_types", &Engine::driverTypes,
          "The audio driver types this engine was built with: 'WASAPI', and 'ASIO' if it had the ASIO SDK.");

    nb::class_<sub::AudioDeviceInfo>(m, "AudioDeviceInfo")
        .def_ro("name", &sub::AudioDeviceInfo::name)
        .def_ro("is_default", &sub::AudioDeviceInfo::isDefault)
        .def("__repr__", [](const sub::AudioDeviceInfo& d) {
            return "AudioDeviceInfo('" + d.name + "'" + (d.isDefault ? ", default)" : ")");
        });

    nb::class_<sub::DeviceStatus>(m, "DeviceStatus")
        .def_ro("open", &sub::DeviceStatus::open)
        .def_ro("name", &sub::DeviceStatus::name)
        .def_ro("backend", &sub::DeviceStatus::backend, "The driver type: 'WASAPI' or 'ASIO'.")
        .def_ro("sample_rate", &sub::DeviceStatus::sampleRate)
        .def_ro("buffer_frames", &sub::DeviceStatus::bufferFrames)
        .def_ro("latency_ms", &sub::DeviceStatus::latencyMs, "Output latency.")
        .def_ro("input_latency_ms", &sub::DeviceStatus::inputLatencyMs)
        .def_ro("input_channels", &sub::DeviceStatus::inputChannels,
                "The device's input channels that are open (0-based), in the order take_input_meters() lists them.")
        .def_ro("output_channels", &sub::DeviceStatus::outputChannels,
                "The device's output channels that are open; the master plays on the first two.")
        .def_ro("exclusive", &sub::DeviceStatus::exclusive);

    nb::class_<sub::DeviceCaps>(m, "DeviceCapabilities")
        .def_ro("input_names", &sub::DeviceCaps::inputNames, "Names of all the device's inputs.")
        .def_ro("output_names", &sub::DeviceCaps::outputNames)
        .def_ro("sample_rates", &sub::DeviceCaps::sampleRates, "The rates it can run at; empty: any.")
        .def_ro("buffer_sizes", &sub::DeviceCaps::bufferSizes, "The buffer sizes it offers; empty: any.")
        .def_ro("preferred_buffer_frames", &sub::DeviceCaps::preferredBufferFrames)
        .def_ro("has_control_panel", &sub::DeviceCaps::hasControlPanel);

    nb::class_<sub::BuiltinInfo>(m, "BuiltinDevice", "A built-in device type, as the registry describes it.")
        .def_ro("id", &sub::BuiltinInfo::id, "What add_builtin_processor takes.")
        .def_ro("name", &sub::BuiltinInfo::name)
        .def_prop_ro("category", [](const sub::BuiltinInfo& d) { return d.isInstrument() ? "Instruments" : "Audio Effects"; })
        .def_prop_ro("is_instrument", &sub::BuiltinInfo::isInstrument)
        .def_ro("params", &sub::BuiltinInfo::params);
    m.def("builtin_devices", [] { return sub::BuiltinRegistry::instance().devices(); },
          "Every built-in device, instruments first, then by name.");
    m.def(
        "eq_response",
        [](int type, double freq, double gain, double q, int slope, double sampleRate,
           nb::ndarray<const double, nb::ndim<1>> freqs) {
            const sub::eq::Design design = sub::eq::design(type, freq, gain, q, slope, sampleRate);
            const size_t size = freqs.shape(0);
            auto buffer = std::make_unique<std::vector<double>>(size);
            for (size_t i = 0; i < size; ++i) {
                const double w = 2.0 * sub::eq::kPi * std::min(freqs(i), 0.5 * sampleRate) / sampleRate;
                (*buffer)[i] = 10.0 * std::log10(std::max(design.magnitudeSquared(w), 1e-30));
            }
            double* data = buffer->data();
            nb::capsule owner(buffer.release(), [](void* p) noexcept { delete static_cast<std::vector<double>*>(p); });
            return nb::ndarray<nb::numpy, double, nb::ndim<1>, nb::c_contig>(data, {size}, owner);
        },
        "type"_a, "freq"_a, "gain"_a, "q"_a, "slope"_a, "sample_rate"_a, "freqs"_a,
        "An EQ band's response in dB at each of `freqs` (Hz, float64): as the EQ device plays it. `type` and "
        "`slope` are its parameters' list indexes.");

    nb::class_<sub::ParamInfo>(m, "ParamInfo")
        .def_ro("id", &sub::ParamInfo::id)
        .def_ro("name", &sub::ParamInfo::name)
        .def_ro("unit", &sub::ParamInfo::unit)
        .def_ro("min_value", &sub::ParamInfo::minValue)
        .def_ro("max_value", &sub::ParamInfo::maxValue)
        .def_ro("default_value", &sub::ParamInfo::defaultValue)
        .def_ro("log_scale", &sub::ParamInfo::logScale)
        .def_ro("value_labels", &sub::ParamInfo::valueLabels)
        .def_ro("steps", &sub::ParamInfo::steps)
        .def_ro("automatable", &sub::ParamInfo::automatable)
        .def_ro("read_only", &sub::ParamInfo::readOnly)
        .def_ro("hidden", &sub::ParamInfo::hidden)
        .def_prop_ro("step_count", &sub::ParamInfo::stepCount,
                     "Steps between the lowest and highest value of a discrete parameter; 0 if continuous.")
        .def("to_normalized", &sub::ParamInfo::toNormalized, "plain"_a,
             "A plain value as automation sees it (0..1).")
        .def("from_normalized", &sub::ParamInfo::fromNormalized, "normalized"_a)
        .def("__repr__", [](const sub::ParamInfo& p) { return "ParamInfo('" + p.id + "', '" + p.name + "')"; });

    nb::class_<sub::AutomationPoint>(m, "AutomationPoint")
        .def(
            "__init__",
            [](sub::AutomationPoint* self, double beat, float value, float curve) {
                new (self) sub::AutomationPoint{beat, value, curve};
            },
            "beat"_a, "value"_a, "curve"_a = 0.0f)
        .def_rw("beat", &sub::AutomationPoint::beat)
        .def_rw("value", &sub::AutomationPoint::value, "Normalized, 0..1.")
        .def_rw("curve", &sub::AutomationPoint::curve, "How the segment to the next point bends (-1..1).")
        .def("__repr__", [](const sub::AutomationPoint& a) {
            return "AutomationPoint(" + std::to_string(a.beat) + ", " + std::to_string(a.value) + ", " +
                   std::to_string(a.curve) + ")";
        });

    nb::class_<sub::AutomationLaneDesc>(m, "AutomationLane")
        .def(
            "__init__",
            [](sub::AutomationLaneDesc* self, uint32_t processorId, std::string param,
               std::vector<sub::AutomationPoint> points) {
                new (self) sub::AutomationLaneDesc{processorId, std::move(param), std::move(points)};
            },
            "processor_id"_a, "param"_a, "points"_a,
            "An envelope: of a device's parameter (by id), or with processor_id 0 of the mixer's "
            "'volume' or 'pan'.")
        .def_rw("processor_id", &sub::AutomationLaneDesc::processorId)
        .def_rw("param", &sub::AutomationLaneDesc::param)
        .def_rw("points", &sub::AutomationLaneDesc::points);
    m.attr("AUTOMATION_CURVATURE") = sub::kAutomationCurvature;
    m.attr("MAX_VOLUME_GAIN") = sub::kMaxVolumeGain;

    nb::class_<sub::PluginDescription>(m, "PluginDescription")
        .def_ro("format", &sub::PluginDescription::format)
        .def_ro("path", &sub::PluginDescription::path)
        .def_ro("uid", &sub::PluginDescription::uid)
        .def_ro("name", &sub::PluginDescription::name)
        .def_ro("vendor", &sub::PluginDescription::vendor)
        .def_ro("version", &sub::PluginDescription::version)
        .def_ro("category", &sub::PluginDescription::category)
        .def_ro("is_instrument", &sub::PluginDescription::isInstrument)
        .def("__repr__", [](const sub::PluginDescription& d) {
            return "PluginDescription('" + d.name + "', " + d.format + ", " + d.uid + ")";
        });

    m.def(
        "scan_vst3", [](const std::string& path) { return sub::vst3::Vst3Format::instance().scanFile(path); },
        "path"_a, ReleaseGil(),
        "List the plug-ins in a VST3 file or bundle. Loads its code: the UI calls this in a child process.");
    m.def("vst3_search_paths", [] { return sub::vst3::Vst3Format::instance().defaultSearchPaths(); },
          "The standard VST3 folders.");

    nb::class_<sub::ProcessorInfo>(m, "ProcessorInfo")
        .def_ro("type_id", &sub::ProcessorInfo::typeId)
        .def_ro("name", &sub::ProcessorInfo::name)
        .def_ro("latency", &sub::ProcessorInfo::latency)
        .def_ro("tail", &sub::ProcessorInfo::tail)
        .def_ro("has_editor", &sub::ProcessorInfo::hasEditor)
        .def_ro("has_sidechain", &sub::ProcessorInfo::hasSidechain, "It has a sidechain (aux) input.");

    nb::class_<sub::DisplayInfo>(m, "DisplayInfo", "A stream of values a device's own editor draws.")
        .def_ro("id", &sub::DisplayInfo::id)
        .def_ro("samples_per_value", &sub::DisplayInfo::samplesPerValue,
                "The audio each value stands for: 1 for samples, more for a meter.");

    nb::enum_<sub::SidechainTap>(m, "SidechainTap")
        .value("POST_FADER", sub::SidechainTap::PostFader)
        .value("PRE_FADER", sub::SidechainTap::PreFader)
        .value("AFTER_DEVICE", sub::SidechainTap::AfterDevice)
        .value("PRE_FX", sub::SidechainTap::PreFx);

    nb::class_<sub::SidechainInfo>(m, "SidechainInfo")
        .def_ro("track_id", &sub::SidechainInfo::trackId, "Its source.")
        .def_ro("tap", &sub::SidechainInfo::tap)
        .def_ro("tap_processor_id", &sub::SidechainInfo::tapProcessorId,
                "AFTER_DEVICE: the source's device it is taken after (0 otherwise).")
        .def("__repr__", [](const sub::SidechainInfo& s) {
            const char* taps[] = {"post-fader", "pre-fader", "after device ", "pre-fx"};
            return "SidechainInfo(" + std::to_string(s.trackId) + ", " + taps[static_cast<int>(s.tap)] +
                   (s.tap == sub::SidechainTap::AfterDevice ? std::to_string(s.tapProcessorId) : "") + ")";
        });

    nb::enum_<sub::ProcessorEvent::Type>(m, "ProcessorEventType")
        .value("PARAM_EDITED", sub::ProcessorEvent::Type::ParamEdited)
        .value("PARAMS_CHANGED", sub::ProcessorEvent::Type::ParamsChanged)
        .value("PARAM_INFO_CHANGED", sub::ProcessorEvent::Type::ParamInfoChanged)
        .value("EDITOR_CLOSED", sub::ProcessorEvent::Type::EditorClosed)
        .value("EDITOR_REQUESTED", sub::ProcessorEvent::Type::EditorRequested)
        .value("STATE_DIRTY", sub::ProcessorEvent::Type::StateDirty)
        .value("LATENCY_CHANGED", sub::ProcessorEvent::Type::LatencyChanged)
        .value("PARAM_TOUCHED", sub::ProcessorEvent::Type::ParamTouched);

    nb::class_<sub::ProcessorEventRecord>(m, "ProcessorEvent")
        .def_ro("processor_id", &sub::ProcessorEventRecord::processorId)
        .def_prop_ro("type", [](const sub::ProcessorEventRecord& e) { return e.type; })
        .def_prop_ro("param_index", [](const sub::ProcessorEventRecord& e) { return e.paramIndex; })
        .def_prop_ro("value", [](const sub::ProcessorEventRecord& e) { return e.value; })
        .def_prop_ro("old_value", [](const sub::ProcessorEventRecord& e) { return e.oldValue; })
        .def_prop_ro("gesture", [](const sub::ProcessorEventRecord& e) { return e.gesture; })
        .def("__repr__", [](const sub::ProcessorEventRecord& e) {
            return "ProcessorEvent(" + std::to_string(e.processorId) + ", type=" +
                   std::to_string(static_cast<int>(e.type)) + ", param=" + std::to_string(e.paramIndex) + ")";
        });

    nb::enum_<sub::MonitorMode>(m, "MonitorMode")
        .value("OFF", sub::MonitorMode::Off)
        .value("IN", sub::MonitorMode::In)
        .value("AUTO", sub::MonitorMode::Auto);
    m.attr("RECORD_PEAK_FRAMES") = Engine::kRecordPeakFrames;

    nb::class_<sub::RecordedTake>(m, "RecordedTake")
        .def_ro("track_id", &sub::RecordedTake::trackId)
        .def_ro("path", &sub::RecordedTake::path, "The WAV file; '' for a MIDI take.")
        .def_ro("start_sample", &sub::RecordedTake::startSample,
                "Timeline sample of its first frame, latency-corrected (negative: it starts before the timeline). "
                "A MIDI take starts where the playhead was when it began.")
        .def_ro("midi", &sub::RecordedTake::midi)
        .def_prop_ro("notes", [](const sub::RecordedTake& t) { return notesArray(t.notes); }, nb::rv_policy::automatic,
                     "A MIDI take's notes, latency-corrected, as an int64 array of rows (start, end, key, velocity, "
                     "channel) in timeline samples, within the take.")
        .def_ro("frames", &sub::RecordedTake::frames, "0: nothing was recorded, and there is no file.")
        .def_ro("channels", &sub::RecordedTake::channels)
        .def_ro("sample_rate", &sub::RecordedTake::sampleRate)
        .def_ro("dropped_frames", &sub::RecordedTake::droppedFrames,
                "Input lost because the disk fell behind (silence in the file).")
        .def_ro("error", &sub::RecordedTake::error)
        .def("__repr__", [](const sub::RecordedTake& t) {
            return "RecordedTake(" + std::to_string(t.trackId) + ", '" + t.path + "', start=" +
                   std::to_string(t.startSample) + ", frames=" + std::to_string(t.frames) + ")";
        });

    nb::class_<sub::RecordingProgress>(m, "RecordingProgress")
        .def_ro("track_id", &sub::RecordingProgress::trackId)
        .def_ro("started", &sub::RecordingProgress::started)
        .def_ro("start_sample", &sub::RecordingProgress::startSample)
        .def_ro("frames", &sub::RecordingProgress::frames)
        .def_prop_ro(
            "peaks",
            [](const sub::RecordingProgress& p) {
                auto buffer = std::make_unique<std::vector<float>>(p.peaks);
                const size_t rows = buffer->size() / 2;
                float* data = buffer->data();
                nb::capsule owner(buffer.release(),
                                  [](void* q) noexcept { delete static_cast<std::vector<float>*>(q); });
                return nb::ndarray<nb::numpy, float, nb::ndim<2>, nb::c_contig>(data, {rows, size_t{2}}, owner);
            },
            nb::rv_policy::automatic,
            "New (min, max) peaks since the last call, each over RECORD_PEAK_FRAMES frames: shape (n, 2).")
        .def_ro("midi", &sub::RecordingProgress::midi)
        .def_prop_ro("notes", [](const sub::RecordingProgress& p) { return notesArray(p.notes); },
                     nb::rv_policy::automatic,
                     "A MIDI take's notes so far, as RecordedTake.notes has them (a held note's end is -1).");

    nb::class_<sub::AudioClockStatus>(m, "AudioClock")
        .def_ro("running", &sub::AudioClockStatus::running)
        .def_ro("host_time_ns", &sub::AudioClockStatus::hostTimeNs, "When the last audio callback began (host_time_ns()).")
        .def_ro("sample_time", &sub::AudioClockStatus::sampleTime, "The device sample it began with.")
        .def_ro("midi_delay", &sub::AudioClockStatus::midiDelay,
                "Samples between a MIDI message's arrival and when it plays (a device buffer).");
    m.def("host_time_ns", &sub::hostTimeNs, "The clock MIDI input is stamped with (steady_clock), in ns.");

    nb::class_<sub::TrackCost>(m, "TrackCost")
        .def_ro("track_id", &sub::TrackCost::trackId)
        .def_ro("ns_per_frame", &sub::TrackCost::nsPerFrame)
        .def("__repr__", [](const sub::TrackCost& c) {
            return "TrackCost(" + std::to_string(c.trackId) + ", " + std::to_string(c.nsPerFrame) + " ns/frame)";
        });
    // The graph's queue order and ranks, for tests.
    const auto taskGraphOrder = [](const std::vector<std::vector<int>>& destinations, const std::vector<float>& costs) {
        if (costs.size() != destinations.size()) throw std::invalid_argument("One cost per node");
        std::vector<std::pair<int, int>> edges;
        for (size_t i = 0; i < destinations.size(); ++i) {
            for (const int to : destinations[i]) {
                if (to >= 0 && static_cast<size_t>(to) <= i) throw std::invalid_argument("A node goes into one listed before it");
                edges.emplace_back(static_cast<int>(i), to);
            }
        }
        sub::TaskGraph graph(static_cast<int>(destinations.size()), edges);
        std::vector<float> ranks;
        for (int i = 0; i < graph.size(); ++i) graph.setCost(i, costs[static_cast<size_t>(i)]);
        graph.orderRoots();
        for (int i = 0; i < graph.size(); ++i) ranks.push_back(graph.rank(i));
        return std::make_pair(graph.roots(), ranks);
    };
    m.def(
        "task_graph_order",
        [taskGraphOrder](const std::vector<int>& outputs, const std::vector<float>& costs) {
            std::vector<std::vector<int>> destinations;
            for (const int out : outputs) destinations.push_back(out >= 0 ? std::vector<int>{out} : std::vector<int>{});
            return taskGraphOrder(destinations, costs);
        },
        "outputs"_a, "costs"_a,
        "For tests: the order a render graph queues its nodes without inputs, and each node's rank (the cost "
        "from it to the end of its longest path). Node i goes into outputs[i] (-1: none), listed after its inputs.");
    m.def("task_graph_order", taskGraphOrder, "destinations"_a, "costs"_a,
          "As above, with any number of edges per node: node i goes into each of destinations[i].");

    nb::class_<sub::SendInfo>(m, "SendInfo")
        .def_ro("track_id", &sub::SendInfo::trackId, "The track it goes into.")
        .def_ro("gain", &sub::SendInfo::gain)
        .def_ro("pre_fader", &sub::SendInfo::preFader)
        .def("__repr__", [](const sub::SendInfo& s) {
            return "SendInfo(" + std::to_string(s.trackId) + ", " + std::to_string(s.gain) +
                   (s.preFader ? ", pre-fader)" : ")");
        });

    nb::class_<sub::MeterReading>(m, "MeterReading")
        .def_ro("track_id", &sub::MeterReading::trackId)
        .def_ro("left", &sub::MeterReading::left)
        .def_ro("right", &sub::MeterReading::right)
        .def_ro("chain_id", &sub::MeterReading::chainId, "A rack chain's fader (on track track_id); 0: the track's own.");

    nb::enum_<sub::WarpMode>(m, "WarpMode")
        .value("TRANSIENTS", sub::WarpMode::Transients)
        .value("STANDARD", sub::WarpMode::Standard)
        .value("SMOOTH", sub::WarpMode::Smooth)
        .value("FORMANTS", sub::WarpMode::Formants)
        .value("RE_PITCH", sub::WarpMode::RePitch);

    nb::class_<sub::ClipDesc>(m, "ClipDesc")
        .def(
            "__init__",
            [](sub::ClipDesc* self, std::string path, double startBeat, double durationSec, double offsetSec,
               float gain, float pan, bool warp, double segmentBpm, sub::WarpMode warpMode, double transpose,
               std::string id) {
                new (self) sub::ClipDesc{std::move(path), startBeat, durationSec, offsetSec, gain, pan,
                                         warp, segmentBpm, warpMode, transpose, std::move(id)};
            },
            "path"_a, "start_beat"_a, "duration_sec"_a, "offset_sec"_a = 0.0, "gain"_a = 1.0f, nb::kw_only(),
            "pan"_a = 0.0f, "warp"_a = false, "segment_bpm"_a = 0.0, "warp_mode"_a = sub::WarpMode::Standard,
            "transpose"_a = 0.0, "id"_a = "")
        .def_rw("path", &sub::ClipDesc::path)
        .def_rw("start_beat", &sub::ClipDesc::startBeat)
        .def_rw("duration_sec", &sub::ClipDesc::durationSec)
        .def_rw("offset_sec", &sub::ClipDesc::offsetSec)
        .def_rw("gain", &sub::ClipDesc::gain)
        .def_rw("pan", &sub::ClipDesc::pan)
        .def_rw("warp", &sub::ClipDesc::warp)
        .def_rw("segment_bpm", &sub::ClipDesc::segmentBpm)
        .def_rw("warp_mode", &sub::ClipDesc::warpMode)
        .def_rw("transpose", &sub::ClipDesc::transpose)
        .def_rw("id", &sub::ClipDesc::id);

    nb::class_<sub::NoteDesc>(m, "NoteDesc")
        .def(
            "__init__",
            [](sub::NoteDesc* self, double startBeat, double lengthBeats, int key, int velocity) {
                new (self) sub::NoteDesc{startBeat, lengthBeats, key, velocity};
            },
            "start_beat"_a, "length_beats"_a, "key"_a, "velocity"_a = 100)
        .def_rw("start_beat", &sub::NoteDesc::startBeat)
        .def_rw("length_beats", &sub::NoteDesc::lengthBeats)
        .def_rw("key", &sub::NoteDesc::key)
        .def_rw("velocity", &sub::NoteDesc::velocity)
        .def("__repr__", [](const sub::NoteDesc& n) {
            return "NoteDesc(" + std::to_string(n.startBeat) + ", " + std::to_string(n.lengthBeats) + ", key=" +
                   std::to_string(n.key) + ", velocity=" + std::to_string(n.velocity) + ")";
        });

    nb::class_<AudioSource>(m, "AudioSource")
        .def_prop_ro("path", &AudioSource::path)
        .def_prop_ro("frames", &AudioSource::frames)
        .def_prop_ro("channels", &AudioSource::channels)
        .def_prop_ro("sample_rate", &AudioSource::sampleRate)
        .def_prop_ro("file_sample_rate", &AudioSource::fileSampleRate)
        .def_prop_ro("duration", &AudioSource::duration)
        .def_prop_ro("peak_levels", &AudioSource::numPeakLevels)
        .def_static("samples_per_peak", &AudioSource::samplesPerPeak, "level"_a)
        .def(
            "peaks",
            [](const AudioSource& self, int level) {
                if (level < 0 || level >= self.numPeakLevels()) throw nb::index_error("peak level out of range");
                // Zero-copy view; the Python AudioSource object keeps the data alive.
                return PeakArray(self.peaks(level),
                                 {static_cast<size_t>(self.numPeaks(level)), self.channels(), size_t{2}},
                                 nb::find(&self));
            },
            "level"_a, "Min/max peaks as a float32 array of shape (n, channels, 2).")
        .def(
            "samples",
            [](const AudioSource& self, int64_t start, int64_t count) {
                start = std::clamp<int64_t>(start, 0, self.frames());
                count = std::clamp<int64_t>(count, 0, self.frames() - start);
                return SampleArray(self.data() + start, {static_cast<size_t>(self.channels()), static_cast<size_t>(count)},
                                   nb::find(&self), {self.frames(), int64_t{1}});
            },
            "start"_a, "count"_a, "Raw samples as a float32 view of shape (channels, count).")
        .def("__repr__", [](const AudioSource& s) {
            return "AudioSource('" + s.path() + "', " + std::to_string(s.frames()) + " frames)";
        });

    nb::class_<Engine>(m, "Engine")
        .def(nb::init<>())
        // Device
        .def("list_devices", &Engine::devices, "driver"_a = "WASAPI", ReleaseGil(),
             "The devices of a driver type: WASAPI outputs, or installed ASIO drivers.")
        .def(
            "open_device",
            [](Engine& self, const std::string& name, uint32_t sampleRate, uint32_t bufferFrames, bool exclusive,
               const std::string& driver, const std::vector<int>& inputChannels,
               const std::vector<int>& outputChannels, uintptr_t window) {
                sub::DeviceConfig config;
                config.driver = driver;
                config.name = name;
                config.sampleRate = sampleRate;
                config.bufferFrames = bufferFrames;
                config.exclusive = exclusive;
                config.inputChannels = inputChannels;
                config.outputChannels = outputChannels;
                config.window = window;
                nb::gil_scoped_release release;  // a driver may show a dialog (a message loop that calls Python)
                self.openDevice(config);
            },
            "name"_a = "", "sample_rate"_a = 0, "buffer_frames"_a = 0, "exclusive"_a = false, nb::kw_only(),
            "driver"_a = "WASAPI", "input_channels"_a = std::vector<int>(), "output_channels"_a = std::vector<int>(),
            "window"_a = 0,
            "Open an audio device, closing the one open. An empty name opens the system default output (WASAPI) "
            "or the first driver (ASIO); sample_rate 0 keeps the device's rate, buffer_frames 0 takes its "
            "preferred size. Channels are the device's (0-based): no inputs, and the first two outputs, unless "
            "given. `window` is the main window's handle, for ASIO drivers' dialogs. Raises RuntimeError.")
        .def("reopen_device", &Engine::reopenDevice, ReleaseGil(),
             "Open the last device again, with the settings its driver asked for (after a 'reset' event).")
        .def("close_device", &Engine::closeDevice, ReleaseGil())
        .def_prop_ro("device_status", &Engine::deviceStatus)
        .def_prop_ro("device_capabilities", &Engine::deviceCapabilities,
                     "What the open device offers (channels, sample rates, buffer sizes).")
        .def("show_device_control_panel", &Engine::showDeviceControlPanel, ReleaseGil(),
             "Show the ASIO driver's own settings. False if it has none (or no device is open).")
        .def_prop_ro("sample_rate", &Engine::sampleRate)
        .def_prop_ro("cpu_load", &Engine::cpuLoad)
        .def("take_device_event", &Engine::takeDeviceEvent,
             "The next device event: '', 'stopped', 'rerouted', 'reset' (call reopen_device()) or 'latency'.")
        .def("take_input_meters", &Engine::takeInputMeters,
             "Peak level of each open input channel (see DeviceStatus.input_channels) since the last call.")
        .def(
            "master_scope",
            [](const Engine& self, size_t frames) {
                auto buffer = std::make_unique<std::vector<float>>(self.masterScope(frames));
                const size_t size = buffer->size();
                float* data = buffer->data();
                nb::capsule owner(buffer.release(),
                                  [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
                return nb::ndarray<nb::numpy, float, nb::ndim<1>, nb::c_contig>(data, {size}, owner);
            },
            "frames"_a,
            "The last `frames` samples of the master output as a float32 array (mono, oldest first; at most 4096).")
        .def_prop_ro("master_scope_written", &Engine::masterScopeWritten,
                     "Samples of the master output played so far (stands still while no device runs).")
        // Sources
        .def("load_source", &Engine::loadSource, "path"_a, ReleaseGil(),
             "Decode an audio file at the engine sample rate (cached). Blocking; call from a worker thread.")
        .def("cached_source", &Engine::cachedSource, "path"_a)
        .def("release_unused_sources", &Engine::releaseUnusedSources)
        // Tracks
        // Tracks. Track id MASTER (0) is the master: devices, mixer and automation, no clips or notes.
        .def("add_track", &Engine::addTrack)
        .def("remove_track", &Engine::removeTrack, "track_id"_a)
        .def("set_track_clips", &Engine::setTrackClips, "track_id"_a, "clips"_a)
        .def("set_track_notes", &Engine::setTrackNotes, "track_id"_a, "notes"_a,
             "The notes a MIDI track plays, in timeline beats.")
        .def("preview_note", &Engine::previewNote, "track_id"_a, "key"_a, "velocity"_a,
             "Play a note on the track's instrument now; velocity 0 releases it.")
        .def("set_track_gain", &Engine::setTrackGain, "track_id"_a, "gain"_a)
        .def("set_track_pan", &Engine::setTrackPan, "track_id"_a, "pan"_a)
        .def("set_track_mute", &Engine::setTrackMute, "track_id"_a, "mute"_a)
        .def("set_track_solo", &Engine::setTrackSolo, "track_id"_a, "solo"_a)
        .def("set_track_output", &Engine::setTrackOutput, "track_id"_a, "output_track_id"_a,
             "Where a track's output goes: MASTER, or another track (a group's bus), which sums it into its input. "
             "Raises ValueError for a route that would close a cycle.")
        .def("track_output", &Engine::trackOutput, "track_id"_a)
        .def("set_track_frozen", &Engine::setTrackFrozen, "track_id"_a, "frozen"_a,
             "Frozen, a track plays its clips (its frozen audio) through its fader, and nothing else: not its "
             "devices, notes, input, nor what goes into it. A track every edge of which ends at frozen tracks "
             "isn't rendered at all.")
        .def("track_frozen", &Engine::trackFrozen, "track_id"_a)
        .def("set_track_send", &Engine::setTrackSend, "track_id"_a, "to_track_id"_a, "gain"_a, "pre_fader"_a = false,
             "A send: the track's signal also goes into another track (a return), at `gain`, after its fader or "
             "before it. Setting it again changes it. Raises ValueError for a send that would close a cycle. "
             "Automated by a lane (processor 0, param 'send:<to_track_id>') of the sending track.")
        .def("remove_track_send", &Engine::removeTrackSend, "track_id"_a, "to_track_id"_a)
        .def("track_sends", &Engine::trackSends, "track_id"_a, "A track's sends, in the order they were made.")
        .def("set_master_gain", &Engine::setMasterGain, "gain"_a)
        .def("set_master_pan", &Engine::setMasterPan, "pan"_a)
        .def("set_track_automation", &Engine::setTrackAutomation, "track_id"_a, "lanes"_a,
             "Replace a track's automation (track_id 0: the master's) with these AutomationLanes: of its mixer, "
             "its devices' parameters (in racks too), or a rack chain's fader (the rack's id, param "
             "'chain:<chain id>:volume' or ':pan').")
        .def("take_meters", &Engine::takeMeters, "Peak levels since the last call; track_id 0 is the master.")
        // Device chains: every track (and the master) has a main chain, by id.
        .def("track_chain", &Engine::trackChain, "track_id"_a, "A track's (or the master's) main device chain.")
        .def("processor_chain", &Engine::processorChain, "processor_id"_a, "The chain a device is in.")
        .def("add_builtin_processor", &Engine::addBuiltinProcessor, "chain_id"_a, "type"_a, "index"_a = -1,
             "Add a built-in device to a chain.")
        .def("add_plugin_processor", &Engine::addPluginProcessor, "chain_id"_a, "format"_a, "path"_a, "uid"_a,
             "index"_a = -1, ReleaseGil(),
             "Load a plug-in into a chain (main thread). Raises RuntimeError if it can't be loaded.")
        .def("remove_processor", &Engine::removeProcessor, "processor_id"_a)
        .def("set_chain_order", &Engine::setChainOrder, "chain_id"_a, "processor_ids"_a,
             "Reorder a chain: processor_ids lists all of its devices.")
        .def("move_processor", &Engine::moveProcessor, "processor_id"_a, "to_chain_id"_a, "index"_a = -1,
             "Move a device into a chain (another track's too), keeping its state (and sidechain: ValueError if "
             "that would close a cycle there): index counts the chain without it, -1 is last.")
        // Racks
        .def("add_rack", &Engine::addRack, "chain_id"_a, "index"_a = -1,
             "Add a rack (a device group) to a chain: its chains each process its input, side by side, and it "
             "puts out their sum (without chains, its input). Racks nest at most MAX_RACK_DEPTH deep (ValueError).")
        .def("add_rack_chain", &Engine::addRackChain, "rack_id"_a, "index"_a = -1,
             "A new chain of a rack (a chain id, for add_*_processor and move_processor), at `index` (-1: last).")
        .def("remove_rack_chain", &Engine::removeRackChain, "chain_id"_a, "A rack's chain goes, with its devices.")
        .def("set_rack_chain_order", &Engine::setRackChainOrder, "rack_id"_a, "chain_ids"_a)
        .def("rack_chains", &Engine::rackChains, "rack_id"_a, "A rack's chains, in order.")
        .def("chain_rack", &Engine::chainRack, "chain_id"_a, "The rack a chain belongs to; 0: a track's own chain.")
        .def("chain_processors", &Engine::chainProcessors, "chain_id"_a, "A chain's devices, in order.")
        .def("set_chain_gain", &Engine::setChainGain, "chain_id"_a, "gain"_a, "A rack chain's fader.")
        .def("set_chain_pan", &Engine::setChainPan, "chain_id"_a, "pan"_a)
        .def("set_chain_mute", &Engine::setChainMute, "chain_id"_a, "mute"_a)
        .def("set_chain_solo", &Engine::setChainSolo, "chain_id"_a, "solo"_a,
             "Soloed chains are the only ones of their rack heard.")
        .def("processor_info", &Engine::processorInfo, "processor_id"_a)
        .def("set_processor_sidechain", &Engine::setProcessorSidechain, "processor_id"_a, "source_track_id"_a,
             "tap"_a = sub::SidechainTap::PostFader, "tap_processor_id"_a = 0,
             "A device's sidechain (its aux input: ProcessorInfo.has_sidechain): a track's signal after its fader, "
             "before it, after one of its devices (tap_processor_id) or before them all (PRE_FX), lined up with the "
             "signal at the device. "
             "Raises ValueError for the master, a device without a sidechain input, or a sidechain that would "
             "close a cycle (its own track, or one its track feeds).")
        .def("clear_processor_sidechain", &Engine::clearProcessorSidechain, "processor_id"_a)
        .def("processor_sidechain", &Engine::processorSidechain, "processor_id"_a,
             "A device's sidechain (None: none). It goes when its source does.")
        .def("processor_params", &Engine::processorParams, "processor_id"_a)
        .def("processor_param_index", &Engine::processorParamIndex, "processor_id"_a, "param_id"_a)
        .def("processor_param", &Engine::processorParam, "processor_id"_a, "index"_a)
        .def("set_processor_param", &Engine::setProcessorParam, "processor_id"_a, "index"_a, "value"_a)
        .def("processor_param_text", &Engine::processorParamText, "processor_id"_a, "index"_a, "value"_a,
             "The processor's own text for a value ('' if it has none).")
        .def("set_processor_enabled", &Engine::setProcessorEnabled, "processor_id"_a, "enabled"_a)
        .def("processor_displays", &Engine::processorDisplays, "processor_id"_a,
             "What a device's own editor draws besides its parameters: streams of values (meters, curves).")
        .def(
            "read_processor_display",
            [](Engine& self, uint32_t processorId, int index, uint64_t position) {
                auto buffer = std::make_unique<std::vector<float>>();
                const uint64_t next = self.readProcessorDisplay(processorId, index, position, *buffer);
                const size_t size = buffer->size();
                float* data = buffer->data();
                nb::capsule owner(buffer.release(),
                                  [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
                return nb::make_tuple(nb::ndarray<nb::numpy, float, nb::ndim<1>, nb::c_contig>(data, {size}, owner),
                                      next);
            },
            "processor_id"_a, "index"_a, "position"_a = 0,
            "(values, next position): display `index`'s values since `position` (0 at first), oldest first, as "
            "float32; at most the latest 8192. Pass the position back next time.")
        .def(
            "processor_state",
            [](Engine& self, uint32_t processorId) {
                std::vector<uint8_t> state;
                {
                    nb::gil_scoped_release release;
                    state = self.processorState(processorId);
                }
                return toBytes(state);
            },
            "processor_id"_a,
            "A device's own state: a plug-in's settings (a .vstpreset); a built-in device's besides its "
            "parameters, as lines \"name=value\" (a sampler's sample; empty if it has none).")
        .def(
            "set_processor_state",
            [](Engine& self, uint32_t processorId, const nb::bytes& state) {
                const auto data = fromBytes(state);
                nb::gil_scoped_release release;
                self.setProcessorState(processorId, data);
            },
            "processor_id"_a, "state"_a,
            "Restore a device's own state. A built-in device loads the files it names (a sample) first, which "
            "may take a while (call it off the UI thread), and raises RuntimeError if one can't be loaded.")
        .def("open_editor", &Engine::openEditor, "processor_id"_a, "owner_window"_a = 0, "title"_a = "",
             ReleaseGil(), "Show a plug-in's editor window (or raise it). False if it has none.")
        .def("close_editor", &Engine::closeEditor, "processor_id"_a)
        .def("is_editor_open", &Engine::isEditorOpen, "processor_id"_a, "Open and not hidden.")
        .def("set_editor_visible", &Engine::setEditorVisible, "processor_id"_a, "visible"_a,
             "Hide an open editor window or show it again (without focusing it). False if none is open.")
        .def("set_editor_title", &Engine::setEditorTitle, "processor_id"_a, "title"_a)
        .def("take_processor_events", &Engine::takeProcessorEvents,
             "What processors reported since the last call (edits in a plug-in's own editor, ...).")
        // Input and recording
        .def("set_track_input", &Engine::setTrackInput, "track_id"_a, "channels"_a,
             "A track's input: device channels (as DeviceStatus.input_channels numbers them): [] none, [c] mono, "
             "[l, r] a stereo pair. Channels not open on the device are silent.")
        .def("set_track_input_track", &Engine::setTrackInputTrack, "track_id"_a, "source_track_id"_a,
             "A track's input from another track's output, after its fader (resampling it), or MASTER's "
             "(resampling the mix: recorded, never monitored), instead of device channels (set_track_input() goes "
             "back to those). Raises ValueError for the track itself or a track it feeds (a cycle).")
        .def("track_input_track", &Engine::trackInputTrack, "track_id"_a,
             "The track whose output a track takes as its input (MASTER: the master's); None: the device's.")
        .def("set_track_monitor", &Engine::setTrackMonitor, "track_id"_a, "mode"_a)
        .def("set_track_armed", &Engine::setTrackArmed, "track_id"_a, "armed"_a)
        .def(
            "start_recording",
            [](Engine& self, const std::vector<std::pair<uint32_t, std::string>>& targets, double countInBeats) {
                std::vector<sub::RecordTarget> list;
                for (const auto& [trackId, path] : targets) list.push_back({trackId, path});
                nb::gil_scoped_release release;
                self.startRecording(list, countInBeats);
            },
            "targets"_a, "count_in_beats"_a = 0.0,
            "Record each (track_id, wav_path)'s input from where the playhead moves next; starts playing (after "
            "the count-in) if stopped. A path of '' records the track's MIDI input. A track whose input is another "
            "track's (or the master's) records that, in stereo. Raises RuntimeError for the user, ValueError for "
            "bad targets.")
        .def("stop_recording", &Engine::stopRecording, ReleaseGil(),
             "End the recording (playing goes on); its takes, and any of a recording a device change ended.")
        .def_prop_ro("is_recording", &Engine::isRecording,
                     "Recording and taking input (not after the playhead jumped or a device change).")
        .def("recording_progress", &Engine::recordingProgress, "The takes being recorded, for the live waveform.")
        // MIDI input
        .def("midi_input_devices", &Engine::midiInputDevices, "The MIDI inputs connected, by name.")
        .def("open_midi_input", &Engine::openMidiInput, "name"_a, ReleaseGil(),
             "Open a MIDI input (main thread). Raises RuntimeError for the user.")
        .def("close_midi_input", &Engine::closeMidiInput, "name"_a, ReleaseGil())
        .def("open_midi_inputs", &Engine::openMidiInputs)
        .def("set_track_midi_input", &Engine::setTrackMidiInput, "track_id"_a, "enabled"_a, "device"_a = "",
             "channel"_a = 0,
             "A track's MIDI input: none (enabled False), every input (device '') or one by name, on every "
             "channel (0) or one (1-16).")
        .def(
            "send_midi_input",
            [](Engine& self, const std::string& device, const std::vector<int>& message, int64_t hostTime) {
                std::vector<uint8_t> bytes;
                for (const int b : message) {
                    if (b < 0 || b > 255) throw nb::value_error("MIDI bytes are 0-255");
                    bytes.push_back(static_cast<uint8_t>(b));
                }
                self.sendMidiInput(device, bytes, hostTime);
            },
            "device"_a, "message"_a, "host_time_ns"_a = 0,
            "A MIDI message as if the input `device` sent it at host_time_ns (host_time_ns(); 0: now).")
        .def_prop_ro("audio_clock", &Engine::audioClock, "The audio device's clock as MIDI input sees it.")
        // Transport
        .def("play", &Engine::play, "count_in_beats"_a = 0.0)
        .def_prop_ro("is_counting_in", &Engine::isCountingIn)
        .def("stop", &Engine::stop)
        .def_prop_ro("is_playing", &Engine::isPlaying)
        .def_prop_rw("position_beats", &Engine::positionBeats, &Engine::setPositionBeats)
        .def_prop_rw("tempo", &Engine::tempo, &Engine::setTempo)
        .def("set_time_signature", &Engine::setTimeSignature, "numerator"_a, "denominator"_a)
        .def("set_loop", &Engine::setLoop, "enabled"_a, "start_beat"_a, "end_beat"_a)
        .def_prop_rw("metronome", &Engine::metronome, &Engine::setMetronome)
        .def("set_clip_fade_ms", &Engine::setClipFadeMs, "ms"_a)
        // Preview
        .def("preview", &Engine::preview, "path"_a, "Audition a loaded source through the master output.")
        .def("stop_preview", &Engine::stopPreview)
        .def_prop_ro("is_previewing", &Engine::isPreviewing)
        // Offline
        .def(
            "render_offline",
            [](Engine& self, double startBeat, int64_t frames, bool loop, bool metronome) {
                auto buffer = std::make_unique<std::vector<float>>();
                {
                    nb::gil_scoped_release release;
                    *buffer = self.renderOffline(startBeat, frames, loop, metronome);
                }
                float* data = buffer->data();
                nb::capsule owner(buffer.release(),
                                  [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
                return StereoArray(data, {static_cast<size_t>(frames), size_t{2}}, owner);
            },
            "start_beat"_a, "frames"_a, "loop"_a = false, "metronome"_a = false,
            "Render the arrangement to a (frames, 2) float32 array.")
        .def("export_wav", &Engine::exportWav, "path"_a, "start_beat"_a, "end_beat"_a, "bit_depth"_a = 24,
             ReleaseGil())
        .def(
            "render_track_offline",
            [](Engine& self, uint32_t trackId, double startBeat, int64_t frames) {
                auto buffer = std::make_unique<std::vector<float>>();
                {
                    nb::gil_scoped_release release;
                    *buffer = self.renderTrackOffline(trackId, startBeat, frames);
                }
                float* data = buffer->data();
                nb::capsule owner(buffer.release(),
                                  [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
                return StereoArray(data, {static_cast<size_t>(frames), size_t{2}}, owner);
            },
            "track_id"_a, "start_beat"_a, "frames"_a,
            "A track's signal before its fader (after its devices: what freezing it keeps), lined up with the "
            "timeline, as a (frames, 2) float32 array. Solo is ignored.")
        .def("render_track_to_wav", &Engine::renderTrackToWav, "track_id"_a, "path"_a, "start_beat"_a, "end_beat"_a,
             "tail_seconds"_a = 0.0, ReleaseGil(),
             "The same from start_beat to end_beat, then on for up to tail_seconds while it isn't silent, into a "
             "new 32-bit float WAV file (freezing the track). Returns the frames written.")
        // Audio threads
        .def_static("default_audio_threads", &Engine::defaultAudioThreads,
                    "The audio threads used unless set: one per core but one (at least 1).")
        .def_prop_rw("audio_threads", &Engine::audioThreads, &Engine::setAudioThreads,
                     "Threads rendering tracks: the audio thread and its workers (1: no workers). "
                     "Renders are the same whatever the number.")
        .def_prop_ro("nodes_on_workers", &Engine::nodesOnWorkers,
                     "Tracks the worker threads have rendered (not the audio thread's) since audio_threads was "
                     "last set.")
        .def_prop_rw("cost_ordering", &Engine::costOrdering, &Engine::setCostOrdering,
                     "Start the tracks with the most work hanging off them first (default); off: in routing "
                     "order (for benchmarks). Renders are the same either way.")
        .def("track_costs", &Engine::trackCosts,
             "What rendering each track takes lately (smoothed, in ns per frame; 0: not rendered yet).")
        .def("idle", &Engine::idle, ReleaseGil(), "Housekeeping; call periodically from the UI thread.");
}
