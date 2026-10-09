#include "plugins/PluginIndex.h"

#include <QFileInfo>
#include <QVariantMap>

#include <exception>
#include <memory>
#include <utility>

#include "browser/PathKeys.h"
#include "model/Numbers.h"
#include "plugins/PluginPaths.h"
#include "plugins/PluginScanner.h"
#include "plugins/PluginSettings.h"

namespace sub::app {

PluginIndex::PluginIndex(QObject* parent, QString scanner)
    : QObject(parent),
      scanner_(scanner.isEmpty() ? PluginScanner::defaultProgram() : std::move(scanner)),
      pluginModel_(new PluginListModel(this)),
      folderModel_(new PluginFolderModel(this)) {}

PluginIndex::~PluginIndex() { wait(); }

QString PluginIndex::statusText() const {
    if (scanning()) return progressText_.isEmpty() ? QStringLiteral("Scanning plug-ins…") : progressText_;
    QString text = countText(pluginCount(), QStringLiteral("plug-in"), QStringLiteral("plug-ins")) + QStringLiteral(" found");
    if (failureCount())
        text += QStringLiteral(" · ") + countText(failureCount(), QStringLiteral("file"), QStringLiteral("files")) +
                QStringLiteral(" could not be read");
    return text;
}

QString PluginIndex::failuresText() const {
    QStringList lines;
    for (size_t i = 0; i < failures_.size() && i < 30; ++i)
        lines << QFileInfo(failures_[i].path).fileName() + QStringLiteral(": ") + failures_[i].reason;
    return lines.join(QLatin1Char('\n'));
}

QVariantList PluginIndex::failureList() const {
    QVariantList list;
    for (const ScanFailure& failure : failures_)
        list << QVariantMap{{QStringLiteral("path"), failure.path},
                            {QStringLiteral("name"), QFileInfo(failure.path).fileName()},
                            {QStringLiteral("reason"), failure.reason}};
    return list;
}

QStringList PluginIndex::standardFolders() const { return standardPluginFolders(); }

QStringList PluginIndex::customFolders() const { return customPluginFolders(); }

void PluginIndex::scan(bool rescan) {
    if (thread_) {
        again_ = rescan || again_.value_or(false);
        return;
    }
    auto outcome = std::make_shared<Outcome>();
    outcome_ = outcome;
    PluginScanner scanner(scanner_, {}, kPluginScanTimeout, pluginFolders());
    // The thread ends before this object does (the destructor waits for it), so it may use `this`.
    thread_ = QThread::create([this, scanner = std::move(scanner), rescan, outcome]() mutable {
        auto report = [this](int done, int total, const QString& path) {
            QMetaObject::invokeMethod(
                this,
                [this, done, total, path] {
                    if (!scanning()) return;
                    setProgressText(QStringLiteral("Scanning %1/%2: %3")
                                        .arg(done + 1)
                                        .arg(total)
                                        .arg(QFileInfo(path).completeBaseName()));
                    Q_EMIT this->progress(done, total, path);
                },
                Qt::QueuedConnection);
        };
        auto cancelled = [] { return QThread::currentThread()->isInterruptionRequested(); };
        try {
            outcome->result = scanner.scan(std::nullopt, rescan, report, cancelled);
        } catch (const std::exception& error) {
            outcome->error = QString::fromUtf8(error.what());
        }
    });
    thread_->setParent(this);
    connect(thread_, &QThread::finished, this, &PluginIndex::finished);
    progressText_.clear();
    thread_->start(QThread::LowPriority);
    Q_EMIT updated();
    Q_EMIT statusTextChanged();
}

void PluginIndex::finished() {
    if (!thread_) return;
    if (outcome_->error) {
        Q_EMIT statusMessage(*outcome_->error);
    } else {
        plugins_ = std::move(outcome_->result.plugins);
        failures_ = std::move(outcome_->result.failures);
        pluginModel_->setPlugins(plugins_);
    }
    outcome_.reset();
    thread_->deleteLater();
    thread_ = nullptr;
    progressText_.clear();
    if (again_) {
        const bool rescan = *again_;
        again_.reset();
        scan(rescan);
    } else {
        Q_EMIT updated();
        Q_EMIT statusTextChanged();
    }
}

void PluginIndex::setProgressText(const QString& text) {
    if (text == progressText_) return;
    progressText_ = text;
    Q_EMIT statusTextChanged();
}

bool PluginIndex::addFolder(const QString& given) {
    const QString folder = normalPath(given);
    if (folder.isEmpty()) return false;
    QStringList folders = customPluginFolders();
    for (const QString& known : standardPluginFolders() + folders)
        if (sameFolder(folder, known)) return false;
    setCustomPluginFolders(folders << folder);
    folderModel_->refresh();
    Q_EMIT foldersChanged();
    scan();  // reads only the new folder's files
    return true;
}

void PluginIndex::removeFolder(const QString& folder) {
    QStringList kept;
    const QStringList folders = customPluginFolders();
    for (const QString& known : folders)
        if (!sameFolder(known, folder)) kept << known;
    if (kept.size() == folders.size()) return;
    setCustomPluginFolders(kept);
    folderModel_->refresh();
    Q_EMIT foldersChanged();
    scan();  // its plug-ins leave the browser
}

void PluginIndex::wait() {
    if (!thread_) return;
    thread_->requestInterruption();
    thread_->wait();
}

}  // namespace sub::app
