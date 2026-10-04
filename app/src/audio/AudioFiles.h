#pragma once
// Where the engine bridge puts the files it makes, and which files are audio:
//
// - takes go into the recordings folder (recordingsFolder): the project's
//   "Recordings" folder once it is saved, else SUBSTATION_RECORDINGS (the tests
//   use it), else Music/SUBstation/Recordings; each named takePath();
// - frozen tracks' audio into the freeze folder (freezeFolder): the project's
//   "Freeze" folder once it is saved, else a "Freeze" folder in the recordings folder;
// - reversed copies of files into the reversed folder (reversedFolder): the
//   project's "Reversed" folder once it is saved, else a "Reversed" folder in
//   the recordings folder; each named reversedPath().

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <vector>

namespace sub::app {

class Project;

// The audio files the application decodes: .wav, .wave, .flac, .mp3.
QStringList audioExtensions();
bool isAudioFile(const QString& path);
// A file's path as the decoded sources are keyed by: absolute and clean (and on
// Windows lower case), so two spellings of one file share one source.
QString sourceKey(const QString& path);

QString recordingsFolder(const Project& project);
// A new file for a take in `folder`: the track's name (characters Windows
// forbids replaced) and the time, numbered if taken ("Vox 2026-10-01 123005 2.wav").
QString takePath(const QString& folder, const QString& trackName, const QDateTime& when);
QString freezeFolder(const Project& project);
QString reversedFolder(const Project& project);
// A new file for the reversed copy of `source`: its name and " R" (numbered if taken).
QString reversedPath(const QString& folder, const QString& source);

// The header of a 32-bit float WAV file of `frames` frames.
QByteArray floatWavHeader(int channels, qint64 frames, int sampleRate);
// Samples (`channels` planar channels of equal length) as a 32-bit float WAV
// file. Returns false if it couldn't be written.
bool writeFloatWav(const QString& path, const std::vector<std::vector<float>>& channels, int sampleRate);

}  // namespace sub::app
