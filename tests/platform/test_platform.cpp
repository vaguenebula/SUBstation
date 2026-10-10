// The platform layer (platform/src/platform): what every layer needs from the
// operating system, tested on whichever this is.

#include <string>
#include <vector>

#include "harness/Test.h"
#include "platform/Bytes.h"
#include "platform/Files.h"
#include "platform/Paths.h"
#include "platform/Unicode.h"

using namespace sub::platform;

namespace {

std::string utf8(uint32_t c) {
    std::string out;
    appendUtf8(out, c);
    return out;
}

}  // namespace

TEST_CASE("WTF-8 and UTF-16 both ways, unpaired surrogates too") {
    const std::string text = "Kick " + utf8(0xE9) + utf8(0x3A3) + utf8(0x1F941);  // é Σ 🥁
    const std::wstring wide = toWide(text);
    CHECK_EQ(wide.size(), size_t(9));  // the drum as a surrogate pair
    CHECK_EQ(fromWide(wide), text);
    // A lone surrogate (Windows file names may hold one) survives the trip as three bytes.
    const std::string lone = "a" + utf8(0xD800) + "b";
    CHECK_EQ(lone.size(), size_t(5));
    CHECK_EQ(toWide(lone), std::wstring({L'a', static_cast<wchar_t>(0xD800), L'b'}));
    CHECK_EQ(fromWide(toWide(lone)), lone);
    // Bytes that make no code point read as U+FFFD, one at a time.
    CHECK_EQ(toWide("\xff" "a"), std::wstring({static_cast<wchar_t>(0xFFFD), L'a'}));
    CHECK(isAscii("Kick.wav"));
    CHECK(!isAscii(text));
}

TEST_CASE("a name is appended with one separator, none after a drive alone on Windows") {
    std::string path = "folder";
    appendName(path, std::string_view("a.wav"));
    CHECK_EQ(path, std::string("folder") + kSeparator + "a.wav");
    std::string separated = "folder/";
    appendName(separated, std::string_view("a.wav"));
    CHECK_EQ(separated, std::string("folder/a.wav"));
    std::string drive = "C:";
    appendName(drive, std::string_view("a.wav"));
    CHECK_EQ(drive, kSeparator == '\\' ? std::string("C:a.wav") : std::string("C:/a.wav"));
    static_assert(isSeparator('/'));
    static_assert(isSeparator('\\') == (kSeparator == '\\'));
}

TEST_CASE("paths compare as the file system does") {
    const std::string accented = "a/" + utf8(0xE9) + ".wav";
    CHECK_EQ(fromPath(toPath(accented)), accented);
    CHECK_EQ(fileKey("a/./b/../c.wav"), fileKey("a/c.wav"));
    if (kCaseSensitivePaths) {
        CHECK_EQ(pathKey("/Samples/Kick.WAV"), std::string("/Samples/Kick.WAV"));
        CHECK_EQ(nameKey("Kick.WAV"), std::string("Kick.WAV"));
        CHECK(fileKey("a/Kick.wav") != fileKey("a/kick.wav"));
    } else {
        CHECK_EQ(pathKey("C:/Samples/Kick.WAV"), std::string("c:\\samples\\kick.wav"));
        CHECK_EQ(nameKey("K/I.WAV"), std::string("k/i.wav"));  // (a name keeps its slashes)
        CHECK_EQ(pathKey("D:/" + utf8(0xC0) + utf8(0x3A3)), "d:\\" + utf8(0xE0) + utf8(0x3C3));
        CHECK_EQ(fileKey("a/Kick.wav"), fileKey("A\\KICK.WAV"));
    }
}

TEST_CASE("binary fields are little-endian and read back; a short read sticks") {
    ByteWriter w;
    w.raw("AB", 2);
    w.u8(7);
    w.u32(0x01020304u);
    w.u64(0x1122334455667788ull);
    w.f32(1.5f);
    w.str("name");
    CHECK_EQ(w.bytes.substr(3, 4), std::string("\x04\x03\x02\x01", 4));
    ByteReader r(w.bytes);
    char magic[2];
    CHECK(r.raw(magic, 2));
    CHECK_EQ(r.u8(), uint8_t(7));
    CHECK_EQ(r.u32(), 0x01020304u);
    CHECK_EQ(r.u64(), 0x1122334455667788ull);
    CHECK_EQ(r.f32(), 1.5f);
    CHECK_EQ(r.str(), std::string("name"));
    CHECK(r.ok());
    CHECK(r.atEnd());
    CHECK_EQ(r.u32(), 0u);  // past the end: zero, and not ok from now on
    CHECK(!r.ok());
    CHECK_EQ(r.u8(), uint8_t(0));
    CHECK(!r.ok());
    // FNV-1a's published value for "a".
    CHECK_EQ(fnv1a("a"), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(fnv1a("b", fnv1a("a")), fnv1a("ab"));
}

TEST_CASE("a file is written whole, its folder made, and read back") {
    std::string folder = fromPath(subtest::tempDir());
    appendName(folder, std::string_view("f" + utf8(0xE9)));
    std::string file = folder;
    appendName(file, std::string_view("store.bin"));
    CHECK(!readFile(file));
    CHECK(!stamp(file));
    const std::string bytes("one\0two", 7);
    REQUIRE(writeFileAtomically(file, bytes));
    CHECK_EQ(readFile(file).value_or(""), bytes);
    const auto asVector = readFile<std::vector<char>>(file);
    REQUIRE(asVector);
    CHECK_EQ(std::string(asVector->begin(), asVector->end()), bytes);
    REQUIRE(stamp(file));
    CHECK_EQ(stamp(file)->size, uint64_t(7));
    CHECK(!stamp(folder));  // a folder has no stamp
    REQUIRE(writeFileAtomically(file, "again"));
    CHECK_EQ(readFile(file).value_or(""), std::string("again"));
    CHECK(!readFile(file + ".tmp"));  // moved over the file, not left beside it
}
