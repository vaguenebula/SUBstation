#include "sg/SgTextureCache.h"

#include <QFontMetricsF>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QSGTexture>

#include <cmath>

namespace sub::ui {

namespace {

QMutex& registryMutex() {
    static QMutex mutex;
    return mutex;
}

// Each window's cache. Windows render on threads of their own, so it is locked.
QHash<QQuickWindow*, SgTextureCache*>& registry() {
    static QHash<QQuickWindow*, SgTextureCache*> caches;
    return caches;
}

}  // namespace

SgTexture::~SgTexture() { delete texture; }

SgTextureCache* SgTextureCache::forWindow(QQuickWindow* window) {
    if (!window)
        return nullptr;
    QMutexLocker lock(&registryMutex());
    auto it = registry().constFind(window);
    if (it != registry().constEnd())
        return it.value();
    auto* cache = new SgTextureCache(window);
    registry().insert(window, cache);
    return cache;
}

int SgTextureCache::windowCount() {
    QMutexLocker lock(&registryMutex());
    return int(registry().size());
}

SgTextureCache::SgTextureCache(QQuickWindow* window) : window_(window) {
    // The scene graph going (the window hidden for good, or destroyed) takes the
    // textures with it; nodes are gone by then. Both run where they are emitted.
    invalidated_ = QObject::connect(window, &QQuickWindow::sceneGraphInvalidated, [window] { drop(window); });
    destroyed_ = QObject::connect(window, &QObject::destroyed, [window] { drop(window); });
}

SgTextureCache::~SgTextureCache() {
    QObject::disconnect(invalidated_);
    QObject::disconnect(destroyed_);
}

void SgTextureCache::drop(QQuickWindow* window) {
    SgTextureCache* cache = nullptr;
    {
        QMutexLocker lock(&registryMutex());
        cache = registry().take(window);
    }
    delete cache;
}

size_t qHash(const SgTextureCache::Key& key, size_t seed) {
    return qHashMulti(seed, key.text, key.font, key.image, key.color, key.dpr, key.flags, key.wrap);
}

std::shared_ptr<SgTexture> SgTextureCache::find(const Key& key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, it.value());  // most recently used
    return it.value()->second;
}

void SgTextureCache::insert(const Key& key, const std::shared_ptr<SgTexture>& texture) {
    entries_.emplace_front(key, texture);
    index_.insert(key, entries_.begin());
    bytes_ += texture->bytes;
    evict();
}

void SgTextureCache::evict() {
    // Least recently used first, skipping what nodes still draw (they hold a reference too).
    auto it = entries_.end();
    while ((int(entries_.size()) > kMaxEntries || bytes_ > kMaxBytes) && it != entries_.begin()) {
        --it;
        if (it->second.use_count() > 1)
            continue;
        bytes_ -= it->second->bytes;
        index_.remove(it->first);
        it = entries_.erase(it);
    }
}

std::shared_ptr<SgTexture> SgTextureCache::text(const QString& text, const QFont& font, const QColor& color,
                                                 int layoutFlags, qreal wrapWidth, qreal dpr) {
    const bool wrap = layoutFlags & Qt::TextWordWrap;
    const bool multiline = wrap || text.contains(QLatin1Char('\n'));
    const int horizontal = multiline ? (layoutFlags & Qt::AlignHorizontal_Mask) : 0;
    Key key;
    key.text = text;
    key.font = font;
    key.color = color.rgba();
    key.dpr = dpr;
    key.flags = horizontal | (wrap ? int(Qt::TextWordWrap) : 0);
    key.wrap = wrap ? wrapWidth : 0.0;
    if (auto found = find(key))
        return found;

    const QFontMetricsF metrics(font);
    QSizeF box;
    if (multiline) {
        const qreal width = wrap ? wrapWidth : 1e6;
        box = metrics.boundingRect(QRectF(0, 0, width, 1e6), horizontal | (wrap ? int(Qt::TextWordWrap) : 0), text)
                  .size();
        if (wrap)
            box.setWidth(wrapWidth);
    } else {
        box = QSizeF(metrics.horizontalAdvance(text), metrics.height());
    }
    // Room around the layout box for glyphs that reach past their advance (italics, overhangs).
    const qreal pad = 2.0 + std::ceil(metrics.height() * 0.25);
    const QSize pixels(qMax(1, int(std::ceil((box.width() + 2 * pad) * dpr))),
                       qMax(1, int(std::ceil((box.height() + 2 * pad) * dpr))));
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QRectF(pad, pad, box.width(), box.height()),
                         horizontal | Qt::AlignTop | (wrap ? int(Qt::TextWordWrap) : 0) | Qt::TextDontClip, text);
    }
    auto texture = std::make_shared<SgTexture>();
    texture->texture = window_->createTextureFromImage(image, QQuickWindow::TextureCanUseAtlas);
    texture->size = QSizeF(pixels.width() / dpr, pixels.height() / dpr);
    texture->dpr = dpr;
    texture->origin = QPointF(pad, pad);
    texture->box = box;
    texture->ascent = metrics.ascent();
    texture->bytes = image.sizeInBytes();
    insert(key, texture);
    return texture;
}

std::shared_ptr<SgTexture> SgTextureCache::image(const QImage& image) {
    Key key;
    key.image = image.cacheKey();
    if (auto found = find(key))
        return found;
    auto texture = std::make_shared<SgTexture>();
    texture->texture = window_->createTextureFromImage(image, QQuickWindow::TextureCanUseAtlas);
    texture->size = QSizeF(image.size());
    texture->bytes = image.sizeInBytes();
    insert(key, texture);
    return texture;
}

}  // namespace sub::ui
