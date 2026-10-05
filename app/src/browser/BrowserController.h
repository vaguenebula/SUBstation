#pragma once
// The browser on the left, without its widgets: categories and places, search,
// sorting, the list shown, preview requests, use counts. A QML panel binds to
// its properties and models and calls its invokables.
//
// "All" lists everything (built-in devices, plug-ins, presets and samples) at
// once; Ctrl+F searches there (focusSearch()). Enter or Down in the search
// field selects the first result (selectFirstResult()), and Enter on a result
// adds it, as a double-click does (activate()). A preview stops when the user
// clicks anywhere outside the browser (the UI calls stopPreview()).
//
// Lists are sorted by Rank (what is used most first) or Name; see
// BrowserSearch.h. An item counts as used when it is added to the project from
// here, by double-click, Enter or a drag that is dropped somewhere; Library
// keeps the counts. Uses are counted but the list is not re-sorted then: the
// selection stays put.
//
// Plug-ins are listed as the background scan finds them (Plug-ins ›
// Instruments / Audio Effects); the status shows the scan's progress, and the
// Plug-ins entry's tooltip lists the files that could not be read.
//
// Presets are listed by the device they are for (Presets › the device's name),
// as setPresets() hands them over (the preset index, ported separately).
//
// Every list is a search, run by the backend on its own thread (FileIndex):
// the controller asks, and shows the results when they come, a page at a time.
// A search asked for replaces the one running. When the index changes (files
// found while scanning, or changed in a place), the list is searched again and
// keeps its current item where it can (if it is within the first 5000 rows). A
// place with no search text shows its folder tree instead (folderModel), and
// results that arrive after the tree was shown are dropped.
//
// Settings (QSettings): the places (browser/places; the first start has the
// user's Music folder, or the home folder) and the sort (browser/sort).

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <vector>

#include "browser/BrowserItem.h"
#include "browser/BrowserSearch.h"
#include "browser/FileIndex.h"
#include "browser/ItemListModel.h"
#include "browser/Library.h"
#include "browser/SidebarModel.h"
#include "plugins/PluginIndex.h"

class QFileSystemModel;

namespace sub::app {

class BrowserController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY sortChanged)  // "rank" or "name"
    Q_PROPERTY(QVariantList sorts READ sorts CONSTANT)                   // [{value, label}]
    // The sidebar entry shown, as [kind] or [kind, sub] (see Scope). Setting
    // ["add"] asks the UI for a folder to add (addPlaceRequested) instead.
    Q_PROPERTY(QStringList scope READ scope WRITE setScope NOTIFY scopeChanged)
    Q_PROPERTY(QStringList places READ places NOTIFY placesChanged)
    Q_PROPERTY(sub::app::ItemListModel* results READ results CONSTANT)
    Q_PROPERTY(sub::app::SidebarModel* sidebar READ sidebar CONSTANT)
    Q_PROPERTY(sub::app::PluginIndex* pluginIndex READ pluginIndex CONSTANT)
    // A place with no search text: the UI shows its folder tree (folderModel,
    // from treeRootIndex) instead of the results.
    Q_PROPERTY(bool showingTree READ showingTree NOTIFY showingTreeChanged)
    Q_PROPERTY(QString treeRoot READ treeRoot NOTIFY showingTreeChanged)
    Q_PROPERTY(QAbstractItemModel* folderModel READ folderModel CONSTANT)
    Q_PROPERTY(QModelIndex treeRootIndex READ treeRootIndex NOTIFY showingTreeChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)  // results are on their way
    Q_PROPERTY(bool indexing READ indexing NOTIFY indexChanged)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY indexChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)  // the footer
    Q_PROPERTY(bool previewEnabled READ previewEnabled WRITE setPreviewEnabled NOTIFY previewEnabledChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewingChanged)  // started and not stopped since
    Q_PROPERTY(QString pluginsToolTip READ pluginsToolTip NOTIFY pluginsToolTipChanged)
    // The list's current row (the UI binds its view's current index to it).
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)

public:
    static constexpr int kKeepWithin = 5000;  // rows: a list searched again keeps its current item if this near the top

    struct Options {
        std::optional<QString> indexPath;  // none: FileIndex::defaultIndexPath(); empty: not saved
        QString libraryPath;               // empty: Library::defaultPath()
        Library::Clock clock;              // empty: the system's
        uint32_t maxFiles = FileIndex::kMaxFiles;
        uint32_t maxDepth = FileIndex::kMaxDepth;
        bool scanPlugins = true;  // scan the plug-ins when made (only new or changed files are read)
    };

    // `plugins`: the plug-in index to list (shared with the preferences); null:
    // one of its own.
    explicit BrowserController(PluginIndex* plugins = nullptr, QObject* parent = nullptr);
    BrowserController(PluginIndex* plugins, Options options, QObject* parent = nullptr);
    ~BrowserController() override;

    static QStringList defaultPlaces();

    QString searchText() const { return searchText_; }
    void setSearchText(const QString& text);
    QString sort() const { return sort_; }
    void setSort(const QString& sort);
    QVariantList sorts() const { return sortOrders(); }
    QStringList scope() const { return scope_.toList(); }
    void setScope(const QStringList& scope);
    const Scope& currentScope() const { return scope_; }
    QStringList places() const { return places_; }
    ItemListModel* results() const { return results_; }
    SidebarModel* sidebar() const { return sidebar_; }
    PluginIndex* pluginIndex() const { return plugins_; }
    bool showingTree() const { return showingTree_; }
    QString treeRoot() const { return treeRoot_; }
    QAbstractItemModel* folderModel() const;
    QModelIndex treeRootIndex() const;
    bool searching() const { return searching_ || searchTimer_.isActive(); }
    bool indexing() const { return index_->indexing(); }
    int fileCount() const { return index_->fileCount(); }
    QString statusText() const { return statusText_; }
    bool previewEnabled() const { return previewEnabled_; }
    void setPreviewEnabled(bool enabled);
    bool previewing() const { return previewing_; }
    QString pluginsToolTip() const { return pluginsToolTip_; }
    int currentRow() const { return currentRow_; }
    // The list's current item changed: an audio file is previewed.
    void setCurrentRow(int row);

    Library& library() { return *library_; }
    FileIndex& index() { return *index_; }

    // --- Places ---
    // Adds a place (indexing only it) and shows it; one that is listed already is just shown.
    Q_INVOKABLE void addPlace(const QString& folder);
    Q_INVOKABLE void removePlace(const QString& place);
    // Lists every folder of every place again (for drives that don't report changes).
    Q_INVOKABLE void rescan();
    // Reads every plug-in file again (also those that failed before).
    Q_INVOKABLE void rescanPlugins();

    // --- The list ---
    // Searches again for what the list should show.
    Q_INVOKABLE void refresh() { refresh(false); }
    // The first row on screen (for keeping the list's place when it is searched again).
    Q_INVOKABLE void setTopRow(int row) { topRow_ = row; }
    // Enter or Down in the search field: selects the first result (once the
    // results are there, if they are still on their way): selectRowRequested.
    Q_INVOKABLE void selectFirstResult();
    // Double-click or Enter on a result: it counts as used, and is added
    // (fileActivated, deviceActivated, pluginActivated or presetActivated).
    Q_INVOKABLE void activate(int row);
    // A drag of these rows was dropped somewhere that took it: they count as used.
    Q_INVOKABLE void dropped(const QList<int>& rows);
    // What a drag of these rows carries, as {mime type: text} (BrowserMime.h).
    Q_INVOKABLE QVariantMap dragData(const QList<int>& rows) const;

    // --- The folder tree ---
    // Its current file changed: an audio file is previewed.
    Q_INVOKABLE void treeCurrentChanged(const QString& path);
    // Double-click or Enter on a file in the tree: used, and added (fileActivated).
    Q_INVOKABLE void activateFile(const QString& path);
    // A drag of these files from the tree was dropped: they count as used.
    Q_INVOKABLE void droppedFiles(const QStringList& paths);

    // --- Preview ---
    Q_INVOKABLE void stopPreview();

    // Ctrl+F: search everything ("All"); the UI focuses the field on searchFocusRequested.
    Q_INVOKABLE void focusSearch();

    // --- Presets ---
    // The presets to list (from the preset index), the devices they are for in
    // the order the sidebar lists them, and the library's folder.
    void setPresets(std::vector<BrowserItem> items, QStringList groups, QString root);
    // The library folder a Presets entry lists (the library itself for the section), for Show in Folder.
    Q_INVOKABLE QString presetFolder(const QStringList& scope) const;
    // The system's file browser at a file (selected) or a folder.
    Q_INVOKABLE void showInFolder(const QString& path) const;
    // Moves a preset's file to the system's trash (the UI asks first): true if
    // it went (presetsChanged), else false and a statusMessage.
    Q_INVOKABLE bool deletePreset(const QString& path);
    // Gives a preset another name (the UI asks for it, offering its current
    // one): its new path (presetsChanged), or "" if it wasn't renamed (an empty
    // or unchanged name; or it couldn't be: a statusMessage says why).
    Q_INVOKABLE QString renamePreset(const QString& path, const QString& name);

    // --- Context menus ---
    // A sidebar entry's actions, in order, as {action, label} ({} for a
    // separator): "removePlace" for a place; "rescanPlugins" for Plug-ins;
    // "showPresetFolder" for Presets; then "addPlace" and "rescan" for all.
    Q_INVOKABLE QVariantList sidebarActions(const QStringList& scope) const;
    // A result's actions (presets only): "renamePreset" (the UI asks for the
    // name, then renamePreset()), "deletePreset", {}, "showInFolder".
    Q_INVOKABLE QVariantList resultActions(int row) const;

    // These items were used (added to the project) just now.
    void recordUse(const QStringList& keys);
    // Stops the backend's threads (saving the index) and, for its own plug-in
    // index, a scan that runs. Done when it is destroyed too.
    void shutdown();

Q_SIGNALS:
    void searchTextChanged();
    void sortChanged();
    void scopeChanged();
    void placesChanged();
    void showingTreeChanged();
    void searchingChanged();
    void indexChanged();
    void statusTextChanged();
    void previewEnabledChanged();
    void previewingChanged();
    void pluginsToolTipChanged();
    void currentRowChanged();

    void fileActivated(const QString& path);       // add the file to the arrangement
    void deviceActivated(const QString& kind);     // add the built-in device to the selected track
    void pluginActivated(const QVariantMap& plugin);  // add the plug-in (PluginRef fields) to the selected track
    void presetActivated(const QString& path);     // add the preset's device to the selected track
    void previewRequested(const QString& path);    // play the file through the engine's preview
    void previewStopped();
    void statusMessage(const QString& message);    // for the window's status line
    void addPlaceRequested();                      // "Add Folder…": the UI asks which folder (then addPlace())
    void presetsChanged();                         // a preset was deleted or renamed here: the preset index lists them again
    void searchFocusRequested();
    void selectRowRequested(int row);              // select and focus this row (of the list, or the tree's)
    void positionRestored(int currentRow, int topRow);  // a list searched again: scroll so `topRow` is at the top
    void resultsShown();                           // the results model shows a new search's results

private:
    struct Keep {
        std::optional<BrowserItem> item;
        int offset = 0;  // its row on screen
        int top = 0;     // the first row on screen
    };

    void refresh(bool keep);
    void buildSidebar(std::optional<Scope> select = std::nullopt);
    void setScopeValue(const Scope& scope);
    bool treeWanted() const;
    void showResults(const SearchResult& result);
    Keep listPosition() const;
    void restorePosition(const Keep& keep, const SearchResult& result);
    void selectFirstRow();
    void updateStatus();
    void setStatusText(const QString& text);
    void setSearching(bool searching);
    void indexUpdated();
    void pluginsUpdated();
    void scanProgress(int done, int total, const QString& path);
    void setPluginItems();
    void updatePluginsToolTip();
    QString presetsToolTip() const;
    void maybePreview(const QString& path);
    void savePlaces() const;

    QStringList places_;
    std::unique_ptr<Library> library_;
    FileIndex* index_ = nullptr;
    PluginIndex* plugins_ = nullptr;
    bool ownPlugins_ = false;
    ItemListModel* results_ = nullptr;
    SidebarModel* sidebar_ = nullptr;
    mutable QFileSystemModel* folderModel_ = nullptr;  // made when the UI first asks

    QString searchText_;
    QString sort_ = QStringLiteral("rank");
    Scope scope_;
    QTimer searchTimer_;  // merges changes to the search text that come together
    bool searching_ = false;    // results asked for and not shown yet
    bool selectFirst_ = false;  // when they come (Enter was pressed before)
    std::optional<Keep> keep_;  // where the list was, to go back to when they come
    bool restoring_ = false;
    bool showingTree_ = false;
    QString treeRoot_;
    int currentRow_ = -1;
    int topRow_ = 0;
    bool previewEnabled_ = true;
    bool previewing_ = false;
    QString statusText_;
    QString scanText_;  // the plug-in scan's progress
    QString pluginsToolTip_;
    std::vector<BrowserItem> presetItems_;
    QStringList presetGroups_;
    QString presetRoot_;
    bool shutDown_ = false;
};

}  // namespace sub::app
