#include "arrangement/TrackHeaderItem.h"

#include "arrangement/Arrangement.h"
#include "arrangement/Envelopes.h"
#include "audio/EngineBridge.h"
#include "controls/Automation.h"
#include "controls/Meter.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Devices.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "model/Routing.h"
#include "model/Timebase.h"
#include "session/ArrangementActions.h"
#include "session/AudioPreferences.h"
#include "session/Selection.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPolygonF>
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

const QString kMain = QStringLiteral("Main");  // the master, as Ableton 12 calls it
const QString kNoInput = QStringLiteral("No Input");
const QString kAllIns = QStringLiteral("All Ins");
const QString kExtIn = QStringLiteral("Ext. In");
const QString kSendsOnly = QStringLiteral("Sends Only");
const QString kTrackIn = QStringLiteral("Track In");

// A channel, or a pair, as Ableton numbers them: "1", "1/2".
QString channelsLabel(const std::vector<int>& channels) {
    QStringList numbers;
    for (int c : channels) numbers << QString::number(c + 1);
    return numbers.join(u'/');
}

// (label, channels) for a device's inputs, as Ableton lists them: each one (mono), then each pair.
std::vector<std::pair<QString, std::vector<int>>> inputChoices(int count) {
    std::vector<std::pair<QString, std::vector<int>>> choices;
    for (int c = 0; c < count; ++c) choices.emplace_back(channelsLabel({c}), std::vector<int>{c});
    for (int c = 0; c + 1 < count; c += 2) choices.emplace_back(channelsLabel({c, c + 1}), std::vector<int>{c, c + 1});
    return choices;
}

// Where an input from a track is taken, as Ableton calls it.
QString tapLabel(const QString& tap) {
    if (tap == app::kPreFx) return QStringLiteral("Pre FX");
    if (tap == app::kPreFader) return QStringLiteral("Post FX");
    return QStringLiteral("Post Mixer");
}

const QStringList kInputTaps{app::kPreFx, app::kPreFader, app::kPostFader};

QString channelLabel(int channel) {
    return channel ? QStringLiteral("Ch. %1").arg(channel) : QStringLiteral("All Channels");
}

QString sidechainLabel(const app::Device& device) { return QStringLiteral("Sidechain-") + app::deviceName(device); }

// What goes into a track's input is heard while it monitors: says so if it doesn't (as Ableton, it plays its clips).
void hintTrackIn(app::Session* session, const QString& trackId) {
    const app::Project& project = *session->project();
    if (!project.hasTrack(trackId) || project.track(trackId).monitor == u"in") return;
    Q_EMIT session->bridge()->statusMessage(
        QStringLiteral("%1 hears what goes into it while it monitors: set its monitoring to In (or arm it, on Auto).")
            .arg(project.track(trackId).name));
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

void TrackHeaderItem::setIoLeft(qreal x) {
    if (x == ioLeft_) return;
    ioLeft_ = x;
    Q_EMIT columnsChanged();
    update();
}

void TrackHeaderItem::setMixerLeft(qreal x) {
    if (x == mixerLeft_) return;
    mixerLeft_ = x;
    Q_EMIT columnsChanged();
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
QString TrackHeaderItem::nameTemplate() const { return track() ? track()->nameSource() : QString(); }
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
    if (t->isMidi()) {
        if (!t->midiInput) return kNoInput;
        return t->midiInput->allDevices() ? kAllIns : t->midiInput->device;
    }
    if (t->inputTrack) return project()->inputName(*t->inputTrack);
    return t->input.empty() ? kNoInput : kExtIn;
}

QString TrackHeaderItem::inputToolTip() const {
    const app::Track* t = track();
    if (!t || !t->hasClips()) return {};
    if (t->isMidi()) return QStringLiteral("MIDI From: the MIDI inputs on in Preferences (All Ins), one of them, or none");
    return QStringLiteral("Audio From: the audio device's inputs (Ext. In), the mix (Resampling), another track's "
                          "output, or none");
}

QString TrackHeaderItem::inputChannelText() const {
    const app::Track* t = track();
    if (!t || !t->hasClips()) return {};
    if (t->isMidi()) return t->midiInput ? channelLabel(t->midiInput->channel) : QString();
    if (t->inputTrack) return *t->inputTrack == app::kMaster ? QString() : tapLabel(t->inputTap);
    return channelsLabel(t->input);
}

QString TrackHeaderItem::inputChannelToolTip() const {
    const app::Track* t = track();
    if (!t || inputChannelText().isEmpty()) return {};
    if (t->isMidi()) return QStringLiteral("The MIDI channel it hears");
    if (t->inputTrack) {
        return QStringLiteral("Where %1's signal is taken: before its devices (Pre FX), after them (Post FX) or "
                              "after its mixer (Post Mixer)")
            .arg(project()->inputName(*t->inputTrack));
    }
    const QStringList names = session_->bridge()->inputNames();
    QStringList named;
    for (int c : t->input) {
        if (c < names.size()) named << names[c];
    }
    return named.isEmpty() ? QStringLiteral("The audio device's input channel, or pair")
                           : QStringLiteral("The audio device's %1").arg(named.join(QStringLiteral(" and ")));
}

QString TrackHeaderItem::monitor() const { return track() ? track()->monitor : QString(); }

QString TrackHeaderItem::monitorToolTip(const QString& mode, bool midi) {
    if (mode == u"in") return QStringLiteral("Monitor In: always hears its input, never its clips");
    if (mode == u"off") return QStringLiteral("Monitor Off: never hears its input");
    if (mode == u"auto") {
        // A MIDI track's clips play on while it hears its input (Auto), as in Ableton.
        return midi ? QStringLiteral("Monitor Auto: hears its input while armed, beside its clips")
                    : QStringLiteral("Monitor Auto: hears its input while armed, unless playing back");
    }
    return {};
}

std::optional<QString> TrackHeaderItem::outputTrack() const {
    const app::Track* t = track();
    if (!t || isMaster()) return std::nullopt;
    return project()->outputTarget(trackId_);
}

QString TrackHeaderItem::outputText() const {
    const app::Track* t = track();
    if (!t || isMaster()) return {};
    switch (t->output.to) {
    case app::Output::To::None: return kSendsOnly;
    case app::Output::To::Master: return kMain;
    case app::Output::To::Group:
    case app::Output::To::Track:
    case app::Output::To::Sidechain: break;
    }
    if (const auto target = outputTrack()) return project()->track(*target).name;
    return kMain;  // (outside a group, or into a device on the master)
}

QString TrackHeaderItem::outputToolTip() const {
    if (!track() || isMaster()) return {};
    return QStringLiteral("Audio To: the master (Main), its group, another track (its input, or a device taking a "
                          "sidechain there), or only its sends");
}

QString TrackHeaderItem::outputChannelText() const {
    const app::Track* t = track();
    if (!t || isMaster()) return {};
    if (t->output.to == app::Output::To::Track) return kTrackIn;
    if (t->output.to == app::Output::To::Sidechain) {
        const auto owner = project()->deviceOwner(t->output.id);
        if (owner) {
            if (const app::Device* device = project()->findDevice(*owner, t->output.id)) return sidechainLabel(*device);
        }
    }
    return {};
}

QString TrackHeaderItem::outputChannelToolTip() const {
    const app::Track* t = track();
    if (!t || outputChannelText().isEmpty()) return {};
    if (t->output.to == app::Output::To::Track) {
        return QStringLiteral("Into that track's input: heard while it monitors (In, or Auto while armed)");
    }
    return QStringLiteral("Into that device's sidechain input: heard only through it");
}

QColor TrackHeaderItem::barColor() const {
    if (isMaster()) return QColor(app::kMasterColor);
    return track() ? QColor(track()->color) : QColor(Theme::surface());
}

QColor TrackHeaderItem::barText() const {
    // Dark on a light bar, light on a dark one, as Ableton writes them: the
    // bar is the track's colour, whatever the theme, so its text is too.
    const QColor bar = barColor();
    const double luma = 0.299 * bar.redF() + 0.587 * bar.greenF() + 0.114 * bar.blueF();
    const Palette& fixed = palettes().front().colors;
    return luma > 0.45 ? fixed.accentText : fixed.text;
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
    if (isReturn() || isMaster()) return 6;
    return foldRect().right() + 3;
}

QRectF TrackHeaderItem::foldRect() const {
    if (isReturn() || isMaster()) return {};
    return QRectF(indent() + 4, kNamePad, kFoldWidth, kNameButton);  // (as the buttons)
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
        // What takes its output, goes into it or is in it shows its name.
        if (t && (t->inputTrack == id || t->parent == id || outputTrack() == id))
            Q_EMIT changed();
        refreshSends();  // its sends or its input: which sends would close a cycle
    });
    connect(p, &app::Project::devicesChanged, this, [this](const QString& id) {
        if (id == trackId_) {
            refresh();
            return;
        }
        refreshSends();  // (a sidechain is a routing edge too)
        const app::Track* t = track();
        if (t && t->output.to == app::Output::To::Sidechain) Q_EMIT changed();  // (its device's name)
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
    connect(bridge, &app::EngineBridge::deviceChanged, this, [this] {
        if (isMaster()) Q_EMIT changed();  // (its Main Out)
    });
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
    return ui::automationState(*session_->bridge(), trackId_, key);
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

void TrackHeaderItem::setMonitor(const QString& mode) {
    if (track() && track()->hasClips()) session_->editor()->trySetTrackMonitor(trackId_, mode);
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

QString TrackHeaderItem::formatVolume(double db) {
    if (db <= automation::kMinVolumeDb) return QStringLiteral("-inf");
    if (std::abs(db) < 0.05) return QStringLiteral("0");
    return QString::number(db, 'f', 1);
}

double TrackHeaderItem::volumeFraction(double db) { return automation::volumeToNormalized(db); }

QString TrackHeaderItem::mainOutText() const {
    if (!isMaster() || !session_) return {};
    const QList<int> channels = session_->bridge()->deviceStatus().outputChannels;
    return channelsLabel(channels.size() >= 2 ? std::vector<int>{channels[0], channels[1]}
                         : channels.size() == 1 ? std::vector<int>{channels[0]}
                                                : std::vector<int>{0, 1});
}

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

void TrackHeaderItem::addConfigure(MenuEntries& menu, bool midi) {
    Arrangement* arrangement = arrangement_;
    menu.add(QStringLiteral("Configure…"), [arrangement, midi] {
        if (arrangement) Q_EMIT arrangement->preferencesRequested(midi ? 1 : 0);
    });
}

MenuEntries TrackHeaderItem::inputMenu() {
    // As Ableton's Audio From: Ext. In (the audio device's inputs), Configure...,
    // Resampling (the master's output), the other tracks', groups' and returns'
    // outputs under a search field (those it feeds greyed out: taking theirs
    // would close a cycle), No Input. MIDI From: All Ins, the computer keyboard
    // and each MIDI input (those connected, and the one chosen if it isn't),
    // Configure..., No Input.
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || !t->hasClips()) return menu;
    app::ProjectEditor* editor = session_->editor();
    app::EngineBridge* bridge = session_->bridge();
    const QString id = trackId_;
    if (t->isMidi()) {
        const std::optional<app::MidiInput> current = t->midiInput;
        const int channel = current ? current->channel : 0;
        const auto add = [&](const QString& label, const QString& device) {
            MenuEntry& entry = menu.add(label, [editor, id, device, channel] {
                editor->trySetTrackMidiInput(id, true, device, channel);
            });
            entry.checkable = true;
            entry.checked = current == app::MidiInput{device, channel} || (current && current->device == device);
        };
        add(kAllIns, QString());
        const QStringList connected = bridge->midiInputChoices();  // (the computer keyboard last)
        QStringList names;
        if (connected.contains(app::kComputerKeyboard)) names << app::kComputerKeyboard;
        for (const QString& name : connected) {
            if (name != app::kComputerKeyboard) names << name;
        }
        if (current && !current->device.isEmpty() && !names.contains(current->device)) names << current->device;
        for (const QString& name : names)
            add(connected.contains(name) ? name : QStringLiteral("%1 (not connected)").arg(name), name);
        menu.addSeparator();
        addConfigure(menu, true);
        menu.addSeparator();
        MenuEntry& none = menu.add(kNoInput, [editor, id] { editor->trySetTrackMidiInput(id, false); });
        none.checkable = true;
        none.checked = !current.has_value();
        return menu;
    }
    const app::Project& p = *project();
    const bool external = !t->inputTrack && !t->input.empty();
    MenuEntry& ext = menu.add(kExtIn, [editor, bridge, id, external] {
        if (external) return;
        // The first pair (or the only channel) of the device's inputs.
        const int count = static_cast<int>(bridge->inputNames().size());
        editor->trySetTrackInput(id, count == 1 ? QList<int>{0} : QList<int>{0, 1});
    });
    ext.checkable = true;
    ext.checked = external;
    ext.toolTip = QStringLiteral("The audio device's inputs: the channel, or pair, below");
    addConfigure(menu);
    menu.addSeparator();
    // (The menu, or the search field's list.)
    const auto addSource = [&](auto& list, const QString& sourceId) -> MenuEntry& {
        const bool usable = !p.inputWouldCycle(id, sourceId);
        const QString label = p.inputName(sourceId);
        MenuEntry& entry = list.add(usable ? label : QStringLiteral("%1 (it takes this track's output)").arg(label),
                                    [editor, id, sourceId] { editor->trySetTrackInputTrack(id, sourceId); });
        entry.checkable = true;
        entry.checked = t->inputTrack == sourceId;
        entry.enabled = usable;
        return entry;
    };
    addSource(menu, app::kMaster).toolTip = QStringLiteral("The master's output: recorded, not heard (it would feed back)");
    const std::vector<const app::Track*> sources = p.inputSources(id);
    if (!sources.empty()) {
        MenuList tracks(menu.addSearch().children);  // (filled before the menu grows)
        for (const app::Track* source : sources) addSource(tracks, source->id);
    }
    menu.addSeparator();
    MenuEntry& none = menu.add(kNoInput, [editor, id] { editor->trySetTrackInput(id, {}); });
    none.checkable = true;
    none.checked = !t->hasInput();
    return menu;
}

MenuEntries TrackHeaderItem::inputChannelMenu() {
    // The channel (or pair) of the audio device's inputs; where an input from a
    // track is taken; a MIDI input's channel.
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || !t->hasClips() || inputChannelText().isEmpty()) return menu;
    app::ProjectEditor* editor = session_->editor();
    const QString id = trackId_;
    if (t->isMidi()) {
        const QString device = t->midiInput->device;
        for (int number = 0; number <= 16; ++number) {
            MenuEntry& entry = menu.add(channelLabel(number), [editor, id, device, number] {
                editor->trySetTrackMidiInput(id, true, device, number);
            });
            entry.checkable = true;
            entry.checked = number == t->midiInput->channel;
            if (number == 0) menu.addSeparator();
        }
        return menu;
    }
    if (t->inputTrack) {
        for (const QString& tap : kInputTaps) {
            MenuEntry& entry = menu.add(tapLabel(tap), [editor, id, tap] { editor->trySetTrackInputTap(id, tap); });
            entry.checkable = true;
            entry.checked = t->inputTap == tap;
        }
        return menu;
    }
    const QStringList names = session_->bridge()->inputNames();
    if (names.isEmpty()) {
#ifdef Q_OS_WIN
        menu.add(QStringLiteral("The audio device has no inputs (choose an ASIO driver)")).enabled = false;
#else  // (no ASIO here)
        menu.add(QStringLiteral("The audio device has no inputs")).enabled = false;
#endif
        addConfigure(menu);
        return menu;
    }
    const auto choices = inputChoices(static_cast<int>(names.size()));
    for (const auto& [label, channels] : choices) {
        if (channels.size() == 2 && channels.front() == 0 && names.size() > 1) menu.addSeparator();
        const QList<int> list(channels.begin(), channels.end());
        MenuEntry& entry = menu.add(label, [editor, id, list] { editor->trySetTrackInput(id, list); });
        entry.checkable = true;
        entry.checked = t->input == channels;
        QStringList named;
        for (int c : channels) named << names[c];
        entry.toolTip = named.join(QStringLiteral(", "));
    }
    return menu;
}

TrackHeaderItem::OutputTarget TrackHeaderItem::outputTarget(const QString& targetId) const {
    // An audio track's input, and the devices on it with a sidechain input.
    OutputTarget target;
    const app::Track* t = project()->findTrack(targetId);
    if (t == nullptr) return target;
    target.track = t;
    target.trackIn = t->isAudio();
    for (const app::Device* device : app::iterDevices(t->devices)) {
        if (session_->bridge()->hasSidechainInput(targetId, device->id)) target.devices << device->id;
    }
    return target;
}

std::vector<TrackHeaderItem::OutputTarget> TrackHeaderItem::outputTargets() const {
    // As Ableton lists them: the tracks (and returns) with an input or a device
    // taking a sidechain, but this one and its own group (listed apart).
    std::vector<OutputTarget> targets;
    const app::Track* t = track();
    if (!t) return targets;
    for (const app::Track* other : project()->senders()) {
        if (other->id == trackId_ || other->id == t->parent) continue;
        OutputTarget target = outputTarget(other->id);
        if (target.trackIn || !target.devices.isEmpty()) targets.push_back(std::move(target));
    }
    return targets;
}

MenuEntries TrackHeaderItem::outputMenu() {
    // As Ableton's Audio To: Ext. Out (not here yet), Configure..., Main (the
    // master), its group, the tracks it can go into under a search field (their
    // input, or a device's sidechain there; those it feeds greyed out: going
    // into them would close a cycle), Sends Only.
    MenuEntries menu;
    const app::Track* t = track();
    if (!t || isMaster()) return menu;
    app::ProjectEditor* editor = session_->editor();
    const app::Project& p = *project();
    const QString id = trackId_;
    // (To the menu, or to the search field's list.)
    const auto add = [&](auto& list, const QString& label, const app::Output& output, bool checked) -> MenuEntry& {
        const QString to = output.to == app::Output::To::Track       ? QStringLiteral("track")
                           : output.to == app::Output::To::Sidechain ? QStringLiteral("sidechain")
                           : output.to == app::Output::To::Master    ? QStringLiteral("master")
                           : output.to == app::Output::To::None      ? QStringLiteral("none")
                                                                     : QStringLiteral("group");
        const QString target = output.id;
        app::Session* session = session_;
        MenuEntry& entry = list.add(label, [editor, session, id, to, target] {
            if (editor->trySetTrackOutput(id, to, target) && to == u"track") hintTrackIn(session, target);
        });
        entry.checkable = true;
        entry.checked = checked;
        return entry;
    };
    MenuEntry& ext = menu.add(QStringLiteral("Ext. Out"));
    ext.enabled = false;
    ext.toolTip = QStringLiteral("Tracks can't play on the audio device's outputs directly (yet): they go into the master");
    addConfigure(menu);
    menu.addSeparator();
    const bool toMain = t->output.to == app::Output::To::Master || (t->output.isDefault() && !t->parent);
    add(menu, kMain, app::Output::master(), toMain);
    if (t->parent) add(menu, p.track(*t->parent).name, app::Output::group(), t->output.isDefault());
    const std::vector<OutputTarget> targets = outputTargets();
    if (!targets.empty()) {
        MenuList tracks(menu.addSearch().children);  // (filled before the menu grows)
        const std::optional<QString> now = outputTrack();
        for (const OutputTarget& target : targets) {
            // Into its input, or (a track without one) the first device taking a
            // sidechain; the one it goes into now if it goes there (which one is
            // outputChannelMenu()'s).
            const bool going = now == target.track->id && !t->output.isDefault();
            const app::Output output = going           ? t->output
                                       : target.trackIn ? app::Output::track(target.track->id)
                                                        : app::Output::sidechain(target.devices.front());
            const bool usable = !p.outputWouldCycle(id, output);
            const QString name = target.track->name;
            MenuEntry& entry = add(tracks, usable ? name : QStringLiteral("%1 (it feeds this track)").arg(name), output, going);
            entry.enabled = usable || going;
        }
    }
    menu.addSeparator();
    add(menu, kSendsOnly, app::Output::none(), t->output.to == app::Output::To::None).toolTip =
        QStringLiteral("Only its sends are heard");
    return menu;
}

MenuEntries TrackHeaderItem::mainOutMenu() {
    // The audio device's outputs the master plays on (an ASIO device's pairs:
    // choosing one opens the device again; other drivers play on the first two).
    MenuEntries menu;
    if (!isMaster() || !session_) return menu;
    app::EngineBridge* bridge = session_->bridge();
    const app::AudioDeviceStatus status = bridge->deviceStatus();
    const auto choices = app::outputChoices(bridge->deviceCapabilities().outputNames);
    if (status.open && status.backend == u"ASIO" && !choices.empty()) {
        const std::vector<int> current(status.outputChannels.begin(), status.outputChannels.end());
        for (const auto& [label, channels] : choices) {
            MenuEntry& entry = menu.add(channelsLabel(channels), [bridge, status, channels] {
                // As the device runs now, on these outputs (saved if it opens).
                app::AudioSettings settings;
                settings.driver = status.backend;
                settings.deviceName = status.name;
                settings.sampleRate = status.sampleRate;
                settings.bufferFrames = status.bufferFrames;
                settings.inputChannels.assign(status.inputChannels.begin(), status.inputChannels.end());
                settings.outputChannels = channels;
                const QString error = bridge->openDevice(settings);
                if (error.isEmpty()) {
                    settings.save();
                } else {
                    Q_EMIT bridge->statusMessage(QStringLiteral("The outputs could not be opened: ") + error);
                }
            });
            entry.checkable = true;
            entry.checked = current == channels;
            entry.toolTip = label;
        }
    } else {
        MenuEntry& entry = menu.add(mainOutText());
        entry.checkable = true;
        entry.checked = true;
        entry.enabled = false;
        entry.toolTip = QStringLiteral("The master plays on the audio device's first two outputs (an ASIO device's can be chosen)");
    }
    menu.addSeparator();
    addConfigure(menu);
    return menu;
}

MenuEntries TrackHeaderItem::outputChannelMenu() {
    // Where in the track it goes into: its input (Track In), or a device's sidechain.
    MenuEntries menu;
    const app::Track* t = track();
    const std::optional<QString> now = outputTrack();
    if (!t || isMaster() || outputChannelText().isEmpty() || !now) return menu;
    app::ProjectEditor* editor = session_->editor();
    const app::Project& p = *project();
    const QString id = trackId_;
    const OutputTarget target = outputTarget(*now);
    if (target.trackIn) {
        const bool usable = !p.outputWouldCycle(id, app::Output::track(*now));
        const QString to = *now;
        app::Session* session = session_;
        MenuEntry& entry = menu.add(kTrackIn, [editor, session, id, to] {
            if (editor->trySetTrackOutput(id, QStringLiteral("track"), to)) hintTrackIn(session, to);
        });
        entry.checkable = true;
        entry.checked = t->output.to == app::Output::To::Track;
        entry.enabled = usable || entry.checked;
    }
    for (const QString& deviceId : target.devices) {
        const app::Device* device = p.findDevice(*now, deviceId);
        if (device == nullptr) continue;
        const bool usable = !p.outputWouldCycle(id, app::Output::sidechain(deviceId));
        MenuEntry& entry = menu.add(sidechainLabel(*device), [editor, id, deviceId] {
            editor->trySetTrackOutput(id, QStringLiteral("sidechain"), deviceId);
        });
        entry.checkable = true;
        entry.checked = t->output == app::Output::sidechain(deviceId);
        entry.enabled = usable || entry.checked;
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
        if (anyAutomated) entry.dot = Theme::automationOn();
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
            if (automated.contains(spec.key)) entry.dot = Theme::automationOn();
        }
    }
    if (menu.isEmpty()) menu.add(QStringLiteral("No parameters")).enabled = false;
    return menu;
}

// --- Painting ----------------------------------------------------------------------------------------------

namespace {

// The ground under a strip's name column below its name bar (its automation
// choosers'): the header's, tinted with its colour, as Ableton shades it.
QColor shade(const QColor& color) {
    constexpr double kTint = 0.16;
    const QColor ground = Theme::panelAlt();
    return QColor::fromRgbF(ground.redF() + kTint * (color.redF() - ground.redF()),
                            ground.greenF() + kTint * (color.greenF() - ground.greenF()),
                            ground.blueF() + kTint * (color.blueF() - ground.blueF()));
}

}  // namespace

void TrackHeaderItem::paint(SgPainter& p) {
    const app::Track* t = track();
    const double w = width(), h = height();
    const bool isSelected = selected();
    p.fillRect(QRectF(0, 0, w, h), isSelected ? Theme::laneSelected() : Theme::panelAlt());
    if (!t) return;
    const double main = row_.mainHeight;
    const bool strip = isReturn() || isMaster();  // (no fold button, no bands)
    // The name column: from the group bands to the In/Out column (or the mixer's).
    const double left = strip ? 1.0 : indent();
    const double right = (ioLeft_ > 0 ? ioLeft_ : mixerLeft_ > 0 ? mixerLeft_ : w) - 1;
    const QColor color = barColor();
    // Its colour: a track's name row; a group's down to its choosers (all of
    // it without them); below, a dark shade of it.
    const double bar = folded_ || (t->isGroup() && !automationShown_) ? main - 1
                       : t->isGroup()                                 ? double(kGroupBlock)
                                                                      : double(kNamePad + kNameButton + kNamePad);
    p.fillRect(QRectF(left, 0, right - left, bar), color);
    if (bar < main - 1) p.fillRect(QRectF(left, bar, right - left, main - 1 - bar), shade(color));
    // The lanes below it: a row each, with a line above.
    for (const LaneRow& lane : row_.lanes) {
        const double top = lane.top - rowTop_;
        p.fillRect(QRectF(left, top, right - left, lane.height), shade(color));
        p.fillRect(QRectF(0, top, w, 1), Theme::gridBar());
    }
    // The columns' edges.
    p.fillRect(QRectF(right, 0, 1, h), Theme::border());
    if (ioLeft_ > 0 && mixerLeft_ > ioLeft_) p.fillRect(QRectF(mixerLeft_ - 1, 0, 1, h), Theme::border());
    const QColor ink = barText();
    const QColor dimInk = QColor::fromRgbF(0.5 * (ink.redF() + color.redF()), 0.5 * (ink.greenF() + color.greenF()),
                                           0.5 * (ink.blueF() + color.blueF()));
    if (!strip) {
        // The fold button, in a circle. A track's: a triangle, pointing right
        // while folded, down while open. A group's: three bars (its tracks),
        // folded or not, as Ableton's (folded, its height and its lane show it).
        const QRectF rect = foldRect();
        const QPointF c = rect.center();
        const double radius = 5.5;
        p.save();
        p.setAntialiasing(true);
        p.drawEllipse(QRectF(c.x() - radius, c.y() - radius, 2 * radius, 2 * radius), ink, 1.2);
        if (t->isGroup()) {
            for (const double dy : {-2.5, 0.0, 2.5}) {
                const double half = dy == 0.0 ? 2.8 : 2.2;
                const QRectF line(c.x() - half, c.y() + dy - 0.6, 2 * half, 1.2);
                p.fillPolygon(QPolygonF({line.topLeft(), line.topRight(), line.bottomRight(), line.bottomLeft()}), ink);
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
    if (!renaming_) {
        const QFont font = uiFont(9, isSelected || strip);
        const double nameX = nameLeft();
        const QRectF nameRect(nameX, kNamePad, nameRight_ - nameX, kNameButton);
        const QString name = isMaster() ? kMain : t->name;
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, SgPainter::elidedText(name, font, nameRect.width()),
                   mute() ? dimInk : ink, font);
    }
    p.fillRect(QRectF(0, h - 1, w, 1), Theme::border());
    p.fillRect(QRectF(0, 0, 1, h), Theme::border());
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
