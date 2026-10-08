#pragma once
// The song's harmony (the intelligence module's harmony/, sub_intelligence) on
// the application thread's side: Session.harmony.
//
// The song's chords and key, inferred from the notes its MIDI tracks play:
// every MIDI track that is heard (not muted, nor in a muted group) and whose
// name doesn't say it plays drums ("Drums", "Kick", "Hats"...), its clips'
// notes as they play them (the part each clip plays, on the timeline). Audio
// is for later. The piano roll shows them over its notes (the chord lane, notes
// out of the key tinted red) and writes parts from them (Generate); MIDI
// generation and the MCP server will take them as context.
//
// Inferring takes well under a millisecond for a song, so nothing runs in the
// background and nothing is kept but the last result: a change to what it
// hears (the clips of the tracks it hears, which tracks those are, the time
// signature, the key; not a fader, a loop or a tempo) marks it stale and,
// kSettleMs later (once for all the edits of a drag in that time), emits
// changed(). It is inferred again the next time it is asked for, on the
// application's thread: with no piano roll showing, nobody asks, and it costs
// nothing.
//
// The key is the project's when it has one, else the one the notes are most
// likely in (none: too few notes to tell).

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <optional>
#include <vector>

#include "harmony/ChordInference.h"
#include "model/Keys.h"
#include "model/Timebase.h"

namespace sub::app {

class Project;

class Harmony : public QObject {
    Q_OBJECT
    // Whether the piano roll shows the chords and tints notes out of the key (C;
    // saved in the settings, on at first).
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    // The key notes are judged by, as the key chooser names it ("A Minor"; "":
    // none), and whether it was inferred (the project has none).
    Q_PROPERTY(QString keyLabel READ keyLabel NOTIFY changed)
    Q_PROPERTY(bool keyInferred READ keyInferred NOTIFY changed)

public:
    using ChordSpan = intelligence::harmony::ChordSpan;

    static constexpr int kSettleMs = 40;
    inline static const QString kShownKey = QStringLiteral("pianoroll/show_harmony");

    explicit Harmony(Project* project, QObject* parent = nullptr);

    // The song's chords, by time, in timeline beats (inferred now if it changed).
    const std::vector<ChordSpan>& chords() const;
    // The key: the project's, else inferred (none: too few notes to tell).
    std::optional<Key> key() const;
    QString keyLabel() const;
    bool keyInferred() const;

    bool shown() const;
    void setShown(bool shown);

    // The tracks it hears (see above), in order, and the notes they play, in timeline beats.
    static QStringList heardTracks(const Project& project);
    static std::vector<intelligence::harmony::Note> songNotes(const Project& project);
    // Whether a track's name says it plays drums (its notes are no harmony).
    static bool isDrumTrack(const QString& name);
    static intelligence::harmony::Key toHarmony(const Key& key) { return {key.tonic, key.minor}; }
    static Key fromHarmony(const intelligence::harmony::Key& key) { return {key.tonic, key.minor}; }

Q_SIGNALS:
    // The song changed: its chords and key may have (emitted kSettleMs after).
    void changed();
    void shownChanged();

private:
    // What the harmony depends on besides the clips' notes.
    struct Inputs {
        QStringList tracks;  // heardTracks()
        TimeSignature timeSignature;
        std::optional<Key> key;

        friend bool operator==(const Inputs&, const Inputs&) = default;
    };
    Inputs inputs() const;
    // Tracks or settings changed: stale if what it depends on did.
    void projectChanged();
    void invalidate();
    // Infer again if stale.
    const intelligence::harmony::Harmony& current() const;

    QPointer<Project> project_;
    QTimer settle_;
    Inputs inputs_;
    mutable bool stale_ = true;
    mutable intelligence::harmony::Harmony result_;
};

}  // namespace sub::app
