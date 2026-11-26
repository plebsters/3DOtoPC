// host_name.h - 3DO file names that Windows can't store.
//
// The 3DO's file system allows any character in a name; Windows refuses
// < > : " / \ | ? * and control characters, names ending in a dot or a space,
// and device names such as CON or NUL. Such characters are stored as %XX (the
// byte in hex), and % itself always is, so the original name comes back
// exactly. The converter writes names this way and the runtime reads them
// back. A name with none of these characters is stored unchanged.
#pragma once
#include <cctype>
#include <string>

namespace tdort {

// Whether a name is well-formed UTF-8 (plain ASCII is).
inline bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : size_t(-1);
        if (n == size_t(-1) || (n && c < 0xC2) || i + n >= s.size() + (n ? 0 : 1)) return false;
        for (size_t k = 1; k <= n; ++k) if (((unsigned char)s[i + k] >> 6) != 2) return false;
        i += n + 1;
    }
    return true;
}

inline std::string encode_host_name(const std::string& name) {
    static const char* hex = "0123456789ABCDEF";
    auto esc = [&](std::string& out, unsigned char ch) { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; };
    // Japanese discs name files in Shift-JIS, which isn't UTF-8: Windows can't
    // make such a name ("No mapping for the Unicode character"), so all bytes
    // from 0x80 up are escaped too in a name that isn't valid UTF-8.
    const bool raw = !valid_utf8(name);
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char ch = (unsigned char)name[i];
        const bool bad = ch < 0x20 || ch == 0x7F || (raw && ch >= 0x80) ||
                         std::string("<>:\"/\\|?*%").find(char(ch)) != std::string::npos;
        const bool trailing = i + 1 == name.size() && (ch == '.' || ch == ' ');
        if (bad || trailing) esc(out, ch); else out += char(ch);
    }
    // Device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9), with or without an
    // extension: the first letter is escaped.
    std::string base = out.substr(0, out.find('.'));
    for (auto& c : base) c = char(std::toupper((unsigned char)c));
    const bool device = base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '1' && base[3] <= '9');
    if (device) { std::string e; esc(e, (unsigned char)out[0]); out = e + out.substr(1); }
    return out;
}

inline std::string decode_host_name(const std::string& name) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size() && val(name[i + 1]) >= 0 && val(name[i + 2]) >= 0) {
            out += char(val(name[i + 1]) * 16 + val(name[i + 2]));
            i += 2;
        } else out += name[i];
    }
    return out;
}

} // namespace tdort
