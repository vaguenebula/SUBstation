#include "audio/ReverseJob.h"

#include "audio/AudioFiles.h"

#include "Engine.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <exception>
#include <utility>

namespace sub::app {

ReverseJob::ReverseJob(sub::Engine& engine, std::shared_ptr<const sub::AudioSource> source, const QString& path,
                       qint64 chunkFrames, QObject* parent)
    : RenderTask(path, parent),
      engine_(engine),
      source_(std::move(source)),
      frames_(source_->frames()),
      seconds_(static_cast<double>(source_->frames()) / source_->sampleRate()),
      chunk_(std::max<qint64>(1, chunkFrames)),
      utf8Path_(path.toStdString()) {}

ReverseJob::~ReverseJob() {
    cancelJob();
    if (thread_.joinable()) thread_.join();
}

void ReverseJob::start() {
    if (thread_.joinable() || done()) return;
    thread_ = std::thread([this] { run(); });
    follow();
}

double ReverseJob::progress() const {
    if (done()) return 1.0;
    return 0.9 * static_cast<double>(written_.load(std::memory_order_relaxed)) /
           static_cast<double>(std::max<qint64>(1, frames_));
}

std::shared_ptr<sub::AudioSource> ReverseJob::finish() {
    start();  // (if it wasn't)
    if (thread_.joinable()) thread_.join();
    if (!error_.empty()) {
        throw EditError(QStringLiteral("Could not write %1: %2")
                            .arg(QFileInfo(path()).fileName(), QString::fromStdString(error_)));
    }
    return loaded_;
}

void ReverseJob::run() {
    const sub::AudioSource& source = *source_;
    const auto channels = static_cast<int>(source.channels());
    QFile file(path());
    try {
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
        if (file.write(floatWavHeader(channels, frames_, static_cast<int>(source.sampleRate()))) < 0) {
            throw std::runtime_error(file.errorString().toStdString());
        }
        QByteArray chunk;
        qint64 end = frames_;
        while (end > 0 && !cancelled()) {
            const qint64 start = std::max<qint64>(0, end - chunk_);
            chunk.resize(static_cast<qsizetype>((end - start) * channels * 4));
            char* out = chunk.data();
            for (qint64 frame = end - 1; frame >= start; --frame) {
                for (int c = 0; c < channels; ++c) {
                    qToLittleEndian(source.channelData(static_cast<uint32_t>(c))[frame], out);
                    out += 4;
                }
            }
            if (file.write(chunk) != chunk.size()) throw std::runtime_error(file.errorString().toStdString());
            written_.fetch_add(end - start, std::memory_order_relaxed);
            end = start;
        }
        file.close();
        if (!cancelled()) loaded_ = engine_.loadSource(utf8Path_);
    } catch (const std::exception& error) {
        error_ = error.what();
    }
    if (!loaded_) {  // (cancelled or failed: what was written of it goes)
        file.close();
        QFile::remove(path());
    }
    done_.store(true, std::memory_order_release);
}

}  // namespace sub::app
