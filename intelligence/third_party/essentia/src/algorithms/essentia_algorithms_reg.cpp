// The algorithms SUBstation builds, registered with Essentia's factories.
//
// Upstream generates this file when configuring (src/wscript, buildRegFile();
// utils/algorithms_info.py, create_registration_cpp()). This is that output for
// the vendored subset, as configured with --fft=KISS --include-algos=...;
// written by ../../vendor.sh (see ../../VERSION.txt).

#include "algorithmfactory.h"
#include "algorithms/complex/magnitude.h"
#include "algorithms/sfx/logattacktime.h"
#include "algorithms/sfx/pitchsalience.h"
#include "algorithms/spectral/energyband.h"
#include "algorithms/spectral/flux.h"
#include "algorithms/spectral/hfc.h"
#include "algorithms/spectral/melbands.h"
#include "algorithms/spectral/mfcc.h"
#include "algorithms/spectral/rolloff.h"
#include "algorithms/spectral/spectralcomplexity.h"
#include "algorithms/spectral/spectralcontrast.h"
#include "algorithms/spectral/spectralpeaks.h"
#include "algorithms/spectral/triangularbands.h"
#include "algorithms/standard/autocorrelation.h"
#include "algorithms/standard/dct.h"
#include "algorithms/standard/fftk.h"
#include "algorithms/standard/ifftk.h"
#include "algorithms/standard/peakdetection.h"
#include "algorithms/standard/spectrum.h"
#include "algorithms/standard/windowing.h"
#include "algorithms/stats/centralmoments.h"
#include "algorithms/stats/centroid.h"
#include "algorithms/stats/crest.h"
#include "algorithms/stats/decrease.h"
#include "algorithms/stats/distributionshape.h"
#include "algorithms/stats/flatness.h"
#include "algorithms/stats/geometricmean.h"
#include "algorithms/temporal/effectiveduration.h"
#include "algorithms/temporal/zerocrossingrate.h"
#include "algorithms/tonal/dissonance.h"
#include "algorithms/tonal/pitchyin.h"

namespace essentia {
namespace standard {

ESSENTIA_API void registerAlgorithm() {
    AlgorithmFactory::Registrar<Magnitude> regMagnitude;
    AlgorithmFactory::Registrar<LogAttackTime> regLogAttackTime;
    AlgorithmFactory::Registrar<PitchSalience> regPitchSalience;
    AlgorithmFactory::Registrar<EnergyBand> regEnergyBand;
    AlgorithmFactory::Registrar<Flux> regFlux;
    AlgorithmFactory::Registrar<HFC> regHFC;
    AlgorithmFactory::Registrar<MelBands> regMelBands;
    AlgorithmFactory::Registrar<MFCC> regMFCC;
    AlgorithmFactory::Registrar<RollOff> regRollOff;
    AlgorithmFactory::Registrar<SpectralComplexity> regSpectralComplexity;
    AlgorithmFactory::Registrar<SpectralContrast> regSpectralContrast;
    AlgorithmFactory::Registrar<SpectralPeaks> regSpectralPeaks;
    AlgorithmFactory::Registrar<TriangularBands> regTriangularBands;
    AlgorithmFactory::Registrar<AutoCorrelation> regAutoCorrelation;
    AlgorithmFactory::Registrar<DCT> regDCT;
    AlgorithmFactory::Registrar<FFTK> regFFTK;
    AlgorithmFactory::Registrar<IFFTK> regIFFTK;
    AlgorithmFactory::Registrar<PeakDetection> regPeakDetection;
    AlgorithmFactory::Registrar<Spectrum> regSpectrum;
    AlgorithmFactory::Registrar<Windowing> regWindowing;
    AlgorithmFactory::Registrar<CentralMoments> regCentralMoments;
    AlgorithmFactory::Registrar<Centroid> regCentroid;
    AlgorithmFactory::Registrar<Crest> regCrest;
    AlgorithmFactory::Registrar<Decrease> regDecrease;
    AlgorithmFactory::Registrar<DistributionShape> regDistributionShape;
    AlgorithmFactory::Registrar<Flatness> regFlatness;
    AlgorithmFactory::Registrar<GeometricMean> regGeometricMean;
    AlgorithmFactory::Registrar<EffectiveDuration> regEffectiveDuration;
    AlgorithmFactory::Registrar<ZeroCrossingRate> regZeroCrossingRate;
    AlgorithmFactory::Registrar<Dissonance> regDissonance;
    AlgorithmFactory::Registrar<PitchYin> regPitchYin;
}}}


namespace essentia {
namespace streaming {

ESSENTIA_API void registerAlgorithm() {
    AlgorithmFactory::Registrar<Magnitude, essentia::standard::Magnitude> regMagnitude;
    AlgorithmFactory::Registrar<LogAttackTime, essentia::standard::LogAttackTime> regLogAttackTime;
    AlgorithmFactory::Registrar<PitchSalience, essentia::standard::PitchSalience> regPitchSalience;
    AlgorithmFactory::Registrar<EnergyBand, essentia::standard::EnergyBand> regEnergyBand;
    AlgorithmFactory::Registrar<Flux, essentia::standard::Flux> regFlux;
    AlgorithmFactory::Registrar<HFC, essentia::standard::HFC> regHFC;
    AlgorithmFactory::Registrar<MelBands, essentia::standard::MelBands> regMelBands;
    AlgorithmFactory::Registrar<MFCC, essentia::standard::MFCC> regMFCC;
    AlgorithmFactory::Registrar<RollOff, essentia::standard::RollOff> regRollOff;
    AlgorithmFactory::Registrar<SpectralComplexity, essentia::standard::SpectralComplexity> regSpectralComplexity;
    AlgorithmFactory::Registrar<SpectralContrast, essentia::standard::SpectralContrast> regSpectralContrast;
    AlgorithmFactory::Registrar<SpectralPeaks, essentia::standard::SpectralPeaks> regSpectralPeaks;
    AlgorithmFactory::Registrar<TriangularBands, essentia::standard::TriangularBands> regTriangularBands;
    AlgorithmFactory::Registrar<AutoCorrelation, essentia::standard::AutoCorrelation> regAutoCorrelation;
    AlgorithmFactory::Registrar<DCT, essentia::standard::DCT> regDCT;
    AlgorithmFactory::Registrar<FFTK, essentia::standard::FFTK> regFFTK;
    AlgorithmFactory::Registrar<IFFTK, essentia::standard::IFFTK> regIFFTK;
    AlgorithmFactory::Registrar<PeakDetection, essentia::standard::PeakDetection> regPeakDetection;
    AlgorithmFactory::Registrar<Spectrum, essentia::standard::Spectrum> regSpectrum;
    AlgorithmFactory::Registrar<Windowing, essentia::standard::Windowing> regWindowing;
    AlgorithmFactory::Registrar<CentralMoments, essentia::standard::CentralMoments> regCentralMoments;
    AlgorithmFactory::Registrar<Centroid, essentia::standard::Centroid> regCentroid;
    AlgorithmFactory::Registrar<Crest, essentia::standard::Crest> regCrest;
    AlgorithmFactory::Registrar<Decrease, essentia::standard::Decrease> regDecrease;
    AlgorithmFactory::Registrar<DistributionShape, essentia::standard::DistributionShape> regDistributionShape;
    AlgorithmFactory::Registrar<Flatness, essentia::standard::Flatness> regFlatness;
    AlgorithmFactory::Registrar<GeometricMean, essentia::standard::GeometricMean> regGeometricMean;
    AlgorithmFactory::Registrar<EffectiveDuration, essentia::standard::EffectiveDuration> regEffectiveDuration;
    AlgorithmFactory::Registrar<ZeroCrossingRate, essentia::standard::ZeroCrossingRate> regZeroCrossingRate;
    AlgorithmFactory::Registrar<Dissonance, essentia::standard::Dissonance> regDissonance;
    AlgorithmFactory::Registrar<PitchYin, essentia::standard::PitchYin> regPitchYin;
}}}
