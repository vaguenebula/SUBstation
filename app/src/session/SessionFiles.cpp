// The session's files: new, open, save, the recent projects, the last folder,
// and exporting audio.

#include "session/Session.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QVariant>

#include "audio/EngineBridge.h"
#include "files/FileManager.h"
#include "io/Serialization.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"
#include "session/Renders.h"
#include "session/Selection.h"
#include "session/SessionSupport.h"

namespace sub::app {

namespace {

// A file as the recent list keeps it: absolute, links resolved, in the
// system's form (as the Python version wrote the same settings).
QString recentEntry(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::toNativeSeparators(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

}  // namespace

// --- New, open, save --------------------------------------------------------------------------

void Session::resetSession() {
    bridge_->stop();
    undoStack_->clear();
    undoStack_->setClean();
    playStart_ = 0.0;
    selection_->clear();
    selection_->selectTrack(QString());
    selection_->setInsert(0.0);
    bridge_->locate(0.0);
    Q_EMIT cleanChanged();
    Q_EMIT titleChanged();
}

void Session::newProject() {
    project_->clear();
    resetSession();
}

bool Session::openProject(const QString& path) {
    try {
        sub::app::loadProject(*project_, path);
    } catch (const ProjectFileError& error) {
        Q_EMIT warning(error.message());
        return false;
    }
    resetSession();
    setLastFolder(QFileInfo(path).absolutePath());
    addRecent(path);
    files_->update();
    Q_EMIT projectOpened();
    const int missing = files_->missingCount();
    QString message = QStringLiteral("Opened ") + QFileInfo(path).fileName();
    if (missing > 0) {
        message += missing == 1 ? QStringLiteral(": 1 file is missing (the File Manager finds it)")
                                : QStringLiteral(": %1 files are missing (the File Manager finds them)").arg(missing);
    }
    Q_EMIT statusMessage(message);
    return true;
}

bool Session::saveProject() {
    if (project_->path().isEmpty()) {
        Q_EMIT saveAsRequested();
        return false;
    }
    return saveTo(project_->path());
}

bool Session::saveProjectAs(const QString& path) {
    if (path.isEmpty()) return false;
    return saveTo(path);
}

bool Session::saveTo(const QString& path) {
    bridge_->storePluginStates();
    try {
        sub::app::saveProject(*project_, path);
    } catch (const ProjectFileError& error) {
        Q_EMIT warning(error.message());
        return false;
    }
    undoStack_->setClean();
    setLastFolder(QFileInfo(path).absolutePath());
    addRecent(path);
    Q_EMIT titleChanged();
    Q_EMIT statusMessage(QStringLiteral("Saved ") + QFileInfo(path).fileName());
    return true;
}

QString Session::suggestedSavePath() const {
    return QDir(lastFolder()).filePath(QStringLiteral("Untitled") + kProjectExtension);
}

QString Session::projectFilter() const { return QStringLiteral("SUBstation Project (*%1)").arg(kProjectExtension); }

QString Session::projectExtension() const { return kProjectExtension; }

// --- Recent projects, the last folder -----------------------------------------------------------

QStringList Session::recentProjects() const {
    const QVariant stored = QSettings().value(kRecentKey);
    if (stored.typeId() == QMetaType::QString) return {stored.toString()};  // a one-item list comes back as a string
    if (stored.typeId() == QMetaType::QStringList || stored.typeId() == QMetaType::QVariantList) {
        return stored.toStringList();
    }
    return {};
}

void Session::setRecent(const QStringList& paths) {
    QSettings().setValue(kRecentKey, paths.mid(0, kMaxRecent));
    Q_EMIT recentProjectsChanged();
}

void Session::addRecent(const QString& path) {
    const QString entry = recentEntry(path);
    const QString key = entry.toCaseFolded();
    QStringList paths{entry};
    for (const QString& known : recentProjects()) {
        if (known.toCaseFolded() != key) paths.append(known);
    }
    setRecent(paths);
}

void Session::clearRecentProjects() { setRecent({}); }

bool Session::recentProjectAvailable(const QString& path) {
    if (QFileInfo(path).isFile()) return true;
    Q_EMIT warning(QStringLiteral("%1 can't be found. It was removed from the list.").arg(QFileInfo(path).fileName()));
    QStringList kept = recentProjects();
    const QString key = recentEntry(path).toCaseFolded();  // (as the list keeps it: in the system's form)
    kept.removeIf([&](const QString& known) { return known.toCaseFolded() == key; });
    setRecent(kept);
    return false;
}

QVariantList Session::recentMenuItems() const {
    QVariantList items;
    const QStringList paths = recentProjects();
    for (qsizetype i = 0; i < paths.size(); ++i) {
        QString name = QFileInfo(paths[i]).fileName();
        name.replace(u'&', QStringLiteral("&&"));  // a literal "&", not a mnemonic
        const QString label = i < 9 ? QStringLiteral("&%1  %2").arg(QString::number(i + 1), name)
                                    : QStringLiteral("%1  %2").arg(QString::number(i + 1), name);
        items.append(QVariantMap{{QStringLiteral("path"), paths[i]},
                                 {QStringLiteral("label"), label},
                                 {QStringLiteral("toolTip"), paths[i]}});
    }
    return items;
}

QString Session::lastFolder() const {
    return QSettings().value(kLastFolderKey, QDir::homePath() + QStringLiteral("/Music")).toString();
}

void Session::setLastFolder(const QString& folder) {
    if (folder == lastFolder()) return;
    QSettings().setValue(kLastFolderKey, QDir::toNativeSeparators(folder));
    Q_EMIT lastFolderChanged();
}

// --- Export Audio -------------------------------------------------------------------------------

QVariantList Session::exportRangeChoices() const {
    QVariantList choices{QVariantMap{{QStringLiteral("label"), QStringLiteral("Arrangement (start to end of last clip)")},
                                     {QStringLiteral("value"), QStringLiteral("arrangement")}}};
    if (project_->loopEnabled() && project_->loopEnd() > project_->loopStart()) {
        choices.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Loop region")},
                                   {QStringLiteral("value"), QStringLiteral("loop")}});
    }
    return choices;
}

QVariantList Session::exportBitDepthChoices() const {
    return {QVariantMap{{QStringLiteral("label"), QStringLiteral("16-bit")}, {QStringLiteral("value"), 16}},
            QVariantMap{{QStringLiteral("label"), QStringLiteral("24-bit")}, {QStringLiteral("value"), 24}},
            QVariantMap{{QStringLiteral("label"), QStringLiteral("32-bit float")}, {QStringLiteral("value"), 32}}};
}

namespace {

std::pair<double, double> exportRange(const Project& project, const QString& range) {
    if (range == u"loop") return {project.loopStart(), project.loopEnd()};
    return {0.0, project.endBeat()};
}

}  // namespace

QString Session::exportProblem(const QString& range) const {
    const auto [start, end] = exportRange(*project_, range);
    return end <= start ? QStringLiteral("There is nothing to export yet.") : QString();
}

QString Session::suggestedExportPath() const {
    const QString path = project_->path();
    const QString name = path.isEmpty() ? QStringLiteral("Untitled") : fileStem(path);
    return QDir(lastFolder()).filePath(name + QStringLiteral(".wav"));
}

bool Session::exportAudio(const QString& path, const QString& range, int bitDepth) {
    const QString problem = exportProblem(range);
    if (!problem.isEmpty()) {
        Q_EMIT information(problem);
        return false;
    }
    if (path.isEmpty() || rendering()) return false;
    if (bridge_->isPlaying()) togglePlay();
    const auto [start, end] = exportRange(*project_, range);
    // In the background (session/Renders.h): the progress in a dialog, with Cancel.
    return startRender(new ExportAudioRender(render_, bridge_, path, start, end, bitDepth, this));
}

}  // namespace sub::app
