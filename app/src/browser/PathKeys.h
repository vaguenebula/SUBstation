#pragma once
// Paths as the browser and the plug-in index compare them.
//
// The application keeps paths in Qt's form ('/' between folders). The browser
// backend (sub_browser) and the files the old version wrote (library.json, the
// plug-in cache) use the system's own form, so keys are made in it: on Windows
// a key is os.path.normcase(os.path.normpath(path)), backslashes and Windows'
// own lower case, exactly as Python made it; elsewhere the normalised path as
// it is, since names that differ in case are different files there (see
// nameKey() in platform/Paths.h).
//
// These keys are made to match those files, not to tell whether two paths are
// one file: they keep the system's separators, aren't made absolute, and use
// Windows' own lower case. The rest of the application compares files with
// pathIdentity() and samePath() (model/Paths.h): absolute, clean, Qt's form,
// case folded where the system ignores case.

#include <QString>

#include <string>

namespace sub::app {

// os.path.normpath in Qt's form: "." and ".." and doubled separators gone, no
// trailing separator (except a root's), '/' between folders.
QString normalPath(const QString& path);

// The path in the system's form, for the browser backend (UTF-8).
std::string toBackendPath(const QString& path);
// And back from it.
QString fromBackendPath(const std::string& path);

// os.path.normcase(path) of a path as it is (no normalising): the plug-in
// cache's keys.
QString caseKey(const QString& path);
// os.path.normcase(os.path.normpath(path)): what identifies a file or folder.
QString pathKey(const QString& path);
// Whether two paths are the same folder (by pathKey).
bool sameFolder(const QString& a, const QString& b);

// "audio:" + pathKey(path): an audio file's key, for what the browser
// remembers about it (use counts).
QString audioKey(const QString& path);

// Where the browser and the plug-in index keep their files: on Windows
// %LOCALAPPDATA%\SUBstation (as before), elsewhere the system's place for
// application data (~/.local/share/SUBstation).
QString localDataDir();
// A file of the application's there (`name`), or the path the environment
// variable `envVar` holds, if it is set and not empty (the tests set them).
QString localDataFile(const char* envVar, const QString& name);

}  // namespace sub::app
