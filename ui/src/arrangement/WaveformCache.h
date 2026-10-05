#pragma once

// The arrangement's waveforms, drawn as columns on the scene graph
// (SgPainter::fillColumns): a column per pixel from the minimum to the maximum
// of what it covers, scaled by the clip's gain (louder is taller, cut off at
// the lane's edges), as it scales its audio.
//
// Columns are worked out a tile (kTile pixels) at a time and kept in an LRU of
// kMaxTiles. Tiles are anchored to the start of the source file (not the
// clip), so moving or trimming a clip reuses them; only zooming, a tempo
// change (frames per pixel) or a new lane height or gain makes new ones. A
// tile is rendered from the coarsest level of the source's peaks with no more
// frames per peak than per pixel (or its samples, zoomed in past the finest
// level), with one padding column each side so a [1 2 1]/4 blur (for peaks:
// envelopes read smoother) is seamless across tile borders.
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

class WaveformCache {
public:
    static constexpr int kTile = 256;  // pixels
    static constexpr int kMaxTiles = 800;

    // A tile's columns: per lane (one, or a channel each when split), each
    // column's top and bottom (pixels from the tile's top; equal: nothing).
    struct Tile {
        std::vector<std::vector<float>> tops;
        std::vector<std::vector<float>> bottoms;
    };

    explicit WaveformCache(int maxTiles = kMaxTiles) : maxTiles_(maxTiles) {}

    void clear();
    int size() const;

    // A clip's waveform over `body` (the painter clipped to it): `clipX` is the
    // clip's start (x), `offsetSec` where in the file it starts, `gain` linear.
    void draw(SgPainter& painter, const app::Waveform& source, const QRectF& body, double clipX, double offsetSec,
              double framesPerPixel, const QColor& color, bool splitChannels, const QRectF& visible,
              double gain = 1.0);

    // A tile of a source's waveform, `height` pixels high (none past the source's end).
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
