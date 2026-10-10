#include "platform/Unicode.h"

namespace sub::platform {

std::wstring toWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        const uint32_t c = decodeUtf8(p, end);
        if (c >= 0x10000) {
            out.push_back(static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(c));
        }
    }
    return out;
}

std::string fromWide(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = static_cast<uint16_t>(s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            const uint32_t low = static_cast<uint16_t>(s[i + 1]);
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        appendUtf8(out, c);  // (an unpaired surrogate as three bytes: WTF-8)
    }
    return out;
}

}  // namespace sub::platform
