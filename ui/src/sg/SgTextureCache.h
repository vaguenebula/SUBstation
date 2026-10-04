#pragma once

// The textures SgPainter draws text and images with, one cache per window.
// Text is rendered with QPainter into an image at the window's device pixel
// ratio, keyed by text, font, colour, layout and ratio; images are keyed by
// QImage::cacheKey(). Entries are shared with the nodes that draw them, so an
// entry evicted (least recently used first) stays alive until its last node
// goes. The whole cache is dropped when the window's scene graph is
// invalidated. Used only on the window's render thread.

#include <QFont>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QPointF>
#include <QRgb>
#include <QSizeF>
#include <QString>

#include <list>
#include <memory>
#include <utility>

class QQuickWindow;
class QSGTexture;

namespace sub::ui {

struct SgTexture {
    QSGTexture* texture = nullptr;  // owned
    QSizeF size;                    // the whole image, in item coordinates (pixels / ratio)
    qreal dpr = 1.0;
    // Text: where its layout box sits in the image (the padding around it, for
    // glyphs reaching outside their advance), the box's size and its first ascent.
    QPointF origin;
    QSizeF box;
    qreal ascent = 0.0;
    qsizetype bytes = 0;

    SgTexture() = default;
    SgTexture(const SgTexture&) = delete;
    SgTexture& operator=(const SgTexture&) = delete;
    ~SgTexture();
};

class SgTextureCache {
public:
    static constexpr int kMaxEntries = 4096;
    static constexpr qsizetype kMaxBytes = qsizetype(64) * 1024 * 1024;

    // The cache of `window` (render thread, during sync), made on first use.
    static SgTextureCache* forWindow(QQuickWindow* window);
    // How many windows have a cache (for tests).
    static int windowCount();

    // `layoutFlags`: horizontal alignment and Qt::TextWordWrap (with `wrapWidth`).
    std::shared_ptr<SgTexture> text(const QString& text, const QFont& font, const QColor& color, int layoutFlags,
                                    qreal wrapWidth, qreal dpr);
    std::shared_ptr<SgTexture> image(const QImage& image);

    int size() const { return int(entries_.size()); }
    qsizetype bytes() const { return bytes_; }
    int hits() const { return hits_; }
    int misses() const { return misses_; }

    // What an entry is: a text as it was rendered, or an image (`image` is its cacheKey()).
    struct Key {
        QString text;
        QFont font;
        qint64 image = 0;
        QRgb color = 0;
        qreal dpr = 0.0;
        int flags = 0;
        qreal wrap = 0.0;
        bool operator==(const Key& other) const = default;
    };

private:
    explicit SgTextureCache(QQuickWindow* window);
    ~SgTextureCache();
    static void drop(QQuickWindow* window);

    std::shared_ptr<SgTexture> find(const Key& key);
    void insert(const Key& key, const std::shared_ptr<SgTexture>& texture);
    void evict();

    using Entry = std::pair<Key, std::shared_ptr<SgTexture>>;
    QQuickWindow* window_;
    std::list<Entry> entries_;  // most recently used first
    QHash<Key, std::list<Entry>::iterator> index_;
    qsizetype bytes_ = 0;
    int hits_ = 0;
    int misses_ = 0;
    QMetaObject::Connection invalidated_;
    QMetaObject::Connection destroyed_;
};

size_t qHash(const SgTextureCache::Key& key, size_t seed = 0);

}  // namespace sub::ui
