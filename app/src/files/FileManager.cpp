#include "files/FileManager.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <algorithm>

#include "Browser.h"  // the browser's index (sub_browser: its Snapshot)
#include "audio/AudioFiles.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "editor/ProjectEditor.h"
#include "files/HotSwap.h"
#include "files/MissingFiles.h"
#include "model/Edits.h"
#include "model/Project.h"
#include "session/Selection.h"

namespace sub::app {

namespace {

QVariantMap action(const QString& id, const QString& label) {
    return {{QStringLiteral("action"), id}, {QStringLiteral("label"), label}};
}

QString stem(const QString& path) { return QFileInfo(path).completeBaseName(); }

QString fileCountText(int count) {
    return count == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(count);
}

}  // namespace

// --- The rows ------------------------------------------------------------------------------

int FileListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant FileListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const Row& row = rows_[static_cast<size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return row.name;
        case PathRole: return row.path;
        case FolderRole: return row.folder;
        case MissingRole: return row.missing;
        case UsesRole: return row.uses;
        case FrozenRole: return row.frozen;
        default: return {};
    }
}

QHash<int, QByteArray> FileListModel::roleNames() const {
    return {{PathRole, "path"},       {NameRole, "name"}, {FolderRole, "folder"},
            {MissingRole, "missing"}, {UsesRole, "uses"}, {FrozenRole, "frozen"}};
}

void FileListModel::setRows(std::vector<Row> rows) {
    if (rows == rows_) return;
    const bool sameFiles = rows.size() == rows_.size() &&
                           std::equal(rows.begin(), rows.end(), rows_.begin(),
                                      [](const Row& a, const Row& b) { return a.path == b.path; });
    if (sameFiles) {  // (what plays them, or whether they are there: the view keeps its place)
        rows_ = std::move(rows);
        Q_EMIT dataChanged(index(0), index(count() - 1));
        return;
    }
    const int before = count();
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    if (count() != before) Q_EMIT countChanged();
}

QVariantMap FileListModel::get(int row) const {
    if (row < 0 || row >= count()) return {};
    const Row& r = rows_[static_cast<size_t>(row)];
    return {{QStringLiteral("path"), r.path},       {QStringLiteral("name"), r.name},
            {QStringLiteral("folder"), r.folder},   {QStringLiteral("uses"), r.uses},
            {QStringLiteral("missing"), r.missing}, {QStringLiteral("frozen"), r.frozen}};
}

int FileListModel::rowOf(const QString& path) const {
    for (int i = 0; i < count(); ++i) {
        if (edits::samePath(rows_[static_cast<size_t>(i)].path, path)) return i;
    }
    return -1;
}

// --- The File Manager ----------------------------------------------------------------------

FileManager::FileManager(Project* project, ProjectEditor* editor, Selection* selection, EngineBridge* bridge,
                         BrowserController* browser, HotSwap* hotSwap, QObject* parent)
    : QObject(parent),
      project_(project),
      editor_(editor),
      selection_(selection),
      bridge_(bridge),
      browser_(browser),
      hotSwap_(hotSwap),
      model_(new FileListModel(this)) {
    updateTimer_.setSingleShot(true);
    updateTimer_.setInterval(0);
    connect(&updateTimer_, &QTimer::timeout, this, &FileManager::update);
    connect(project_, &Project::reset, this, [this] {
        cancelSearch();
        exists_.clear();
        update();
    });
    connect(project_, &Project::clipsChanged, this, &FileManager::scheduleUpdate);
    connect(project_, &Project::devicesChanged, this, &FileManager::scheduleUpdate);
    connect(project_, &Project::deviceStateChanged, this, &FileManager::scheduleUpdate);
    connect(project_, &Project::freezeChanged, this, &FileManager::scheduleUpdate);
    for (auto changed : {&Project::trackInserted, &Project::trackRemoved, &Project::returnInserted,
                         &Project::returnRemoved}) {
        connect(project_, changed, this, &FileManager::scheduleUpdate);
    }
    // A file the engine couldn't decode may have gone; one it could may be back
    // (or it had it in memory still): looked at again.
    auto lookAgain = [this](const QString& path) {
        if (exists_.remove(sourceKey(path)) > 0) scheduleUpdate();
    };
    connect(bridge_, &EngineBridge::sourceReady, this, lookAgain);
    connect(bridge_, &EngineBridge::sourceFailed, this,
            [lookAgain](const QString& path, const QString&) { lookAgain(path); });
    update();
}

FileManager::~FileManager() { search_.reset(); }  // (cancelled, and waited for)

void FileManager::scheduleUpdate() { updateTimer_.start(); }

bool FileManager::exists(const QString& path) const {
    const QString key = sourceKey(path);
    const auto known = exists_.constFind(key);
    if (known != exists_.constEnd()) return *known;
    const bool there = QFileInfo(path).isFile();
    exists_.insert(key, there);
    return there;
}

void FileManager::update() {
    updateTimer_.stop();
    files_ = projectFiles(*project_);
    showRows();
    Q_EMIT filesChanged();
}

void FileManager::showRows() {
    const QStringList words = filter_.split(u' ', Qt::SkipEmptyParts);
    std::vector<FileListModel::Row> rows;
    for (const ProjectFile& file : files_) {
        FileListModel::Row row;
        const QFileInfo info(file.path);
        row.path = file.path;
        row.name = info.fileName();
        row.folder = QDir::toNativeSeparators(info.path());
        row.missing = !exists(file.path);
        FileUses frozen;
        changeableUses(*project_, file.uses, &frozen);
        row.frozen = !frozen.isEmpty();
        row.uses = usesText(*project_, file.uses);
        const bool shown = std::all_of(words.begin(), words.end(), [&](const QString& word) {
            return row.name.contains(word, Qt::CaseInsensitive) || row.folder.contains(word, Qt::CaseInsensitive);
        });
        if (shown) rows.push_back(std::move(row));
    }
    // The missing first, then by name.
    std::stable_sort(rows.begin(), rows.end(), [](const FileListModel::Row& a, const FileListModel::Row& b) {
        if (a.missing != b.missing) return a.missing;
        const int order = a.name.compare(b.name, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a.folder.compare(b.folder, Qt::CaseInsensitive) < 0;
    });
    model_->setRows(std::move(rows));
}

int FileManager::missingCount() const {
    return static_cast<int>(
        std::count_if(files_.begin(), files_.end(), [this](const ProjectFile& file) { return !exists(file.path); }));
}

QStringList FileManager::missingFiles() const {
    QStringList missing;
    for (const FileListModel::Row& row : model_->rows()) {
        if (row.missing) missing << row.path;
    }
    // (Those the filter hides too.)
    for (const ProjectFile& file : files_) {
        if (!exists(file.path) && !missing.contains(file.path)) missing << file.path;
    }
    return missing;
}

bool FileManager::isMissing(const QString& path) const { return !exists(path); }

QString FileManager::summary() const {
    if (files_.empty()) return QStringLiteral("No files");
    const int missing = missingCount();
    const QString all = fileCountText(fileCount());
    return missing == 0 ? all : QStringLiteral("%1, %2 missing").arg(all).arg(missing);
}

void FileManager::setFilter(const QString& filter) {
    if (filter == filter_) return;
    filter_ = filter;
    Q_EMIT filterChanged();
    showRows();
}

bool FileManager::canFindSimilar() const { return browser_ && browser_->canFindSimilar(); }

void FileManager::refresh() {
    exists_.clear();
    update();
}

// --- Search -------------------------------------------------------------------------------

void FileManager::search() {
    const QString projectPath = project_->path();
    QStringList folders;
    if (!projectPath.isEmpty()) folders << QFileInfo(projectPath).absolutePath();
    startSearch(folders, {});
}

void FileManager::searchFolder(const QString& folder) {
    if (folder.isEmpty()) return;
    startSearch({folder}, QStringLiteral(" in ") + QDir::toNativeSeparators(folder));
}

void FileManager::startSearch(const QStringList& folders, const QString& where) {
    cancelSearch();
    refresh();  // (what is missing now)
    const QStringList missing = missingFiles();
    if (missing.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("No files are missing"));
        return;
    }
    missing::MissingFileSearch::Request request;
    request.missing = missing;
    request.folders = folders;
    if (browser_) request.index = browser_->index().backend().snapshot();
    const quint64 generation = ++searchGeneration_;
    searchWhere_ = where;
    searchedCount_ = static_cast<int>(missing.size());
    setSearching(true, QStringLiteral("Searching…"));
    // (Its thread posts to this object: it is waited for before this goes.)
    search_ = std::make_unique<missing::MissingFileSearch>(
        std::move(request),
        [this, generation](QMap<QString, QString> found, bool cancelled) {
            QMetaObject::invokeMethod(
                this, [this, generation, found, cancelled] { searchFinished(generation, found, cancelled); },
                Qt::QueuedConnection);
        },
        [this, generation](const QString& folder) {
            const QString status = QStringLiteral("Searching %1…").arg(QDir::toNativeSeparators(folder));
            QMetaObject::invokeMethod(
                this,
                [this, generation, status] {
                    if (generation == searchGeneration_ && searching_) setSearching(true, status);
                },
                Qt::QueuedConnection);
        });
}

void FileManager::cancelSearch() {
    if (!search_ && !searching_) return;
    ++searchGeneration_;  // (what it posts now is dropped)
    search_.reset();
    setSearching(false);
}

bool FileManager::waitForSearch(int timeoutMs) {
    const QDeadlineTimer deadline(timeoutMs);
    while (searching_ && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return !searching_;
}

void FileManager::searchFinished(quint64 generation, const QMap<QString, QString>& found, bool cancelled) {
    if (generation != searchGeneration_) return;
    search_.reset();
    setSearching(false);
    if (cancelled) return;
    // (What was found for files that are still missing.)
    QMap<QString, QString> moved;
    const QStringList missing = missingFiles();
    for (auto it = found.constBegin(); it != found.constEnd(); ++it) {
        if (missing.contains(it.key())) moved.insert(it.key(), it.value());
    }
    if (moved.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("No missing files were found%1").arg(searchWhere_));
        return;
    }
    for (const QString& path : moved) exists_.insert(sourceKey(path), true);
    editor_->relinkFiles(moved);
    update();
    const int count = static_cast<int>(moved.size());
    QString message;
    if (count < searchedCount_)
        message = QStringLiteral("Found %1 of %2 missing files").arg(count).arg(searchedCount_);
    else
        message = count == 1 ? QStringLiteral("Found the missing file")
                             : QStringLiteral("Found all %1 missing files").arg(count);
    Q_EMIT statusMessage(message + searchWhere_);
}

void FileManager::setSearching(bool searching, const QString& status) {
    if (searching == searching_ && status == searchStatus_) return;
    searching_ = searching;
    searchStatus_ = searching ? status : QString();
    Q_EMIT searchingChanged();
}

// --- What it does with a file -------------------------------------------------------------

bool FileManager::replace(const QString& path, const QString& with) {
    if (path.isEmpty() || with.isEmpty() || edits::samePath(path, with)) return false;
    if (!isAudioFile(with) || !QFileInfo(with).isFile()) {
        Q_EMIT statusMessage(QStringLiteral("%1 isn't an audio file").arg(QFileInfo(with).fileName()));
        return false;
    }
    FileUses frozen;
    const FileUses uses = changeableUses(*project_, fileUses(*project_, path), &frozen);
    if (uses.isEmpty()) {
        if (!frozen.isEmpty())
            Q_EMIT statusMessage(QStringLiteral("%1 plays only on frozen tracks: unfreeze them to replace it")
                                     .arg(QFileInfo(path).fileName()));
        return false;
    }
    const std::optional<AudioFileInfo> info = bridge_->fileInfo(with);  // (it says why it couldn't read it)
    if (!info || info->duration <= 0.0) return false;
    const QString text = QStringLiteral("Replace %1 with %2").arg(stem(path), stem(with));
    if (!editor_->replaceFile(uses, with, info->duration, text)) return false;
    exists_.insert(sourceKey(with), true);
    QString message =
        QStringLiteral("Replaced %1 with %2 (%3)").arg(stem(path), stem(with), usesText(*project_, uses));
    if (!frozen.isEmpty()) message += QStringLiteral("; frozen tracks' are left as they are");
    Q_EMIT statusMessage(message);
    return true;
}

bool FileManager::locate(const QString& path, const QString& found) {
    if (path.isEmpty() || found.isEmpty()) return false;
    if (!isAudioFile(found) || !QFileInfo(found).isFile()) {
        Q_EMIT statusMessage(QStringLiteral("%1 isn't an audio file").arg(QFileInfo(found).fileName()));
        return false;
    }
    if (edits::samePath(path, found)) return false;
    QStringList others = missingFiles();
    others.removeAll(path);
    const auto isFile = [](const QString& candidate) { return QFileInfo(candidate).isFile(); };
    QMap<QString, QString> moved = missing::moveAlong(path, found, others, isFile);
    moved.insert(path, found);
    for (const QString& to : moved) exists_.insert(sourceKey(to), true);
    if (!editor_->relinkFiles(moved, moved.size() == 1 ? QStringLiteral("Locate Missing File")
                                                       : QStringLiteral("Locate Missing Files")))
        return false;
    update();
    const int more = static_cast<int>(moved.size()) - 1;
    const QString name = QFileInfo(path).fileName();
    if (more == 0)
        Q_EMIT statusMessage(QStringLiteral("Located %1").arg(name));
    else
        Q_EMIT statusMessage(QStringLiteral("Located %1, and %2 more where it went").arg(name, fileCountText(more)));
    return true;
}

bool FileManager::hotSwap(const QString& path) { return hotSwap_ && hotSwap_->startFile(path); }

void FileManager::selectClips(const QString& path) {
    const ClipRefs clips = fileUses(*project_, path).clips;
    if (clips.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("No clips play %1").arg(QFileInfo(path).fileName()));
        return;
    }
    selection_->selectClips(*editor_, clips);
}

void FileManager::findSimilar(const QString& path) {
    if (browser_) browser_->findSimilar(path);
}

void FileManager::showInFolder(const QString& path) const {
    if (browser_) browser_->showInFolder(path);
}

QVariantList FileManager::actions(const QString& path) const {
    QVariantList list;
    const bool missing = isMissing(path);
    const FileUses uses = fileUses(*project_, path);
    list << action(QStringLiteral("hotSwap"), QStringLiteral("Hot-Swap"));
    list << QVariantMap() << action(QStringLiteral("replace"), QStringLiteral("Replace…"));
    if (missing) list << action(QStringLiteral("locate"), QStringLiteral("Locate…"));
    list << QVariantMap();
    if (!uses.clips.isEmpty()) list << action(QStringLiteral("selectClips"), QStringLiteral("Select Clips"));
    if (!missing) {
        if (canFindSimilar()) list << action(QStringLiteral("findSimilar"), QStringLiteral("Find Similar Sounds"));
        list << action(QStringLiteral("showInFolder"), QStringLiteral("Show in Folder"));
    }
    return list;
}

QString FileManager::dialogFolder(const QString& path) const {
    const QString folder = QFileInfo(path).absolutePath();
    if (!path.isEmpty() && QFileInfo(folder).isDir()) return folder;
    if (!project_->path().isEmpty()) return QFileInfo(project_->path()).absolutePath();
    return QDir::homePath();
}

bool FileManager::isAudioFile(const QString& path) const { return !path.isEmpty() && sub::app::isAudioFile(path); }

void FileManager::reveal(const QString& path) {
    if (!filter_.isEmpty() && model_->rowOf(path) < 0) setFilter({});
    Q_EMIT revealRequested(path);
}

}  // namespace sub::app
