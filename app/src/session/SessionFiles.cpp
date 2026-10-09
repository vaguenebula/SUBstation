// The session's files: new, open, save, the template, the recent projects, the
// last folder, and exporting audio.

#include "session/Session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QVariant>

#include <new>

#include "audio/EngineBridge.h"
#include "browser/PathKeys.h"
#include "files/FileManager.h"
#include "io/LiveImport.h"
#include "io/LiveSet.h"
#include "io/Serialization.h"
#include "model/Errors.h"
#include "model/Paths.h"
#include "model/Project.h"
#include "model/Timebase.h"
#include "plugins/PluginIndex.h"
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

void Session::resetSession(const QString& untitledName) {
    untitledName_ = untitledName;
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
    bool fromTemplate = false;
    if (hasTemplate()) {
        try {
            sub::app::loadTemplate(*project_, templatePath());
            fromTemplate = true;
        } catch (const ProjectFileError& error) {
            Q_EMIT warning(error.message() + QStringLiteral("\nThe new project starts empty."));
        }
    }
    if (!fromTemplate) project_->clear();
    resetSession();
}

bool Session::openProject(const QString& path) {
    try {
        sub::app::loadProject(*project_, path);
    } catch (const ProjectFileError& error) {
        Q_EMIT warning(error.message());
        return false;
    }
    loadedFrom(path);
    addRecent(path);
    Q_EMIT statusMessage(QStringLiteral("Opened ") + QFileInfo(path).fileName() + missingFilesText());
    return true;
}

void Session::loadedFrom(const QString& path, const QString& untitledName) {
    resetSession(untitledName);
    setLastFolder(QFileInfo(path).absolutePath());
    files_->update();
    Q_EMIT projectOpened();
}

QString Session::missingFilesText() const {
    const int missing = files_->missingCount();
    if (missing <= 0) return {};
    return missing == 1 ? QStringLiteral(": 1 file is missing (the File Manager finds it)")
                        : QStringLiteral(": %1 files are missing (the File Manager finds them)").arg(missing);
}

bool Session::importLiveSet(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    live::ImportResult imported;
    try {
        const std::unique_ptr<live::Element> set = live::readLiveSet(path);
        live::ImportOptions options;
        options.plugins = plugins_->plugins();
        imported = live::importLiveSet(*set, path, options);
    } catch (const ProjectFileError& error) {
        Q_EMIT warning(error.message());
        return false;
    } catch (const std::bad_alloc&) {
        Q_EMIT warning(QStringLiteral("%1 is too large to import: there isn't enough memory.").arg(name));
        return false;
    }
    // Loaded as a project file is, once the set's elements are freed. Running out
    // of memory from here on isn't caught, as in Open: the project could be half
    // replaced, and going on with it would be worse.
    try {
        sub::app::loadInto(*project_, imported.project);
    } catch (const ProjectFileError& error) {
        Q_EMIT warning(error.message());
        return false;
    }
    loadedFrom(path, fileStem(path));
    // It has no file yet: unsaved until it is saved (New, Open and Quit ask first).
    undoStack_->resetClean();
    const auto count = [](int n, const QString& one, const QString& many) {
        return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
    };
    Q_EMIT statusMessage(QStringLiteral("Imported %1: %2, %3")
                             .arg(name, count(imported.tracks, QStringLiteral("track"), QStringLiteral("tracks")),
                                  count(imported.clips, QStringLiteral("clip"), QStringLiteral("clips"))));
    if (!imported.notes.isEmpty()) {
        Q_EMIT information(QStringLiteral("%1 was imported. What didn't come across as it was:\n\n• %2")
                               .arg(name, imported.notes.join(QStringLiteral("\n• "))));
    }
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
    const QString name = untitledName_.isEmpty() ? QStringLiteral("Untitled") : untitledName_;
    return QDir(lastFolder()).filePath(name + kProjectExtension);
}

QString Session::projectFilter() const { return QStringLiteral("SUBstation Project (*%1)").arg(kProjectExtension); }

QString Session::liveSetFilter() const { return QStringLiteral("Ableton Live Set (*.als)"); }

QString Session::projectExtension() const { return kProjectExtension; }

// --- The template ------------------------------------------------------------------------------

QString Session::templatePath() {
    return localDataFile("SUBSTATION_TEMPLATE", QStringLiteral("Template") + kProjectExtension);
}

bool Session::hasTemplate() const { return QFileInfo(templatePath()).isFile(); }

bool Session::saveAsTemplate() {
    bridge_->storePluginStates();
    try {
        sub::app::saveTemplate(*project_, templatePath());
    } catch (const ProjectFileError& error) {
        Q_EMIT warning(error.message());
        return false;
    }
    Q_EMIT templateChanged();
    Q_EMIT statusMessage(QStringLiteral("Saved as the template: new projects start as this one is now"));
    return true;
}

void Session::clearTemplate() {
    if (!hasTemplate()) return;
    if (!QFile::remove(templatePath())) {
        Q_EMIT warning(QStringLiteral("Could not remove the template (%1).").arg(QDir::toNativeSeparators(templatePath())));
        return;
    }
    Q_EMIT templateChanged();
    Q_EMIT statusMessage(QStringLiteral("Template cleared: new projects start empty"));
}

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
    const QString key = pathIdentity(entry);
    QStringList paths{entry};
    for (const QString& known : recentProjects()) {
        if (pathIdentity(known) != key) paths.append(known);
    }
    setRecent(paths);
}

void Session::clearRecentProjects() { setRecent({}); }

bool Session::recentProjectAvailable(const QString& path) {
    if (QFileInfo(path).isFile()) return true;
    Q_EMIT warning(QStringLiteral("%1 can't be found. It was removed from the list.").arg(QFileInfo(path).fileName()));
    QStringList kept = recentProjects();
    const QString key = pathIdentity(recentEntry(path));
    kept.removeIf([&](const QString& known) { return pathIdentity(known) == key; });
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
    if (selection_->hasTimeRange() && selection_->rangeEnd() > selection_->rangeStart()) {
        const TimeSignature ts = project_->timeSignature();
        choices.append(QVariantMap{
            {QStringLiteral("label"), QStringLiteral("Time Selection (%1 to %2)")
                                          .arg(formatPosition(selection_->rangeStart(), ts),
                                               formatPosition(selection_->rangeEnd(), ts))},
            {QStringLiteral("value"), QStringLiteral("selection")}});
    }
    return choices;
}

QVariantList Session::exportBitDepthChoices() const {
    return {QVariantMap{{QStringLiteral("label"), QStringLiteral("16-bit")}, {QStringLiteral("value"), 16}},
            QVariantMap{{QStringLiteral("label"), QStringLiteral("24-bit")}, {QStringLiteral("value"), 24}},
            QVariantMap{{QStringLiteral("label"), QStringLiteral("32-bit float")}, {QStringLiteral("value"), 32}}};
}

QVariantList Session::exportFileTypeChoices() const {
    return {QVariantMap{{QStringLiteral("label"), QStringLiteral("WAV")}, {QStringLiteral("value"), QStringLiteral("wav")}},
            QVariantMap{{QStringLiteral("label"), QStringLiteral("MP3")}, {QStringLiteral("value"), QStringLiteral("mp3")}}};
}

QVariantList Session::exportBitrateChoices() const {
    QVariantList choices;
    for (const int kbps : {320, 256, 192, 160, 128}) {
        choices.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("%1 kbps").arg(kbps)},
                                   {QStringLiteral("value"), kbps}});
    }
    return choices;
}

namespace {

std::pair<double, double> exportRange(const Project& project, const Selection& selection, const QString& range) {
    if (range == u"loop") return {project.loopStart(), project.loopEnd()};
    if (range == u"selection") {
        if (!selection.hasTimeRange()) return {0.0, 0.0};
        return {selection.rangeStart(), selection.rangeEnd()};
    }
    return {0.0, project.endBeat()};
}

}  // namespace

QString Session::exportProblem(const QString& range) const {
    if (range == u"selection" && !selection_->hasTimeRange()) {
        return QStringLiteral("There is no time selection to export: select a time range in the arrangement first.");
    }
    const auto [start, end] = exportRange(*project_, *selection_, range);
    return end <= start ? QStringLiteral("There is nothing to export yet.") : QString();
}

QString Session::suggestedExportPath(const QString& fileType) const {
    const QString path = project_->path();
    const QString name = !path.isEmpty()            ? fileStem(path)
                         : !untitledName_.isEmpty() ? untitledName_
                                                    : QStringLiteral("Untitled");
    return QDir(lastFolder()).filePath(name + (fileType == u"mp3" ? QStringLiteral(".mp3") : QStringLiteral(".wav")));
}

bool Session::exportAudio(const QString& path, const QString& range, int bitDepth, const QString& fileType,
                          int bitrate) {
    const QString problem = exportProblem(range);
    if (!problem.isEmpty()) {
        Q_EMIT information(problem);
        return false;
    }
    if (path.isEmpty() || rendering()) return false;
    if (bridge_->isPlaying()) togglePlay();
    const auto [start, end] = exportRange(*project_, *selection_, range);
    const AudioExportFormat format{fileType == u"mp3", bitDepth, bitrate};
    // In the background (session/Renders.h): the progress in a dialog, with Cancel.
    return startRender(new ExportAudioRender(render_, bridge_, path, start, end, format, this));
}

}  // namespace sub::app
