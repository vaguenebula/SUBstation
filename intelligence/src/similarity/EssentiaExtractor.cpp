#include "similarity/EssentiaExtractor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

// Essentia's headers, without their warnings (or the warnings its config.h
// turns off for MSVC) reaching the module's own code.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <essentia/algorithmfactory.h>
#include <essentia/essentia.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "core/AudioReader.h"
#include "similarity/Descriptors.h"

namespace sub::intelligence {

namespace {

using namespace descriptors;
namespace es = essentia::standard;
using essentia::Real;

constexpr double kRate = kAnalysisRate;
constexpr int kFrame = 1024;  // 23 ms
constexpr int kHop = kFrame / 4;
constexpr int kBins = kFrame / 2 + 1;
constexpr double kMinHz = 20.0;
constexpr double kMaxHz = 16000.0;
constexpr int kMelBands = 40;
constexpr double kSubBassHz = 120.0;
constexpr double kAirHz = 8000.0;
constexpr double kRolloff = 0.85;
constexpr double kPowerFloor = 1e-12;   // (no bin quite 0: flatness's geometric mean stays finite)
constexpr double kMelFloor = 1e-10;     // MFCC's silence threshold (-100 dB)
constexpr double kPeakFloor = 1e-3;     // spectral peaks within 60 dB of the frame's loudest bin
constexpr double kFrameRangeDb = 50.0;  // frames within this of the loudest are weighed in, louder ones more
constexpr double kAttackSeconds = 0.03;
constexpr double kBodySeconds = 0.25;
constexpr double kDurationDb = -30.0;
// Pitch: YIN (PitchYin) on the sound brought down to 11.025 kHz, 93 ms windows
// (down to 21.5 Hz: an 808's fundamental), at most kPitchFrames of them from
// the peak on, one after another (370 ms). (Not PitchYinFFT: its loudness
// weighting leaves nothing of a 40 Hz fundamental to find.)
constexpr uint32_t kPitchRate = kAnalysisRate / 4;
constexpr int kPitchFrame = 1024;
constexpr double kMinPitchHz = 21.5;
constexpr double kMaxPitchHz = 2000.0;
constexpr double kYinThreshold = 0.15;
constexpr int kPitchFrames = 4;
constexpr double kPitchRangeDb = 40.0;
constexpr double kVoicedConfidence = 0.7;  // (aperiodicity under 0.3)
constexpr double kPitchReferenceHz = 220.0;
constexpr size_t kCancelEvery = 32;  // frames between looks at the cancel flag
// PitchSalience autocorrelates the spectrum through FFTs of four times its
// size: it is summed up over every fourth frame (a 93 ms stride) only.
constexpr size_t kSalienceEvery = 4;
static_assert(kContourPoints == kDecayPoints);

std::once_flag essentiaReady;

using Algorithm = std::unique_ptr<es::Algorithm>;

struct Frame {
    double db = 0.0;
    double centroid = 0.0, bandwidth = 0.0, rolloff = 0.0, flatness = 0.0, sub = 0.0, air = 0.0;
    std::array<float, kMelBands> mel{};  // dB
    std::array<float, kMfccs> mfcc{};    // 1-12
    std::array<float, kContrastBands> contrast{}, valley{};
    double skewness = 0.0, kurtosis = 0.0, crest = 0.0, hfc = 0.0, zcr = 0.0, flux = 0.0, decrease = 0.0;
    double complexity = 0.0, dissonance = 0.0, salience = 0.0;
    bool hasSalience = false;
    size_t start = 0;  // its first sample
};

bool cancelled(const CancelFlag* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

}  // namespace

struct EssentiaExtractor::State {
    // The analysis band, [lo, hi] in bins: 20 Hz to 16 kHz.
    const double binHz = kRate / kFrame;
    const int lo = static_cast<int>(std::ceil(kMinHz / binHz));
    const int hi = std::min(kFrame / 2, static_cast<int>(std::floor(kMaxHz / binHz)));
    const int bandBins = hi - lo + 1;

    // A frame's algorithms and what they read and write (bound once).
    Algorithm windowing, spectrum, mfcc, centroid, moments, shape, rolloff, flatness, subBass, air, total;
    Algorithm contrast, crest, hfc, zcr, flux, decrease, peaks, complexity, dissonance, salience;
    std::vector<Real> frame, windowed, magnitude, bandMagnitude, bandPower, melBands, mfccs, centralMoments;
    std::vector<Real> unitMagnitude, contrasts, valleys, relative, relativeBand, peakHz, peakMagnitudes;
    Real centroidHz = 0, variance = 0, skewness = 0, kurtosis = 0, rolloffHz = 0, flatnessRatio = 0;
    Real subEnergy = 0, airEnergy = 0, totalEnergy = 0;
    Real crestRatio = 0, hfcValue = 0, zcrRatio = 0, fluxValue = 0, decreaseSlope = 0, complexityCount = 0,
         dissonanceValue = 0, salienceValue = 0;

    // The envelope's.
    Algorithm attack, duration, temporalCentroid;
    std::vector<Real> amplitude, energy;
    Real logAttack = 0, attackStart = 0, attackStop = 0, effectiveDuration = 0, centreIndex = 0;

    // The pitch's.
    Algorithm pitchYin;
    std::vector<float> pitchSignal;
    std::vector<Real> pitchFrame;
    Real pitchHz = 0, pitchConfidence = 0;

    std::vector<float> clean, resampled;
    Prepared prepared;
    Envelope envelope;
    std::vector<Frame> frames;

    State() {
        std::call_once(essentiaReady, [] { essentia::init(); });
        const double envelopeRate = kRate / std::lround(kBlockSeconds * kRate);  // blocks a second
        const Real bandRange = static_cast<Real>((bandBins - 1) * binHz);

        windowing.reset(es::AlgorithmFactory::create("Windowing", "type", "hann", "size", kFrame, "zeroPhase", false,
                                                     "normalized", false));
        spectrum.reset(es::AlgorithmFactory::create("Spectrum", "size", kFrame));
        mfcc.reset(es::AlgorithmFactory::create("MFCC", "inputSize", kBins, "sampleRate", kRate, "numberBands", kMelBands,
                                                "numberCoefficients", kMfccs + 1, "lowFrequencyBound", kMinHz,
                                                "highFrequencyBound", kMaxHz, "type", "power", "logType", "dbpow",
                                                "silenceThreshold", kMelFloor));
        // Over the analysis band: its first bin is index 0, its last bandRange Hz above it.
        centroid.reset(es::AlgorithmFactory::create("Centroid", "range", bandRange));
        moments.reset(es::AlgorithmFactory::create("CentralMoments", "mode", "pdf", "range", bandRange));
        shape.reset(es::AlgorithmFactory::create("DistributionShape"));
        rolloff.reset(es::AlgorithmFactory::create("RollOff", "cutoff", kRolloff, "sampleRate", 2.0 * bandRange));
        flatness.reset(es::AlgorithmFactory::create("Flatness"));
        subBass.reset(es::AlgorithmFactory::create("EnergyBand", "startCutoffFrequency", kMinHz, "stopCutoffFrequency",
                                                   kSubBassHz, "sampleRate", kRate));
        air.reset(es::AlgorithmFactory::create("EnergyBand", "startCutoffFrequency", kAirHz, "stopCutoffFrequency", kMaxHz,
                                               "sampleRate", kRate));
        total.reset(es::AlgorithmFactory::create("EnergyBand", "startCutoffFrequency", kMinHz, "stopCutoffFrequency",
                                                 kMaxHz, "sampleRate", kRate));
        contrast.reset(es::AlgorithmFactory::create("SpectralContrast", "frameSize", kFrame, "sampleRate", kRate,
                                                    "numberBands", kContrastBands, "lowFrequencyBound", kMinHz,
                                                    "highFrequencyBound", kMaxHz));
        crest.reset(es::AlgorithmFactory::create("Crest"));
        hfc.reset(es::AlgorithmFactory::create("HFC", "sampleRate", kRate));
        zcr.reset(es::AlgorithmFactory::create("ZeroCrossingRate"));
        flux.reset(es::AlgorithmFactory::create("Flux", "norm", "L2", "halfRectify", true));
        decrease.reset(es::AlgorithmFactory::create("Decrease", "range", 1.0));
        // Peaks of the spectrum relative to its loudest bin (so the floor is a level below it).
        peaks.reset(es::AlgorithmFactory::create("SpectralPeaks", "sampleRate", kRate, "maxPeaks", 100, "minFrequency",
                                                 kMinHz, "maxFrequency", kMaxHz, "magnitudeThreshold", kPeakFloor,
                                                 "orderBy", "frequency"));
        complexity.reset(es::AlgorithmFactory::create("SpectralComplexity", "sampleRate", kRate, "magnitudeThreshold",
                                                      kPeakFloor));
        dissonance.reset(es::AlgorithmFactory::create("Dissonance"));
        salience.reset(es::AlgorithmFactory::create("PitchSalience", "sampleRate", kRate, "lowBoundary", 200.0,
                                                    "highBoundary", 5000.0));

        attack.reset(es::AlgorithmFactory::create("LogAttackTime", "sampleRate", envelopeRate, "startAttackThreshold", 0.2,
                                                  "stopAttackThreshold", 0.9));
        duration.reset(es::AlgorithmFactory::create("EffectiveDuration", "sampleRate", envelopeRate, "thresholdRatio",
                                                    std::pow(10.0, kDurationDb / 20.0)));
        temporalCentroid.reset(es::AlgorithmFactory::create("Centroid", "range", 1.0));  // (in blocks, scaled below)

        pitchYin.reset(es::AlgorithmFactory::create("PitchYin", "frameSize", kPitchFrame, "sampleRate",
                                                    static_cast<double>(kPitchRate), "minFrequency", kMinPitchHz,
                                                    "maxFrequency", kMaxPitchHz, "tolerance", kYinThreshold));

        frame.resize(kFrame);
        bandMagnitude.resize(bandBins);
        bandPower.resize(bandBins);
        relative.resize(kBins);
        unitMagnitude.resize(kBins);
        relativeBand.resize(bandBins);
        pitchFrame.resize(kPitchFrame);
        windowing->input("frame").set(frame);
        windowing->output("frame").set(windowed);
        spectrum->input("frame").set(windowed);
        spectrum->output("spectrum").set(magnitude);
        mfcc->input("spectrum").set(magnitude);
        mfcc->output("bands").set(melBands);
        mfcc->output("mfcc").set(mfccs);
        centroid->input("array").set(bandPower);
        centroid->output("centroid").set(centroidHz);
        moments->input("array").set(bandPower);
        moments->output("centralMoments").set(centralMoments);
        shape->input("centralMoments").set(centralMoments);
        shape->output("spread").set(variance);
        shape->output("skewness").set(skewness);
        shape->output("kurtosis").set(kurtosis);
        rolloff->input("spectrum").set(bandMagnitude);
        rolloff->output("rollOff").set(rolloffHz);
        flatness->input("array").set(bandPower);
        flatness->output("flatness").set(flatnessRatio);
        subBass->input("spectrum").set(magnitude);
        subBass->output("energyBand").set(subEnergy);
        air->input("spectrum").set(magnitude);
        air->output("energyBand").set(airEnergy);
        total->input("spectrum").set(magnitude);
        total->output("energyBand").set(totalEnergy);
        contrast->input("spectrum").set(unitMagnitude);
        contrast->output("spectralContrast").set(contrasts);
        contrast->output("spectralValley").set(valleys);
        crest->input("array").set(bandPower);
        crest->output("crest").set(crestRatio);
        hfc->input("spectrum").set(magnitude);
        hfc->output("hfc").set(hfcValue);
        zcr->input("signal").set(frame);
        zcr->output("zeroCrossingRate").set(zcrRatio);
        flux->input("spectrum").set(relative);
        flux->output("flux").set(fluxValue);
        decrease->input("array").set(relativeBand);
        decrease->output("decrease").set(decreaseSlope);
        peaks->input("spectrum").set(relative);
        peaks->output("frequencies").set(peakHz);
        peaks->output("magnitudes").set(peakMagnitudes);
        complexity->input("spectrum").set(relative);
        complexity->output("spectralComplexity").set(complexityCount);
        dissonance->input("frequencies").set(peakHz);
        dissonance->input("magnitudes").set(peakMagnitudes);
        dissonance->output("dissonance").set(dissonanceValue);
        salience->input("spectrum").set(magnitude);
        salience->output("pitchSalience").set(salienceValue);
        attack->input("signal").set(amplitude);
        attack->output("logAttackTime").set(logAttack);
        attack->output("attackStart").set(attackStart);
        attack->output("attackStop").set(attackStop);
        duration->input("signal").set(amplitude);
        duration->output("effectiveDuration").set(effectiveDuration);
        temporalCentroid->input("array").set(energy);
        temporalCentroid->output("centroid").set(centreIndex);
        pitchYin->input("signal").set(pitchFrame);
        pitchYin->output("pitch").set(pitchHz);
        pitchYin->output("pitchConfidence").set(pitchConfidence);
    }

    // One frame's descriptors, from frame (filled); its pitch salience if `salient`.
    void analyseFrame(Frame& fr, bool salient) {
        windowing->compute();
        spectrum->compute();
        double power = 0.0;
        for (int k = 0; k < bandBins; ++k) {
            const Real m = magnitude[static_cast<size_t>(lo + k)];
            bandMagnitude[static_cast<size_t>(k)] = m;
            bandPower[static_cast<size_t>(k)] = m * m + static_cast<Real>(kPowerFloor);
            power += static_cast<double>(m) * m;
        }
        fr.db = dB(power);
        centroid->compute();
        moments->compute();
        shape->compute();
        rolloff->compute();
        flatness->compute();
        subBass->compute();
        air->compute();
        total->compute();
        mfcc->compute();
        // SpectralContrast raises each band's peak/valley to 1 / ln(the band's
        // mean): it wants magnitudes below 1, as a sine at full scale has with a
        // window summing to 2 (Essentia's own extractors normalise theirs), or
        // it blows up where a band's mean comes near 1.
        constexpr Real kUnit = Real(2) / (kFrame / 2);  // (the Hann window sums to kFrame / 2)
        for (size_t k = 0; k < magnitude.size(); ++k) unitMagnitude[k] = magnitude[k] * kUnit;
        contrast->compute();
        crest->compute();
        hfc->compute();
        zcr->compute();
        // The spectrum relative to its loudest bin: what the peaks, the flux and
        // the decrease read (level-free).
        const Real top = *std::max_element(magnitude.begin(), magnitude.end());
        const Real scale = top > 0 ? 1 / top : 0;
        for (size_t k = 0; k < magnitude.size(); ++k) relative[k] = magnitude[k] * scale;
        std::copy(relative.begin() + lo, relative.begin() + hi + 1, relativeBand.begin());
        flux->compute();
        decrease->compute();
        peaks->compute();
        complexity->compute();
        dissonance->compute();
        if (salient) salience->compute();

        const double bandStart = lo * binHz;
        fr.centroid = std::log2(std::max(bandStart + centroidHz, kMinHz) / 1000.0);
        fr.bandwidth = std::log2(std::max(std::sqrt(std::max<double>(variance, 0.0)), 10.0) / 1000.0);
        fr.rolloff = std::log2(std::max(bandStart + rolloffHz, kMinHz) / 1000.0);
        fr.flatness = flatnessRatio > 0 ? std::clamp(10.0 * std::log10(static_cast<double>(flatnessRatio)), -60.0, 0.0) : -60.0;
        fr.sub = totalEnergy > 0 ? subEnergy / totalEnergy : 0.0;
        fr.air = totalEnergy > 0 ? airEnergy / totalEnergy : 0.0;
        for (int b = 0; b < kMelBands; ++b) fr.mel[b] = static_cast<float>(dB(melBands[static_cast<size_t>(b)], kMelFloor));
        for (int j = 0; j < kMfccs; ++j) fr.mfcc[j] = mfccs[static_cast<size_t>(j + 1)];  // (not MFCC 0: the level)
        for (size_t b = 0; b < static_cast<size_t>(kContrastBands) && b < contrasts.size() && b < valleys.size(); ++b) {
            fr.contrast[b] = contrasts[b];
            fr.valley[b] = valleys[b];
        }
        fr.skewness = std::copysign(std::log1p(std::fabs(static_cast<double>(skewness))), static_cast<double>(skewness));
        fr.kurtosis = std::log(std::max(static_cast<double>(kurtosis) + 3.0, 1e-3));  // (of the plain fourth moment)
        fr.crest = std::log10(std::max<double>(crestRatio, 1.0));
        fr.hfc = power > 0 ? std::log2(std::max(1.0, static_cast<double>(hfcValue) / power) / 1000.0) : 0.0;
        fr.zcr = std::log2(1.0 + zcrRatio * kRate);
        fr.flux = fluxValue;
        fr.decrease = decreaseSlope;
        fr.complexity = std::log2(1.0 + complexityCount);
        fr.dissonance = dissonanceValue;
        fr.hasSalience = salient;
        fr.salience = salient ? salienceValue : 0.0;
    }

    Extraction analyse(const float* samples, size_t count, double fileSeconds, bool truncated, float* fp,
                       const CancelFlag* cancel);
};

Extraction EssentiaExtractor::State::analyse(const float* samples, size_t count, double fileSeconds, bool truncated,
                                             float* fp, const CancelFlag* cancel) {
    // --- Level: relative to the peak; where the sound starts ------------------------------
    if (!prepare(samples, count, kRate, static_cast<size_t>(kAnalysisSeconds * kRate), fileSeconds, truncated, prepared))
        return Extraction::Silent;  // (below -100 dBFS)
    const float* x = prepared.signal.data();
    const size_t length = prepared.length;
    std::fill(fp, fp + kDims, 0.f);

    // --- The envelope, in 2 ms blocks -------------------------------------------------------
    descriptors::envelope(x, length, kRate, envelope);
    const size_t blocks = envelope.blocks.size();
    amplitude.resize(blocks);
    energy.resize(blocks);
    for (size_t b = 0; b < blocks; ++b) {
        energy[b] = static_cast<Real>(envelope.blocks[b]);
        amplitude[b] = static_cast<Real>(std::sqrt(envelope.blocks[b]));
    }
    attack->compute();
    // (Essentia's says -5 for an attack inside one block; half a block it is, as
    // the attack time between two blocks is up to one: no jump at 2 ms.)
    fp[feature::AttackTime] = static_cast<float>(
        std::log10(std::max<double>(attackStop - attackStart, 0.0) + 0.5 * envelope.blockSeconds));
    duration->compute();
    fp[feature::Duration] = static_cast<float>(std::log10(std::max<double>(effectiveDuration, envelope.blockSeconds)));
    double centre = 0.0;  // (in blocks)
    if (blocks >= 2) {
        temporalCentroid->compute();
        centre = static_cast<double>(centreIndex) * static_cast<double>(blocks - 1);
    }
    fp[feature::TemporalCentroid] = static_cast<float>(std::log10((std::max(centre, 0.0) + 0.5) * envelope.blockSeconds));
    decayContour(envelope, prepared.cut, fp + feature::Contour);

    // --- The spectrum, frame by frame -------------------------------------------------------
    const size_t frameCount = length <= kFrame ? 1 : 1 + (length - kFrame + kHop - 1) / kHop;
    frames.resize(frameCount);
    flux->reset();  // (each sound's first frame is its own)
    double loudest = -1e9;
    for (size_t t = 0; t < frameCount; ++t) {
        if (t % kCancelEvery == 0 && cancelled(cancel)) return Extraction::Cancelled;
        Frame& fr = frames[t];
        fr.start = t * kHop;
        for (size_t i = 0; i < static_cast<size_t>(kFrame); ++i) {
            const size_t at = fr.start + i;
            frame[i] = at < length ? x[at] : 0.f;
        }
        analyseFrame(fr, t % kSalienceEvery == 0);
        loudest = std::max(loudest, fr.db);
    }

    // Louder frames count more; those more than kFrameRangeDb down, not at all.
    std::array<Weighted, kMfccs> mfccMean{};
    std::array<std::array<Weighted, kPartMfccs>, kParts> parts{};
    std::array<Weighted, kContrastBands> contrastMean{}, valleyMean{};
    Weighted centroidMean, bandwidthMean, rolloffMean, flatnessMean, subMean, airMean, attackCentroid, attackFlatness;
    Weighted skewnessMean, kurtosisMean, crestMean, hfcMean, zcrMean, fluxMean, decreaseMean, complexityMean,
        dissonanceMean, salienceMean;
    const auto attackEnd = static_cast<size_t>(kAttackSeconds * kRate);
    const auto bodyEnd = static_cast<size_t>(kBodySeconds * kRate);
    for (const Frame& fr : frames) {
        const double w = std::clamp((fr.db - (loudest - kFrameRangeDb)) / kFrameRangeDb, 0.0, 1.0);
        if (w <= 0.0) continue;
        const int part = fr.start < attackEnd ? 0 : fr.start < bodyEnd ? 1 : 2;
        for (int j = 0; j < kMfccs; ++j) {
            mfccMean[j].add(fr.mfcc[j], w);
            if (j < kPartMfccs) parts[part][j].add(fr.mfcc[j], w);
        }
        for (int b = 0; b < kContrastBands; ++b) {
            contrastMean[b].add(fr.contrast[b], w);
            valleyMean[b].add(fr.valley[b], w);
        }
        centroidMean.add(fr.centroid, w);
        bandwidthMean.add(fr.bandwidth, w);
        rolloffMean.add(fr.rolloff, w);
        flatnessMean.add(fr.flatness, w);
        subMean.add(fr.sub, w);
        airMean.add(fr.air, w);
        if (fr.start <= attackEnd || attackCentroid.sw <= 0.0) {
            attackCentroid.add(fr.centroid, w);
            attackFlatness.add(fr.flatness, w);
        }
        skewnessMean.add(fr.skewness, w);
        kurtosisMean.add(fr.kurtosis, w);
        crestMean.add(fr.crest, w);
        hfcMean.add(fr.hfc, w);
        zcrMean.add(fr.zcr, w);
        fluxMean.add(fr.flux, w);
        decreaseMean.add(fr.decrease, w);
        complexityMean.add(fr.complexity, w);
        dissonanceMean.add(fr.dissonance, w);
        if (fr.hasSalience) salienceMean.add(fr.salience, w);
    }
    for (int j = 0; j < kMfccs; ++j) {
        fp[feature::MfccMean + j] = static_cast<float>(mfccMean[j].mean());
        fp[feature::MfccSpread + j] = static_cast<float>(mfccMean[j].sd());
    }
    // The timbre of the attack, the body and the tail; a part the sound doesn't
    // reach has the one before it.
    for (int part = 0; part < kParts; ++part) {
        for (int j = 0; j < kPartMfccs; ++j) {
            const Weighted& here = parts[part][j];
            const double value = here.sw > 0.0 ? here.mean()
                                 : part > 0  ? fp[feature::PartMfcc + (part - 1) * kPartMfccs + j]
                                             : mfccMean[j].mean();
            fp[feature::PartMfcc + part * kPartMfccs + j] = static_cast<float>(value);
        }
    }
    fp[feature::Centroid] = static_cast<float>(centroidMean.mean());
    fp[feature::CentroidSpread] = static_cast<float>(centroidMean.sd());
    fp[feature::Bandwidth] = static_cast<float>(bandwidthMean.mean());
    fp[feature::Rolloff] = static_cast<float>(rolloffMean.mean());
    fp[feature::Flatness] = static_cast<float>(flatnessMean.mean());
    fp[feature::SubBass] = static_cast<float>(dB(subMean.mean(), 1e-4));
    fp[feature::Air] = static_cast<float>(dB(airMean.mean(), 1e-4));
    fp[feature::AttackCentroid] = static_cast<float>(attackCentroid.mean());
    fp[feature::AttackFlatness] = static_cast<float>(attackFlatness.mean());
    for (int b = 0; b < kContrastBands; ++b) {
        fp[feature::Contrast + b] = static_cast<float>(contrastMean[b].mean());
        fp[feature::Valley + b] = static_cast<float>(valleyMean[b].mean());
    }
    fp[feature::Skewness] = static_cast<float>(skewnessMean.mean());
    fp[feature::Kurtosis] = static_cast<float>(kurtosisMean.mean());
    fp[feature::Crest] = static_cast<float>(crestMean.mean());
    fp[feature::Hfc] = static_cast<float>(hfcMean.mean());
    fp[feature::ZeroCrossings] = static_cast<float>(zcrMean.mean());
    fp[feature::Flux] = static_cast<float>(fluxMean.mean());
    fp[feature::Decrease] = static_cast<float>(decreaseMean.mean());
    fp[feature::Complexity] = static_cast<float>(complexityMean.mean());
    fp[feature::Dissonance] = static_cast<float>(dissonanceMean.mean());
    fp[feature::Salience] = static_cast<float>(salienceMean.mean());

    // --- Onsets after the first ---------------------------------------------------------------
    const size_t onsets = countOnsets(
        frameCount, kMelBands, [this](size_t t, int b) { return frames[t].mel[b]; },
        [this](size_t t) { return frames[t].db; }, loudest, kHop / kRate);
    fp[feature::OnsetRate] =
        static_cast<float>(std::log2(1.0 + static_cast<double>(onsets) / std::max(prepared.seconds, 0.05)));
    fp[feature::Length] = static_cast<float>(std::log10(std::clamp(prepared.fileSeconds, 0.01, 60.0)));

    // --- Pitch, from the peak on --------------------------------------------------------------
    {
        // What the windows cover, brought down to kPitchRate: each 4 samples'
        // mean (YIN looks for a period below 2 kHz; what folds down from above
        // 5.5 kHz is weak, and only makes a window a little less periodic).
        const size_t from = envelope.peakBlock * envelope.blockLength;
        const size_t factor = kAnalysisRate / kPitchRate;
        const size_t span =
            std::min(length - std::min(from, length), static_cast<size_t>(kPitchFrames * kPitchFrame) * factor);
        pitchSignal.resize(span / factor);
        for (size_t i = 0; i < pitchSignal.size(); ++i) {
            float sum = 0.f;
            for (size_t j = 0; j < factor; ++j) sum += x[from + i * factor + j];
            pitchSignal[i] = sum / static_cast<float>(factor);
        }
        std::vector<PitchEstimate> voiced;
        double totalPitchEnergy = 0.0, maxEnergy = 0.0;
        for (int i = 0; i < kPitchFrames; ++i) {
            if (cancelled(cancel)) return Extraction::Cancelled;
            const size_t start = static_cast<size_t>(i) * kPitchFrame;
            if (start >= pitchSignal.size()) break;
            double e = 0.0;
            for (size_t j = 0; j < static_cast<size_t>(kPitchFrame); ++j) {
                pitchFrame[j] = start + j < pitchSignal.size() ? pitchSignal[start + j] : 0.f;
                e += static_cast<double>(pitchFrame[j]) * pitchFrame[j];
            }
            maxEnergy = std::max(maxEnergy, e);
            if (e < maxEnergy * std::pow(10.0, -kPitchRangeDb / 10.0) || e <= 0.0) break;
            totalPitchEnergy += e;
            pitchYin->compute();
            if (pitchConfidence >= kVoicedConfidence && pitchHz > 0)
                voiced.push_back({std::log2(pitchHz / kPitchReferenceHz), 1.0 - static_cast<double>(pitchConfidence), e});
        }
        pitchSummary(voiced, totalPitchEnergy, fp[feature::PitchConfidence], fp[feature::Pitch]);
    }

    for (size_t d = 0; d < kDims; ++d)
        if (!std::isfinite(fp[d])) fp[d] = 0.f;
    return Extraction::Done;
}

EssentiaExtractor::EssentiaExtractor() : state_(std::make_unique<State>()) {}

EssentiaExtractor::~EssentiaExtractor() = default;

MonoAudio EssentiaExtractor::decode(const std::string& path, double start, double seconds) {
    return readMonoAt(path, start, seconds, kAnalysisRate);
}

Extraction EssentiaExtractor::extract(const SoundBuffer& sound, float* out, const CancelFlag* cancel) {
    if (cancelled(cancel)) return Extraction::Cancelled;
    if (!sound.samples || sound.count == 0) return Extraction::Silent;
    if (sound.sampleRate < 1000) throw AudioError("unsupported sample rate");
    State& s = *state_;
    const double rate = sound.sampleRate;
    const double fileSeconds = sound.fileSeconds > 0.0 ? sound.fileSeconds : static_cast<double>(sound.count) / rate;
    // As much as a file's analysis decodes (the lead-in and what is analysed).
    const size_t count = std::min(sound.count, static_cast<size_t>((kLeadInSeconds + kAnalysisSeconds) * rate));
    const bool truncated = sound.truncated || count < sound.count;
    const float* samples = sound.samples;
    // Broken samples (NaN, infinity) are silence: they would poison every feature.
    if (!std::all_of(samples, samples + count, [](float v) { return std::isfinite(v); })) {
        s.clean.assign(samples, samples + count);
        for (float& v : s.clean)
            if (!std::isfinite(v)) v = 0.f;
        samples = s.clean.data();
    }
    size_t analysed = count;
    if (sound.sampleRate != kAnalysisRate) {
        s.resampled = resampleMono(samples, count, sound.sampleRate, kAnalysisRate);
        samples = s.resampled.data();
        analysed = s.resampled.size();
        if (analysed == 0) return Extraction::Silent;
    }
    try {
        return s.analyse(samples, analysed, fileSeconds, truncated, out, cancel);
    } catch (const essentia::EssentiaException& e) {
        throw AudioError(std::string("cannot analyse: ") + e.what());
    }
}

const std::vector<FeatureInfo>& featureInfo() {
    static const std::vector<FeatureInfo> info = [] {
        std::vector<FeatureInfo> a(kDims);
        static const char* const partNames[kParts] = {"attack", "body", "tail"};
        static const char* const contour[kContourPoints] = {"level20ms",  "level40ms",  "level80ms",  "level160ms",
                                                            "level320ms", "level640ms", "level1280ms", "level2560ms"};
        for (int j = 0; j < kMfccs; ++j) {
            a[feature::MfccMean + j] = {"mfcc" + std::to_string(j + 1), Aspect::Timbre, 1.0f};
            a[feature::MfccSpread + j] = {"mfccSd" + std::to_string(j + 1), Aspect::TimbreSpread, 0.25f};
        }
        for (int part = 0; part < kParts; ++part)
            for (int j = 0; j < kPartMfccs; ++j)
                a[feature::PartMfcc + part * kPartMfccs + j] = {std::string(partNames[part]) + "Mfcc" + std::to_string(j + 1),
                                                                Aspect::TimbreMotion, 1.0f};
        a[feature::Centroid] = {"centroid", Aspect::Spectrum, 0.1f};
        a[feature::CentroidSpread] = {"centroidSd", Aspect::Spectrum, 0.05f};
        a[feature::Bandwidth] = {"bandwidth", Aspect::Spectrum, 0.1f};
        a[feature::Rolloff] = {"rolloff", Aspect::Spectrum, 0.1f};
        a[feature::Flatness] = {"flatness", Aspect::Spectrum, 1.0f};
        a[feature::SubBass] = {"subBass", Aspect::Spectrum, 1.0f};
        a[feature::Air] = {"air", Aspect::Spectrum, 1.0f};
        a[feature::AttackCentroid] = {"attackCentroid", Aspect::Spectrum, 0.1f};
        a[feature::AttackFlatness] = {"attackFlatness", Aspect::Spectrum, 1.0f};
        a[feature::AttackTime] = {"attackTime", Aspect::Envelope, 0.05f};
        a[feature::Duration] = {"duration", Aspect::Envelope, 0.05f};
        a[feature::TemporalCentroid] = {"temporalCentroid", Aspect::Envelope, 0.05f};
        for (int i = 0; i < kContourPoints; ++i) a[feature::Contour + i] = {contour[i], Aspect::Envelope, 1.0f};
        a[feature::PitchConfidence] = {"pitchConfidence", Aspect::Pitch, 0.05f};
        a[feature::Pitch] = {"pitch", Aspect::Pitch, 0.05f};
        a[feature::OnsetRate] = {"onsetRate", Aspect::Rhythm, 0.1f};
        a[feature::Length] = {"length", Aspect::Rhythm, 0.05f};
        for (int b = 0; b < kContrastBands; ++b) {
            a[feature::Contrast + b] = {"contrast" + std::to_string(b + 1), Aspect::Contrast, 0.01f};
            a[feature::Valley + b] = {"valley" + std::to_string(b + 1), Aspect::Contrast, 0.15f};
        }
        a[feature::Skewness] = {"skewness", Aspect::SpectralShape, 0.1f};
        a[feature::Kurtosis] = {"kurtosis", Aspect::SpectralShape, 0.15f};
        a[feature::Crest] = {"crest", Aspect::SpectralShape, 0.03f};
        a[feature::Hfc] = {"hfc", Aspect::SpectralShape, 0.15f};
        a[feature::ZeroCrossings] = {"zeroCrossings", Aspect::SpectralShape, 0.15f};
        a[feature::Flux] = {"flux", Aspect::SpectralShape, 0.05f};
        a[feature::Decrease] = {"decrease", Aspect::SpectralShape, 0.007f};
        a[feature::Complexity] = {"complexity", Aspect::Tonality, 0.1f};
        a[feature::Dissonance] = {"dissonance", Aspect::Tonality, 0.01f};
        a[feature::Salience] = {"salience", Aspect::Tonality, 0.01f};
        return a;
    }();
    return info;
}

const FeatureSchema& essentiaSchema() {
    static const FeatureSchema schema = [] {
        FeatureSchema s;
        s.extractor = "essentia";
        s.version = kFeatureVersion;
        s.settings = std::string("essentia=") + ESSENTIA_VERSION +
                     " rate=44100 frame=1024 hop=256 window=hann mel=htk40:20-16000:power:dbpow:dct2"
                     " band=20-16000 rolloff=0.85 contrast=6:20-16000 peaks=100:-60dB salience=200-5000/4"
                     " pitch=yin1024x4@11025(mean4):21.5-2000:0.15 envelope=2ms analysed=6s";
        s.features = featureInfo();
        // Timbre, TimbreMotion, Spectrum, Envelope, Pitch, Rhythm: as tuned on a
        // 5000-file sample library; TimbreSpread, Contrast, SpectralShape,
        // Tonality: 1 each, as measured on Dirt-Samples, held out
        // (docs/intelligence.md, sound_similarity_bench). No embedding yet.
        s.weights.weight = {0.5f, 1.5f, 2.5f, 2.0f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 1.f, 0.f};
        return s;
    }();
    return schema;
}

}  // namespace sub::intelligence
