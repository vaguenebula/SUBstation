#include "arrangement/TrackHeaderItem.h"

#include "arrangement/Arrangement.h"
#include "arrangement/Envelopes.h"
#include "audio/EngineBridge.h"
#include "controls/Meter.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "model/Routing.h"
#include "model/Timebase.h"
#include "session/ArrangementActions.h"
#include "session/Selection.h"
#include "sg/SgPainter.h"
#include "theme/Icons.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPolygonF>
#include <QQuickWindow>
#include <QStyleHints>
#include <QVariantMap>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

using namespace arrangement;
namespace automation = app::automation;

namespace {

const QString kTrack = QStringLiteral("track");
const QString kGroup = QStringLiteral("group");
const QString kReturn = QStringLiteral("return");
const QString kMasterKind = QStringLiteral("master");

QString monitorLabel(const QString& mode) {
    if (mode == u"in") return QStringLiteral("In");
    if (mode == u"off") return QStringLiteral("Off");
    return QStringLiteral("Auto");
}

QString monitorTip(const QString& mode, bool midi) {
    if (mode == u"in") return QStringLiteral("In: always hears its input, never its clips");
    if (mode == u"off") return QStringLiteral("Off: never hears its input");
    if (mode == u"auto") {
        // A MIDI track's clips play on while it hears its input (Auto), as in Ableton.
        return midi ? QStringLiteral("Auto: hears its input while armed, beside its clips")
                    : QStringLiteral("Auto: hears its input while armed, unless playing back");
    }
    return {};
}

QString inputLabel(const std::vector<int>& channels) {
    if (channels.empty()) return QStringLiteral("No Input");
    QStringList numbers;
    for (int c : channels) numbers << QString::number(c + 1);
    return QStringLiteral("In ") + numbers.join(u'/');
}

QString midiInputLabel(const std::optional<app::MidiInput>& input) {
    if (!input) return QStringLiteral("No Input");
    const QString name = input->device.isEmpty() ? QStringLiteral("All Ins") : input->device;
    return input->channel ? QStringLiteral("%1 · Ch %2").arg(name).arg(input->channel) : name;
}

// (label, channels) for a device's inputs: each one (mono), then each pair.
std::vector<std::pair<QString, std::vector<int>>> inputChoices(const QStringList& names) {
    std::vector<std::pair<QString, std::vector<int>>> choices;
    for (int c = 0; c < names.size(); ++c)
        choices.emplace_back(QStringLiteral("%1  (%2)").arg(inputLabel({c}), names[c]), std::vector<int>{c});
    for (int c = 0; c + 1 < names.size(); c += 2) choices.emplace_back(inputLabel({c, c + 1}), std::vector<int>{c, c + 1});
    return choices;
}

// A send level typed: "-6", "-6 dB".
std::optional<double> parseDb(const QString& text) {
    bool ok = false;
    const double value = text.toLower().replace(QStringLiteral("db"), QString()).trimmed().toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

}  // namespace

TrackHeaderItem::TrackHeaderItem(QQuickItem* parent) : SgCanvas(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
}

TrackHeaderItem::~TrackHeaderItem() = default;

// --- Properties -----------------------------------------------------------------------------------

void TrackHeaderItem::setSession(app::Session* session) {
    if (session == session_) return;
    session_ = session;
    connectAll();
    Q_EMIT sessionChanged();
}

void TrackHeaderItem::setArrangement(Arrangement* arrangement) {
    if (arrangement == arrangement_) return;
    arrangement_ = arrangement;
    connectAll();
    Q_EMIT arrangementChanged();
}

void TrackHeaderItem::setTrackId(const QString& trackId) {
    if (trackId == trackId_) return;
    trackId_ = trackId;
    refresh();
    Q_EMIT trackIdChanged();
}

void TrackHeaderItem::setMeter(Meter* meter) {
    if (meter == meter_) return;
    meter_ = meter;
    Q_EMIT meterChanged();
}

void TrackHeaderItem::setNumber(int number) {
    if (number == number_) return;
    number_ = number;
    Q_EMIT numberChanged();
}

void TrackHeaderItem::setNameRight(qreal x) {
    if (x == nameRight_) return;
    nameRight_ = x;
    Q_EMIT nameRightChanged();
    update();
}

app::Project* TrackHeaderItem::project() const { return session_ ? session_->project() : nullptr; }

const app::Track* TrackHeaderItem::track() const {
    const app::Project* p = project();
    return p && p->hasOwner(trackId_) ? p->findTrack(trackId_) : nullptr;
}

bool TrackHeaderItem::isMaster() const { return trackId_ == app::kMaster; }

bool TrackHeaderItem::isReturn() const { return project() && project()->hasReturn(trackId_); }

QString TrackHeaderItem::kind() const {
    if (isMaster()) return kMasterKind;
    if (isReturn()) return kReturn;
    const app::Track* t = track();
    return t && t->isGroup() ? kGroup : kTrack;
}

bool TrackHeaderItem::midi() const { return track() && track()->isMidi(); }
bool TrackHeaderItem::records() const { return track() && track()->hasClips(); }  // groups record nothing
QString TrackHeaderItem::name() const { return track() ? track()->name : QString(); }
QColor TrackHeaderItem::color() const { return track() ? QColor(track()->color) : QColor(); }
bool TrackHeaderItem::mute() const { return track() && mute_; }
bool TrackHeaderItem::solo() const { return track() && track()->solo; }
bool TrackHeaderItem::armed() const { return track() && track()->armed; }

bool TrackHeaderItem::selected() const {
    if (!session_) return false;
    if (isMaster()) return session_->selection()->trackId() == app::kMaster;
    return session_->selection()->trackIds().contains(trackId_);
}

QString TrackHeaderItem::letter() const { return isReturn() ? project()->returnLetter(trackId_) : QString(); }

QString TrackHeaderItem::soloToolTip() const {
    if (isReturn())
        return QStringLiteral("Solo: keeps what sends to it sending; Ctrl-click to solo it along with others");
    if (track() && track()->isGroup())
        return QStringLiteral("Solo (S): a group solos what is in it; Ctrl-click to solo it along with others");
    return QStringLiteral("Solo (S); Ctrl-click to solo it along with others");
}

QString TrackHeaderItem::inputText() const {
    const app::Track* t = track();
    if (!t || !t->hasClips()) return {};  // nothing to record: no input
    if (t->isMidi()) return midiInputLabel(t->midiInput);
    if (t->inputTrack) return project()->inputName(*t->inputTrack);
    return inputLabel(t->input);
}

QString TrackHeaderItem::inputToolTip() const {
    const app::Track* t = track();
    if (!t || !t->hasClips()) return {};
    if (t->isMidi()) return QStringLiteral("MIDI input (the MIDI inputs on in Preferences, and a channel)");
    if (t->inputTrack) {
        if (*t->inputTrack == app::kMaster)
            return QStringLiteral("Audio input: the master's output (resampling: recorded, not heard)");
        return QStringLiteral("Audio input: %1's output, after its fader").arg(project()->inputName(*t->inputTrack));
    }
    return QStringLiteral("Audio input (the audio device's channels, or another track's output)");
}

QString TrackHeaderItem::monitorText() const { return track() ? monitorLabel(track()->monitor) : QString(); }

QString TrackHeaderItem::monitorToolTip() const {
    const app::Track* t = track();
    return t ? QStringLiteral("Monitoring. ") + monitorTip(t->monitor, t->isMidi()) : QString();
}

QVariantList TrackHeaderItem::lanes() const {
    QVariantList list;
    for (const LaneRow& lane : row_.lanes) {
        list.append(QVariantMap{{QStringLiteral("index"), lane.index},
                                {QStringLiteral("key"), lane.key},
                                {QStringLiteral("top"), lane.top - rowTop_},
                                {QStringLiteral("height"), lane.height}});
    }
    return list;
}

qreal TrackHeaderItem::nameLeft() const {
    const app::Project* p = project();
    const bool frozen = p && !isMaster() && p->frozenBy(trackId_).has_value();
    if (isReturn() || isMaster()) return 10 + (frozen ? kSnowflake + 3 : 0);
    return foldRect().right() + 3 + (p && p->isFrozen(trackId_) ? kSnowflake + 3 : 0);
}

int TrackHeaderItem::nameTop() const {
    const app::Track* t = track();
    return t && t->isGroup() ? arrangement::kGroupBar : 0;
}

double TrackHeaderItem::stripWidth() const {
    const app::Track* t = track();
    return t && t->isGroup() ? arrangement::kGroupBand : kStrip;
}

QRectF TrackHeaderItem::foldRect() const {
    if (isReturn() || isMaster()) return {};
    return QRectF(indent() + stripWidth() + 2, nameTop() + kNamePad, kFoldWidth, kNameButton);  // (as the buttons)
}

bool TrackHeaderItem::mixerAutomated() const {
    return volumeAutomation_ == u"on" || panAutomation_ == u"on" || activatorAutomation_ == u"on" || sendsAutomated_;
}

bool TrackHeaderItem::inResizeZone(double y) const {
    // The bottom edge of the track's own lane (automation lanes below keep their
    // height). A folded track keeps the height it had: it can't be resized.
    if (folded_ || isReturn() || isMaster()) return false;
    return row_.mainHeight - kResizeGrab <= y && y < row_.mainHeight;
}

QStringList TrackHeaderItem::draggedTracks() const {
    QStringList selected;
    if (session_) {
        for (const QString& id : session_->selection()->trackIds()) {
            if (project()->hasTrack(id)) selected << id;
        }
    }
    return selected.contains(trackId_) ? selected : QStringList{trackId_};
}

// --- Following the model ---------------------------------------------------------------------------

void TrackHeaderItem::connectAll() {
    for (QObject* sender : connected_) {
        if (sender) disconnect(sender, nullptr, this, nullptr);
    }
    connected_.clear();
    if (!session_ || !arrangement_) return;
    app::Project* p = session_->project();
    app::EngineBridge* bridge = session_->bridge();
    connected_ = {session_.data(), p, bridge, session_->selection(), arrangement_.data()};
    connect(p, &app::Project::trackChanged, this, [this](const QString& id) {
        if (id == trackId_) {
            refresh();
            return;
        }
        const app::Track* t = track();
        if (t && t->inputTrack == id) Q_EMIT changed();  // what takes its output shows its name
        refreshSends();  // its sends or its input: which sends would close a cycle
    });
    connect(p, &app::Project::devicesChanged, this, [this](const QString& id) {
        if (id == trackId_)
            refresh();
        else
            refreshSends();  // (a sidechain is a routing edge too)
    });
    for (auto signal : {&app::Project::trackInserted, &app::Project::trackRemoved, &app::Project::returnInserted,
                        &app::Project::returnRemoved}) {
        connect(p, signal, this, [this] { refresh(); });
    }
    connect(p, &app::Project::tracksArranged, this, [this] {
        refreshSends();
        Q_EMIT changed();
        update();  // its indent follows the groups
    });
    connect(p, &app::Project::reset, this, &TrackHeaderItem::refresh);
    connect(p, &app::Project::freezeChanged, this, [this] {  // (and what is in it)
        Q_EMIT changed();
        update();
    });
    connect(p, &app::Project::automationViewChanged, this, [this](const QString& owner) {
        if (owner == trackId_) refreshChoosers();
    });
    connect(bridge, &app::EngineBridge::automationStateChanged, this, [this](const QString& owner) {
        if (owner == trackId_) refresh();
    });
    connect(bridge, &app::EngineBridge::pluginParamsRebuilt, this, [this](const QString& owner) {
        if (owner == trackId_) refreshChoosers();
    });
    connect(bridge, &app::EngineBridge::positionChanged, this, &TrackHeaderItem::followAutomation);
    connect(bridge, &app::EngineBridge::metersUpdated, this, &TrackHeaderItem::updateMeter);
    connect(session_->selection(), &app::Selection::changed, this, [this] {
        Q_EMIT selectedChanged();
        update();
    });
    for (auto signal : {&Arrangement::layoutChanged, &Arrangement::returnRowsChanged, &Arrangement::masterRowsChanged})
        connect(arrangement_, signal, this, &TrackHeaderItem::refreshRow);
    connect(arrangement_, &Arrangement::renameRequested, this, [this](const QString& id) {
        if (id == trackId_) startRename();
    });
    refresh();
}

void TrackHeaderItem::itemChange(ItemChange change, const ItemChangeData& value) {
    SgCanvas::itemChange(change, value);
    if (change == ItemSceneChange && value.window) {
        const qreal dpr = value.window->effectiveDevicePixelRatio();
        snowflake_ = Icons::image(QStringLiteral("snowflake"), static_cast<int>(std::ceil(kSnowflake * dpr)));
        update();
    }
}

void TrackHeaderItem::refresh() {
    if (!session_ || !arrangement_ || !track()) return;
    refreshRow();
    refreshMixer();
    refreshSends();
    refreshChoosers();
    Q_EMIT changed();
    Q_EMIT selectedChanged();
    update();
}

void TrackHeaderItem::refreshRow() {
    if (!arrangement_ || !project()) return;
    AutomationRows row{row_.mainHeight, {}};
    int top = 0, depth = 0;
    bool folded = false, shown = false;
    if (isMaster()) {
        row = arrangement_->masterRows();
        shown = project()->master().automationView.shown;
    } else if (const auto rows = arrangement_->returnRows(trackId_)) {
        row = *rows;
        shown = project()->automationView(trackId_).shown;
    } else if (const Row* r = arrangement_->layout().rowFor(trackId_)) {
        row.mainHeight = r->mainHeight;
        row.lanes = r->lanes;
        top = r->top;
        depth = r->depth;
        folded = r->folded;
        shown = r->automation;
    }
    const bool changedRow = row.mainHeight != row_.mainHeight || row.lanes != row_.lanes || top != rowTop_ ||
                            depth != depth_ || folded != folded_ || shown != automationShown_;
    row_ = row;
    rowTop_ = top;
    depth_ = depth;
    folded_ = folded;
    automationShown_ = shown;
    if (changedRow) {
        Q_EMIT rowChanged();
        Q_EMIT changed();  // (where the name starts)
        refreshChoosers();
        update();
    }
}

QString TrackHeaderItem::automationState(const QString& key) const {
    // How a control shows its automation: "on" while it plays, "off" when overridden.
    const app::EngineBridge& bridge = *session_->bridge();
    if (bridge.isOverridden(trackId_, key)) return QStringLiteral("off");
    return bridge.isAutomated(trackId_, key) ? QStringLiteral("on") : QString();
}

void TrackHeaderItem::refreshMixer() {
    // Volume, pan and the activator as they are heard: following their automation while it plays.
    const app::Track* t = track();
    if (!t) return;
    app::EngineBridge& bridge = *session_->bridge();
    volumeAutomation_ = automationState(automation::kMixerVolume);
    panAutomation_ = automationState(automation::kMixerPan);
    activatorAutomation_ = isMaster() ? QString() : automationState(automation::kMixerOn);
    volume_ = t->volumeDb;
    pan_ = t->pan;
    const bool wasMuted = mute_;
    mute_ = t->mute;
    if (volumeAutomation_ == u"on") volume_ = bridge.currentValue(trackId_, automation::kMixerVolume).value_or(volume_);
    if (panAutomation_ == u"on") pan_ = bridge.currentValue(trackId_, automation::kMixerPan).value_or(pan_);
    if (activatorAutomation_ == u"on") {
        mute_ = bridge.currentValue(trackId_, automation::kMixerOn).value_or(mute_ ? 0.0 : 1.0) < 0.5;
    }
    if (mute_ != wasMuted) update();  // (its name is dimmed while muted)
    Q_EMIT mixerChanged();
}

void TrackHeaderItem::refreshSends() {
    // Values, automation, letters and which can be used, as they are now.
    QVariantList sends;
    bool automated = false;
    const app::Track* t = track();
    if (t && !isMaster() && arrangement_) {
        const app::Project& p = *project();
        app::EngineBridge& bridge = *session_->bridge();
        const app::RoutingGraph& graph = arrangement_->routingGraph();
        for (const app::Track& ret : p.returns()) {
            const QString letter = p.returnLetter(ret.id);
            const bool exists = t->sends.contains(ret.id);
            const app::Send send = t->sends.value(ret.id, app::Send{});
            const QString key = automation::sendKey(ret.id);
            const QString state = automationState(key);
            double level = send.levelDb;
            if (state == u"on") level = bridge.currentValue(trackId_, key).value_or(level);
            automated = automated || state == u"on";
            const bool usable = !app::feeds(graph, ret.id, trackId_);
            QString tip;
            if (!usable) {
                tip = QStringLiteral("Send %1 (%2): it feeds this track").arg(letter, ret.name);
            } else {
                const QString tap = send.preFader ? QStringLiteral("before the fader") : QStringLiteral("after the fader");
                tip = QStringLiteral("Send %1 to %2: %3, %4 (right-click: pre/post-fader)")
                          .arg(letter, ret.name, app::formatDb(level), tap);
            }
            sends.append(QVariantMap{{QStringLiteral("returnId"), ret.id},
                                     {QStringLiteral("letter"), letter},
                                     {QStringLiteral("value"), automation::volumeToNormalized(level)},
                                     {QStringLiteral("automation"), state},
                                     {QStringLiteral("enabled"), usable},
                                     {QStringLiteral("preFader"), send.preFader && exists},
                                     {QStringLiteral("toolTip"), tip}});
        }
    }
    sendsAutomated_ = automated;
    if (sends != sends_) {
        sends_ = sends;
        Q_EMIT sendsChanged();
    }
}

QString TrackHeaderItem::laneKey(int lane) const {
    if (!project() || !project()->hasOwner(trackId_)) return {};
    const app::AutomationView& view = project()->automationView(trackId_);
    if (lane < 0) return view.key.value_or(QString());
    return lane < view.lanes.size() ? view.lanes[lane] : QString();
}

void TrackHeaderItem::refreshChoosers() {
    QVariantList choosers;
    if (track() && session_) {
        app::EngineBridge& bridge = *session_->bridge();
        const auto chooser = [&](int lane) {
            const QString key = laneKey(lane);
            const auto spec = key.isEmpty() ? std::nullopt : bridge.paramSpec(trackId_, key);
            choosers.append(QVariantMap{{QStringLiteral("lane"), lane},
                                        {QStringLiteral("device"), spec ? spec->group : QStringLiteral("None")},
                                        {QStringLiteral("param"), spec ? spec->name : QStringLiteral("None")}});
        };
        if (automationShown_) chooser(-1);
        for (const LaneRow& lane : row_.lanes) chooser(lane.index);
    }
    if (choosers != choosers_) {
        choosers_ = choosers;
        Q_EMIT choosersChanged();
    }
}

void TrackHeaderItem::followAutomation() {
    // Every frame while playing: only what automation moves.
    if (!mixerAutomated()) return;
    refreshMixer();
    if (sendsAutomated_) refreshSends();
}

void TrackHeaderItem::updateMeter() {
    if (!meter_ || !session_) return;
    const app::MeterLevel level = session_->bridge()->trackMeter(trackId_);
    meter_->setLevels(level.left, level.right);
}

// --- Controls -----------------------------------------------------------------------------------------

void TrackHeaderItem::setMixer(const QString& field, double value, const QString& gestureKey, bool relative) {
    // Volume or pan changed here; on a selected track, every selected track follows
    // (by the same amount when dragged, to the same value when typed or reset).
    const app::Track* t = track();
    if (!t) return;
    app::ProjectEditor& editor = *session_->editor();
    const QStringList selected = session_->selection()->trackIds();
    if (isMaster() || isReturn() || !selected.contains(trackId_) || selected.size() < 2) {
        editor.trySetTrackParam(trackId_, field, value, gestureKey);
        return;
    }
    const bool volume = field == u"volume_db";
    const double delta = value - (volume ? t->volumeDb : t->pan);
    QMap<QString, double> values;
    for (const app::Track* sender : project()->senders()) {
        if (!selected.contains(sender->id)) continue;
        const double own = volume ? sender->volumeDb : sender->pan;
        values.insert(sender->id, relative ? own + delta : value);
    }
    try {
        editor.setTracksParam(values, volume ? app::TrackField::VolumeDb : app::TrackField::Pan, gestureKey);
    } catch (const app::EditError& error) {
        Q_EMIT editor.refused(error.message());
    }
}

void TrackHeaderItem::setVolume(double value, const QString& gestureKey, bool relative) {
    setMixer(QStringLiteral("volume_db"), value, gestureKey, relative);
}

void TrackHeaderItem::setPan(double value, const QString& gestureKey, bool relative) {
    setMixer(QStringLiteral("pan"), value, gestureKey, relative);
}

void TrackHeaderItem::touchVolume() {
    if (track()) session_->editor()->touchParameter(trackId_, automation::kMixerVolume);
}

void TrackHeaderItem::touchPan() {
    if (track()) session_->editor()->touchParameter(trackId_, automation::kMixerPan);
}

void TrackHeaderItem::activatorToggled(bool on) {
    if (!track()) return;
    session_->editor()->trySetTrackParam(trackId_, QStringLiteral("mute"), on ? 0.0 : 1.0);  // (overriding its automation)
    refreshMixer();
}

QStringList TrackHeaderItem::clickedTracks() const {
    const QStringList selected = session_->selection()->trackIds();
    return selected.contains(trackId_) ? selected : QStringList{trackId_};
}

void TrackHeaderItem::soloClicked(bool on) {
    // Soloing a track unsoloes the others, and unsoloing one unsoloes them
    // all, unless Ctrl is held. Clicking a selected track's solo acts on all
    // the selected tracks.
    if (!track()) return;
    QStringList tracks = clickedTracks();
    const bool exclusive = !(QGuiApplication::keyboardModifiers() & Qt::ControlModifier);
    if (exclusive && !on) {
        tracks.clear();
        for (const app::Track* sender : project()->senders()) tracks << sender->id;
    }
    session_->editor()->soloTracks(tracks, on, exclusive);
    Q_EMIT changed();
}

void TrackHeaderItem::armClicked(bool on) {
    // Arming a track disarms the others, unless Ctrl is held (as in Ableton).
    // Clicking a selected track's arm acts on all the selected tracks.
    if (!track()) return;
    const bool exclusive = !(QGuiApplication::keyboardModifiers() & Qt::ControlModifier);
    session_->editor()->armTracks(clickedTracks(), on, exclusive);
    Q_EMIT changed();
    const app::Track* t = track();
    if (on && t && !t->hasInput()) {
        const QString what = t->isMidi() ? QStringLiteral("MIDI input") : QStringLiteral("input");
        Q_EMIT session_->bridge()->statusMessage(QStringLiteral("%1 has no %2: choose one to record.").arg(t->name, what));
    }
}

void TrackHeaderItem::toggleFold() {
    // Fold or unfold it; when it is one of several selected tracks, they all take its new state.
    const app::Track* t = track();
    if (!t || isReturn() || isMaster()) return;
    const bool folded = !t->folded;
    for (const QString& id : draggedTracks()) session_->editor()->setFolded(id, folded);
}

void TrackHeaderItem::setSend(const QString& returnId, double value, const QString& gestureKey) {
    if (!track()) return;
    session_->editor()->trySetSendLevel(trackId_, returnId, automation::normalizedToVolume(value), gestureKey);
}

void TrackHeaderItem::touchSend(const QString& returnId) {
    if (track()) session_->editor()->touchParameter(trackId_, automation::sendKey(returnId));
}

QVariant TrackHeaderItem::parseSendLevel(const QString& text) const {
    const auto db = parseDb(text);
    return db ? QVariant(automation::volumeToNormalized(*db)) : QVariant();
}

QString TrackHeaderItem::formatDb(double db) { return app::formatDb(db); }

QString TrackHeaderItem::formatPan(double pan) { return app::formatPan(pan); }

QVariant TrackHeaderItem::parsePan(const QString& text) {
    const auto pan = app::parsePan(text);
    return pan ? QVariant(*pan) : QVariant();
}

void TrackHeaderItem::addLane() {
    if (track()) session_->editor()->addAutomationLane(trackId_);
}

void TrackHeaderItem::removeLane(int lane) {
    if (track()) session_->editor()->removeAutomationLane(trackId_, lane);
}

void TrackHeaderItem::startRename() {
    if (renaming_ || isMaster() || !track()) return;
    renaming_ = true;
    Q_EMIT renamingChanged();
    update();
}

void TrackHeaderItem::finishRename(const QString& text) {
    if (!renaming_) return;
    renaming_ = false;
    Q_EMIT renamingChanged();
    const QString name = text.trimmed();
    if (!name.isEmpty() && track()) session_->editor()->renameTrack(trackId_, name);
    update();
}

// --- Menus ---------------------------------------------------------------------------------------------

QVariantList TrackHeaderItem::show(const MenuEntries& menu) {
    menu_ = menu;
    return menu.toVariant();
}

void TrackHeaderItem::triggerMenu(int id) {
    const MenuEntries menu = menu_;  // (what it does may show another)
    menu.trigger(id);
}

void TrackHeaderItem::addFreezeEntries(MenuEntries& menu, const QStringList& trackIds) {
    // Freeze (or Unfreeze) and Flatten, for the tracks a track menu acts on.
    app::Session* s = session_;
    const QVariantMap actions = s->freezeActions(trackIds);
    MenuEntry* freeze = nullptr;
    if (actions.value(QStringLiteral("unfreeze")).toBool()) {
        freeze = &menu.add(actions.value(QStringLiteral("freezeText")).toString(), [s, trackIds] { s->unfreezeTracks(trackIds); });
    } else {
        app::Project* p = project();
        QStringList unfrozen;
        for (const QString& id : trackIds) {
            if (p->hasOwner(id) && !p->isFrozen(id)) unfrozen << id;
        }
        freeze = &menu.add(actions.value(QStringLiteral("freezeText")).toString(), [s, unfrozen] { s->freezeTracks(unfrozen); });
        freeze->enabled = actions.value(QStringLiteral("freezeEnabled")).toBool();
        freeze->toolTip = actions.value(QStringLiteral("freezeToolTip")).toString();
    }
    freeze->shortcut = QStringLiteral("Ctrl+Shift+F");
    MenuEntry& flatten =
        menu.add(actions.value(QStringLiteral("flattenText")).toString(), [s, trackIds] { s->flattenTracks(trackIds); });
    flatten.enabled = actions.value(QStringLiteral("flattenEnabled")).toBool();
}

void TrackHeaderItem::addAutomationEntries(MenuEntries& menu) {
    app::ProjectEditor* editor = session_->editor();
    app::EngineBridge* bridge = session_->bridge();
    const QString id = trackId_;
    const app::Track* t = track();
    if (!t) return;
    if (t->folded) {
        // Its automation doesn't show while folded: unfold it first.
    } else if (project()->automationView(id).shown) {
        menu.add(QStringLiteral("Hide Automation"), [editor, id] { editor->hideAutomation(id); });
        menu.add(QStringLiteral("Show Automation in New Lane"), [editor, id] { editor->addAutomationLane(id); });
    } else {
        menu.add(QStringLiteral("Show Automation"), [editor, id] { editor->showAutomation(id); });
    }
    const auto& envelopes = t->automation;
    for (auto it = envelopes.begin(); it != envelopes.end(); ++it) {
        if (bridge->isOverridden(id, it->first)) {
            menu.add(QStringLiteral("Re-Enable Automation"), [bridge, id] { bridge->reEnableAutomation(id); });
            break;
        }
    }
}

MenuEntries TrackHeaderItem::contextMenu() {
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || !session_) return menu;
    app::Session* s = session_;
    app::Selection& selection = *s->selection();
    app::ProjectEditor* editor = s->editor();
    app::Project* p = project();
    const QString id = trackId_;
    if (isMaster()) {
        selection.selectTrack(app::kMaster, true);  // the device view shows its effects
        addAutomationEntries(menu);
        return menu;
    }
    QStringList selected = selection.trackIds();
    if (!selected.contains(id)) {
        selection.selectTrack(id, true);
        selected = {id};
    } else if (!isReturn()) {
        selection.focusTracks();  // (its Cut and Copy: these tracks, not devices or clips)
    }
    menu.add(QStringLiteral("Rename"), [this] { startRename(); });
    MenuEntry& colors = menu.addSubmenu(QStringLiteral("Color"));
    for (const QString& color : app::kTrackColors) {
        MenuList(colors.children).add(color, [editor, id, color] { editor->setTrackColor(id, color); }).swatch = QColor(color);
    }
    menu.addSeparator();
    if (isReturn()) {
        const int index = p->returnIndex(id);
        menu.add(QStringLiteral("Insert Return Track"), [editor, index] { editor->addReturnTrack(index + 1); }).shortcut =
            QStringLiteral("Ctrl+Alt+T");
        menu.add(selected.size() == 1 ? QStringLiteral("Delete Return Track") : QStringLiteral("Delete Tracks"),
                 [editor, selected] { editor->deleteTracks(selected); });
        menu.addSeparator();
        addFreezeEntries(menu, selected);
        menu.addSeparator();
        addAutomationEntries(menu);
        return menu;
    }
    app::ArrangementActions* actions = s->arrangement();
    menu.add(QStringLiteral("Insert Audio Track"), [actions, id] { actions->insertTrackAfter(id, false); });
    menu.add(QStringLiteral("Insert MIDI Track"), [actions, id] { actions->insertTrackAfter(id, true); });
    menu.add(QStringLiteral("Insert Return Track"), [editor] { editor->addReturnTrack(); });
    // The window's Cut, Copy and Paste: on the selected tracks.
    menu.add(QStringLiteral("Cut"), [s] { s->cut(); }).shortcut = QStringLiteral("Ctrl+X");
    menu.add(QStringLiteral("Copy"), [s] { s->copy(); }).shortcut = QStringLiteral("Ctrl+C");
    menu.add(QStringLiteral("Paste"), [s] { s->paste(); }).shortcut = QStringLiteral("Ctrl+V");
    menu.add(selected.size() == 1 ? QStringLiteral("Duplicate Track") : QStringLiteral("Duplicate Tracks"),
             [actions, selected] { actions->duplicateTracks(selected); })
        .shortcut = QStringLiteral("Ctrl+D");
    menu.add(selected.size() == 1 ? QStringLiteral("Delete Track") : QStringLiteral("Delete Tracks"),
             [editor, selected] { editor->deleteTracks(selected); });
    menu.addSeparator();
    menu.add(QStringLiteral("Group Tracks"), [editor, s, selected] {
            const QString group = editor->groupTracks(selected);
            if (!group.isEmpty()) s->selection()->selectTrack(group, true);
        }).shortcut = QStringLiteral("Ctrl+G");
    QStringList groups;
    for (const QString& sel : selected) {
        const app::Track* other = p->findTrack(sel);
        if (other && other->isGroup()) groups << sel;
    }
    if (!groups.isEmpty())
        menu.add(QStringLiteral("Ungroup Tracks"), [editor, groups] { editor->ungroup(groups); }).shortcut =
            QStringLiteral("Ctrl+Shift+G");
    const QStringList folding = draggedTracks();
    QString what;
    if (folding.size() > 1) {
        const bool allGroups = std::all_of(folding.begin(), folding.end(), [p](const QString& f) {
            const app::Track* other = p->findTrack(f);
            return other && other->isGroup();
        });
        what = allGroups ? QStringLiteral("Groups") : QStringLiteral("Tracks");
    } else {
        what = t->isGroup() ? QStringLiteral("Group") : QStringLiteral("Track");
    }
    menu.add((t->folded ? QStringLiteral("Unfold ") : QStringLiteral("Fold ")) + what, [this] { toggleFold(); });
    if (t->parent) {
        const QString parent = *t->parent;
        menu.add(QStringLiteral("Move Out of Group"), [editor, p, selected, parent] {
            if (!p->hasTrack(parent)) return;
            const app::Track& group = p->track(parent);
            editor->moveTracks(selected, p->subtreeEnd(p->trackIndex(parent)), group.parent.value_or(QString()));
        });
    }
    menu.addSeparator();
    addFreezeEntries(menu, selected);
    menu.addSeparator();
    addAutomationEntries(menu);
    return menu;
}

MenuEntries TrackHeaderItem::inputMenu() {
    // No input, the audio device's inputs (each, then each pair), then the
    // master's output (resampling) and the other tracks', groups' and returns'
    // (those it feeds greyed out: taking theirs would close a cycle). A MIDI
    // track: no input, every input or one (those connected, and the one chosen
    // if it isn't), and in a submenu the channel.
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || !t->hasClips()) return menu;
    app::ProjectEditor* editor = session_->editor();
    app::EngineBridge* bridge = session_->bridge();
    const QString id = trackId_;
    if (t->isMidi()) {
        const std::optional<app::MidiInput> current = t->midiInput;
        const int channel = current ? current->channel : 0;
        const auto add = [&](const QString& label, const std::optional<QString>& device) {
            MenuEntry& entry = menu.add(label, [editor, id, device, channel] {
                editor->trySetTrackMidiInput(id, device.has_value(), device.value_or(QString()), channel);
            });
            entry.checkable = true;
            entry.checked = device ? current == app::MidiInput{*device, channel} : !current.has_value();
        };
        add(QStringLiteral("No Input"), std::nullopt);
        add(QStringLiteral("All Ins"), QString());
        const QStringList connected = bridge->midiInputChoices();
        QStringList names = connected;
        if (current && !current->device.isEmpty() && !names.contains(current->device)) names << current->device;
        if (!names.isEmpty()) menu.addSeparator();
        for (const QString& name : names)
            add(connected.contains(name) ? name : QStringLiteral("%1 (not connected)").arg(name), name);
        if (names.isEmpty()) menu.add(QStringLiteral("No MIDI input is connected")).enabled = false;
        menu.addSeparator();
        MenuEntry& channels = menu.addSubmenu(
            QStringLiteral("Channel: %1").arg(channel ? QString::number(channel) : QStringLiteral("All")));
        channels.enabled = current.has_value();
        const QString device = current ? current->device : QString();
        for (int number = 0; number <= 16; ++number) {
            MenuEntry& entry = MenuList(channels.children)
                                   .add(number == 0 ? QStringLiteral("All Channels") : QStringLiteral("Channel %1").arg(number),
                                        [editor, id, device, number] { editor->trySetTrackMidiInput(id, true, device, number); });
            entry.checkable = true;
            entry.checked = number == channel;
            if (number == 0) MenuList(channels.children).addSeparator();
        }
        return menu;
    }
    const app::Project& p = *project();
    MenuEntry& none = menu.add(QStringLiteral("No Input"), [editor, id] { editor->trySetTrackInput(id, {}); });
    none.checkable = true;
    none.checked = !t->hasInput();
    const QStringList names = bridge->inputNames();
#ifdef Q_OS_WIN
    if (names.isEmpty()) menu.add(QStringLiteral("The audio device has no inputs (choose an ASIO driver)")).enabled = false;
#else  // (no ASIO here)
    if (names.isEmpty()) menu.add(QStringLiteral("The audio device has no inputs")).enabled = false;
#endif
    const std::optional<std::vector<int>> current = t->inputTrack ? std::nullopt : std::optional(t->input);
    for (const auto& [label, channels] : inputChoices(names)) {
        if (channels.size() == 2 && names.size() > 2 && channels == std::vector<int>{0, 1}) menu.addSeparator();
        const QList<int> list(channels.begin(), channels.end());
        MenuEntry& entry = menu.add(label, [editor, id, list] { editor->trySetTrackInput(id, list); });
        entry.checkable = true;
        entry.checked = current && *current == channels;
    }
    menu.addSeparator();
    std::vector<QString> sources{app::kMaster};
    for (const app::Track* source : p.inputSources(id)) sources.push_back(source->id);
    for (const QString& sourceId : sources) {
        const bool usable = !p.inputWouldCycle(id, sourceId);
        const QString label = p.inputName(sourceId);
        MenuEntry& entry = menu.add(usable ? label : QStringLiteral("%1 (it takes this track's output)").arg(label),
                                    [editor, id, sourceId] { editor->trySetTrackInputTrack(id, sourceId); });
        entry.checkable = true;
        entry.checked = t->inputTrack == sourceId;
        entry.enabled = usable;
        if (sourceId == app::kMaster) {
            entry.toolTip = QStringLiteral("The master's output: recorded, not heard (it would feed back)");
            menu.addSeparator();
        }
    }
    return menu;
}

MenuEntries TrackHeaderItem::monitorMenu() {
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || !t->hasClips()) return menu;
    app::ProjectEditor* editor = session_->editor();
    const QString id = trackId_;
    for (const QString& mode : {QStringLiteral("in"), QStringLiteral("auto"), QStringLiteral("off")}) {
        MenuEntry& entry = menu.add(monitorTip(mode, t->isMidi()), [editor, id, mode] { editor->trySetTrackMonitor(id, mode); });
        entry.checkable = true;
        entry.checked = t->monitor == mode;
    }
    return menu;
}

MenuEntries TrackHeaderItem::sendMenu(const QString& returnId) {
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || !project()->hasReturn(returnId) || !arrangement_) return menu;
    if (app::feeds(arrangement_->routingGraph(), returnId, trackId_)) return menu;  // (greyed out: no menu)
    app::ProjectEditor* editor = session_->editor();
    const QString id = trackId_;
    const bool exists = t->sends.contains(returnId);
    const bool pre = exists && t->sends.value(returnId).preFader;
    MenuEntry& preFader = menu.add(QStringLiteral("Pre-Fader"), [editor, id, returnId, pre] {
        editor->trySetSendPreFader(id, returnId, !pre);
    });
    preFader.checkable = true;
    preFader.checked = pre;
    menu.add(QStringLiteral("Remove Send"), [editor, id, returnId] { editor->removeSend(id, returnId); }).enabled = exists;
    menu.addSeparator();
    const QString key = automation::sendKey(returnId);
    menu.add(QStringLiteral("Show Automation"), [editor, id, key] { editor->showAutomation(id, key); });
    return menu;
}

MenuEntries TrackHeaderItem::activatorMenu() {
    // Its automation: show it, delete it, re-enable it while overridden.
    MenuEntries menu;
    if (!track() || isMaster()) return menu;
    app::ProjectEditor* editor = session_->editor();
    app::EngineBridge* bridge = session_->bridge();
    const QString id = trackId_;
    const QString key = automation::kMixerOn;
    menu.add(QStringLiteral("Show Automation"), [editor, id, key] { editor->showAutomation(id, key); });
    menu.add(QStringLiteral("Delete Automation"), [editor, id, key] { editor->clearEnvelope(id, key); }).enabled =
        !project()->envelope(id, key).empty();
    if (bridge->isOverridden(id, key)) {
        menu.add(QStringLiteral("Re-Enable Automation"), [bridge, id] { bridge->reEnableAutomation(id); });
    }
    return menu;
}

void TrackHeaderItem::chooseLane(int lane, const QString& key) {
    if (track()) session_->editor()->setAutomationLane(trackId_, lane, key);
}

namespace {

// The group a key's parameter is in: "mixer", or its device's id.
QString groupOf(const QString& key) {
    if (key.isEmpty()) return {};
    if (automation::isMixerKey(key)) return QStringLiteral("mixer");
    return automation::keyDevice(key).value_or(QString());
}

}  // namespace

MenuEntries TrackHeaderItem::deviceMenu(int lane) {
    MenuEntries menu;
    if (!track()) return menu;
    const QString current = groupOf(laneKey(lane));
    const auto& automated = project()->automation(trackId_);
    for (const app::ParamGroup& group : session_->bridge()->paramGroups(trackId_)) {
        if (group.specs.empty()) {
            menu.add(group.name).enabled = false;
            continue;
        }
        // A device's automated parameter first, as Ableton does.
        QString key = group.specs.front().key;
        bool anyAutomated = false;
        for (const app::ParamSpec& spec : group.specs) {
            if (automated.contains(spec.key)) {
                if (!anyAutomated) key = spec.key;
                anyAutomated = true;
            }
        }
        MenuEntry& entry = menu.add(group.name, [this, lane, key] { chooseLane(lane, key); });
        entry.checkable = true;
        entry.checked = group.id == current;
        if (anyAutomated) entry.dot = kEnvelope;
    }
    return menu;
}

MenuEntries TrackHeaderItem::paramMenu(int lane) {
    MenuEntries menu;
    if (!track()) return menu;
    const QString key = laneKey(lane);
    const QString current = groupOf(key);
    const auto& automated = project()->automation(trackId_);
    for (const app::ParamGroup& group : session_->bridge()->paramGroups(trackId_)) {
        if (group.id != current) continue;
        for (const app::ParamSpec& spec : group.specs) {
            const QString chosen = spec.key;
            MenuEntry& entry = menu.add(spec.name, [this, lane, chosen] { chooseLane(lane, chosen); });
            entry.checkable = true;
            entry.checked = spec.key == key;
            if (automated.contains(spec.key)) entry.dot = kEnvelope;
        }
    }
    if (menu.isEmpty()) menu.add(QStringLiteral("No parameters")).enabled = false;
    return menu;
}

// --- Painting ----------------------------------------------------------------------------------------------

void TrackHeaderItem::paint(SgPainter& p) {
    const app::Track* t = track();
    const double w = width(), h = height();
    const bool isSelected = selected();
    p.fillRect(QRectF(0, 0, w, h), isSelected ? Theme::kLaneSelected : Theme::kPanelAlt);
    if (!t) return;
    const app::Project& project = *this->project();
    // The lanes below it: a panel each, with a line above.
    for (const LaneRow& lane : row_.lanes) {
        const double top = lane.top - rowTop_;
        p.fillRect(QRectF(0, top, w, lane.height), Theme::kPanel);
        p.fillRect(QRectF(0, top, w, 1), Theme::kGridBar);
    }
    const auto drawFrozen = [&](double x) {
        // A snowflake in the name row if it is frozen (dimmer if it is in a frozen group, not frozen itself).
        const auto holder = project.frozenBy(trackId_);
        if (!holder || snowflake_.isNull()) return false;
        p.save();
        p.setOpacity(*holder == trackId_ ? 1.0 : 0.5);
        p.drawImage(QRectF(x, nameTop() + kNamePad + (kNameButton - kSnowflake) / 2, kSnowflake, kSnowflake), snowflake_);
        p.restore();
        return true;
    };
    if (isMaster()) {
        p.fillRect(QRectF(0, 0, 5, h), Theme::kTextDim);
        p.fillRect(QRectF(0, 0, w, 1), Theme::kBorder);
        p.fillRect(QRectF(0, 0, 1, h), Theme::kBorder);
        p.drawText(QRectF(12, 0, 100, kMasterHeight), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Master"),
                   Theme::kText, uiFont(9, true));
        return;
    }
    if (isReturn()) {
        p.fillRect(QRectF(0, 0, 5, h), QColor(t->color));
        p.fillRect(QRectF(0, 0, w, 1), Theme::kBorder);
        p.fillRect(QRectF(0, 0, 1, h), Theme::kBorder);
        const double left = 10 + (drawFrozen(10) ? kSnowflake + 3 : 0);
        if (!renaming_) {
            const QFont font = uiFont(9, true);
            const QRectF nameRect(left, kNamePad, nameRight_ - left, kNameButton);  // (as a track's)
            p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, SgPainter::elidedText(t->name, font, nameRect.width()),
                       mute() ? Theme::kTextDim : Theme::kText, font);
        }
        return;
    }
    // Its own colour, after the bands of the groups it is in (GroupBands draws
    // those, over the headers: a group's runs on down its tracks). A group's
    // goes across its top too, above its name row.
    p.fillRect(QRectF(indent(), 0, stripWidth(), h - 1), QColor(t->color));
    if (t->isGroup()) p.fillRect(QRectF(indent(), 0, w - indent(), arrangement::kGroupBar), QColor(t->color));
    // The fold button, in a circle. A track's: a triangle, pointing right while
    // folded, down while open. A group's: three bars (its tracks), the circle
    // filled while folded (its tracks tucked away).
    {
        const QRectF rect = foldRect();
        const QPointF c = rect.center();
        const double radius = 5.5;
        const QColor ink = Theme::kText;
        p.save();
        p.setAntialiasing(true);
        if (t->isGroup() && t->folded) p.fillEllipse(c, radius + 0.6, radius + 0.6, ink);
        p.drawEllipse(QRectF(c.x() - radius, c.y() - radius, 2 * radius, 2 * radius), ink, 1.2);
        if (t->isGroup()) {
            const QColor bars = t->folded ? Theme::kPanelAlt : ink;
            for (const double dy : {-2.5, 0.0, 2.5}) {
                const double half = dy == 0.0 ? 2.8 : 2.2;
                const QRectF bar(c.x() - half, c.y() + dy - 0.6, 2 * half, 1.2);
                p.fillPolygon(QPolygonF({bar.topLeft(), bar.topRight(), bar.bottomRight(), bar.bottomLeft()}), bars);
            }
        } else if (t->folded) {
            p.fillPolygon(QPolygonF({QPointF(c.x() - 1.5, c.y() - 3.0), QPointF(c.x() + 2.5, c.y()),
                                     QPointF(c.x() - 1.5, c.y() + 3.0)}),
                          ink);
        } else {
            p.fillPolygon(QPolygonF({QPointF(c.x() - 3.0, c.y() - 1.5), QPointF(c.x() + 3.0, c.y() - 1.5),
                                     QPointF(c.x(), c.y() + 2.5)}),
                          ink);
        }
        p.restore();
    }
    drawFrozen(foldRect().right() + 3);
    if (!renaming_) {
        const QFont font = uiFont(9, isSelected || t->isGroup());
        const double left = nameLeft();
        const QRectF nameRect(left, nameTop() + kNamePad, nameRight_ - left, kNameButton);
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, SgPainter::elidedText(t->name, font, nameRect.width()),
                   mute() ? Theme::kTextDim : Theme::kText, font);
    }
    p.fillRect(QRectF(0, h - 1, w, 1), Theme::kBorder);
    p.fillRect(QRectF(0, 0, 1, h), Theme::kBorder);
}

// --- Mouse ---------------------------------------------------------------------------------------------------

double TrackHeaderItem::columnY(const QPointF& pos) const {
    // In the header column's coordinates (the headers' parent), as the drop target has them.
    return parentItem() ? mapToItem(parentItem(), pos).y() : pos.y() + y();
}

void TrackHeaderItem::mousePressEvent(QMouseEvent* event) {
    if (event->flags() & Qt::MouseEventCreatedDoubleClick) return;  // (the double-click stands for it)
    if (!track()) return;
    const QPointF pos = event->position();
    app::Selection& selection = *session_->selection();
    if (event->button() == Qt::RightButton) {
        menu_ = contextMenu();
        if (!menu_.isEmpty()) Q_EMIT menuRequested(menu_.toVariant(), pos);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (isMaster()) {
        selection.selectTrack(app::kMaster, true);  // the device view shows its effects
        return;
    }
    if (isReturn()) {
        const auto mode = event->modifiers() & Qt::ControlModifier ? app::Selection::Mode::Toggle
                                                                   : app::Selection::Mode::Replace;
        selection.selectTrack(trackId_, true, mode);
        return;
    }
    if (inResizeZone(pos.y())) {
        resize_ = std::make_pair(event->scenePosition().y(), row_.mainHeight);  // as tall as it shows
    } else if (foldRect().contains(pos)) {
        toggleFold();
    } else {
        const Qt::KeyboardModifiers mods = event->modifiers();
        const auto mode = mods & Qt::ControlModifier ? app::Selection::Mode::Toggle
                          : mods & Qt::ShiftModifier ? app::Selection::Mode::Range
                                                     : app::Selection::Mode::Replace;
        const QStringList selected = selection.trackIds();
        if (!(mode == app::Selection::Mode::Replace && selected.contains(trackId_) && selected.size() > 1)) {
            QStringList order;
            for (const app::Track& other : project()->tracks()) order << other.id;
            selection.selectTrack(trackId_, true, mode, order);
        }
        press_ = pos;  // dragging moves the track (the selected tracks)
    }
}

void TrackHeaderItem::mouseMoveEvent(QMouseEvent* event) {
    if (!track()) return;
    const QPointF pos = event->position();
    if (resize_) {
        const auto [startY, startHeight] = *resize_;
        const int height = static_cast<int>(startHeight + event->scenePosition().y() - startY);
        session_->editor()->setTrackHeight(trackId_, std::clamp(height, app::kMinTrackHeight, app::kMaxTrackHeight));
        return;
    }
    if (press_) {  // the left button is down (pressed here)
        if (!dragging_ && (pos - *press_).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance()) {
            dragging_ = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (dragging_ && arrangement_) arrangement_->dragTracks(draggedTracks(), columnY(pos));
    }
}

void TrackHeaderItem::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    resize_.reset();
    const bool dragging = dragging_;
    dragging_ = false;
    const std::optional<QPointF> pressed = press_;
    press_.reset();
    if (!track()) return;
    if (dragging && arrangement_) {
        setCursor(Qt::ArrowCursor);
        arrangement_->dropTracks(draggedTracks(), columnY(event->position()));
        return;
    }
    app::Selection& selection = *session_->selection();
    const QStringList selected = selection.trackIds();
    if (pressed && selected.contains(trackId_) && selected.size() > 1 &&
        !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        // A click (not a drag) on one of several selected tracks selects just it.
        selection.selectTrack(trackId_, true);
    }
}

void TrackHeaderItem::mouseUngrabEvent() {
    resize_.reset();
    press_.reset();
    if (dragging_) {
        dragging_ = false;
        setCursor(Qt::ArrowCursor);
        if (arrangement_) arrangement_->dragTracks({}, -1e9);  // (the marker goes)
    }
}

void TrackHeaderItem::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && foldRect().contains(event->position()))
        toggleFold();  // each click of a double-click counts
}

void TrackHeaderItem::hoverMoveEvent(QHoverEvent* event) {
    setCursor(inResizeZone(event->position().y()) ? Qt::SplitVCursor : Qt::ArrowCursor);
}

void TrackHeaderItem::wheelEvent(QWheelEvent* event) {
    if (!arrangement_ || !track() || isMaster() || isReturn()) {
        event->ignore();
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers();
    const QPoint delta = event->angleDelta();
    if ((mods & Qt::AltModifier) && !(mods & Qt::ControlModifier)) {
        // Alt+wheel resizes this track, folding (or unfolding) it at its smallest
        // (over any of its controls too: theirs don't take the wheel).
        arrangement_->wheelResize(trackId_, delta.y() ? delta.y() : delta.x());
    } else {
        arrangement_->setScrollY(arrangement_->scrollY() - delta.y() / 120.0 * 48);  // the headers scroll with the lanes
    }
    event->accept();
}

}  // namespace sub::ui
