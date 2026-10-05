// substation-scan: the child process that reads plug-in files for the
// application's plug-in index (app/src/plugins/PluginIndex.h).
//
// Reading a plug-in file means running its code, and a broken plug-in can crash
// or hang the process that loads it, so the application never loads files to
// scan them itself: it starts this program, many files per process. If one
// takes the process down or doesn't answer in time, the application marks it
// failed and carries on in a new process.
//
// Protocol (UTF-8, one JSON value per line): says {"ready": true} once it can
// scan, then reads one JSON-encoded path per line from stdin and answers each
// with {"path": ..., "plugins": [...]} or {"path": ..., "error": ...} on stdout.
// Whatever plug-ins print goes nowhere: stdout is kept for the answers.
//
// Links the engine's VST3 host (sub_engine) and nothing of Qt.

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "plugins/Vst3Format.h"

namespace {

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

void appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// A JSON string (the whole line); throws std::invalid_argument for anything else.
std::string unquote(const std::string& line) {
    size_t at = line.find_first_not_of(" \t\r");
    if (at == std::string::npos || line[at] != '"') throw std::invalid_argument("not a JSON string");
    std::string out;
    for (++at; at < line.size(); ++at) {
        const char c = line[at];
        if (c == '"') return out;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++at >= line.size()) break;
        switch (line[at]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (at + 4 >= line.size()) throw std::invalid_argument("bad escape");
                unsigned code = std::stoul(line.substr(at + 1, 4), nullptr, 16);
                at += 4;
                if (code >= 0xD800 && code < 0xDC00 && at + 6 < line.size() && line[at + 1] == '\\' &&
                    line[at + 2] == 'u') {
                    const unsigned low = std::stoul(line.substr(at + 3, 4), nullptr, 16);
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    at += 6;
                }
                appendUtf8(out, code);
                break;
            }
            default: out += line[at];
        }
    }
    throw std::invalid_argument("unterminated JSON string");
}

// Keeps stdout for our answers; returns the stream to answer on.
FILE* quiet() {
#ifdef _WIN32
    // No "program stopped working" or "insert a disk" dialogs if a plug-in misbehaves.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int answers = _dup(_fileno(stdout));
    int devnull = -1;
    _sopen_s(&devnull, "NUL", _O_WRONLY, _SH_DENYNO, _S_IWRITE);
    _dup2(devnull, _fileno(stdout));
    _dup2(devnull, _fileno(stderr));
    _setmode(answers, _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    return _fdopen(answers, "wb");
#else
    const int answers = dup(STDOUT_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    return fdopen(answers, "w");
#endif
}

void answer(FILE* out, const std::string& json) {
    std::fputs(json.c_str(), out);
    std::fputc('\n', out);
    std::fflush(out);
}

}  // namespace

int main() {
    FILE* out = quiet();
    answer(out, "{\"ready\": true}");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::string path;
        try {
            path = unquote(line);
        } catch (const std::exception&) {
            continue;
        }
        std::string json = "{\"path\": " + quote(path);
        try {
            const auto found = sub::vst3::Vst3Format::instance().scanFile(path);
            json += ", \"plugins\": [";
            for (size_t i = 0; i < found.size(); ++i) {
                const auto& d = found[i];
                if (i) json += ", ";
                json += "{\"uid\": " + quote(d.uid) + ", \"name\": " + quote(d.name) + ", \"vendor\": " +
                        quote(d.vendor) + ", \"version\": " + quote(d.version) + ", \"category\": " +
                        quote(d.category) + ", \"instrument\": " + (d.isInstrument ? "true" : "false") + "}";
            }
            json += "]}";
        } catch (const std::exception& e) {
            const std::string what = e.what();
            json += ", \"error\": " + quote(what.empty() ? std::string("error") : what) + "}";
        } catch (...) {
            json += ", \"error\": \"error\"}";
        }
        answer(out, json);
    }
    return 0;
}
