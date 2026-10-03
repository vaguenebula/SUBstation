// Python bindings for the browser's backend (module substation._browser).
//
// Thin: arguments are converted while holding the GIL, then it is released for
// the call. The backend's threads never call into Python; the UI learns that
// there is something to take through a Win32 event (QWinEventNotifier).
//
// Strings cross as WTF-8, so file names with unpaired surrogates (which Windows
// allows) survive the trip, as they do through os.scandir.

#include <nanobind/nanobind.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "Browser.h"
#include "Search.h"
#include "Text.h"

namespace nb = nanobind;
using namespace nb::literals;
using namespace sub::browser;

namespace {

std::string fromPy(nb::handle s) {
    if (!PyUnicode_Check(s.ptr())) throw nb::type_error("expected a str");
    Py_ssize_t size = 0;
    if (const char* data = PyUnicode_AsUTF8AndSize(s.ptr(), &size)) return {data, static_cast<size_t>(size)};
    PyErr_Clear();  // unpaired surrogates
    PyObject* bytes = PyUnicode_AsEncodedString(s.ptr(), "utf-8", "surrogatepass");
    if (!bytes) throw nb::python_error();
    std::string out(PyBytes_AS_STRING(bytes), static_cast<size_t>(PyBytes_GET_SIZE(bytes)));
    Py_DECREF(bytes);
    return out;
}

nb::object toPy(std::string_view s) {
    PyObject* o = PyUnicode_DecodeUTF8(s.data(), static_cast<Py_ssize_t>(s.size()), "surrogatepass");
    if (!o) throw nb::python_error();
    return nb::steal(o);
}

nb::list toPyList(const std::vector<std::string>& items) {
    nb::list out;
    for (const auto& s : items) out.append(toPy(s));
    return out;
}

// Byte offsets in UTF-8 as indexes of code points.
nb::list codePointIndexes(std::string_view s, const std::vector<uint32_t>& offsets) {
    nb::list out;
    for (const uint32_t offset : offsets) {
        size_t index = 0;
        for (size_t b = 0; b < offset; ++b)
            if ((static_cast<unsigned char>(s[b]) & 0xC0) != 0x80) ++index;
        out.append(index);
    }
    return out;
}

using ResultPtr = std::shared_ptr<Result>;

}  // namespace

NB_MODULE(_browser, m) {
    m.doc() = "SUBstation browser backend: file index and search";
    nb::set_leak_warnings(false);
    m.attr("AUDIO") = kAudioGroup;
    m.attr("UNICODE_VERSION") = unicodeVersion();

    nb::class_<Result>(m, "Result")
        .def_ro("generation", &Result::generation)
        .def_ro("search_ms", &Result::searchMs, "How long the search took on its thread.")
        .def_prop_ro("total", [](const Result& r) { return r.hits.size(); })
        .def(
            "rows",
            [](const Result& r, size_t start, size_t count) {
                nb::list out;
                const size_t end = std::min(r.hits.size(), start + std::min(count, r.hits.size()));
                const nb::object none = toPy("");
                for (size_t i = start; i < end; ++i) {
                    const Hit& hit = r.hits[i];
                    if (hit.group == kAudioGroup) {
                        const AudioRef ref = r.snapshot->audio[hit.index];
                        const SnapFolder& folder = r.snapshot->folders[ref.folder];
                        const std::string_view name = folder.files->name(ref.file);
                        out.append(nb::make_tuple(static_cast<int>(Kind::Audio), toPy(name),
                                                  toPy(Snapshot::join(folder.path, name)), toPy(folder.detail), none));
                    } else {
                        const ExternalItem& item = r.groups.at(static_cast<int>(hit.group))->items[hit.index];
                        out.append(nb::make_tuple(static_cast<int>(item.kind), toPy(item.name), toPy(item.path),
                                                  toPy(item.detail), toPy(item.key)));
                    }
                }
                return out;
            },
            "start"_a, "count"_a,
            "Rows [start, start + count) as (kind, name, path, detail, key); key is '' for indexed files.")
        .def(
            "find", [](const Result& r, int kind, nb::handle identity) { return r.find(Kind(kind), fromPy(identity)); },
            "kind"_a, "identity"_a, "Row of an item (an indexed file by path, others by key), or -1.");

    nb::class_<Browser>(m, "Browser")
        .def(
            "__init__",
            [](Browser* self, nb::handle store, std::vector<std::string> extensions, uint32_t maxFiles,
               uint32_t maxDepth) {
                Limits limits{maxFiles, maxDepth, std::move(extensions)};
                const std::wstring path = toWide(fromPy(store));
                nb::gil_scoped_release release;
                new (self) Browser(path, std::move(limits));
            },
            "store"_a, "extensions"_a, "max_files"_a = 300000, "max_depth"_a = 16,
            "Start the backend. `store` is where the index is saved ('' for nowhere); `extensions` the "
            "audio files' (lower case, with the dot).")
        .def_prop_ro("event_handle", [](const Browser& b) { return reinterpret_cast<uintptr_t>(b.event()); },
                     "A Win32 event, set when there is something to take().")
        .def(
            "set_places",
            [](Browser& self, nb::iterable places) {
                std::vector<PlaceSpec> specs;
                for (nb::handle place : places) {
                    const auto t = nb::cast<nb::tuple>(place);
                    specs.push_back({fromPy(t[0]), fromPy(t[1]), fromPy(t[2])});
                }
                nb::gil_scoped_release release;
                self.setPlaces(std::move(specs));
            },
            "places"_a, "The places as (root, os.path.normcase(os.path.normpath(root)), os.path.basename(root)).")
        .def("rescan", &Browser::rescan, nb::call_guard<nb::gil_scoped_release>(), "List every folder again.")
        .def(
            "set_external",
            [](Browser& self, int group, nb::iterable items) {
                std::vector<ExternalItem> list;
                for (nb::handle item : items) {
                    const auto t = nb::cast<nb::tuple>(item);
                    ExternalItem e;
                    e.kind = static_cast<Kind>(nb::cast<int>(t[0]));
                    e.name = fromPy(t[1]);
                    e.path = fromPy(t[2]);
                    e.detail = fromPy(t[3]);
                    e.key = fromPy(t[4]);
                    e.tag = fromPy(t[5]);
                    list.push_back(std::move(e));
                }
                nb::gil_scoped_release release;
                self.setExternal(group, std::move(list));
            },
            "group"_a, "items"_a, "A group of other items, as (kind, name, path, detail, key, tag).")
        .def(
            "set_usage",
            [](Browser& self, nb::iterable records, double halfLifeDays) {
                std::vector<UsageRecord> list;
                for (nb::handle record : records) {
                    const auto t = nb::cast<nb::tuple>(record);
                    list.push_back({fromPy(t[0]), nb::cast<double>(t[1]), nb::cast<double>(t[2])});
                }
                nb::gil_scoped_release release;
                self.setUsage(std::move(list), halfLifeDays);
            },
            "records"_a, "half_life_days"_a, "Use counts as (key, score, last_used); last_used is NaN when unknown.")
        .def(
            "search",
            [](Browser& self, nb::handle text, const std::string& sort, double now, std::vector<int> groups,
               nb::handle tag, nb::handle placePrefix) {
                Query q;
                q.text = fromPy(text);
                q.sort = sort == "name" ? Sort::Name : Sort::Rank;
                q.now = now;
                q.groups = std::move(groups);
                q.tag = fromPy(tag);
                q.placePrefix = fromPy(placePrefix);
                nb::gil_scoped_release release;
                return self.search(std::move(q));
            },
            "text"_a, "sort"_a, "now"_a, "groups"_a, "tag"_a = "", "place_prefix"_a = "",
            "Start a search (replacing any that runs); returns its generation.")
        .def(
            "take",
            [](Browser& self) {
                Browser::Update update;
                {
                    nb::gil_scoped_release release;
                    update = self.take();
                }
                nb::object result = nb::none();
                if (update.result) result = nb::cast(std::const_pointer_cast<Result>(update.result));
                return nb::make_tuple(update.status.busy, update.status.version, update.status.files, result);
            },
            "(indexing, index version, files, results of the latest search or None).")
        .def_prop_ro("indexing", [](const Browser& b) { return b.status().busy; })
        .def_prop_ro("version", [](const Browser& b) { return b.status().version; })
        .def_prop_ro("file_count", [](const Browser& b) { return b.status().files; })
        .def_prop_ro("searching", &Browser::searching)
        .def_prop_ro(
            "stats",
            [](const Browser& b) {
                const IndexStatus s = b.status();
                nb::dict d;
                d["files"] = s.files;
                d["folders"] = s.folders;
                d["load_ms"] = s.loadMs;
                d["build_ms"] = s.buildMs;
                d["pass_ms"] = s.passMs;
                d["listed"] = s.listed;
                d["checked"] = s.checked;
                return d;
            },
            "What the index holds and how long its work took: reading the saved index, making the last "
            "snapshot, the last walk over the places (and how many folders it listed, and checked by time).")
        .def("wait_idle", &Browser::waitIdle, "seconds"_a, nb::call_guard<nb::gil_scoped_release>(),
             "Wait until the index settled and no search runs. False on timeout.")
        .def("close", &Browser::close, nb::call_guard<nb::gil_scoped_release>(),
             "Stop the threads, saving the index.");

    // The text functions, to test them against Python's own.
    m.def("lower", [](nb::handle s) { return toPy(pyLower(fromPy(s))); });
    m.def("casefold", [](nb::handle s) { return toPy(pyCasefold(fromPy(s))); });
    m.def("split", [](nb::handle s) { return toPyList(pySplit(fromPy(s))); });
    m.def("nt_lower", [](nb::handle s) { return toPy(ntLower(toWide(fromPy(s)))); });
    m.def("word_starts", [](nb::handle s) {
        const std::string text = fromPy(s);
        std::vector<uint32_t> starts;
        wordStarts(text, starts);
        return codePointIndexes(text, starts);
    });
    m.def(
        "match_quality",
        [](nb::handle name, bool audio, nb::handle query) {
            const std::string lower = pyLower(fromPy(name));
            const auto terms = pySplit(pyLower(fromPy(query)));
            std::string joined;
            for (const auto& t : terms) joined += (joined.empty() ? "" : " ") + t;
            std::vector<uint32_t> scratch;
            return matchQuality(lower, audio, terms, joined, scratch);
        },
        "name"_a, "audio"_a, "query"_a);
}
