#include "platform/Files.h"

#include <filesystem>
#include <memory>
#include <system_error>

#include "platform/Paths.h"

namespace sub::platform {

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const {
        if (f) std::fclose(f);
    }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace

template <typename Bytes>
std::optional<Bytes> readFile(const std::string& path) {
    const File file(openFile(path, false));
    if (!file) return std::nullopt;
    Bytes data;
    if (const auto s = stamp(path)) data.reserve(static_cast<size_t>(s->size));
    char chunk[1 << 16];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, file.get())) > 0;) data.insert(data.end(), chunk, chunk + n);
    if (std::ferror(file.get())) return std::nullopt;
    return data;
}

template std::optional<std::string> readFile(const std::string&);
template std::optional<std::vector<char>> readFile(const std::string&);

bool writeFileAtomically(const std::string& path, std::string_view bytes) {
    std::error_code ignored;
    const std::filesystem::path folder = toPath(path).parent_path();
    if (!folder.empty()) std::filesystem::create_directories(folder, ignored);
    const std::string temporary = path + ".tmp";
    {
        const File file(openFile(temporary, true));
        if (!file) return false;
        if (std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) return false;
        if (std::fflush(file.get()) != 0) return false;
    }
    return replaceFile(temporary, path);
}

}  // namespace sub::platform
