// hle_jstring.cpp - the JString folio: Japanese text conversions between
// Shift-JIS, Unicode, ASCII, romaji, hiragana and full/half-width katakana.
//
// Every call is (const source *string, result *buf, uint32 resultSize,
// uint8 filler) -> characters written (not counting the terminator) or
// JSTR_ERR_BUFFERTOOSMALL. Text is decoded to Unicode code points, converted,
// and encoded again; characters the target cannot hold become `filler`.
#include "hle_os.h"
#include "jstring_tables.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace tdort {
using namespace abi;

namespace {
constexpr int32_t kErrJstrBufferTooSmall = int32_t(0xD576F101);
constexpr uint32_t kFiller = 0xFFFFFFFF;   // marks "put the filler byte here"

struct Maps {
    std::unordered_map<uint16_t, uint16_t> sjis2uni, uni2sjis;
    std::unordered_map<uint32_t, uint32_t> full2half;      // katakana -> half (low 16) [+ mark (high 16)]
    std::unordered_map<uint32_t, uint16_t> half2full;      // half (+ mark << 16) -> katakana
    Maps() {
        for (auto& p : kSjisToUni) { sjis2uni[p[0]] = p[1]; uni2sjis.emplace(p[1], p[0]); }
        for (auto& p : kFullToHalfKana) {
            full2half[p[0]] = p[1] | (uint32_t(p[2]) << 16);
            half2full.emplace(p[1] | (uint32_t(p[2]) << 16), p[0]);
        }
    }
};
const Maps& maps() { static const Maps m; return m; }

bool is_hira(uint32_t u) { return u >= 0x3041 && u <= 0x3096; }
bool is_kata(uint32_t u) { return u >= 0x30A1 && u <= 0x30F6; }
bool is_half(uint32_t u) { return u >= 0xFF61 && u <= 0xFF9F; }
bool is_vowel(char ch) { return std::strchr("aiueo", ch) != nullptr; }

std::vector<uint32_t> to_kata(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o; for (uint32_t u : t) o.push_back(is_hira(u) ? u + 0x60 : u); return o;
}
std::vector<uint32_t> to_hira(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o; for (uint32_t u : t) o.push_back(is_kata(u) && u <= 0x30F6 ? u - 0x60 : u); return o;
}
std::vector<uint32_t> full_to_half(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o;
    for (uint32_t u : t) {
        auto it = maps().full2half.find(u);
        if (it == maps().full2half.end()) { o.push_back(u); continue; }
        o.push_back(it->second & 0xFFFF);
        if (it->second >> 16) o.push_back(it->second >> 16);
    }
    return o;
}
std::vector<uint32_t> half_to_full(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o;
    for (size_t i = 0; i < t.size(); ++i) {
        if (!is_half(t[i])) { o.push_back(t[i]); continue; }
        if (i + 1 < t.size() && (t[i + 1] == 0xFF9E || t[i + 1] == 0xFF9F)) {
            auto it = maps().half2full.find(t[i] | (t[i + 1] << 16));
            if (it != maps().half2full.end()) { o.push_back(it->second); ++i; continue; }
        }
        auto it = maps().half2full.find(t[i]);
        o.push_back(it != maps().half2full.end() ? it->second : t[i]);
    }
    return o;
}
// romaji -> hiragana: longest match; a doubled consonant is a small tsu;
// a lone n (not before a vowel or y) is ん.
std::vector<uint32_t> romaji_to_hira(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o;
    for (size_t i = 0; i < t.size();) {
        auto lc = [&](size_t k) -> char { return k < t.size() && t[k] < 128 ? char(std::tolower(int(t[k]))) : 0; };
        const char ch = lc(i);
        if (ch && ch == lc(i + 1) && std::isalpha((unsigned char)ch) && !is_vowel(ch) && ch != 'n') {
            o.push_back(0x3063); ++i; continue;                     // っ
        }
        if (ch == 'n' && !is_vowel(lc(i + 1)) && lc(i + 1) != 'y' && lc(i + 1) != '\'') {
            o.push_back(0x3093); ++i; continue;                     // ん
        }
        bool hit = false;
        for (auto& r : kRomaji) {                                   // longest first
            const size_t n = std::strlen(r.romaji);
            size_t k = 0;
            while (k < n && lc(i + k) == r.romaji[k]) ++k;
            if (k == n) {
                o.push_back(r.kana[0]); if (r.kana[1]) o.push_back(r.kana[1]);
                i += n; hit = true; break;
            }
        }
        if (!hit) { o.push_back(t[i]); ++i; }
    }
    return o;
}
// hiragana -> romaji (Hepburn); a small tsu doubles the next consonant.
std::vector<uint32_t> hira_to_romaji(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o;
    bool dbl = false;
    for (size_t i = 0; i < t.size();) {
        if (t[i] == 0x3063) { dbl = true; ++i; continue; }
        const char* r = nullptr; size_t used = 0;
        for (auto& e : kKanaRomaji) {
            if (e.kana[1] ? (i + 1 < t.size() && t[i] == e.kana[0] && t[i + 1] == e.kana[1]) : t[i] == e.kana[0]) {
                r = e.romaji; used = e.kana[1] ? 2 : 1; break;
            }
        }
        if (!r) {
            if (dbl) { o.push_back(kFiller); dbl = false; }
            o.push_back((is_hira(t[i]) || is_kata(t[i])) ? kFiller : t[i]); ++i; continue;
        }
        if (dbl) { o.push_back(uint32_t(r[0] == 'c' ? 't' : r[0])); dbl = false; }
        for (const char* p = r; *p; ++p) o.push_back(uint32_t(*p));
        i += used;
    }
    if (dbl) o.push_back(kFiller);
    return o;
}
// ASCII <-> full-width forms
std::vector<uint32_t> ascii_to_wide(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o;
    for (uint32_t u : t) o.push_back(u == ' ' ? 0x3000 : (u > 0x20 && u < 0x7F) ? u - 0x21 + 0xFF01 : u);
    return o;
}
std::vector<uint32_t> wide_to_ascii(const std::vector<uint32_t>& t) {
    std::vector<uint32_t> o;
    for (uint32_t u : t) {
        if (u < 0x80) o.push_back(u);
        else if (u == 0x3000) o.push_back(' ');
        else if (u >= 0xFF01 && u <= 0xFF5E) o.push_back(u - 0xFF01 + 0x21);
        else if (u == 0x2019) o.push_back('\'');
        else if (u == 0x201D) o.push_back('"');
        else if (u == 0xFFE5) o.push_back('\\');
        else o.push_back(kFiller);
    }
    return o;
}
}  // namespace

// Shift-JIS bytes from guest memory -> code points (stops at NUL).
std::vector<uint32_t> Portfolio::read_sjis(uint32_t a) {
    std::vector<uint32_t> o;
    for (int guard = 0; a && guest_range_ok(a, 1) && guard < 65536; ++guard) {
        const uint8_t b = r8(a);
        if (!b) break;
        if (b < 0x80) { o.push_back(b); ++a; continue; }
        if (b >= 0xA1 && b <= 0xDF) { o.push_back(0xFF61 + (b - 0xA1)); ++a; continue; }
        const uint8_t b2 = guest_range_ok(a + 1, 1) ? r8(a + 1) : 0;
        if (!b2) { o.push_back(kFiller); break; }
        auto it = maps().sjis2uni.find(uint16_t(b << 8 | b2));
        o.push_back(it != maps().sjis2uni.end() ? it->second : kFiller);
        a += 2;
    }
    return o;
}

// Writes text as Shift-JIS (or plain ASCII when `ascii`), NUL-terminated.
int32_t Portfolio::write_sjis(const std::vector<uint32_t>& t, uint32_t out, uint32_t size, uint8_t filler, bool ascii) {
    std::vector<uint8_t> bytes;
    for (uint32_t u : t) {
        if (u == kFiller) { bytes.push_back(filler); continue; }
        if (u < 0x80) { bytes.push_back(uint8_t(u)); continue; }
        if (ascii) { bytes.push_back(filler); continue; }
        if (is_half(u) && u <= 0xFF9F) { bytes.push_back(uint8_t(0xA1 + (u - 0xFF61))); continue; }
        auto it = maps().uni2sjis.find(uint16_t(u));
        if (u > 0xFFFF || it == maps().uni2sjis.end()) { bytes.push_back(filler); continue; }
        bytes.push_back(uint8_t(it->second >> 8)); bytes.push_back(uint8_t(it->second));
    }
    if (!out || bytes.size() + 1 > size) return kErrJstrBufferTooSmall;
    for (size_t i = 0; i < bytes.size(); ++i) w8(out + uint32_t(i), bytes[i]);
    w8(out + uint32_t(bytes.size()), 0);
    return int32_t(bytes.size());
}

void Portfolio::install_jstring() {
    std::map<int32_t, HleFn> v;
    using Conv = std::vector<uint32_t> (*)(const std::vector<uint32_t>&);
    // Shift-JIS -> Shift-JIS conversions: (vector, function, ASCII-only output)
    auto sjis = [this](Conv f, bool ascii_out) {
        return [this, f, ascii_out](Cpu*) {
            c.r[0] = uint32_t(write_sjis(f(read_sjis(arg(0))), arg(1), arg(2), uint8_t(arg(3)), ascii_out));
        };
    };
    static const Conv id = [](const std::vector<uint32_t>& t) { return t; };
    // ShiftJIS2UniCode(string, unichar *result, size, filler)
    v[-1] = [this](Cpu*) {
        const auto t = read_sjis(arg(0));
        const uint32_t out = arg(1), size = arg(2);
        if (!out || (t.size() + 1) * 2 > size) { c.r[0] = uint32_t(kErrJstrBufferTooSmall); return; }
        for (size_t i = 0; i < t.size(); ++i) { const uint16_t u = uint16_t(t[i] == kFiller ? arg(3) & 0xFF : t[i]); w8(out + uint32_t(i * 2), uint8_t(u >> 8)); w8(out + uint32_t(i * 2 + 1), uint8_t(u)); }
        w8(out + uint32_t(t.size() * 2), 0); w8(out + uint32_t(t.size() * 2 + 1), 0);
        c.r[0] = uint32_t(t.size());
    };
    // UniCode2ShiftJIS(unichar *string, char *result, size, filler)
    v[-2] = [this](Cpu*) {
        std::vector<uint32_t> t;
        for (uint16_t u : read_ustr(arg(0))) t.push_back(u);
        c.r[0] = uint32_t(write_sjis(t, arg(1), arg(2), uint8_t(arg(3)), false));
    };
    v[-3]  = sjis(wide_to_ascii, true);                                                       // ShiftJIS2ASCII
    v[-4]  = sjis(ascii_to_wide, false);                                                      // ASCII2ShiftJIS
    v[-5]  = sjis(romaji_to_hira, false);                                                     // Romaji2Hiragana
    v[-6]  = sjis([](const std::vector<uint32_t>& t) { return to_kata(romaji_to_hira(t)); }, false);
    v[-7]  = sjis([](const std::vector<uint32_t>& t) { return full_to_half(to_kata(romaji_to_hira(t))); }, false);
    v[-8]  = sjis(hira_to_romaji, false);                                                     // Hiragana2Romaji
    v[-9]  = sjis(to_kata, false);                                                            // Hiragana2FullKana
    v[-10] = sjis([](const std::vector<uint32_t>& t) { return full_to_half(to_kata(t)); }, false);
    v[-11] = sjis(to_hira, false);                                                            // FullKana2Hiragana
    v[-12] = sjis(full_to_half, false);                                                       // FullKana2HalfKana
    v[-13] = sjis([](const std::vector<uint32_t>& t) { return hira_to_romaji(to_hira(t)); }, false);
    v[-14] = sjis([](const std::vector<uint32_t>& t) { return to_hira(half_to_full(t)); }, false);
    v[-15] = sjis(half_to_full, false);                                                       // HalfKana2FullKana
    v[-16] = sjis([](const std::vector<uint32_t>& t) { return hira_to_romaji(to_hira(half_to_full(t))); }, false);
    (void)id;
    make_folio("JString", kSubsysKernel, 32, v);
}

}  // namespace tdort
