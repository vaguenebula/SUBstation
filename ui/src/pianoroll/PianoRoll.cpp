#include "pianoroll/PianoRoll.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "harmony/Accompaniment.h"
#include "intelligence/Harmony.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "model/Project.h"
#include "pianoroll/NoteGrid.h"
#include "pianoroll/NoteSet.h"
#include "session/Selection.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

using app::Note;

PianoRoll::PianoRoll(QObject* parent) : QObject(parent), rng_(QRandomGenerator::global()->generate()) {
    view_.setGridLevel(1);  // a little wider than the arrangement's: 1/16 notes across a bar
}

PianoRoll::~PianoRoll() {
    // A key still sounding stops with the roll.
    if (auditioned_ && session_ && !trackId_.isEmpty()) bridge()->previewNote(trackId_, *auditioned_, 0);
}

void PianoRoll::setSession(app::Session* session) {
    if (session == session_) return;
    releaseAudition();
    if (session_) {
        disconnect(session_->project(), nullptr, this, nullptr);
        disconnect(session_->selection(), nullptr, this, nullptr);
        disconnect(session_->bridge(), nullptr, this, nullptr);
        disconnect(session_->harmony(), nullptr, this, nullptr);
    }
    session_ = session;
    view_.setProject(session ? session->project() : nullptr);
    connectSession();
    Q_EMIT sessionChanged();
    refresh();
}

void PianoRoll::connectSession() {
    if (!session_) return;
    app::Project* p = session_->project();
    connect(p, &app::Project::clipsChanged, this, [this](const QString& trackId) {
        if (trackId == trackId_) refresh();
    });
    connect(p, &app::Project::settingsChanged, this, &PianoRoll::repaintAll);
    connect(p, &app::Project::trackChanged, this, [this](const QString& trackId) {
        if (trackId == trackId_) repaintAll();
    });
    // The start marker follows the arrangement's insert marker.
    connect(session_->selection(), &app::Selection::insertChanged, this, &PianoRoll::repaintAll);
    app::EngineBridge* b = session_->bridge();
    connect(b, &app::EngineBridge::positionChanged, this, &PianoRoll::onPosition);
    connect(b, &app::EngineBridge::transportChanged, this, [this](bool) { onPosition(bridge()->position()); });
    connect(session_->harmony(), &app::Harmony::changed, this, &PianoRoll::refreshHarmony);
    connect(session_->harmony(), &app::Harmony::shownChanged, this, &PianoRoll::refreshHarmony);
}

app::Project* PianoRoll::project() const { return session_ ? session_->project() : nullptr; }
app::ProjectEditor* PianoRoll::editor() const { return session_ ? session_->editor() : nullptr; }
app::EngineBridge* PianoRoll::bridge() const { return session_ ? session_->bridge() : nullptr; }
app::Selection* PianoRoll::selection() const { return session_ ? session_->selection() : nullptr; }
app::Harmony* PianoRoll::harmony() const { return session_ ? session_->harmony() : nullptr; }

// --- The clip ------------------------------------------------------------------------

void PianoRoll::setClip(const QString& trackId, const QString& clipId) {
    releaseAudition();
    if (trackId != trackId_ || clipId != clipId_) {
        trackId_ = trackId;
        clipId_ = clipId;
        selected_.clear();
        toolsWanted_ = false;
        fitPending_ = !clipId.isEmpty();
        Q_EMIT clipChanged();
        Q_EMIT selectionChanged();
    }
    refreshHarmony();
    refresh();
}

const app::Clip* PianoRoll::clip() const {
    const app::Project* p = project();
    if (!p || trackId_.isEmpty()) return nullptr;
    const app::Clip* found = p->findClip(trackId_, clipId_);
    return found && found->isMidi() ? found : nullptr;
}

QColor PianoRoll::trackColor() const {
    return clip() ? QColor(project()->track(trackId_).color) : Theme::kAccent;
}

void PianoRoll::refresh() {
    const app::Clip* c = clip();
    const size_t count = selected_.size();
    if (c) {
        const std::vector<Note> notes = roll::noteSet(c->notes);
        std::vector<Note> kept;
        for (const Note& note : selected_) {
            if (roll::contains(notes, note)) kept.push_back(note);
        }
        selected_ = std::move(kept);
    } else {
        selected_.clear();
    }
    if (selected_.size() != count) Q_EMIT selectionChanged();
    fitIfReady();
    updateBars();
    if (app::EngineBridge* b = bridge()) onPosition(b->position());
    repaintAll();
}

void PianoRoll::commit(const std::vector<Note>& clipNotes, const QString& text, const QString& mergeKey,
                       const std::optional<std::vector<Note>>& selected) {
    if (!clip()) return;
    if (selected) {
        selected_ = roll::noteSet(*selected);
        Q_EMIT selectionChanged();
    }
    editor()->setClipNotes({trackId_, clipId_}, clipNotes, text, mergeKey);
    repaintAll();
}

// --- Selected notes ------------------------------------------------------------------

void PianoRoll::setSelection(const std::vector<Note>& notes, bool tools) {
    selected_ = roll::noteSet(notes);
    toolsWanted_ = tools && !selected_.empty();
    Q_EMIT selectionChanged();
    repaintAll();
}

bool PianoRoll::isSelected(const Note& note) const { return roll::contains(selected_, note); }

// --- Note tools ------------------------------------------------------------------------

void PianoRoll::placeTools() {
    const bool shown = toolsWanted_ && !(grid_ && grid_->dragging()) && !selected_.empty();
    if (shown) {
        QRectF area;
        for (const Note& note : selected_) area = area.isNull() ? noteRect(note) : area.united(noteRect(note));
        const int count = static_cast<int>(selected_.size());
        if (toolsShown_ && area == toolsArea_ && count == toolsCount_) return;
        toolsShown_ = true;
        toolsArea_ = area;
        toolsCount_ = count;
        Q_EMIT toolsChanged();
    } else if (toolsShown_) {
        toolsShown_ = false;  // fading out where it was
        Q_EMIT toolsChanged();
    }
}

std::vector<Note> PianoRoll::toolTargets() const {
    const app::Clip* c = clip();
    if (!c) return {};
    return roll::byTime(selected_.empty() ? c->notes : selected_);
}

void PianoRoll::applyTool(const std::vector<Note>& targets, const std::vector<Note>& changed, const QString& text) {
    const app::Clip* c = clip();
    if (!c || targets.empty()) return;
    commit(app::notes::place(c->notes, targets, changed), text, {},
           selected_.empty() ? std::vector<Note>() : changed);
}

void PianoRoll::legato() {
    const app::Clip* c = clip();
    const std::vector<Note> targets = toolTargets();
    if (c) applyTool(targets, app::notes::legato(targets, c->notes, c->windowEnd()), QStringLiteral("Legato"));
}

void PianoRoll::scaleTime(double factor) {
    const std::vector<Note> targets = toolTargets();
    applyTool(targets, app::notes::timeScaled(targets, factor),
              factor > 1 ? QStringLiteral("Timing ×2") : QStringLiteral("Timing ÷2"));
}

void PianoRoll::quantize() {
    const std::vector<Note> targets = toolTargets();
    applyTool(targets, app::notes::quantized(targets, quantizeStep(), quantizeAmount_ / 100.0),
              QStringLiteral("Quantize"));
}

void PianoRoll::humanize() {
    const std::vector<Note> targets = toolTargets();
    applyTool(targets, app::notes::humanized(targets, rng_, humanizeAmount_ / 100.0), QStringLiteral("Humanize"));
}

QStringList PianoRoll::quantizeGrids() {
    QStringList names;
    for (const auto& grid : app::notes::kQuantizeGrids) names.append(QString::fromLatin1(grid.name));
    return names;
}

void PianoRoll::setQuantizeGrid(const QString& grid) {
    if (grid == quantizeGrid_ || !quantizeGrids().contains(grid)) return;
    quantizeGrid_ = grid;
    Q_EMIT toolSettingsChanged();
}

void PianoRoll::setQuantizeAmount(double percent) {
    percent = std::clamp(percent, 0.0, 100.0);
    if (percent == quantizeAmount_) return;
    quantizeAmount_ = percent;
    Q_EMIT toolSettingsChanged();
}

void PianoRoll::setHumanizeAmount(double percent) {
    percent = std::clamp(percent, 0.0, 100.0);
    if (percent == humanizeAmount_) return;
    humanizeAmount_ = percent;
    Q_EMIT toolSettingsChanged();
}

double PianoRoll::quantizeStep() const {
    for (const auto& grid : app::notes::kQuantizeGrids) {
        if (quantizeGrid_ == QLatin1String(grid.name)) return grid.beats;
    }
    return 0.25;
}

// --- Harmony -------------------------------------------------------------------------------

void PianoRoll::refreshHarmony() {
    // Only while it shows: nobody else asks, and the harmony isn't inferred
    // until somebody does.
    std::vector<RollChord> chords;
    std::optional<app::Key> key;
    const app::Harmony* h = harmony();
    const app::Clip* c = clip();
    if (h && c && h->shown() && gridShowing()) {
        key = h->key();
        const double from = c->startBeat, to = c->endBeat();
        const double shift = c->offsetBeats - c->startBeat;  // timeline beats to content beats
        for (const app::Harmony::ChordSpan& span : h->chords()) {
            if (span.end <= from || span.start >= to) continue;
            const auto quality = span.chord.quality;
            using Q = intelligence::harmony::Quality;
            chords.push_back({std::max(span.start, from) + shift, std::min(span.end, to) + shift,
                              QString::fromStdString(span.chord.name()), span.chord.root,
                              quality == Q::Minor || quality == Q::Minor7 || quality == Q::Diminished ||
                                  quality == Q::HalfDiminished7 || quality == Q::Diminished7});
        }
    }
    if (chords == chords_ && key == scaleKey_) return;
    chords_ = std::move(chords);
    scaleKey_ = key;
    repaintAll();
}

bool PianoRoll::outOfKey(int pitch) const {
    return scaleKey_ && !app::Harmony::toHarmony(*scaleKey_).contains(intelligence::harmony::pitchClass(pitch));
}

void PianoRoll::generateChords() { generate(false); }
void PianoRoll::generateBass() { generate(true); }

void PianoRoll::generate(bool bass) {
    namespace harmony = intelligence::harmony;
    const app::Clip* c = clip();
    const app::Harmony* h = this->harmony();
    if (!c || !h) return;
    const double from = c->startBeat, to = c->endBeat();
    const double barBeats = project()->timeSignature().beatsPerBar();
    std::vector<harmony::ChordSpan> spans;
    for (const harmony::ChordSpan& span : h->chords()) {
        if (span.end > from && span.start < to)
            spans.push_back({std::max(span.start, from), std::min(span.end, to), span.chord});
    }
    if (spans.empty()) {
        const auto key = h->key();
        spans = harmony::starterProgression(key ? app::Harmony::toHarmony(*key) : harmony::Key{}, from, to, barBeats);
    }
    std::vector<Note> added;
    for (const harmony::GeneratedNote& n : bass ? harmony::bassPart(spans, barBeats) : harmony::chordPart(spans, barBeats))
        added.push_back({n.pitch, n.start - c->startBeat + c->offsetBeats, n.length, n.velocity});
    // The clip's own notes win where a written one would overlap them on their key.
    std::vector<Note> all = c->notes;
    all.insert(all.end(), added.begin(), added.end());
    const std::vector<Note> notes = app::notes::normalize(app::notes::resolveOverlaps(all, c->notes));
    if (notes == c->notes) return;
    const std::vector<Note> before = roll::noteSet(c->notes);
    std::vector<Note> written;
    for (const Note& note : notes) {
        if (!roll::contains(before, note)) written.push_back(note);
    }
    commit(notes, bass ? QStringLiteral("Generate Bass") : QStringLiteral("Generate Chords"), {}, written);
}

// --- Geometry ------------------------------------------------------------------------------

double PianoRoll::pitchTop(int pitch) const { return (127 - pitch) * rowHeight_ - view_.scrollY(); }

int PianoRoll::pitchAt(double y) const {
    return std::clamp(127 - static_cast<int>(std::floor((y + view_.scrollY()) / rowHeight_)), 0, 127);
}

QRectF PianoRoll::noteRect(const Note& note) const {
    const double x0 = view_.beatToX(note.start), x1 = view_.beatToX(note.end());
    return QRectF(x0, pitchTop(note.pitch), std::max(3.0, x1 - x0), rowHeight_);
}

void PianoRoll::setScrollBeats(double beats) {
    if (view_.setScrollBeats(beats)) viewMoved();
}

void PianoRoll::setScrollY(double y) {
    if (view_.setScrollY(y)) vscrolled();
}

void PianoRoll::zoomAt(double x, double factor) {
    if (view_.zoomAt(x, factor)) viewMoved();
}

void PianoRoll::zoomRows(double notches, double anchorY) {
    const double exact = std::clamp(rowHeightExact_ + notches * kRowHeightStep, double(kMinRowHeight),
                                    double(kMaxRowHeight));
    rowHeightExact_ = exact;
    const int height = static_cast<int>(app::roundHalfEven(exact));
    if (height == rowHeight_) return;
    const double rows = (anchorY + view_.scrollY()) / rowHeight_;
    rowHeight_ = height;
    updateBars();
    view_.setScrollY(rows * height - anchorY);
    vscrolled();
}

void PianoRoll::wheel(const QPointF& pos, const QPoint& delta, Qt::KeyboardModifiers mods) {
    if (mods & Qt::AltModifier && !(mods & Qt::ControlModifier)) {
        // Qt may report Alt+wheel as horizontal scrolling, so accept either axis.
        zoomRows((delta.y() ? delta.y() : delta.x()) / 120.0, pos.y());
    } else if (mods & Qt::ControlModifier) {
        zoomAt(pos.x(), std::pow(1.2, delta.y() / 120.0));
    } else if (mods & Qt::ShiftModifier || delta.x()) {
        const double pixels = -(delta.x() ? delta.x() : delta.y()) / 120.0 * 80.0;
        setScrollBeats(view_.scrollBeats() + pixels / view_.pxPerBeat());
    } else {
        setScrollY(view_.scrollY() - delta.y() / 120.0 * 3 * rowHeight_);
    }
}

double PianoRoll::hScrollPage() const { return std::max(1.0, gridWidth()); }
double PianoRoll::vScrollTotal() const { return view_.maxScrollY() + vScrollPage(); }
double PianoRoll::vScrollPage() const { return std::max(1.0, gridHeight()); }

void PianoRoll::scrollToX(double pixels) {
    const double value = std::clamp(std::floor(pixels), 0.0, std::max(0.0, hTotal_ - hScrollPage()));
    setScrollBeats(value / view_.pxPerBeat());
}

void PianoRoll::scrollToY(double pixels) { setScrollY(pixels); }

void PianoRoll::fitIfReady() {
    // Waits until the note grid has its size (the clip view may not be laid out yet).
    const app::Clip* c = clip();
    if (!fitPending_ || !c || gridWidth() <= 1 || !gridShowing()) return;
    fitPending_ = false;
    view_.zoomToFit(c->offsetBeats, c->windowEnd(), gridWidth() * 0.96);
    int low = kDefaultPitch, high = kDefaultPitch;
    if (!c->notes.empty()) {
        low = high = c->notes.front().pitch;
        for (const Note& note : c->notes) {
            low = std::min(low, note.pitch);
            high = std::max(high, note.pitch);
        }
    }
    updateBars();
    const double centre = (low + high) / 2.0;
    view_.setScrollY((127.5 - centre) * rowHeight_ - gridHeight() / 2);
    viewMoved();
    vscrolled();
}

void PianoRoll::updateBars() {
    const double width = std::max(1.0, gridWidth()), height = std::max(1.0, gridHeight());
    double end = view_.xToBeat(width);
    if (const app::Clip* c = clip()) {
        double notesEnd = 0.0;
        for (const Note& note : c->notes) notesEnd = std::max(notesEnd, note.end());
        end = std::max({end, c->windowEnd(), notesEnd});
    }
    const double contentEnd = end + 4 * view_.timeSignature().beatsPerBar();
    const double hMax = std::max(0.0, std::trunc(contentEnd * view_.pxPerBeat() - width));
    hTotal_ = hMax + width;
    hValue_ = std::trunc(view_.scrollBeats() * view_.pxPerBeat());
    view_.setMaxScrollY(std::max(0, static_cast<int>(128 * rowHeight_ - height)));
    view_.setScrollY(view_.scrollY());  // within the new range
    Q_EMIT scrollBarsChanged();
    vscrolled();
}

void PianoRoll::viewMoved() {
    updateBars();
    Q_EMIT viewChanged();
    repaintAll();
}

void PianoRoll::vscrolled() {
    Q_EMIT vscrollChanged();
    Q_EMIT scrollBarsChanged();
    placeTools();
}

void PianoRoll::repaintAll() {
    Q_EMIT contentChanged();
    placeTools();
}

// --- The note grid -----------------------------------------------------------------------

NoteGrid* PianoRoll::grid() const { return grid_; }

void PianoRoll::setGrid(NoteGrid* grid) {
    grid_ = grid;
    gridGeometryChanged();
}

void PianoRoll::gridGeometryChanged() {
    fitIfReady();
    updateBars();
}

void PianoRoll::gridVisibilityChanged(bool visible) {
    if (visible) {
        fitIfReady();
        updateBars();
        refreshHarmony();
    } else {
        releaseAudition();
    }
}

void PianoRoll::focusGrid() {
    if (grid_) grid_->forceActiveFocus(Qt::MouseFocusReason);
}

bool PianoRoll::gridShowing() const { return grid_ && grid_->isVisible(); }
double PianoRoll::gridWidth() const { return grid_ ? grid_->width() : 0.0; }
double PianoRoll::gridHeight() const { return grid_ ? grid_->height() : 0.0; }

// --- Playhead ----------------------------------------------------------------------------

void PianoRoll::onPosition(double beat) {
    const app::Clip* c = clip();
    std::optional<double> playhead;
    if (c && bridge()->isPlaying() && c->startBeat <= beat && beat < c->endBeat())
        playhead = beat - c->startBeat + c->offsetBeats;
    if (playhead == playhead_) return;
    playhead_ = playhead;
    Q_EMIT playheadChanged();
}

std::optional<double> PianoRoll::startBeat() const {
    const app::Clip* c = clip();
    const app::Selection* s = selection();
    if (!c || !s) return std::nullopt;
    const double beat = s->insertBeat();
    if (!(c->startBeat <= beat && beat <= c->endBeat())) return std::nullopt;
    return beat - c->startBeat + c->offsetBeats;
}

void PianoRoll::requestLocate(double contentBeat) {
    if (const app::Clip* c = clip()) Q_EMIT locateRequested(std::max(0.0, c->toTimeline(contentBeat)));
}

// --- Hearing notes ---------------------------------------------------------------------------

void PianoRoll::setPreview(bool enabled) {
    if (enabled == preview_) return;
    preview_ = enabled;
    if (!enabled) releaseAudition();
    Q_EMIT previewChanged();
}

void PianoRoll::audition(int pitch, int velocity) {
    if (auditioned_ == pitch) return;
    releaseAudition();
    if (!preview_ || !clip()) return;
    bridge()->previewNote(trackId_, pitch, velocity);
    auditioned_ = pitch;
    Q_EMIT auditionChanged();
}

void PianoRoll::releaseAudition() {
    if (auditioned_ && !trackId_.isEmpty() && bridge()) bridge()->previewNote(trackId_, *auditioned_, 0);
    if (!auditioned_) return;
    auditioned_.reset();
    Q_EMIT auditionChanged();
}

}  // namespace sub::ui
