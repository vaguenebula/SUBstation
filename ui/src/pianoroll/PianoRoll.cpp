#include "pianoroll/PianoRoll.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "harmony/Accompaniment.h"
#include "intelligence/Harmony.h"
#include "intelligence/Humanizer.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "model/Project.h"
#include "pianoroll/NoteGrid.h"
#include "session/Selection.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

using app::Note;
namespace notes = app::notes;

PianoRoll::PianoRoll(QObject* parent) : QObject(parent), rng_(QRandomGenerator::global()->generate()) {
    view_.setGridLevel(1);  // a little wider than the arrangement's: 1/16 notes across a bar
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(kChordPreviewMs);
    connect(&previewTimer_, &QTimer::timeout, this, &PianoRoll::stopPreview);
}

PianoRoll::~PianoRoll() {
    // Keys still sounding stop with the roll.
    if (session_) {
        if (auditioned_ && !auditionTrack_.isEmpty()) bridge()->previewNote(auditionTrack_, *auditioned_, 0);
        for (const auto& [track, pitch] : previewing_) bridge()->previewNote(track, pitch, 0);
    }
}

void PianoRoll::setSession(app::Session* session) {
    if (session == session_) return;
    releaseAudition();
    stopPreview();
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
        if (showsTrack(trackId)) refresh();
    });
    connect(p, &app::Project::settingsChanged, this, &PianoRoll::repaintAll);
    connect(p, &app::Project::trackChanged, this, [this](const QString& trackId) {
        if (showsTrack(trackId)) repaintAll();
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

// --- The clips -----------------------------------------------------------------------

void PianoRoll::setClips(const app::ClipRefs& refs) {
    releaseAudition();
    stopPreview();
    if (refs != clips_) {
        clips_ = refs;
        selected_.clear();
        span_.reset();
        pasteBeat_.reset();
        toolsWanted_ = false;
        fitPending_ = !refs.isEmpty();
        Q_EMIT clipChanged();
        Q_EMIT selectionChanged();
    }
    refreshHarmony();
    refresh();
}

void PianoRoll::setClip(const QString& trackId, const QString& clipId) {
    setClips(trackId.isEmpty() || clipId.isEmpty() ? app::ClipRefs() : app::ClipRefs{{trackId, clipId}});
}

const app::Clip* PianoRoll::clipAt(int index) const {
    const app::Project* p = project();
    if (!p || index < 0 || index >= clipCount()) return nullptr;
    const app::ClipRef& ref = clips_[index];
    const app::Clip* found = p->findClip(ref.trackId, ref.clipId);
    return found && found->isMidi() ? found : nullptr;
}

QColor PianoRoll::colorOf(int index) const {
    return clipAt(index) ? QColor(project()->track(clips_[index].trackId).color) : Theme::accent();
}

bool PianoRoll::showsTrack(const QString& trackId) const {
    return std::any_of(clips_.begin(), clips_.end(), [&](const app::ClipRef& ref) { return ref.trackId == trackId; });
}

void PianoRoll::relayout() {
    // One clip: its content beats; several: the arrangement's beats.
    const app::Clip* lead = clip();
    origin_ = clips_.size() == 1 && lead ? lead->startBeat - lead->offsetBeats : 0.0;
    shifts_.assign(clips_.size(), 0.0);
    for (int i = 0; i < clipCount(); ++i) {
        if (const app::Clip* c = clipAt(i)) shifts_[static_cast<size_t>(i)] = c->startBeat - c->offsetBeats - origin_;
    }
}

std::vector<PianoRoll::Span> PianoRoll::windows() const {
    std::vector<Span> spans;
    for (int i = 0; i < clipCount(); ++i) {
        if (const app::Clip* c = clipAt(i)) spans.emplace_back(c->offsetBeats + shift(i), c->windowEnd() + shift(i));
    }
    return spans;
}

std::vector<ClipNote> PianoRoll::allNotes() const {
    std::vector<ClipNote> all;
    forEachNote([&](int clip, const Note& note) { all.push_back({clip, note}); });
    return all;
}

std::optional<int> PianoRoll::clipFor(double beat, const QString& trackId) const {
    for (int i = 0; i < clipCount(); ++i) {
        if (!trackId.isEmpty() && clips_[i].trackId != trackId) continue;
        const app::Clip* c = clipAt(i);
        if (c && c->offsetBeats + shift(i) <= beat + 1e-9 && beat < c->windowEnd() + shift(i) - 1e-9) return i;
    }
    if (!trackId.isEmpty() || !clip()) return std::nullopt;
    return 0;
}

void PianoRoll::refresh() {
    relayout();
    const size_t count = selected_.size();
    std::map<int, std::vector<Note>> present;  // each clip's notes, as a set (looked up once a clip)
    std::vector<ClipNote> kept;
    for (const ClipNote& note : selected_) {
        auto found = present.find(note.clip);
        if (found == present.end()) {
            const app::Clip* c = clipAt(note.clip);
            found = present.emplace(note.clip, c ? roll::noteSet(c->notes) : std::vector<Note>()).first;
        }
        if (roll::contains(found->second, note.note)) kept.push_back(note);
    }
    selected_ = std::move(kept);
    if (selected_.size() != count) {
        span_.reset();
        Q_EMIT selectionChanged();
    }
    fitIfReady();
    updateBars();
    if (app::EngineBridge* b = bridge()) onPosition(b->position());
    repaintAll();
}

void PianoRoll::commitNotes(const std::map<int, std::vector<Note>>& notes, const QString& text,
                            const QString& mergeKey, const std::optional<std::vector<ClipNote>>& selected) {
    std::vector<std::pair<app::ClipRef, std::vector<Note>>> changes;
    for (const auto& [index, list] : notes) {
        if (clipAt(index)) changes.emplace_back(clips_[index], list);
    }
    if (changes.empty()) return;
    if (selected) {
        selected_ = roll::clipNoteSet(*selected);
        span_.reset();
        Q_EMIT selectionChanged();
    }
    editor()->setClipsNotes(changes, text, mergeKey);
    repaintAll();
}

void PianoRoll::commit(const std::vector<Note>& clipNotes, const QString& text, const QString& mergeKey,
                       const std::optional<std::vector<Note>>& selected) {
    std::optional<std::vector<ClipNote>> chosen;
    if (selected) chosen = roll::tagged(*selected, 0);
    commitNotes({{0, clipNotes}}, text, mergeKey, chosen);
}

// --- Selected notes ------------------------------------------------------------------

void PianoRoll::selectNotes(const std::vector<ClipNote>& notes, bool tools) {
    selected_ = roll::clipNoteSet(notes);
    span_.reset();
    toolsWanted_ = tools && !selected_.empty();
    Q_EMIT selectionChanged();
    repaintAll();
}

void PianoRoll::setSelection(const std::vector<Note>& notes, bool tools) { selectNotes(roll::tagged(notes, 0), tools); }

std::vector<Note> PianoRoll::selected() const {
    std::vector<Note> result;
    result.reserve(selected_.size());
    for (const ClipNote& note : selected_) result.push_back(note.note);
    return result;
}

bool PianoRoll::isSelected(const ClipNote& note) const { return roll::contains(selected_, note); }

void PianoRoll::setSelectedSpan(const std::optional<Span>& span) {
    if (span == span_) return;
    span_ = span;
    repaintAll();
}

// --- Editing the selection -------------------------------------------------------------

std::optional<PianoRoll::Span> PianoRoll::selectionStretch() const {
    if (selected_.empty()) return std::nullopt;
    double start = rollStart(selected_.front()), end = start;
    for (const ClipNote& note : selected_) {
        start = std::min(start, rollStart(note));
        end = std::max(end, rollStart(note) + note.note.length);
    }
    if (span_) {
        start = std::min(start, span_->first);
        end = std::max(end, span_->second);
    }
    return Span{start, end};
}

void PianoRoll::duplicateSelected() {
    const auto stretch = selectionStretch();
    if (!stretch) return;
    const double length = stretch->second - stretch->first;
    const bool spanned = span_.has_value();
    std::map<int, std::vector<Note>> changes;
    std::vector<ClipNote> copies;
    for (const int index : roll::clipsOf(selected_)) {
        const app::Clip* c = clipAt(index);
        if (!c) continue;
        const std::vector<Note> moved = notes::shifted(roll::notesOf(selected_, index), length, 0);
        changes[index] = notes::place(c->notes, {}, moved);
        for (const Note& note : moved) copies.push_back({index, note});
    }
    commitNotes(changes, QStringLiteral("Duplicate Notes"), {}, copies);
    // The stretch after it is selected now: Ctrl+D again goes on.
    if (spanned) setSelectedSpan(Span{stretch->first + length, stretch->second + length});
}

void PianoRoll::copySelected() {
    const auto stretch = selectionStretch();
    if (!stretch) return;
    Copied copied;
    copied.start = stretch->first;
    copied.length = stretch->second - stretch->first;
    for (const ClipNote& note : selected_) {
        Note relative = note.note;
        relative.start = rollStart(note) - stretch->first;
        copied.notes.emplace_back(clips_[note.clip].trackId, relative);
    }
    copied_ = std::move(copied);
    Q_EMIT copiedChanged();
}

void PianoRoll::cutSelected() {
    if (selected_.empty()) return;
    copySelected();
    std::map<int, std::vector<Note>> changes;
    for (const int index : roll::clipsOf(selected_)) {
        if (const app::Clip* c = clipAt(index)) changes[index] = notes::place(c->notes, roll::notesOf(selected_, index), {});
    }
    commitNotes(changes, selected_.size() == 1 ? QStringLiteral("Cut Note") : QStringLiteral("Cut Notes"), {},
                std::vector<ClipNote>());
}

void PianoRoll::paste() {
    if (copied_.notes.empty() || !clip()) return;
    const double at = std::max(0.0, pasteBeat_ ? *pasteBeat_ : copied_.start + copied_.length);
    std::map<int, std::vector<Note>> added;
    for (const auto& [trackId, note] : copied_.notes) {
        const double beat = at + note.start;
        std::optional<int> target = clipFor(beat, trackId);  // on its own track, where one plays there
        if (!target) target = clipFor(beat);
        if (!target) continue;
        Note placed = note;
        placed.start = std::max(0.0, beat - shift(*target));
        added[*target].push_back(placed);
    }
    std::map<int, std::vector<Note>> changes;
    std::vector<ClipNote> pasted;
    for (const auto& [index, list] : added) {
        changes[index] = notes::place(clipAt(index)->notes, {}, list);
        for (const Note& note : list) pasted.push_back({index, note});
    }
    if (changes.empty()) return;
    commitNotes(changes, pasted.size() == 1 ? QStringLiteral("Paste Note") : QStringLiteral("Paste Notes"), {},
                pasted);
    setSelectedSpan(Span{at, at + copied_.length});
    setPasteBeat(at + copied_.length);  // pasting again appends
}

void PianoRoll::toggleSelectedActive() {
    if (selected_.empty()) return;
    const bool activate =
        std::all_of(selected_.begin(), selected_.end(), [](const ClipNote& note) { return note.note.muted; });
    std::map<int, std::vector<Note>> changes;
    std::vector<ClipNote> toggled;
    for (const int index : roll::clipsOf(selected_)) {
        const app::Clip* c = clipAt(index);
        if (!c) continue;
        const std::vector<Note> targets = roll::notesOf(selected_, index);
        changes[index] = notes::withActive(c->notes, targets, activate);
        for (Note note : targets) {
            note.muted = !activate;
            toggled.push_back({index, note});
        }
    }
    const std::optional<Span> span = span_;
    const QString what = selected_.size() == 1 ? QStringLiteral(" Note") : QStringLiteral(" Notes");
    commitNotes(changes, (activate ? QStringLiteral("Activate") : QStringLiteral("Deactivate")) + what, {}, toggled);
    setSelectedSpan(span);  // (the same stretch stays selected)
}

// --- Note tools ------------------------------------------------------------------------

void PianoRoll::placeTools() {
    const bool shown = toolsWanted_ && !(grid_ && grid_->dragging()) && !selected_.empty();
    if (shown) {
        QRectF area;
        for (const ClipNote& note : selected_) area = area.isNull() ? noteRect(note) : area.united(noteRect(note));
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

std::vector<ClipNote> PianoRoll::toolTargets() const {
    std::vector<ClipNote> targets = selected_.empty() ? allNotes() : selected_;
    // By time on the roll (Humanize › Timing draws its numbers in this order).
    std::stable_sort(targets.begin(), targets.end(), [this](const ClipNote& a, const ClipNote& b) {
        const double sa = rollStart(a), sb = rollStart(b);
        if (sa != sb) return sa < sb;
        return a.note.pitch < b.note.pitch;
    });
    return targets;
}

void PianoRoll::applyTool(const std::vector<ClipNote>& targets, const std::vector<ClipNote>& changed,
                          const QString& text) {
    if (targets.empty() || changed.size() != targets.size()) return;
    std::map<int, std::vector<Note>> changes;
    for (const int index : roll::clipsOf(targets)) {
        const app::Clip* c = clipAt(index);
        if (!c) continue;
        std::vector<Note> removed, added;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i].clip != index) continue;
            removed.push_back(targets[i].note);
            added.push_back(changed[i].note);
        }
        changes[index] = notes::place(c->notes, removed, added);
    }
    commitNotes(changes, text, {}, selected_.empty() ? std::vector<ClipNote>() : changed);
}

void PianoRoll::applyOnRoll(const std::vector<ClipNote>& targets,
                            const std::function<std::vector<Note>(const std::vector<Note>&)>& tool,
                            const QString& text) {
    std::vector<Note> onRoll;
    onRoll.reserve(targets.size());
    for (const ClipNote& note : targets) {
        Note moved = note.note;
        moved.start = rollStart(note);
        onRoll.push_back(moved);
    }
    const std::vector<Note> changed = tool(onRoll);  // (note for note, in order)
    std::vector<ClipNote> back;
    for (size_t i = 0; i < targets.size() && i < changed.size(); ++i) {
        Note note = changed[i];
        note.start = std::max(0.0, note.start - shift(targets[i].clip));
        back.push_back({targets[i].clip, note});
    }
    applyTool(targets, back, text);
}

void PianoRoll::legato() {
    // In each clip, up to its own next notes (or its end).
    const std::vector<ClipNote> targets = toolTargets();
    std::vector<ClipNote> from, to;
    for (const int index : roll::clipsOf(targets)) {
        const app::Clip* c = clipAt(index);
        if (!c) continue;
        const std::vector<Note> mine = roll::notesOf(targets, index);
        const std::vector<Note> changed = notes::legato(mine, c->notes, c->windowEnd());
        for (size_t i = 0; i < mine.size() && i < changed.size(); ++i) {
            from.push_back({index, mine[i]});
            to.push_back({index, changed[i]});
        }
    }
    applyTool(from, to, QStringLiteral("Legato"));
}

void PianoRoll::scaleTime(double factor) {
    applyOnRoll(toolTargets(), [factor](const std::vector<Note>& notes) { return notes::timeScaled(notes, factor); },
                factor > 1 ? QStringLiteral("Timing ×2") : QStringLiteral("Timing ÷2"));
}

void PianoRoll::quantize() {
    const double step = quantizeStep(), amount = quantizeAmount_ / 100.0;
    applyOnRoll(toolTargets(), [step, amount](const std::vector<Note>& notes) { return notes::quantized(notes, step, amount); },
                QStringLiteral("Quantize"));
}

void PianoRoll::humanizeVelocity() {
    app::Humanizer* humanizer = session_ ? session_->humanizer() : nullptr;
    const std::vector<ClipNote> targets = toolTargets();
    if (!humanizer || targets.empty()) return;
    std::vector<app::Humanizer::Target> wanted;
    wanted.reserve(targets.size());
    for (const ClipNote& note : targets) wanted.push_back({clips_[note.clip].trackId, clips_[note.clip].clipId, note.note});
    const auto velocities = humanizer->velocities(wanted, humanizeVelocityAmount_ / 100.0);
    if (!velocities) return;
    std::vector<ClipNote> changed = targets;
    for (size_t i = 0; i < changed.size(); ++i) changed[i].note.velocity = (*velocities)[i];
    applyTool(targets, changed, QStringLiteral("Humanize Velocity"));
}

void PianoRoll::humanizeTiming() {
    const double amount = humanizeTimingAmount_ / 100.0;
    applyOnRoll(toolTargets(),
                [this, amount](const std::vector<Note>& notes) { return notes::humanizedTiming(notes, rng_, amount); },
                QStringLiteral("Humanize Timing"));
}

QStringList PianoRoll::quantizeGrids() {
    QStringList names;
    for (const auto& grid : notes::kQuantizeGrids) names.append(QString::fromLatin1(grid.name));
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

void PianoRoll::setHumanizeVelocityAmount(double percent) {
    percent = std::clamp(percent, 0.0, 100.0);
    if (percent == humanizeVelocityAmount_) return;
    humanizeVelocityAmount_ = percent;
    Q_EMIT toolSettingsChanged();
}

void PianoRoll::setHumanizeTimingAmount(double percent) {
    percent = std::clamp(percent, 0.0, 100.0);
    if (percent == humanizeTimingAmount_) return;
    humanizeTimingAmount_ = percent;
    Q_EMIT toolSettingsChanged();
}

bool PianoRoll::velocityModelAvailable() const { return session_ && session_->humanizer()->velocityAvailable(); }

double PianoRoll::quantizeStep() const {
    for (const auto& grid : notes::kQuantizeGrids) {
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
    if (h && clip() && h->shown() && gridShowing()) {
        key = h->key();
        double from = clip()->startBeat, to = clip()->endBeat();  // what the clips play, on the timeline
        for (int i = 1; i < clipCount(); ++i) {
            if (const app::Clip* c = clipAt(i)) {
                from = std::min(from, c->startBeat);
                to = std::max(to, c->endBeat());
            }
        }
        const double shift = -origin();  // timeline beats to roll beats
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
    const std::vector<Note> notes = notes::normalize(notes::resolveOverlaps(all, c->notes));
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

QRectF PianoRoll::noteRect(const ClipNote& note) const {
    const double start = rollStart(note);
    const double x0 = view_.beatToX(start), x1 = view_.beatToX(start + note.note.length);
    return QRectF(x0, pitchTop(note.note.pitch), std::max(3.0, x1 - x0), rowHeight_);
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
    if (!fitPending_ || !clip() || gridWidth() <= 1 || !gridShowing()) return;
    fitPending_ = false;
    const std::vector<Span> lit = windows();
    double from = lit.front().first, to = lit.front().second;
    for (const Span& span : lit) {
        from = std::min(from, span.first);
        to = std::max(to, span.second);
    }
    view_.zoomToFit(from, to, gridWidth() * 0.96);
    const std::vector<ClipNote> all = allNotes();
    int low = kDefaultPitch, high = kDefaultPitch;
    if (!all.empty()) {
        low = high = all.front().note.pitch;
        for (const ClipNote& note : all) {
            low = std::min(low, note.note.pitch);
            high = std::max(high, note.note.pitch);
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
    for (const Span& span : windows()) end = std::max(end, span.second);
    forEachNote([&](int clip, const Note& note) { end = std::max(end, note.end() + shift(clip)); });
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
        stopPreview();
    }
}

void PianoRoll::focusGrid() {
    if (grid_) grid_->forceActiveFocus(Qt::MouseFocusReason);
}

bool PianoRoll::gridShowing() const { return grid_ && grid_->isVisible(); }
double PianoRoll::gridWidth() const { return grid_ ? grid_->width() : 0.0; }
double PianoRoll::gridHeight() const { return grid_ ? grid_->height() : 0.0; }

// --- Playhead and the insert marker ------------------------------------------------------

void PianoRoll::onPosition(double beat) {
    std::optional<double> playhead;
    if (bridge() && bridge()->isPlaying()) {
        for (int i = 0; i < clipCount(); ++i) {
            const app::Clip* c = clipAt(i);
            if (c && c->startBeat <= beat && beat < c->endBeat()) {
                playhead = beat - origin();
                break;
            }
        }
    }
    if (playhead == playhead_) return;
    playhead_ = playhead;
    Q_EMIT playheadChanged();
}

std::optional<double> PianoRoll::startBeat() const {
    if (!clip() || !selection()) return std::nullopt;
    const double beat = selection()->insertBeat() - origin();
    for (const Span& span : windows()) {
        if (span.first - 1e-9 <= beat && beat <= span.second + 1e-9) return beat;
    }
    return std::nullopt;
}

void PianoRoll::setPasteBeat(double beat) {
    if (!clip()) return;
    pasteBeat_ = std::max(0.0, beat);
    repaintAll();
}

void PianoRoll::requestLocate(double rollBeat) {
    if (clip()) Q_EMIT locateRequested(std::max(0.0, rollBeat + origin()));
}

// --- Hearing notes ---------------------------------------------------------------------------

void PianoRoll::setPreview(bool enabled) {
    if (enabled == preview_) return;
    preview_ = enabled;
    if (!enabled) {
        releaseAudition();
        stopPreview();
    }
    Q_EMIT previewChanged();
}

void PianoRoll::audition(int pitch, int velocity, int clip) {
    const QString track = clipAt(clip) ? clips_[clip].trackId : QString();
    if (auditioned_ == pitch && auditionTrack_ == track) return;
    releaseAudition();
    stopPreview();
    if (!preview_ || track.isEmpty()) return;
    bridge()->previewNote(track, pitch, velocity);
    auditioned_ = pitch;
    auditionTrack_ = track;
    Q_EMIT auditionChanged();
}

void PianoRoll::releaseAudition() {
    if (auditioned_ && !auditionTrack_.isEmpty() && bridge()) bridge()->previewNote(auditionTrack_, *auditioned_, 0);
    if (!auditioned_) return;
    auditioned_.reset();
    auditionTrack_.clear();
    Q_EMIT auditionChanged();
}

void PianoRoll::previewNotes(const std::vector<ClipNote>& notes) {
    if (!preview_ || !bridge()) return;
    std::map<std::pair<QString, int>, int> loudest;  // (track, pitch) -> velocity
    for (const ClipNote& note : notes) {
        if (!clipAt(note.clip) || note.note.muted) continue;  // (a deactivated note stays silent)
        const std::pair<QString, int> key{clips_[note.clip].trackId, note.note.pitch};
        if (std::find(previewing_.begin(), previewing_.end(), key) != previewing_.end()) continue;  // (sounding)
        int& velocity = loudest[key];
        velocity = std::max(velocity, note.note.velocity);
    }
    for (const auto& [key, velocity] : loudest) {
        bridge()->previewNote(key.first, key.second, velocity);
        previewing_.push_back(key);
    }
    if (!loudest.empty()) previewTimer_.start();  // (again: from the last caught)
}

void PianoRoll::stopPreview() {
    previewTimer_.stop();
    if (app::EngineBridge* b = bridge()) {
        for (const auto& [track, pitch] : previewing_) b->previewNote(track, pitch, 0);
    }
    previewing_.clear();
}

}  // namespace sub::ui
