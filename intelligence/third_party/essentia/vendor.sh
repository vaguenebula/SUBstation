#!/usr/bin/env bash
# Vendors the subset of Essentia that SUBstation builds (see VERSION.txt), from
# a checkout of the pinned release:
#
#   git clone --depth 1 --branch v2.1_beta5 https://github.com/MTG/essentia.git /tmp/essentia
#   intelligence/third_party/essentia/vendor.sh /tmp/essentia
#
# It replaces src/ and the licence files here with upstream's, applies
# local-changes.patch, and writes the two files upstream's build generates
# (src/essentia/version.h and src/algorithms/essentia_algorithms_reg.cpp).
# To build another algorithm, add it to ALGORITHMS (its folder/file and class)
# and run this again (intelligence/CMakeLists.txt builds every .cpp here).
set -euo pipefail

TAG=v2.1_beta5
COMMIT=ed59cc48e37ac33ac15b252fc5ad7af4b9ecd51d
VERSION=2.1-beta5

# The algorithms, as <folder>/<file>:<class>: what the similarity extractor
# uses (EssentiaExtractor.cpp) and what those create internally.
ALGORITHMS=(
    complex/magnitude:Magnitude
    sfx/logattacktime:LogAttackTime
    sfx/pitchsalience:PitchSalience
    spectral/energyband:EnergyBand
    spectral/flux:Flux
    spectral/melbands:MelBands
    spectral/mfcc:MFCC
    spectral/rolloff:RollOff
    spectral/spectralcontrast:SpectralContrast
    spectral/spectralpeaks:SpectralPeaks
    spectral/triangularbands:TriangularBands
    standard/autocorrelation:AutoCorrelation
    standard/dct:DCT
    standard/fftk:FFTK
    standard/ifftk:IFFTK
    standard/peakdetection:PeakDetection
    standard/spectrum:Spectrum
    standard/windowing:Windowing
    stats/centralmoments:CentralMoments
    stats/centroid:Centroid
    stats/crest:Crest
    stats/decrease:Decrease
    stats/distributionshape:DistributionShape
    stats/flatness:Flatness
    stats/geometricmean:GeometricMean
    temporal/effectiveduration:EffectiveDuration
    temporal/zerocrossingrate:ZeroCrossingRate
    tonal/dissonance:Dissonance
    tonal/pitchyin:PitchYin
)

SRC=${1:?usage: vendor.sh <essentia checkout at $TAG>}
DEST=$(cd "$(dirname "$0")" && pwd)
HEAD=$(git -C "$SRC" rev-parse HEAD)
if [ "$HEAD" != "$COMMIT" ]; then
    echo "$SRC is at $HEAD, not $TAG ($COMMIT)" >&2
    exit 1
fi

rm -rf "$DEST/src"
mkdir -p "$DEST/src"

# The core library, without what reads and writes files (FFmpeg, YAML, JSON),
# the ready-made extractors and the synthesis utilities.
cp -R "$SRC/src/essentia" "$DEST/src/"
rm -rf "$DEST/src/essentia/utils/extractor_freesound" "$DEST/src/essentia/utils/extractor_music"
rm -f "$DEST"/src/essentia/utils/{audiocontext.h,audiocontext.cpp,ffmpegapi.h,jsonconvert.h,jsonconvert.cpp} \
      "$DEST"/src/essentia/utils/{yamlast.h,yamlast.cpp,metadatautils.h,synth_utils.h,synth_utils.cpp}

for entry in "${ALGORITHMS[@]}"; do
    file=${entry%%:*}
    mkdir -p "$DEST/src/algorithms/$(dirname "$file")"
    cp "$SRC/src/algorithms/$file.h" "$SRC/src/algorithms/$file.cpp" "$DEST/src/algorithms/$(dirname "$file")/"
done

# KISS FFT (BSD-3-Clause), Essentia's FFT when built with --fft=KISS: no FFTW.
mkdir -p "$DEST/src/3rdparty/kiss_fft130/tools"
cp "$SRC"/src/3rdparty/kiss_fft130/{COPYING,README,kiss_fft.c,kiss_fft.h,_kiss_fft_guts.h} "$DEST/src/3rdparty/kiss_fft130/"
cp "$SRC"/src/3rdparty/kiss_fft130/tools/{kiss_fftr.c,kiss_fftr.h} "$DEST/src/3rdparty/kiss_fft130/tools/"

cp "$SRC/COPYING.txt" "$SRC/Essentia Licensing.txt" "$SRC/AUTHORS" "$SRC/README.md" "$DEST/"

(cd "$DEST" && patch -p1 --no-backup-if-mismatch < local-changes.patch)

# What upstream's configure writes (utils/algorithms_info.py): the version...
cat > "$DEST/src/essentia/version.h" <<EOF

#ifndef VERSION_H_
#define VERSION_H_
#define ESSENTIA_VERSION "$VERSION"
#define ESSENTIA_GIT_SHA "$TAG-$COMMIT"
#endif /* VERSION_H_ */
EOF

# ...and the factories' registrations of the algorithms built, as
# create_registration_cpp(algos, path, use_streaming=True) writes them.
{
    echo '// The algorithms SUBstation builds, registered with Essentia'"'"'s factories.'
    echo '//'
    echo '// Upstream generates this file when configuring (src/wscript, buildRegFile();'
    echo '// utils/algorithms_info.py, create_registration_cpp()). This is that output for'
    echo '// the vendored subset, as configured with --fft=KISS --include-algos=...;'
    echo '// written by ../../vendor.sh (see ../../VERSION.txt).'
    echo
    echo '#include "algorithmfactory.h"'
    for entry in "${ALGORITHMS[@]}"; do echo "#include \"algorithms/${entry%%:*}.h\""; done
    echo
    echo 'namespace essentia {'
    echo 'namespace standard {'
    echo
    echo 'ESSENTIA_API void registerAlgorithm() {'
    for entry in "${ALGORITHMS[@]}"; do c=${entry##*:}; echo "    AlgorithmFactory::Registrar<$c> reg$c;"; done
    echo '}}}'
    echo
    echo
    echo 'namespace essentia {'
    echo 'namespace streaming {'
    echo
    echo 'ESSENTIA_API void registerAlgorithm() {'
    for entry in "${ALGORITHMS[@]}"; do c=${entry##*:}; echo "    AlgorithmFactory::Registrar<$c, essentia::standard::$c> reg$c;"; done
    echo '}}}'
} > "$DEST/src/algorithms/essentia_algorithms_reg.cpp"

echo "Essentia $TAG vendored into $DEST"
