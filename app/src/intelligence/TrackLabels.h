#pragma once
// Track labels (the intelligence module's labels/, sub_intelligence) on the
// application thread's side: Session.trackLabels.
//
// What each track is, in a few words ("Washed Out Serum Pluck", "Orchestral
// Trumpet", "Drum Group"), with what the words were made from: shown over a
// track's name in the arrangement (its tooltip, in the info view), and the
// context the MCP server will hand agents (describe()).
//
// It tells the labeller what the project holds: each track's name (and whether
// it is the user's own), its devices and racks, the preset each plug-in has
// loaded (as the plug-in says now, through the engine; else as its saved state
// says, for a plug-in that isn't loaded: missing, frozen, still loading), the
// parameters the labeller reads (a reverb's mix, an OTT's depth: a plug-in's
// as it has them while loaded, else as its saved state has them, where it keeps
// them as text), each plug-in's VST3 category (from the scan), the
// audio files its clips play and the notes they play (heard ones only), its
// sends, fader, pan and mute.
//
// Labelling takes a few milliseconds for a song of 80 tracks, so nothing runs in
// the background and nothing is kept but the last result: a change to anything the
// labels depend on marks it stale and, kSettleMs after the last of a burst
// (a drag's edits, a plug-in's knob turned), emits changed(). It is labelled
// again the next time anything asks.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <vector>

#include "labels/Facts.h"

namespace sub::app {

class EngineBridge;
class PluginIndex;
class Project;
struct Device;
struct Track;

class TrackLabels : public QObject {
    Q_OBJECT
    // Goes up whenever the labels may have changed (what changed() says).
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    static constexpr int kSettleMs = 250;

    // `bridge` and `plugins` may be null (no live presets or parameters; no categories).
    TrackLabels(Project* project, EngineBridge* bridge, PluginIndex* plugins, QObject* parent = nullptr);

    // A track's label (a group's, a return's, the master's too); "" if there is no such track.
    Q_INVOKABLE QString label(const QString& trackId) const;
    // What a tooltip over its name shows: its label, then what it was made from, a line each.
    Q_INVOKABLE QString toolTip(const QString& trackId) const;
    // Every track, for agents, in the arrangement's order (its tracks, then the
    // returns, then the master): {id, name, kind, parent, label, family, role,
    // traits, details} each.
    Q_INVOKABLE QVariantList describe() const;
    // The labeller's answer for a track (null: no such track).
    const intelligence::labels::TrackLabel* find(const QString& trackId) const;

    int revision() const { return revision_; }

    // What the labeller is told of the project: its tracks, returns and master.
    std::vector<intelligence::labels::TrackFacts> facts() const;
    // Whether a track's name is the user's own, not one SUBstation gave it (after
    // what it holds, its number, a file, an instrument): such names say what it is.
    static bool namedByUser(const Track& track);

Q_SIGNALS:
    void changed();

private:
    void invalidate();
    // Labels again if stale.
    void refresh() const;
    intelligence::labels::DeviceFact deviceFacts(const QString& trackId, const Device& device) const;
    // What a plug-in device's saved state says: its preset, the parameters the labeller reads.
    struct SavedState {
        QString state;  // the text it was read from (shared: its data pointer says whether it is the same)
        QString preset;
        std::vector<intelligence::labels::ParamFact> params;
    };
    SavedState saved(const Device& device) const;  // (a copy: the cache may rehash)

    QPointer<Project> project_;
    QPointer<EngineBridge> bridge_;
    QPointer<PluginIndex> plugins_;
    QTimer settle_;
    int revision_ = 0;
    QHash<QString, QString> categories_;  // plug-in uid -> its VST3 sub-categories
    // Device id -> what its saved state says (decoding a large state again at every change would cost).
    mutable QHash<QString, SavedState> saved_;
    mutable bool stale_ = true;
    mutable QHash<QString, intelligence::labels::TrackLabel> labels_;
};

}  // namespace sub::app
