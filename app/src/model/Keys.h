#pragma once
// Musical keys, and reading a sample's tempo and key from its file name.
//
// Sample packs name loops like "Pack_Bass_Loop_128_Am.wav", "Keys 92bpm F# minor"
// or "Vox_Ebmaj_140BPM". parseFilename finds the tempo and key in such a name;
// clipSettings turns them into what a dropped clip starts with: warped at that
// tempo, and transposed to the project's key.

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace sub::app {

struct Clip;

// The names keys are spelled with (flats for the black keys but F#).
inline const QStringList kKeyNoteNames{QStringLiteral("C"),  QStringLiteral("C#"), QStringLiteral("D"),
                                       QStringLiteral("Eb"), QStringLiteral("E"),  QStringLiteral("F"),
                                       QStringLiteral("F#"), QStringLiteral("G"),  QStringLiteral("Ab"),
                                       QStringLiteral("A"),  QStringLiteral("Bb"), QStringLiteral("B")};

// Audio at least this long is warped even when its name gives no tempo (it is
// then taken to be at the project tempo, as when Warp is turned on by hand).
// Shorter files without a tempo in their name are one-shots and play as they are.
inline constexpr double kAutoWarpMinSec = 6.0;
// A tempo in a name outside this is some other number.
inline constexpr double kMinBpm = 50.0;
inline constexpr double kMaxBpm = 250.0;

struct Key {
    int tonic = 0;  // pitch class, 0 = C
    bool minor = false;

    // "F#m", "Bb": as projects save it.
    QString name() const;
    // "F# Minor".
    QString label() const;
    // The tonic of the major key with the same notes (A minor: C).
    int relativeMajor() const;

    friend bool operator==(const Key&, const Key&) = default;
};

// Every key, in the order the key chooser lists them: C, Cm, C#, C#m, ...
std::vector<Key> allKeys();

// A key as Key::name writes it ("F#m", "Bb"), as saved in projects.
std::optional<Key> keyFromName(const QString& text);

// Semitones that bring audio in `source` into `target` (0 if either is
// unknown). A minor key and its relative major share their notes, so a loop
// in A minor needs no shift in C major. The shift is the smallest one, down
// rather than up at a tritone (shifting down sounds more natural).
int transposeTo(const std::optional<Key>& source, const std::optional<Key>& target);

// The tempo and key a file name gives, as far as it gives them.
struct FileInfo {
    std::optional<double> bpm;
    std::optional<Key> key;

    friend bool operator==(const FileInfo&, const FileInfo&) = default;
};

FileInfo parseFilename(const QString& name);

// What a newly added audio clip starts with: the clip fields to set (none set:
// the file plays as it is).
struct ClipSettings {
    std::optional<bool> warp;
    std::optional<double> segmentBpm;
    std::optional<int> transpose;

    bool isEmpty() const { return !warp && !segmentBpm && !transpose; }
    // Sets these fields on a clip.
    void applyTo(Clip& clip) const;

    friend bool operator==(const ClipSettings&, const ClipSettings&) = default;
};

// From a file's name and length: warped at the tempo in its name (or, if it's
// long, at the project tempo, so it plays as it is until the tempo changes),
// and transposed from the key in its name to the project's.
ClipSettings clipSettings(const QString& name, double durationSec, double tempo,
                          const std::optional<Key>& projectKey);

}  // namespace sub::app
