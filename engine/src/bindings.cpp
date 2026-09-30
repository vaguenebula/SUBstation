// Python bindings for the engine (module gilstudio._engine).
//
// Long-running calls release the GIL. The audio thread never calls into Python,
// so audio keeps running no matter what the interpreter is doing.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "Engine.h"
#include "plugins/Vst3Format.h"

namespace nb = nanobind;
using namespace nb::literals;

using gil::AudioSource;
using gil::Engine;

namespace {

nb::bytes toBytes(const std::vector<uint8_t>& data) {
    return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
}

std::vector<uint8_t> fromBytes(const nb::bytes& data) {
    const auto* begin = static_cast<const uint8_t*>(data.data());
    return {begin, begin + data.size()};
}

}  // namespace

using PeakArray = nb::ndarray<nb::numpy, const float, nb::ndim<3>, nb::c_contig>;
using SampleArray = nb::ndarray<nb::numpy, const float, nb::ndim<2>>;
using StereoArray = nb::ndarray<nb::numpy, float, nb::ndim<2>, nb::c_contig>;

using ReleaseGil = nb::call_guard<nb::gil_scoped_release>;

NB_MODULE(_engine, m) {
    m.doc() = "GIL Studio real-time audio engine";
    // Qt/PySide can keep engine objects alive until interpreter teardown; that
    // is harmless, so don't print nanobind's leak report at exit.
    nb::set_leak_warnings(false);
    m.attr("MAX_BLOCK") = gil::Renderer::kMaxBlock;
    m.attr("PEAK_LEVELS") = AudioSource::kNumPeakLevels;

    nb::class_<gil::AudioFileInfo>(m, "AudioFileInfo")
        .def_ro("frames", &gil::AudioFileInfo::frames)
        .def_ro("channels", &gil::AudioFileInfo::channels)
        .def_ro("sample_rate", &gil::AudioFileInfo::sampleRate)
        .def_ro("duration", &gil::AudioFileInfo::duration)
        .def("__repr__", [](const gil::AudioFileInfo& info) {
            return "AudioFileInfo(frames=" + std::to_string(info.frames) + ", channels=" +
                   std::to_string(info.channels) + ", sample_rate=" + std::to_string(info.sampleRate) + ")";
        });

    m.def("probe_file", &AudioSource::probe, "path"_a, ReleaseGil(),
          "Read an audio file's length and format without decoding it.");

    m.def("driver_types", &Engine::driverTypes,
          "The audio driver types this engine was built with: 'WASAPI', and 'ASIO' if it had the ASIO SDK.");

    nb::class_<gil::AudioDeviceInfo>(m, "AudioDeviceInfo")
        .def_ro("name", &gil::AudioDeviceInfo::name)
        .def_ro("is_default", &gil::AudioDeviceInfo::isDefault)
        .def("__repr__", [](const gil::AudioDeviceInfo& d) {
            return "AudioDeviceInfo('" + d.name + "'" + (d.isDefault ? ", default)" : ")");
        });

    nb::class_<gil::DeviceStatus>(m, "DeviceStatus")
        .def_ro("open", &gil::DeviceStatus::open)
        .def_ro("name", &gil::DeviceStatus::name)
        .def_ro("backend", &gil::DeviceStatus::backend, "The driver type: 'WASAPI' or 'ASIO'.")
        .def_ro("sample_rate", &gil::DeviceStatus::sampleRate)
        .def_ro("buffer_frames", &gil::DeviceStatus::bufferFrames)
        .def_ro("latency_ms", &gil::DeviceStatus::latencyMs, "Output latency.")
        .def_ro("input_latency_ms", &gil::DeviceStatus::inputLatencyMs)
        .def_ro("input_channels", &gil::DeviceStatus::inputChannels,
                "The device's input channels that are open (0-based), in the order take_input_meters() lists them.")
        .def_ro("output_channels", &gil::DeviceStatus::outputChannels,
                "The device's output channels that are open; the master plays on the first two.")
        .def_ro("exclusive", &gil::DeviceStatus::exclusive);

    nb::class_<gil::DeviceCaps>(m, "DeviceCapabilities")
        .def_ro("input_names", &gil::DeviceCaps::inputNames, "Names of all the device's inputs.")
        .def_ro("output_names", &gil::DeviceCaps::outputNames)
        .def_ro("sample_rates", &gil::DeviceCaps::sampleRates, "The rates it can run at; empty: any.")
        .def_ro("buffer_sizes", &gil::DeviceCaps::bufferSizes, "The buffer sizes it offers; empty: any.")
        .def_ro("preferred_buffer_frames", &gil::DeviceCaps::preferredBufferFrames)
        .def_ro("has_control_panel", &gil::DeviceCaps::hasControlPanel);

    nb::class_<gil::ParamInfo>(m, "ParamInfo")
        .def_ro("id", &gil::ParamInfo::id)
        .def_ro("name", &gil::ParamInfo::name)
        .def_ro("unit", &gil::ParamInfo::unit)
        .def_ro("min_value", &gil::ParamInfo::minValue)
        .def_ro("max_value", &gil::ParamInfo::maxValue)
        .def_ro("default_value", &gil::ParamInfo::defaultValue)
        .def_ro("log_scale", &gil::ParamInfo::logScale)
        .def_ro("value_labels", &gil::ParamInfo::valueLabels)
        .def_ro("steps", &gil::ParamInfo::steps)
        .def_ro("automatable", &gil::ParamInfo::automatable)
        .def_ro("read_only", &gil::ParamInfo::readOnly)
        .def_ro("hidden", &gil::ParamInfo::hidden)
        .def_prop_ro("step_count", &gil::ParamInfo::stepCount,
                     "Steps between the lowest and highest value of a discrete parameter; 0 if continuous.")
        .def("to_normalized", &gil::ParamInfo::toNormalized, "plain"_a,
             "A plain value as automation sees it (0..1).")
        .def("from_normalized", &gil::ParamInfo::fromNormalized, "normalized"_a)
        .def("__repr__", [](const gil::ParamInfo& p) { return "ParamInfo('" + p.id + "', '" + p.name + "')"; });

    nb::class_<gil::AutomationPoint>(m, "AutomationPoint")
        .def(
            "__init__",
            [](gil::AutomationPoint* self, double beat, float value, float curve) {
                new (self) gil::AutomationPoint{beat, value, curve};
            },
            "beat"_a, "value"_a, "curve"_a = 0.0f)
        .def_rw("beat", &gil::AutomationPoint::beat)
        .def_rw("value", &gil::AutomationPoint::value, "Normalized, 0..1.")
        .def_rw("curve", &gil::AutomationPoint::curve, "How the segment to the next point bends (-1..1).")
        .def("__repr__", [](const gil::AutomationPoint& a) {
            return "AutomationPoint(" + std::to_string(a.beat) + ", " + std::to_string(a.value) + ", " +
                   std::to_string(a.curve) + ")";
        });

    nb::class_<gil::AutomationLaneDesc>(m, "AutomationLane")
        .def(
            "__init__",
            [](gil::AutomationLaneDesc* self, uint32_t processorId, std::string param,
               std::vector<gil::AutomationPoint> points) {
                new (self) gil::AutomationLaneDesc{processorId, std::move(param), std::move(points)};
            },
            "processor_id"_a, "param"_a, "points"_a,
            "An envelope: of a device's parameter (by id), or with processor_id 0 of the mixer's "
            "'volume' or 'pan'.")
        .def_rw("processor_id", &gil::AutomationLaneDesc::processorId)
        .def_rw("param", &gil::AutomationLaneDesc::param)
        .def_rw("points", &gil::AutomationLaneDesc::points);
    m.attr("AUTOMATION_CURVATURE") = gil::kAutomationCurvature;
    m.attr("MAX_VOLUME_GAIN") = gil::kMaxVolumeGain;

    nb::class_<gil::PluginDescription>(m, "PluginDescription")
        .def_ro("format", &gil::PluginDescription::format)
        .def_ro("path", &gil::PluginDescription::path)
        .def_ro("uid", &gil::PluginDescription::uid)
        .def_ro("name", &gil::PluginDescription::name)
        .def_ro("vendor", &gil::PluginDescription::vendor)
        .def_ro("version", &gil::PluginDescription::version)
        .def_ro("category", &gil::PluginDescription::category)
        .def_ro("is_instrument", &gil::PluginDescription::isInstrument)
        .def("__repr__", [](const gil::PluginDescription& d) {
            return "PluginDescription('" + d.name + "', " + d.format + ", " + d.uid + ")";
        });

    m.def(
        "scan_vst3", [](const std::string& path) { return gil::vst3::Vst3Format::instance().scanFile(path); },
        "path"_a, ReleaseGil(),
        "List the plug-ins in a VST3 file or bundle. Loads its code: the UI calls this in a child process.");
    m.def("vst3_search_paths", [] { return gil::vst3::Vst3Format::instance().defaultSearchPaths(); },
          "The standard VST3 folders.");

    nb::class_<gil::ProcessorInfo>(m, "ProcessorInfo")
        .def_ro("type_id", &gil::ProcessorInfo::typeId)
        .def_ro("name", &gil::ProcessorInfo::name)
        .def_ro("latency", &gil::ProcessorInfo::latency)
        .def_ro("tail", &gil::ProcessorInfo::tail)
        .def_ro("has_editor", &gil::ProcessorInfo::hasEditor);

    nb::enum_<gil::ProcessorEvent::Type>(m, "ProcessorEventType")
        .value("PARAM_EDITED", gil::ProcessorEvent::Type::ParamEdited)
        .value("PARAMS_CHANGED", gil::ProcessorEvent::Type::ParamsChanged)
        .value("PARAM_INFO_CHANGED", gil::ProcessorEvent::Type::ParamInfoChanged)
        .value("EDITOR_CLOSED", gil::ProcessorEvent::Type::EditorClosed)
        .value("EDITOR_REQUESTED", gil::ProcessorEvent::Type::EditorRequested)
        .value("STATE_DIRTY", gil::ProcessorEvent::Type::StateDirty)
        .value("LATENCY_CHANGED", gil::ProcessorEvent::Type::LatencyChanged)
        .value("PARAM_TOUCHED", gil::ProcessorEvent::Type::ParamTouched);

    nb::class_<gil::ProcessorEventRecord>(m, "ProcessorEvent")
        .def_ro("processor_id", &gil::ProcessorEventRecord::processorId)
        .def_prop_ro("type", [](const gil::ProcessorEventRecord& e) { return e.type; })
        .def_prop_ro("param_index", [](const gil::ProcessorEventRecord& e) { return e.paramIndex; })
        .def_prop_ro("value", [](const gil::ProcessorEventRecord& e) { return e.value; })
        .def_prop_ro("old_value", [](const gil::ProcessorEventRecord& e) { return e.oldValue; })
        .def_prop_ro("gesture", [](const gil::ProcessorEventRecord& e) { return e.gesture; })
        .def("__repr__", [](const gil::ProcessorEventRecord& e) {
            return "ProcessorEvent(" + std::to_string(e.processorId) + ", type=" +
                   std::to_string(static_cast<int>(e.type)) + ", param=" + std::to_string(e.paramIndex) + ")";
        });

    nb::class_<gil::MeterReading>(m, "MeterReading")
        .def_ro("track_id", &gil::MeterReading::trackId)
        .def_ro("left", &gil::MeterReading::left)
        .def_ro("right", &gil::MeterReading::right);

    nb::enum_<gil::WarpMode>(m, "WarpMode")
        .value("TRANSIENTS", gil::WarpMode::Transients)
        .value("STANDARD", gil::WarpMode::Standard)
        .value("SMOOTH", gil::WarpMode::Smooth)
        .value("FORMANTS", gil::WarpMode::Formants)
        .value("RE_PITCH", gil::WarpMode::RePitch);

    nb::class_<gil::ClipDesc>(m, "ClipDesc")
        .def(
            "__init__",
            [](gil::ClipDesc* self, std::string path, double startBeat, double durationSec, double offsetSec,
               float gain, float pan, bool warp, double segmentBpm, gil::WarpMode warpMode, double transpose,
               std::string id) {
                new (self) gil::ClipDesc{std::move(path), startBeat, durationSec, offsetSec, gain, pan,
                                         warp, segmentBpm, warpMode, transpose, std::move(id)};
            },
            "path"_a, "start_beat"_a, "duration_sec"_a, "offset_sec"_a = 0.0, "gain"_a = 1.0f, nb::kw_only(),
            "pan"_a = 0.0f, "warp"_a = false, "segment_bpm"_a = 0.0, "warp_mode"_a = gil::WarpMode::Standard,
            "transpose"_a = 0.0, "id"_a = "")
        .def_rw("path", &gil::ClipDesc::path)
        .def_rw("start_beat", &gil::ClipDesc::startBeat)
        .def_rw("duration_sec", &gil::ClipDesc::durationSec)
        .def_rw("offset_sec", &gil::ClipDesc::offsetSec)
        .def_rw("gain", &gil::ClipDesc::gain)
        .def_rw("pan", &gil::ClipDesc::pan)
        .def_rw("warp", &gil::ClipDesc::warp)
        .def_rw("segment_bpm", &gil::ClipDesc::segmentBpm)
        .def_rw("warp_mode", &gil::ClipDesc::warpMode)
        .def_rw("transpose", &gil::ClipDesc::transpose)
        .def_rw("id", &gil::ClipDesc::id);

    nb::class_<gil::NoteDesc>(m, "NoteDesc")
        .def(
            "__init__",
            [](gil::NoteDesc* self, double startBeat, double lengthBeats, int key, int velocity) {
                new (self) gil::NoteDesc{startBeat, lengthBeats, key, velocity};
            },
            "start_beat"_a, "length_beats"_a, "key"_a, "velocity"_a = 100)
        .def_rw("start_beat", &gil::NoteDesc::startBeat)
        .def_rw("length_beats", &gil::NoteDesc::lengthBeats)
        .def_rw("key", &gil::NoteDesc::key)
        .def_rw("velocity", &gil::NoteDesc::velocity)
        .def("__repr__", [](const gil::NoteDesc& n) {
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
                gil::DeviceConfig config;
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
        // Sources
        .def("load_source", &Engine::loadSource, "path"_a, ReleaseGil(),
             "Decode an audio file at the engine sample rate (cached). Blocking; call from a worker thread.")
        .def("cached_source", &Engine::cachedSource, "path"_a)
        .def("release_unused_sources", &Engine::releaseUnusedSources)
        // Tracks
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
        .def("set_master_gain", &Engine::setMasterGain, "gain"_a)
        .def("set_master_pan", &Engine::setMasterPan, "pan"_a)
        .def("set_track_automation", &Engine::setTrackAutomation, "track_id"_a, "lanes"_a,
             "Replace a track's automation (track_id 0: the master's) with these AutomationLanes.")
        .def("take_meters", &Engine::takeMeters, "Peak levels since the last call; track_id 0 is the master.")
        // Insert chain
        .def("add_builtin_processor", &Engine::addBuiltinProcessor, "track_id"_a, "type"_a, "index"_a = -1)
        .def("add_plugin_processor", &Engine::addPluginProcessor, "track_id"_a, "format"_a, "path"_a, "uid"_a,
             "index"_a = -1, ReleaseGil(),
             "Load a plug-in into a track's chain (main thread). Raises RuntimeError if it can't be loaded.")
        .def("remove_processor", &Engine::removeProcessor, "processor_id"_a)
        .def("set_track_processor_order", &Engine::setTrackProcessorOrder, "track_id"_a, "processor_ids"_a)
        .def("processor_info", &Engine::processorInfo, "processor_id"_a)
        .def("processor_params", &Engine::processorParams, "processor_id"_a)
        .def("processor_param_index", &Engine::processorParamIndex, "processor_id"_a, "param_id"_a)
        .def("processor_param", &Engine::processorParam, "processor_id"_a, "index"_a)
        .def("set_processor_param", &Engine::setProcessorParam, "processor_id"_a, "index"_a, "value"_a)
        .def("processor_param_text", &Engine::processorParamText, "processor_id"_a, "index"_a, "value"_a,
             "The processor's own text for a value ('' if it has none).")
        .def("set_processor_enabled", &Engine::setProcessorEnabled, "processor_id"_a, "enabled"_a)
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
            "processor_id"_a, "A plug-in's settings (a .vstpreset); empty for built-in devices.")
        .def(
            "set_processor_state",
            [](Engine& self, uint32_t processorId, const nb::bytes& state) {
                const auto data = fromBytes(state);
                nb::gil_scoped_release release;
                self.setProcessorState(processorId, data);
            },
            "processor_id"_a, "state"_a)
        .def("open_editor", &Engine::openEditor, "processor_id"_a, "owner_window"_a = 0, "title"_a = "",
             ReleaseGil(), "Show a plug-in's editor window (or raise it). False if it has none.")
        .def("close_editor", &Engine::closeEditor, "processor_id"_a)
        .def("is_editor_open", &Engine::isEditorOpen, "processor_id"_a, "Open and not hidden.")
        .def("set_editor_visible", &Engine::setEditorVisible, "processor_id"_a, "visible"_a,
             "Hide an open editor window or show it again (without focusing it). False if none is open.")
        .def("set_editor_title", &Engine::setEditorTitle, "processor_id"_a, "title"_a)
        .def("take_processor_events", &Engine::takeProcessorEvents,
             "What processors reported since the last call (edits in a plug-in's own editor, ...).")
        // Transport
        .def("play", &Engine::play)
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
        .def("idle", &Engine::idle, ReleaseGil(), "Housekeeping; call periodically from the UI thread.");
}
