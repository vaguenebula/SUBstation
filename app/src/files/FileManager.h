#pragma once
// The File Manager (View › File Manager, on the right of the window, as
// Ableton's): Session.files.
//
// It lists the files the project plays (ProjectFiles.h), each with its folder,
// what plays it and whether it is missing (not there any more: its clips show
// "Missing file" and are silent); the missing first, then by name. Typing in
// its field filters the list by name and folder. What it does with a file:
//
// - Search: the missing files are looked for by name (MissingFiles.h), in the
//   background, in the project's folder and in the browser's places (its
//   index); Search Folder… in a folder the user picks (and the places). What
//   is found is put in place in one undo step (Locate Missing Files), and the
//   status line says how many were found.
// - Locate…: the user points at a missing file; those missing from its folder,
//   or from folders beside it, are looked for where it went too.
// - Replace…: another audio file in its place, wherever it plays (the
//   project's clips and samplers), as dropping a file onto its row does; one
//   undo step (ProjectEditor::replaceFile). What is on frozen tracks is left
//   as it is.
// - Hot-Swap: a hot swap (HotSwap.h) of every clip and sampler playing it (the
//   browser lists the sounds most like it).
// - Select Clips, Find Similar Sounds, Show in Folder.
//
// Whether a file is there is checked once for each file it lists (a file it
// hasn't seen yet, or one the engine couldn't decode), and again for all of
// them on refresh() (the panel showing, its Refresh). The list follows the
// project (after the edit that changed it, once).

#include <QAbstractListModel>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "files/ProjectFiles.h"

namespace sub::app {

class BrowserController;
class EngineBridge;
class HotSwap;
class Project;
class ProjectEditor;
class Selection;

namespace missing {
class MissingFileSearch;
}

// The File Manager's rows (FileManager::files).
class FileListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        NameRole,     // the file's name ("Kick 01.wav")
        FolderRole,   // its folder, as the system writes it
        MissingRole,  // not there
        UsesRole,     // "3 clips, Sampler"
        FrozenRole,   // some of it is on frozen tracks
    };

    struct Row {
        QString path;
        QString name;
        QString folder;
        QString uses;
        bool missing = false;
        bool frozen = false;

        friend bool operator==(const Row&, const Row&) = default;
    };

    using QAbstractListModel::QAbstractListModel;

    int count() const { return static_cast<int>(rows_.size()); }
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const std::vector<Row>& rows() const { return rows_; }
    // The rows to show (the same ones: nothing happens).
    void setRows(std::vector<Row> rows);
    // A row as {path, name, folder, uses, missing, frozen} ({} if there is none).
    Q_INVOKABLE QVariantMap get(int row) const;
    // The row of a file (-1: none).
    Q_INVOKABLE int rowOf(const QString& path) const;

Q_SIGNALS:
    void countChanged();

private:
    std::vector<Row> rows_;
};

class FileManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(sub::app::FileListModel* files READ files CONSTANT)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY filesChanged)        // every file (not only those shown)
    Q_PROPERTY(int missingCount READ missingCount NOTIFY filesChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY filesChanged)        // "12 files, 2 missing"
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY searchingChanged)  // "Searching D:\Samples…"
    Q_PROPERTY(bool canFindSimilar READ canFindSimilar CONSTANT)

public:
    FileManager(Project* project, ProjectEditor* editor, Selection* selection, EngineBridge* bridge,
                BrowserController* browser, HotSwap* hotSwap, QObject* parent = nullptr);
    ~FileManager() override;

    FileListModel* files() const { return model_; }
    int fileCount() const { return static_cast<int>(files_.size()); }
    int missingCount() const;
    QString summary() const;
    QString filter() const { return filter_; }
    void setFilter(const QString& filter);
    bool searching() const { return searching_; }
    QString searchStatus() const { return searchStatus_; }
    bool canFindSimilar() const;

    // The files missing now, in the list's order.
    QStringList missingFiles() const;
    bool isMissing(const QString& path) const;
    // The list as the project is now (it follows the project by itself, after each edit).
    void update();

    // Checks again which files are there.
    Q_INVOKABLE void refresh();
    // Looks for the missing files in the project's folder and the browser's
    // places; searchFolder() in `folder` and the places.
    Q_INVOKABLE void search();
    Q_INVOKABLE void searchFolder(const QString& folder);
    Q_INVOKABLE void cancelSearch();
    // Until a search ends (the tests).
    bool waitForSearch(int timeoutMs = 10000);
    // `with` in the place of `path`, everywhere it plays (but frozen tracks).
    // Whether anything changed.
    Q_INVOKABLE bool replace(const QString& path, const QString& with);
    // The missing file `path` is at `found`; others missing from its folder (or
    // beside it) are looked for where it went. Whether anything changed.
    Q_INVOKABLE bool locate(const QString& path, const QString& found);
    // A hot swap of every clip and sampler playing it (the browser lists the
    // sounds most like it).
    Q_INVOKABLE bool hotSwap(const QString& path);
    // Its clips, selected in the arrangement.
    Q_INVOKABLE void selectClips(const QString& path);
    Q_INVOKABLE void findSimilar(const QString& path);
    Q_INVOKABLE void showInFolder(const QString& path) const;
    // A row's menu, as {action, label} ({} for a separator): "hotSwap", {},
    // "replace", "locate" (a missing file's), {}, "selectClips",
    // "findSimilar", "showInFolder" (but a missing file's).
    Q_INVOKABLE QVariantList actions(const QString& path) const;
    // Where Replace… and Locate…'s file dialogs start: the file's folder if it
    // is there, else the project's, else the home folder.
    Q_INVOKABLE QString dialogFolder(const QString& path) const;
    // Show in File Manager (an audio clip's menu): the panel shows, its row selected.
    Q_INVOKABLE void reveal(const QString& path);
    // Whether a file dropped onto a row can replace it: an audio file (by its name).
    Q_INVOKABLE bool isAudioFile(const QString& path) const;

Q_SIGNALS:
    void filesChanged();
    void filterChanged();
    void searchingChanged();
    void statusMessage(const QString& message);
    void revealRequested(const QString& path);

private:
    void scheduleUpdate();
    void showRows();
    bool exists(const QString& path) const;
    void startSearch(const QStringList& folders, const QString& where);
    void searchFinished(quint64 generation, const QMap<QString, QString>& found, bool cancelled);
    void setSearching(bool searching, const QString& status = {});

    Project* project_;
    ProjectEditor* editor_;
    Selection* selection_;
    EngineBridge* bridge_;
    QPointer<BrowserController> browser_;
    QPointer<HotSwap> hotSwap_;
    FileListModel* model_;
    std::vector<ProjectFile> files_;
    mutable QHash<QString, bool> exists_;  // by pathIdentity
    QString filter_;
    QTimer updateTimer_;
    std::unique_ptr<missing::MissingFileSearch> search_;
    quint64 searchGeneration_ = 0;
    QString searchWhere_;  // " in D:\Samples" ("": the project's folder and the places)
    int searchedCount_ = 0;
    bool searching_ = false;
    QString searchStatus_;
};

}  // namespace sub::app
