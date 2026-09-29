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

namespace nb = nanobind;
using namespace nb::literals;

using gil::AudioSource;
using gil::Engine;

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

    nb::class_<gil::OutputDeviceInfo>(m, "OutputDeviceInfo")
        .def_ro("name", &gil::OutputDeviceInfo::name)
        .def_ro("is_default", &gil::OutputDeviceInfo::isDefault)
        .def("__repr__", [](const gil::OutputDeviceInfo& d) {
            return "OutputDeviceInfo('" + d.name + "'" + (d.isDefault ? ", default)" : ")");
        });

    nb::class_<gil::DeviceStatus>(m, "DeviceStatus")
        .def_ro("open", &gil::DeviceStatus::open)
        .def_ro("name", &gil::DeviceStatus::name)
        .def_ro("backend", &gil::DeviceStatus::backend)
        .def_ro("sample_rate", &gil::DeviceStatus::sampleRate)
        .def_ro("buffer_frames", &gil::DeviceStatus::bufferFrames)
        .def_ro("latency_ms", &gil::DeviceStatus::latencyMs)
        .def_ro("exclusive", &gil::DeviceStatus::exclusive);

    nb::class_<gil::ParamInfo>(m, "ParamInfo")
        .def_ro("id", &gil::ParamInfo::id)
        .def_ro("name", &gil::ParamInfo::name)
        .def_ro("unit", &gil::ParamInfo::unit)
        .def_ro("min_value", &gil::ParamInfo::minValue)
        .def_ro("max_value", &gil::ParamInfo::maxValue)
        .def_ro("default_value", &gil::ParamInfo::defaultValue);

    nb::class_<gil::MeterReading>(m, "MeterReading")
        .def_ro("track_id", &gil::MeterReading::trackId)
        .def_ro("left", &gil::MeterReading::left)
        .def_ro("right", &gil::MeterReading::right);

    nb::enum_<gil::WarpMode>(m, "WarpMode")
        .value("BEATS", gil::WarpMode::Beats)
        .value("TONES", gil::WarpMode::Tones)
        .value("TEXTURE", gil::WarpMode::Texture)
        .value("RE_PITCH", gil::WarpMode::RePitch)
        .value("COMPLEX", gil::WarpMode::Complex)
        .value("COMPLEX_PRO", gil::WarpMode::ComplexPro);

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
            "pan"_a = 0.0f, "warp"_a = false, "segment_bpm"_a = 0.0, "warp_mode"_a = gil::WarpMode::Beats,
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
        .def("list_output_devices", &Engine::outputDevices)
        .def("open_device", &Engine::openDevice, "name"_a = "", "sample_rate"_a = 0, "buffer_frames"_a = 512,
             "exclusive"_a = false, ReleaseGil(),
             "Open an output device (empty name = system default, sample_rate 0 = native).")
        .def("close_device", &Engine::closeDevice, ReleaseGil())
        .def_prop_ro("device_status", &Engine::deviceStatus)
        .def_prop_ro("sample_rate", &Engine::sampleRate)
        .def_prop_ro("cpu_load", &Engine::cpuLoad)
        .def("take_device_event", &Engine::takeDeviceEvent)
        // Sources
        .def("load_source", &Engine::loadSource, "path"_a, ReleaseGil(),
             "Decode an audio file at the engine sample rate (cached). Blocking; call from a worker thread.")
        .def("cached_source", &Engine::cachedSource, "path"_a)
        .def("release_unused_sources", &Engine::releaseUnusedSources)
        // Tracks
        .def("add_track", &Engine::addTrack)
        .def("remove_track", &Engine::removeTrack, "track_id"_a)
        .def("set_track_clips", &Engine::setTrackClips, "track_id"_a, "clips"_a)
        .def("set_track_gain", &Engine::setTrackGain, "track_id"_a, "gain"_a)
        .def("set_track_pan", &Engine::setTrackPan, "track_id"_a, "pan"_a)
        .def("set_track_mute", &Engine::setTrackMute, "track_id"_a, "mute"_a)
        .def("set_track_solo", &Engine::setTrackSolo, "track_id"_a, "solo"_a)
        .def("set_master_gain", &Engine::setMasterGain, "gain"_a)
        .def("take_meters", &Engine::takeMeters, "Peak levels since the last call; track_id 0 is the master.")
        // Insert chain
        .def("add_builtin_processor", &Engine::addBuiltinProcessor, "track_id"_a, "type"_a, "index"_a = -1)
        .def("remove_processor", &Engine::removeProcessor, "processor_id"_a)
        .def("processor_params", &Engine::processorParams, "processor_id"_a)
        .def("processor_param", &Engine::processorParam, "processor_id"_a, "index"_a)
        .def("set_processor_param", &Engine::setProcessorParam, "processor_id"_a, "index"_a, "value"_a)
        .def("set_processor_enabled", &Engine::setProcessorEnabled, "processor_id"_a, "enabled"_a)
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
        .def("idle", &Engine::idle, "Housekeeping; call periodically from the UI thread.");
}
