#pragma once
// Paths as the application compares them.
//
// Two spellings of one file are the same file: relative or absolute, with "."
// and ".." or without, and where the system ignores case (Windows), in another
// case. Elsewhere names that differ in case are different files, so they are
// kept apart (sub::platform::kCaseSensitivePaths).
//
// (The browser's keys, browser/PathKeys.h, are another thing: in the system's
// own form and Windows' own lower case, as the Python version made them, since
// the files it wrote are keyed by them.)

#include <QString>

namespace sub::app {

// A path made absolute (from the current folder) and clean: "." and ".." and
// doubled separators gone, '/' between folders.
QString absoluteCleanPath(const QString& path);
// What identifies a file however its path is written: absoluteCleanPath(), and
// case folded where the system ignores case. Decoded sources, the File
// Manager's files and the recent projects are keyed by it.
QString pathIdentity(const QString& path);
// Whether two paths are the same file (by pathIdentity).
bool samePath(const QString& a, const QString& b);

}  // namespace sub::app
