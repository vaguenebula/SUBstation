# Platform layer

What every layer needs from the operating system, in one place: [platform/src/platform](../platform/src/platform),
the static library `sub_platform` (namespace `sub::platform`). The engine, the browser backend and the intelligence
module stand on it, and so, through them, the application layer. It has no Qt and nothing of the layers on it
(`ctest -R boundaries` checks: [architecture.md](architecture.md#the-boundaries-checked)).

Each system's code is a file of its own, chosen by CMake ([platform/CMakeLists.txt](../platform/CMakeLists.txt)):
[PlatformWin32.cpp](../platform/src/platform/PlatformWin32.cpp) on Windows,
[PlatformPosix.cpp](../platform/src/platform/PlatformPosix.cpp) elsewhere. The headers declare the same functions for
every system, so what calls them has no `#ifdef`s. Supporting another system (macOS's own calls, say) means another
file behind the same headers, or a branch in the POSIX one; nothing that calls them changes.

Headers are included by their folder: `#include "platform/Paths.h"`.

## Files

| File | What it holds |
|---|---|
| [Unicode.h](../platform/src/platform/Unicode.h) / [.cpp](../platform/src/platform/Unicode.cpp) | WTF-8 one code point at a time (`decodeUtf8`, `appendUtf8`, inline: the browser's text matching runs on them), `isAscii`, and WTF-8 to UTF-16 and back (`toWide`, `fromWide`). |
| [Paths.h](../platform/src/platform/Paths.h) | `NativeString` (UTF-16 on Windows, bytes elsewhere), `NativeChar`, `kSeparator`, `kCaseSensitivePaths`, `isSeparator()`, `appendName()`; `toNative()`/`fromNative()` and `toPath()`/`fromPath()` (a UTF-8 path as the system's calls and `std::filesystem` take it, and back); keys: `nameKey()`, `pathKey()`, `normalPath()`, `fileKey()`. |
| [Files.h](../platform/src/platform/Files.h) / [Files.cpp](../platform/src/platform/Files.cpp) | By UTF-8 path: `FileStamp`/`stamp()` (size and last-write time), `replaceFile()`, `openFile()`, `readFile()` (a whole file), `writeFileAtomically()` (to `.tmp`, then over the file). |
| [Bytes.h](../platform/src/platform/Bytes.h) | Binary files' fields, little-endian on every system: `ByteWriter`, `ByteReader` (a read past the end makes `ok()` false for good), and `fnv1a()`, their checksum (and a hash of keys). The browser's saved index and the sound similarity's store are made of these. |
| [Threads.h](../platform/src/platform/Threads.h) | Priorities: `enterBackgroundMode()` (the browser's indexer, the sound similarity's threads) and `ScopedRealtimePriority` (the engine's render workers). |
| [PlatformWin32.cpp](../platform/src/platform/PlatformWin32.cpp) | Windows: `LCMapStringEx` for keys, the wide (`W`) file calls, `MoveFileExW`, `THREAD_MODE_BACKGROUND_BEGIN`, MMCSS (`AvSetMmThreadCharacteristicsW`, "Pro Audio"). |
| [PlatformPosix.cpp](../platform/src/platform/PlatformPosix.cpp) | Elsewhere: `stat`, `rename`, `fopen`; on Linux the thread's nice value and idle I/O class. No real-time priority yet. |

## Paths

Paths cross every layer as UTF-8 strings. On Windows they are WTF-8: UTF-8 that may also hold unpaired surrogates,
which Windows file names can contain, so a name read from the system and handed back is the same name
(`toWide`/`fromWide` keep them; `std::filesystem`'s own UTF-8 conversion would not). Where a path meets the system it
is in the system's own form: `toNative()` (UTF-16 on Windows; the bytes as they are elsewhere), or `toPath()` for
`std::filesystem` (never its `char` constructor, which on Windows reads the ANSI code page). Code that opens files
through a library with narrow and wide calls (miniaudio) uses the wide one on Windows and the narrow one elsewhere:
the engine's [MiniaudioFiles.h](../engine/src/MiniaudioFiles.h), the intelligence module's `AudioReader`.

Keys say when two paths are one file. `pathKey()` is what `os.path.normcase()` made, so keys match those the
Python-era files hold: on Windows backslashes and Windows' own lower case (`LCMapStringEx`, the invariant locale; ASCII
is lowered directly, which gives the same), elsewhere the path as it is, since names that differ in case are different
files there. `fileKey()` normalises the path first (`normalPath()`: "." and ".." and doubled separators gone, the
system's separators): the engine's decoded sources and loaded plug-in modules are kept by it.

## Threads

`enterBackgroundMode()` lowers the calling thread for good: CPU, I/O and memory priority on Windows
(`THREAD_MODE_BACKGROUND_BEGIN`), the lowest nice value and the idle I/O class on Linux (both per thread there). It
does nothing yet on other systems.

`ScopedRealtimePriority` raises the calling thread while it lives: the engine's render workers
([engine/scheduler.md](engine/scheduler.md)) hold one for their lives. On Windows it is MMCSS's "Pro Audio" task, as
the driver's own thread has; elsewhere it does nothing yet and `active()` is false. That is where Linux's `SCHED_FIFO`
(through rtkit) and macOS's time-constraint policy and audio workgroups go ([TODO.md](../TODO.md), platform support,
stage 1).

## Performance

Nothing here costs the layers on it anything they didn't pay before: what runs per character or per field
(`decodeUtf8`, `appendUtf8`, `isAscii`, `appendName`, `ByteWriter`, `ByteReader`, `fnv1a`) is inline in the headers,
and numbers are copied as they are on little-endian CPUs (every one SUBstation runs on); only a big-endian one would
swap their bytes. What calls the system (keys, stamps, files, priorities) was a call into another file before as well.
