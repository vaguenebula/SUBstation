#pragma once
// The benchmarks' reports as JSON, written as Python's json.dumps(indent=n)
// wrote them before: keys in the order they were set, numbers as Python shows
// them (a float always with a point or an exponent, the shortest that reads
// back the same), and printf-style formatting for the text reports.

#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sub::bench {

// printf into a std::string.
inline std::string format(const char* pattern, ...) {
    va_list args;
    va_start(args, pattern);
    va_list again;
    va_copy(again, args);
    const int size = std::vsnprintf(nullptr, 0, pattern, args);
    va_end(args);
    std::string out(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    if (size > 0) std::vsnprintf(out.data(), out.size() + 1, pattern, again);
    va_end(again);
    return out;
}

// repr() of a float: the shortest text that reads back as the same number.
inline std::string pythonFloat(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    char text[64];
    const auto [end, error] = std::to_chars(text, text + sizeof text, value);
    std::string out(text, error == std::errc() ? end : text);
    if (out.find_first_of(".en") == std::string::npos) out += ".0";
    return out;
}

// round(value, digits), as Python rounds for a report.
inline double rounded(double value, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(value * scale) / scale;
}

class Json {
public:
    Json() = default;
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<int64_t>(value)) {}
    Json(int64_t value) : value_(value) {}
    Json(uint32_t value) : value_(static_cast<int64_t>(value)) {}
    Json(uint64_t value) : value_(static_cast<int64_t>(value)) {}
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}

    static Json object() {
        Json json;
        json.value_ = Object{};
        return json;
    }
    static Json array() {
        Json json;
        json.value_ = Array{};
        return json;
    }

    // An object's field (replacing one of that name, in its place).
    Json& set(const std::string& key, Json value) {
        auto& fields = std::get<Object>(value_);
        for (auto& [name, field] : fields) {
            if (name == key) {
                field = std::move(value);
                return *this;
            }
        }
        fields.emplace_back(key, std::move(value));
        return *this;
    }
    // An array's next item.
    Json& push(Json value) {
        std::get<Array>(value_).push_back(std::move(value));
        return *this;
    }

    std::string dump(int indent) const {
        std::string out;
        write(out, indent, 0);
        return out;
    }

    // Writes the report to a file (UTF-8). False if it can't.
    bool save(const std::filesystem::path& path, int indent) const {
        std::ofstream file(path, std::ios::binary);
        file << dump(indent);
        return static_cast<bool>(file);
    }

private:
    using Object = std::vector<std::pair<std::string, Json>>;
    using Array = std::vector<Json>;

    static void writeString(std::string& out, const std::string& text) {
        out += '"';
        for (const char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += format("\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    else
                        out += c;  // UTF-8 as it is (ensure_ascii=False)
            }
        }
        out += '"';
    }

    void write(std::string& out, int indent, int depth) const {
        const std::string inner(static_cast<size_t>(indent * (depth + 1)), ' ');
        const std::string outer(static_cast<size_t>(indent * depth), ' ');
        if (std::holds_alternative<std::monostate>(value_)) {
            out += "null";
        } else if (const bool* b = std::get_if<bool>(&value_)) {
            out += *b ? "true" : "false";
        } else if (const int64_t* i = std::get_if<int64_t>(&value_)) {
            out += std::to_string(*i);
        } else if (const double* d = std::get_if<double>(&value_)) {
            out += pythonFloat(*d);
        } else if (const std::string* s = std::get_if<std::string>(&value_)) {
            writeString(out, *s);
        } else if (const Array* a = std::get_if<Array>(&value_)) {
            if (a->empty()) {
                out += "[]";
                return;
            }
            out += "[\n";
            for (size_t n = 0; n < a->size(); ++n) {
                out += inner;
                (*a)[n].write(out, indent, depth + 1);
                out += n + 1 < a->size() ? ",\n" : "\n";
            }
            out += outer + "]";
        } else if (const Object* o = std::get_if<Object>(&value_)) {
            if (o->empty()) {
                out += "{}";
                return;
            }
            out += "{\n";
            for (size_t n = 0; n < o->size(); ++n) {
                out += inner;
                writeString(out, (*o)[n].first);
                out += ": ";
                (*o)[n].second.write(out, indent, depth + 1);
                out += n + 1 < o->size() ? ",\n" : "\n";
            }
            out += outer + "}";
        }
    }

    std::variant<std::monostate, bool, int64_t, double, std::string, Array, Object> value_;
};

}  // namespace sub::bench
