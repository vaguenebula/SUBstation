#pragma once

// The arrangement's waveforms, drawn as Ableton draws them (waveformOutline:
// an antialiased band on the scene graph, SgPainter::fillBand), scaled by the
// clip's gain (louder is taller, cut off at the lane's edges), as it scales
// its audio.
//
// Outlines are worked out a tile (kTile pixels) at a time and kept in an LRU
// of kMaxTiles. Tiles are anchored to the start of the source file (not the
// clip), so moving or trimming a clip reuses them; only zooming, a tempo
// change (frames per pixel) or a new lane height or gain makes new ones. A
// tile has a point more each side, drawn clipped to it, so the band is
// seamless across tile borders.
//
// The cache is the one thing the lanes' paint() changes (tiles are made as
// they are first drawn); a mutex keeps clear() (the GUI thread, on a project
// reset) apart from it.

#include "audio/Waveform.h"

#include <QColor>
#include <QRectF>
#include <QString>

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

namespace sub::ui {

class SgPainter;

namespace arrangement {

// A waveform as Ableton draws them: an outline through the centre of each
// pixel column, from the minimum to the maximum of two columns' worth of
// frames about it (fuller and smoother than a column's own, the peaks never
// lowered), at least kMinThickness thick about the middle of a pixel (the line
// through silence: a pixel and a faint edge), filled and antialiased
// (SgPainter::fillBand). The frames come from the coarsest level of the
// source's peaks with no more frames per peak than per pixel (or its samples,
// zoomed in past the finest level).
struct WaveformOutline {
    static constexpr float kMinThickness = 1.5f;  // pixels

    int count = 0;  // points (none past the source's end)
    // Per lane (one, or a channel each when split): each point's top and
    // bottom, in pixels from the top.
    std::vector<std::vector<float>> tops;
    std::vector<std::vector<float>> bottoms;

    // Each lane's band: point i the column from x0 + i (a pixel wide), y from `top`.
    void draw(SgPainter& painter, double x0, double top, const QColor& color) const;
};

// The outline of `count` columns of `framesPerPixel` frames from column `first`
// (column 0 starts at the source's first frame; point i is column first + i's
// centre), `height` pixels high, scaled by `gain` (linear); empty when it is
// all past the source's end.
WaveformOutline waveformOutline(const app::Waveform& source, double framesPerPixel, qint64 first, int count,
                                int height, bool splitChannels, double gain = 1.0);

class WaveformCache {
public:
    static constexpr int kTile = 256;  // pixels
    static constexpr int kMaxTiles = 800;

    // A tile: kTile + 2 points, from the centre of the column before it to that of the column after it.
    using Tile = WaveformOutline;

    explicit WaveformCache(int maxTiles = kMaxTiles) : maxTiles_(maxTiles) {}

    void clear();
    int size() const;

    // A clip's waveform over `body` (the painter clipped to it): `clipX` is the
    // clip's start (x), `offsetSec` where in the file it starts, `gain` linear.
    void draw(SgPainter& painter, const app::Waveform& source, const QRectF& body, double clipX, double offsetSec,
              double framesPerPixel, const QColor& color, bool splitChannels, const QRectF& visible,
              double gain = 1.0);

    // A tile of a source's waveform, `height` pixels high (none past the
    // source's end): point i is column index * kTile - 1 + i's centre.
    static std::shared_ptr<const Tile> renderTile(const app::Waveform& source, double framesPerPixel, int index,
                                                  int height, bool splitChannels, double gain = 1.0);

private:
    // (path, frames, sample rate, frames per pixel, tile, height, split, gain)
    using Key = std::tuple<QString, qint64, int, double, int, int, bool, double>;

    std::shared_ptr<const Tile> tile(const app::Waveform& source, double framesPerPixel, int index, int height,
                                     bool split, double gain);

    int maxTiles_;
    mutable std::mutex mutex_;
    std::list<Key> order_;  // least recently used first
    std::map<Key, std::pair<std::shared_ptr<const Tile>, std::list<Key>::iterator>> tiles_;
};

// A gain (linear) to 0.1 dB: steps that look the same (turning a gain knob
// renders fewer tiles), and a quiet clip's waveform stays as small as it is
// (never flat, as rounding the linear gain would make it).
double quantizedGain(double gain);

}  // namespace arrangement
}  // namespace sub::ui
