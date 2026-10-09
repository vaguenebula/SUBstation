#include "browser/BrowserController.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QProcess>
#include <QSettings>
#include <QUrl>

#include <algorithm>
#include <utility>

#include "Text.h"
#include "browser/BrowserMime.h"
#include "browser/PathKeys.h"
#include "io/Presets.h"
#include "model/Errors.h"

namespace sub::app {

namespace {

constexpr const char* kPlacesKey = "browser/places";
constexpr const char* kSortKey = "browser/sort";
const QStringList kPluginCategories{QStringLiteral("Instruments"), QStringLiteral("Audio Effects")};

QString plural(qsizetype n, const QString& word) {
    return QStringLiteral("%1 %2%3").arg(n).arg(word, n != 1 ? QStringLiteral("s") : QString());
}

// Whether text has words (Python's text.split()).
bool hasWords(const QString& text) { return !browser::pySplit(text.toStdString()).empty(); }

std::vector<std::pair<BrowserItem, QString>> taggedByDetail(const std::vector<BrowserItem>& items) {
    std::vector<std::pair<BrowserItem, QString>> tagged;
    for (const BrowserItem& item : items) tagged.emplace_back(item, item.detail);
    return tagged;
}

}  // namespace

BrowserController::BrowserController(PluginIndex* plugins, QObject* parent)
    : BrowserController(plugins, Options(), parent) {}

BrowserController::BrowserController(PluginIndex* plugins, Options options, QObject* parent) : QObject(parent) {
    QSettings settings;
    for (const QString& place : settings.value(QString::fromLatin1(kPlacesKey)).toStringList())
        if (!place.isEmpty()) places_ << normalPath(place);
    if (places_.isEmpty()) places_ = defaultPlaces();
    library_ = std::make_unique<Library>(options.libraryPath, options.clock);

    index_ = new FileIndex(this, options.indexPath, options.maxFiles, options.maxDepth);
    connect(index_, &FileIndex::updated, this, &BrowserController::indexUpdated);
    connect(index_, &FileIndex::results, this, &BrowserController::showResults);
    index_->setItems(kBuiltinGroup, taggedByDetail(builtinItems()));
    index_->setUsage(*library_);

    ownPlugins_ = plugins == nullptr;
    plugins_ = plugins ? plugins : new PluginIndex(this);
    connect(plugins_, &PluginIndex::updated, this, &BrowserController::pluginsUpdated);
    connect(plugins_, &PluginIndex::progress, this, &BrowserController::scanProgress);
    connect(plugins_, &PluginIndex::statusMessage, this, &BrowserController::statusMessage);
    setPluginItems();
    index_->setItems(kPresetsGroup, {});

    results_ = new ItemListModel(library_.get(), this);
    sidebar_ = new SidebarModel(this);
    const QString stored = settings.value(QString::fromLatin1(kSortKey)).toString();
    if (isSortOrder(stored)) sort_ = stored;

    // Searching doesn't hold up the UI, so there is no need to wait for typing
    // to pause; this only merges changes that come together.
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(0);
    connect(&searchTimer_, &QTimer::timeout, this, [this] { refresh(false); });

    // Find Similar: the sounds analysed are this index's files.
    similarity_ = options.similarity;
    refineTimer_.setSingleShot(true);
    refineTimer_.setInterval(kRefineMs);
    if (similarity_) {
        similarity_->setLibrary(index_);
        connect(similarity_, &SoundSimilarity::found, this, &BrowserController::similarFound);
        connect(similarity_, &SoundSimilarity::progressChanged, this, &BrowserController::similarProgress);
        connect(&refineTimer_, &QTimer::timeout, this, [this] {
            if (!similar_ || !similarity_) return;
            refinedAt_ = similarity_->analysedFiles();
            similar_->generation = similarity_->find(similar_->path, similar_->start, similar_->length);
        });
    }

    updatePluginsToolTip();
    index_->setPlaces(places_);
    buildSidebar();
    if (options.scanPlugins) plugins_->scan();
}

BrowserController::~BrowserController() { shutdown(); }

QStringList BrowserController::defaultPlaces() {
    const QString music = QDir::home().filePath(QStringLiteral("Music"));
    return {normalPath(QFileInfo(music).isDir() ? music : QDir::homePath())};
}

// --- Properties ---------------------------------------------------------------------------

void BrowserController::setSearchText(const QString& text) {
    if (text == searchText_) return;
    searchText_ = text;
    Q_EMIT searchTextChanged();
    searchTimer_.start();
    Q_EMIT searchingChanged();
}

void BrowserController::setSort(const QString& sort) {
    if (!isSortOrder(sort)) return;  // ("similar" only comes with findSimilar())
    const bool leaving = similar_.has_value();
    if (leaving) leaveSimilar();
    if (sort != sort_) {
        sort_ = sort;
        QSettings().setValue(QString::fromLatin1(kSortKey), sort_);
        Q_EMIT sortChanged();
    } else if (!leaving) {
        return;
    }
    refresh(false);
}

QVariantList BrowserController::sorts() const {
    QVariantList list = sortOrders();
    if (similar_)
        list.prepend(QVariantMap{{QStringLiteral("value"), kSimilarSort}, {QStringLiteral("label"), QStringLiteral("Similarity")}});
    return list;
}

void BrowserController::setScope(const QStringList& list) {
    const Scope scope = Scope::fromList(list);
    if (scope.kind == QStringLiteral("add")) {
        Q_EMIT addPlaceRequested();
        return;
    }
    if (scope == scope_ || sidebar_->find(scope) < 0) return;
    setScopeValue(scope);
    if (similar_ && !showsAudioOnly()) leaveSimilar();  // (similar sounds are files: Samples or a place)
    refresh(false);
}

bool BrowserController::showsAudioOnly() const {
    return scope_.kind == QStringLiteral("samples") || scope_.kind == QStringLiteral("place");
}

void BrowserController::setScopeValue(const Scope& scope) {
    if (scope == scope_) return;
    scope_ = scope;
    Q_EMIT scopeChanged();
}

QAbstractItemModel* BrowserController::folderModel() const {
    if (!folderModel_) {
        auto* model = new QFileSystemModel(const_cast<BrowserController*>(this));
        model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
        QStringList filters;
        for (const QString& extension : FileIndex::audioExtensions()) filters << QLatin1Char('*') + extension;
        model->setNameFilters(filters);
        model->setNameFilterDisables(false);
        if (showingTree_) model->setRootPath(treeRoot_);
        folderModel_ = model;
    }
    return folderModel_;
}

QModelIndex BrowserController::treeRootIndex() const {
    if (!showingTree_) return {};
    folderModel();
    return folderModel_->index(treeRoot_);
}

void BrowserController::setPreviewEnabled(bool enabled) {
    if (enabled == previewEnabled_) return;
    previewEnabled_ = enabled;
    Q_EMIT previewEnabledChanged();
    if (!enabled) stopPreview();
}

void BrowserController::setCurrentRow(int row) {
    if (row == currentRow_) return;
    currentRow_ = row;
    Q_EMIT currentRowChanged();
    const BrowserItem* item = results_->item(row);
    if (item && item->kind == ItemKind::Audio && !restoring_) maybePreview(item->path);
}

void BrowserController::choose(int row) {
    const BrowserItem* item = results_->item(row);
    if (item && item->kind == ItemKind::Audio) Q_EMIT fileChosen(item->path);
}

// --- Sidebar ------------------------------------------------------------------------------

void BrowserController::buildSidebar(std::optional<Scope> select) {
    std::vector<SidebarModel::Entry> entries;
    auto section = [&](const QString& title) { entries.push_back({title, Scope{{}, {}}, true, 0, {}, {}, true}); };
    auto entry = [&](const QString& title, const Scope& scope, const QString& icon, const QString& toolTip = {},
                     int depth = 0) { entries.push_back({title, scope, false, depth, icon, toolTip, false}); };
    const QString plugin = QStringLiteral("plugin");
    section(QStringLiteral("CATEGORIES"));
    entry(QStringLiteral("All"), {QStringLiteral("all"), {}}, QStringLiteral("search"));
    entry(QStringLiteral("Samples"), {QStringLiteral("samples"), {}}, QStringLiteral("waveform"));
    entry(QStringLiteral("Built-in"), {QStringLiteral("builtin"), {}}, plugin);
    for (const QString& name : builtinCategoryNames()) entry(name, {QStringLiteral("builtin"), name}, plugin, {}, 1);
    entry(QStringLiteral("Plug-ins"), {QStringLiteral("plugins"), {}}, plugin, pluginsToolTip_);
    for (const QString& name : kPluginCategories) entry(name, {QStringLiteral("plugins"), name}, plugin, {}, 1);
    entry(QStringLiteral("Presets"), {QStringLiteral("presets"), {}}, QStringLiteral("preset"), presetsToolTip());
    for (const QString& name : presetGroups_)
        entry(name, {QStringLiteral("presets"), name}, QStringLiteral("preset"), {}, 1);
    section(QStringLiteral("PLACES"));
    for (const QString& place : places_) {
        const QString name = QFileInfo(place).fileName();
        entry(name.isEmpty() ? QDir::toNativeSeparators(place) : name, {QStringLiteral("place"), place},
              QStringLiteral("folder"), QDir::toNativeSeparators(place));
    }
    entries.push_back({QStringLiteral("Add Folder…"), {QStringLiteral("add"), {}}, false, 0, {}, {}, true});
    sidebar_->setEntries(std::move(entries));

    Scope target{QStringLiteral("samples"), {}};
    if (select && sidebar_->find(*select) >= 0) target = *select;
    setScopeValue(target);
    if (similar_ && !showsAudioOnly()) leaveSimilar();
    refresh(false);
}

void BrowserController::addPlace(const QString& folder) {
    const QString place = folder.isEmpty() ? QString() : normalPath(folder);
    if (!place.isEmpty() && !places_.contains(place)) {
        places_ << place;
        savePlaces();
        index_->setPlaces(places_);  // scans the new one only
        Q_EMIT placesChanged();
        buildSidebar(Scope{QStringLiteral("place"), place});
    } else {
        buildSidebar(scope_);
    }
}

void BrowserController::removePlace(const QString& place) {
    if (!places_.contains(place)) return;
    places_.removeAll(place);
    savePlaces();
    index_->setPlaces(places_);
    Q_EMIT placesChanged();
    buildSidebar();
}

void BrowserController::savePlaces() const { QSettings().setValue(QString::fromLatin1(kPlacesKey), places_); }

void BrowserController::rescan() { index_->rebuild(places_); }

void BrowserController::rescanPlugins() { plugins_->scan(true); }

void BrowserController::focusSearch() {
    const Scope all{QStringLiteral("all"), {}};
    if (scope_ != all) buildSidebar(all);
    Q_EMIT searchFocusRequested();
}

// --- Plug-ins -----------------------------------------------------------------------------

void BrowserController::scanProgress(int done, int total, const QString& path) {
    scanText_ = QStringLiteral("Scanning plug-ins %1/%2: %3").arg(done + 1).arg(total).arg(QFileInfo(path).completeBaseName());
    if (scope_.kind == QStringLiteral("plugins")) setStatusText(scanText_);
}

void BrowserController::pluginsUpdated() {
    if (!plugins_->scanning()) {
        scanText_.clear();
        const int failures = plugins_->failureCount();
        if (failures)
            Q_EMIT statusMessage(plural(failures, QStringLiteral("plug-in file")) +
                               QStringLiteral(" could not be read (hover over Plug-ins in the browser for details)."));
    }
    setPluginItems();
    updatePluginsToolTip();
    refresh(true);
}

void BrowserController::setPluginItems() {
    std::vector<std::pair<BrowserItem, QString>> items;
    for (const PluginInfo& plugin : plugins_->plugins()) {
        BrowserItem item = pluginItem(plugin);
        QString tag = pluginTag(item);
        items.emplace_back(std::move(item), std::move(tag));
    }
    index_->setItems(kPluginsGroup, items);
}

void BrowserController::updatePluginsToolTip() {
    const std::vector<ScanFailure>& failures = plugins_->failures();
    QStringList lines{QStringLiteral("VST3 plug-ins")};
    if (!failures.empty()) {
        lines << QString() << QStringLiteral("Could not be read:");
        for (size_t i = 0; i < failures.size() && i < 30; ++i)
            lines << QFileInfo(failures[i].path).fileName() + QStringLiteral(": ") + failures[i].reason;
        if (failures.size() > 30) lines << QStringLiteral("...and %1 more").arg(failures.size() - 30);
    }
    const QString toolTip = lines.join(QLatin1Char('\n'));
    sidebar_->setToolTip({QStringLiteral("plugins"), {}}, toolTip);
    if (toolTip == pluginsToolTip_) return;
    pluginsToolTip_ = toolTip;
    Q_EMIT pluginsToolTipChanged();
}

// --- Presets ------------------------------------------------------------------------------

void BrowserController::setPresets(std::vector<BrowserItem> items, QStringList groups, QString root) {
    presetItems_ = std::move(items);
    presetRoot_ = std::move(root);
    index_->setItems(kPresetsGroup, taggedByDetail(presetItems_));
    const Scope presets{QStringLiteral("presets"), {}};
    const QStringList shown = sidebar_->children(presets);
    presetGroups_ = std::move(groups);
    if (shown != presetGroups_) {  // the sidebar lists the groups: made again
        Scope scope = scope_;
        if (scope.kind == QStringLiteral("presets") && !scope.sub.isEmpty() && !presetGroups_.contains(scope.sub))
            scope = presets;  // (its group went)
        buildSidebar(scope);  // (it searches again)
    } else {
        sidebar_->setToolTip(presets, presetsToolTip());
        refresh(true);
    }
}

QString BrowserController::presetsToolTip() const {
    QString toolTip = QStringLiteral("Saved with a device's save button");
    if (!presetRoot_.isEmpty()) toolTip += QLatin1Char('\n') + QDir::toNativeSeparators(presetRoot_);
    return toolTip;
}

QString BrowserController::presetFolder(const QStringList& list) const {
    const Scope scope = Scope::fromList(list);
    if (!scope.sub.isEmpty()) {
        for (const BrowserItem& item : presetItems_)
            if (item.detail == scope.sub) return QFileInfo(item.path).absolutePath();
    }
    return presetRoot_;
}

void BrowserController::showInFolder(const QString& path) const {
#ifdef _WIN32
    if (QFileInfo(path).isFile()) {
        QProcess::startDetached(QStringLiteral("explorer"),
                                {QStringLiteral("/select,") + QDir::toNativeSeparators(normalPath(path))});
        return;
    }
#endif
    const QString folder = QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

bool BrowserController::deletePreset(const QString& path) {
    if (!QFile::moveToTrash(path)) {
        Q_EMIT statusMessage(QStringLiteral("Could not delete the preset %1.").arg(QFileInfo(path).completeBaseName()));
        return false;
    }
    Q_EMIT presetsChanged();
    return true;
}

QString BrowserController::renamePreset(const QString& path, const QString& given) {
    const QString name = given.trimmed();
    if (name.isEmpty() || name == QFileInfo(path).completeBaseName()) return {};
    QString renamed;
    try {
        renamed = sub::app::renamePreset(path, name);
    } catch (const EditError& error) {
        Q_EMIT statusMessage(QStringLiteral("Could not rename the preset: ") + error.message());
        return {};
    }
    Q_EMIT presetsChanged();
    return renamed;
}

// --- Context menus ------------------------------------------------------------------------

QVariantList BrowserController::sidebarActions(const QStringList& list) const {
    const Scope scope = Scope::fromList(list);
    auto action = [](const char* id, const QString& label) {
        return QVariantMap{{QStringLiteral("action"), QString::fromLatin1(id)}, {QStringLiteral("label"), label}};
    };
    QVariantList actions;
    if (scope.kind == QStringLiteral("place")) actions << action("removePlace", QStringLiteral("Remove from Places"));
    if (scope.kind == QStringLiteral("plugins")) actions << action("rescanPlugins", QStringLiteral("Rescan Plug-ins")) << QVariantMap();
    if (scope.kind == QStringLiteral("presets")) actions << action("showPresetFolder", QStringLiteral("Show in Folder")) << QVariantMap();
    actions << action("addPlace", QStringLiteral("Add Folder…")) << action("rescan", QStringLiteral("Rescan"));
    return actions;
}

QVariantList BrowserController::resultActions(int row) const {
    const BrowserItem* item = results_->item(row);
    auto action = [](const char* id, const QString& label) {
        return QVariantMap{{QStringLiteral("action"), QString::fromLatin1(id)}, {QStringLiteral("label"), label}};
    };
    if (item && item->kind == ItemKind::Audio) {
        QVariantList actions;
        if (similarity_) actions << action("findSimilar", QStringLiteral("Find Similar Sounds")) << QVariantMap();
        actions << action("showInFolder", QStringLiteral("Show in Folder"));
        return actions;
    }
    if (!item || item->kind != ItemKind::Preset) return {};
    return {action("renamePreset", QStringLiteral("Rename…")), action("deletePreset", QStringLiteral("Delete")), QVariantMap(),
            action("showInFolder", QStringLiteral("Show in Folder"))};
}

// --- Find Similar -------------------------------------------------------------------------

QString BrowserController::similarTo() const { return similar_ ? similar_->path : QString(); }

QString BrowserController::similarName() const { return similar_ ? QFileInfo(similar_->path).fileName() : QString(); }

void BrowserController::findSimilar(const QString& path, double start, double length) {
    if (!similarity_ || path.isEmpty()) return;
    if (!similar_) sortBeforeSimilar_ = sort_;
    similar_ = Similar{normalPath(path), std::max(0.0, start), length, 0, {}};
    refinedAt_ = similarity_->analysedFiles();
    similar_->generation = similarity_->find(similar_->path, similar_->start, similar_->length);
    refineTimer_.stop();
    if (!showsAudioOnly()) setScopeValue({QStringLiteral("samples"), {}});
    searchTimer_.stop();
    if (!searchText_.isEmpty()) {
        searchText_.clear();
        Q_EMIT searchTextChanged();
    }
    if (sort_ != kSimilarSort) {
        sort_ = kSimilarSort;
        Q_EMIT sortChanged();
    }
    Q_EMIT sortsChanged();
    Q_EMIT similarChanged();
    keep_.reset();
    selectFirst_ = false;
    setSearching(true);  // (the list is searched when the sounds are found)
    setStatusText(QStringLiteral("Finding sounds like %1…").arg(similarName()));
}

void BrowserController::clearSimilar() {
    if (!similar_) return;
    leaveSimilar();
    refresh(false);
}

void BrowserController::leaveSimilar() {
    similar_.reset();
    refineTimer_.stop();
    sort_ = isSortOrder(sortBeforeSimilar_) ? sortBeforeSimilar_ : QStringLiteral("rank");
    Q_EMIT sortChanged();
    Q_EMIT sortsChanged();
    Q_EMIT similarChanged();
}

void BrowserController::similarFound(const SimilarSounds& result) {
    if (!similar_ || result.generation() != similar_->generation) return;
    const bool again = !similar_->result.isNull();  // searched again as more was analysed: keep the list's place
    similar_->result = result;
    if (!result.error().isEmpty() && !again)
        Q_EMIT statusMessage(QStringLiteral("Could not analyse %1: %2").arg(similarName(), result.error()));
    refresh(again);
}

void BrowserController::similarProgress() {
    if (!similar_ || !similarity_) return;
    // More of the library analysed: the list is searched again (now and then).
    if (similarity_->analysedFiles() != refinedAt_ && !similar_->result.isNull() && !refineTimer_.isActive())
        refineTimer_.start();
    if (!searching()) updateStatus();
}

// --- The list -----------------------------------------------------------------------------

bool BrowserController::treeWanted() const {
    return scope_.kind == QStringLiteral("place") && !hasWords(searchText_) && !similar_;
}

void BrowserController::refresh(bool keep) {
    searchTimer_.stop();
    if (treeWanted()) {
        selectFirst_ = false;
        keep_.reset();
        treeRoot_ = scope_.sub;
        showingTree_ = true;
        if (folderModel_) folderModel_->setRootPath(treeRoot_);
        Q_EMIT showingTreeChanged();
        setSearching(false);
        setStatusText(QDir::toNativeSeparators(treeRoot_));
        return;
    }
    if (similar_ && similar_->result.isNull()) {  // the similar sounds are still being found
        setSearching(true);
        return;
    }
    const ScopeQuery query = scopeQuery(scope_);
    if (keep && !showingTree_) {
        if (!keep_) keep_ = listPosition();  // (a search replacing one that was to keep it keeps that)
    } else {
        keep_.reset();
    }
    index_->search(searchText_, sort_, library_->now(), query.groups, query.tag, query.placePrefix,
                   similar_ ? similar_->result.scorer() : nullptr);
    setSearching(true);
}

void BrowserController::setSearching(bool searching) {
    searching_ = searching;
    Q_EMIT searchingChanged();
}

void BrowserController::indexUpdated() {
    Q_EMIT indexChanged();
    if (treeWanted()) return;  // the tree shows the folder itself
    refresh(true);
}

void BrowserController::showResults(const SearchResult& result) {
    if (!searching_) return;  // the tree was shown meanwhile
    const std::optional<Keep> keep = std::exchange(keep_, std::nullopt);
    restoring_ = true;
    results_->setSource(result);
    currentRow_ = -1;
    if (showingTree_) {
        showingTree_ = false;
        Q_EMIT showingTreeChanged();
    }
    if (keep) restorePosition(*keep, result);
    Q_EMIT currentRowChanged();
    restoring_ = false;
    setSearching(false);
    updateStatus();
    Q_EMIT resultsShown();
    if (selectFirst_) {
        selectFirst_ = false;
        selectFirstRow();
    }
}

BrowserController::Keep BrowserController::listPosition() const {
    Keep keep;
    if (const BrowserItem* item = results_->item(currentRow_)) keep.item = *item;
    keep.offset = keep.item && topRow_ >= 0 ? currentRow_ - topRow_ : 0;
    keep.top = std::max(topRow_, 0);
    return keep;
}

void BrowserController::restorePosition(const Keep& keep, const SearchResult& result) {
    const int row = keep.item ? result.find(*keep.item) : -1;
    int top = keep.top;
    if (row >= 0 && row < kKeepWithin) {
        top = std::max(0, row - keep.offset);
        results_->ensureRows(row + 1);
        currentRow_ = row;
    }
    if (top > 0) {
        results_->ensureRows(top + 1);
        top = std::min(top, results_->rowCount() - 1);
    }
    topRow_ = std::max(top, 0);
    Q_EMIT positionRestored(currentRow_, topRow_);
}

void BrowserController::selectFirstResult() {
    if (searchTimer_.isActive()) refresh(false);
    if (searching_) {  // Enter typed before the results came: select when they do
        selectFirst_ = true;
        return;
    }
    selectFirstRow();
}

void BrowserController::selectFirstRow() {
    if (showingTree_) {
        Q_EMIT selectRowRequested(0);  // the tree's first entry (the UI's view)
        return;
    }
    if (results_->rowCount() == 0) return;
    setCurrentRow(0);
    Q_EMIT selectRowRequested(0);
}

void BrowserController::updateStatus() {
    const QString kind = scope_.kind;
    const int count = results_->total();
    const QString items = plural(count, QStringLiteral("item"));
    if (similar_) {
        if (!similar_->result.error().isEmpty()) {
            setStatusText(QStringLiteral("Could not analyse %1").arg(similarName()));
        } else {
            QString text = plural(count, QStringLiteral("sound")) + QStringLiteral(" like ") + similarName();
            // (The sound similarity may be gone already when the session goes.)
            const int done = similarity_ ? similarity_->analysedFiles() + similarity_->failedFiles() : 0;
            if (similarity_ && similarity_->analysing() && done < similarity_->libraryFiles())  // (not while only checking files)
                text += QStringLiteral(" (analysing %1 of %2…)").arg(done).arg(similarity_->libraryFiles());
            setStatusText(text);
        }
    } else if (kind == QStringLiteral("presets") && presetItems_.empty()) {
        setStatusText(QStringLiteral("No presets yet: save one with a device's save button"));
    } else if (kind == QStringLiteral("presets")) {
        setStatusText(plural(count, QStringLiteral("preset")));
    } else if (kind == QStringLiteral("plugins")) {
        const int failures = plugins_->failureCount();
        if (plugins_->scanning()) {
            setStatusText(scanText_.isEmpty() ? QStringLiteral("Scanning plug-ins…") : scanText_);
        } else if (plugins_->plugins().empty()) {
            setStatusText(QStringLiteral("No VST3 plug-ins found"));
        } else {
            const QString note = failures && !hasWords(searchText_)
                                     ? QStringLiteral(", %1 could not be read").arg(failures)
                                     : QString();
            setStatusText(plural(count, QStringLiteral("plug-in")) + note);
        }
    } else if (kind == QStringLiteral("all") && (index_->indexing() || plugins_->scanning())) {
        const QString busy = index_->indexing() ? QStringLiteral("Indexing") : QStringLiteral("Scanning plug-ins");
        setStatusText(items + QStringLiteral(" (") + busy + QStringLiteral("…)"));
    } else if (kind != QStringLiteral("builtin") && index_->indexing()) {
        setStatusText(items + QStringLiteral(" (Indexing…)"));
    } else {
        setStatusText(items);
    }
}

void BrowserController::setStatusText(const QString& text) {
    if (text == statusText_) return;
    statusText_ = text;
    Q_EMIT statusTextChanged();
}

// --- Using items --------------------------------------------------------------------------

void BrowserController::recordUse(const QStringList& keys) {
    if (keys.isEmpty()) return;
    library_->recordUse(keys);
    index_->setUsage(*library_);
    results_->usesChanged();
}

void BrowserController::activate(int row) {
    const BrowserItem* found = results_->item(row);
    if (!found) return;
    const BrowserItem item = *found;
    recordUse({item.key()});
    switch (item.kind) {
        case ItemKind::Audio: Q_EMIT fileActivated(item.path); break;
        case ItemKind::Device: Q_EMIT deviceActivated(item.path); break;
        case ItemKind::Plugin:
            if (item.plugin) Q_EMIT pluginActivated(item.plugin->toRef());
            break;
        case ItemKind::Preset: Q_EMIT presetActivated(item.path); break;
    }
}

void BrowserController::dropped(const QList<int>& rows) {
    QStringList keys;
    for (const BrowserItem& item : results_->items(rows)) keys << item.key();
    recordUse(keys);
}

QVariantMap BrowserController::dragData(const QList<int>& rows) const { return browserDragData(results_->items(rows)); }

void BrowserController::treeCurrentChanged(const QString& path) {
    if (!path.isEmpty() && !QFileInfo(path).isDir()) maybePreview(path);
}

void BrowserController::chooseFile(const QString& path) {
    if (!path.isEmpty() && FileIndex::isAudioFile(path) && !QFileInfo(path).isDir()) Q_EMIT fileChosen(path);
}

void BrowserController::activateFile(const QString& path) {
    if (path.isEmpty() || QFileInfo(path).isDir()) return;
    recordUse({audioKey(path)});
    Q_EMIT fileActivated(path);
}

void BrowserController::droppedFiles(const QStringList& paths) {
    QStringList keys;
    for (const QString& path : paths)
        if (!QFileInfo(path).isDir()) keys << audioKey(path);
    recordUse(keys);
}

// --- Preview ------------------------------------------------------------------------------

void BrowserController::maybePreview(const QString& path) {
    if (!previewEnabled_ || !FileIndex::isAudioFile(path)) return;
    if (!previewing_) {
        previewing_ = true;
        Q_EMIT previewingChanged();
    }
    Q_EMIT previewRequested(path);
}

void BrowserController::stopPreview() {
    if (previewing_) {
        previewing_ = false;
        Q_EMIT previewingChanged();
    }
    Q_EMIT previewStopped();
}

void BrowserController::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    searchTimer_.stop();
    refineTimer_.stop();
    if (similarity_) similarity_->setLibrary(nullptr);  // (before the index closes: it reads the index's files)
    index_->close();
    if (ownPlugins_) plugins_->wait();
}

}  // namespace sub::app
