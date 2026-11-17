// hle_intl.cpp - the International folio: the console's locale and text helpers.
//
// Games open the locale (intlOpenLocale = FindAndOpenItem of type
// NST_INTL/INTL_LOCALE_NODE) to learn the language and country, often to pick
// their text. This console is English / United States. Strings are 16-bit
// big-endian Unicode ("unichar"), zero-terminated.
#include "hle_os.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace tdort {
using namespace abi;

namespace {
constexpr uint8_t kSubsysIntl = 9, kNodeLocale = 1;
// Locale structure (after its 36-byte ItemNode)
namespace loc {
constexpr uint32_t Language = 36, Dialects = 40, Country = 44, GMTOffset = 48, Measuring = 52,
                   Calendar = 56, Driving = 60, Numbers = 64, Currency = 136, SmallCurrency = 208,
                   Date = 280, ShortDate = 284, Time = 288, ShortTime = 292, Size = 296;
}
// NumericSpec words
namespace ns {
constexpr uint32_t PosGroups = 0, PosGroupSep = 4, PosRadix = 8, PosFracGroups = 12, PosFracGroupSep = 16,
                   PosFormat = 20, PosMinFrac = 24, PosMaxFrac = 28, NegGroups = 32, NegGroupSep = 36,
                   NegRadix = 40, NegFracGroups = 44, NegFracGroupSep = 48, NegFormat = 52, NegMinFrac = 56,
                   NegMaxFrac = 60, Zero = 64, Flags = 68, Size = 72;
}
constexpr uint32_t kLangEnglish = ('e' << 24) | ('n' << 16), kCountryUS = 840;
constexpr uint32_t kMeasAmerican = 0, kCalGregorianSunday = 2, kDriveRight = 1;
// intlGetCharAttrs flags
constexpr uint32_t kUpper = 1, kLower = 2, kPunct = 4, kDigit = 8, kNumber = 16, kSpace = 64,
                   kHalf = 128, kFull = 256, kKana = 512, kHiragana = 1024, kKanji = 2048;
// intlConvertString flags
constexpr uint32_t kConvUpper = 1, kConvLower = 2, kConvStripDiacriticals = 4;
// CharacterSets (intlTransliterateString)
constexpr uint32_t kCsAscii = 0, kCsUnicode = 1, kCsRomaji = 3;

uint16_t to_upper(uint16_t ch) {
    if (ch < 0x80) return uint16_t(std::toupper(ch));
    if ((ch >= 0xE0 && ch <= 0xFE && ch != 0xF7)) return uint16_t(ch - 0x20);     // Latin-1
    return ch;
}
uint16_t to_lower(uint16_t ch) {
    if (ch < 0x80) return uint16_t(std::tolower(ch));
    if ((ch >= 0xC0 && ch <= 0xDE && ch != 0xD7)) return uint16_t(ch + 0x20);
    return ch;
}
// Latin-1 letters with accents -> their base letter.
uint16_t strip_accent(uint16_t ch) {
    static const char* base = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (ch >= 0xC0 && ch <= 0xFF) { char b = base[ch - 0xC0]; if (b != '*' && b != '/') return uint16_t(b); }
    return ch;
}
}  // namespace

std::vector<uint16_t> Portfolio::read_ustr(uint32_t a, uint32_t max_chars) {
    std::vector<uint16_t> s;
    while (a && guest_range_ok(a, 2) && s.size() < max_chars) {
        uint16_t ch = uint16_t((r8(a) << 8) | r8(a + 1));
        if (!ch) break;
        s.push_back(ch);
        a += 2;
    }
    return s;
}

// Writes a zero-terminated string into a buffer of `bytes`; returns the characters written.
int32_t Portfolio::write_ustr(uint32_t a, uint32_t bytes, const std::vector<uint16_t>& s) {
    if (!a || bytes < 2 || !guest_range_ok(a, bytes)) return 0;
    const uint32_t room = bytes / 2 - 1;
    const uint32_t n = std::min<uint32_t>(room, uint32_t(s.size()));
    for (uint32_t i = 0; i < n; ++i) { w8(a + i * 2, uint8_t(s[i] >> 8)); w8(a + i * 2 + 1, uint8_t(s[i])); }
    w8(a + n * 2, 0); w8(a + n * 2 + 1, 0);
    return int32_t(n);
}

uint32_t Portfolio::alloc_ustr(const char* s) {
    const uint32_t n = uint32_t(std::char_traits<char>::length(s));
    const uint32_t a = alloc((n + 1) * 2, 0);
    for (uint32_t i = 0; i <= n; ++i) { w8(a + i * 2, 0); w8(a + i * 2 + 1, uint8_t(i < n ? s[i] : 0)); }
    return a;
}

void Portfolio::install_international() {
    std::map<int32_t, HleFn> v;

    // ---- the locale: English, United States
    HleItem& l = new_item(ItemKind::Generic, kSubsysIntl, kNodeLocale, loc::Size, "", false);
    locale_item_ = l.num;
    const uint32_t L = l.addr;
    const uint32_t dialects = alloc(8, 0);
    w32(dialects, 1 /*INTL_ED_AMERICAN*/);
    w32(dialects + 4, 0xFFFFFFFFu);               // end of the list
    w32(L + loc::Language, kLangEnglish);
    w32(L + loc::Dialects, dialects);
    w32(L + loc::Country, kCountryUS);
    w32(L + loc::GMTOffset, uint32_t(-5 * 60));   // minutes from GMT (US Eastern)
    w32(L + loc::Measuring, kMeasAmerican);
    w32(L + loc::Calendar, kCalGregorianSunday);
    w32(L + loc::Driving, kDriveRight);
    uint32_t every3 = 0;                          // separator after digits 3, 6, 9, ...
    for (int b = 3; b < 32; b += 3) every3 |= 1u << b;
    const uint32_t comma = alloc_ustr(","), dot = alloc_ustr("."), zero = alloc_ustr("0");
    auto numeric = [&](uint32_t at, uint32_t min_frac, uint32_t max_frac, const char* pos_fmt, const char* neg_fmt) {
        w32(at + ns::PosGroups, every3);   w32(at + ns::PosGroupSep, comma); w32(at + ns::PosRadix, dot);
        w32(at + ns::PosFormat, pos_fmt ? alloc_ustr(pos_fmt) : 0);
        w32(at + ns::PosMinFrac, min_frac); w32(at + ns::PosMaxFrac, max_frac);
        w32(at + ns::NegGroups, every3);   w32(at + ns::NegGroupSep, comma); w32(at + ns::NegRadix, dot);
        w32(at + ns::NegFormat, neg_fmt ? alloc_ustr(neg_fmt) : 0);
        w32(at + ns::NegMinFrac, min_frac); w32(at + ns::NegMaxFrac, max_frac);
        w32(at + ns::Zero, zero);
    };
    numeric(L + loc::Numbers, 0, 8, nullptr, nullptr);
    numeric(L + loc::Currency, 2, 2, "$%s", "($%s)");
    numeric(L + loc::SmallCurrency, 0, 0, "%s c", "-%s c");

    // ---- intlGetCharAttrs(locale, char)
    v[-6] = [this](Cpu*) {
        const uint32_t ch = arg(1) & 0xFFFF;
        uint32_t a = 0;
        if (ch < 0x100) {
            a |= kHalf;
            if (std::isupper(int(ch)) || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7)) a |= kUpper;
            if (std::islower(int(ch)) || (ch >= 0xDF && ch <= 0xFF && ch != 0xF7)) a |= kLower;
            if (ch < 0x80 && std::ispunct(int(ch))) a |= kPunct;
            if (ch < 0x80 && std::isdigit(int(ch))) a |= kDigit | kNumber;
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == 0xA0) a |= kSpace;
        } else if (ch >= 0x3040 && ch <= 0x309F) a |= kHiragana | kFull;
        else if (ch >= 0x30A0 && ch <= 0x30FF) a |= kKana | kFull;
        else if (ch >= 0xFF61 && ch <= 0xFF9F) a |= kKana | kHalf;
        else if (ch >= 0x4E00 && ch <= 0x9FFF) a |= kKanji | kFull;
        else if (ch == 0x3000) a |= kSpace | kFull;
        else if (ch >= 0xFF10 && ch <= 0xFF19) a |= kDigit | kNumber | kFull;
        else if (ch >= 0xFF01 && ch <= 0xFF5E) a |= kFull;
        c.r[0] = a;
    };

    // ---- intlConvertString(locale, string, result, resultSize, flags) -> characters
    v[-5] = [this](Cpu*) {
        std::vector<uint16_t> s = read_ustr(arg(1));
        const uint32_t flags = r32(c.r[13]);          // 5th argument: on the stack
        for (auto& ch : s) {
            if (flags & kConvStripDiacriticals) ch = strip_accent(ch);
            if (flags & kConvUpper) ch = to_upper(ch);
            if (flags & kConvLower) ch = to_lower(ch);
        }
        c.r[0] = uint32_t(write_ustr(arg(2), arg(3), s));
    };

    // ---- intlCompareStrings(locale, s1, s2) -> -1 / 0 / 1 (case only breaks ties)
    v[-4] = [this](Cpu*) {
        const std::vector<uint16_t> a = read_ustr(arg(1)), b = read_ustr(arg(2));
        auto key = [](uint16_t ch) { return to_upper(strip_accent(ch)); };
        int r = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()) && !r; ++i)
            if (key(a[i]) != key(b[i])) r = key(a[i]) < key(b[i]) ? -1 : 1;
        if (!r && a.size() != b.size()) r = a.size() < b.size() ? -1 : 1;
        if (!r && a != b) r = a < b ? -1 : 1;
        c.r[0] = uint32_t(r);
    };

    // ---- intlTransliterateString(string, fromSet, result, toSet, resultSize, filler) -> characters
    v[-3] = [this](Cpu*) {
        const uint32_t src = arg(0), from = arg(1), dst = arg(2), to = arg(3);
        const uint32_t size = r32(c.r[13]), filler = r32(c.r[13] + 4) & 0xFF;
        std::vector<uint16_t> s;
        const bool from_wide = from != kCsAscii && from != kCsRomaji;
        if (from_wide) s = read_ustr(src);
        else for (uint32_t a = src; guest_range_ok(a, 1) && r8(a) && s.size() < 65536; ++a) s.push_back(r8(a));
        const bool to_wide = to != kCsAscii && to != kCsRomaji;
        if (to_wide) { c.r[0] = uint32_t(write_ustr(dst, size, s)); return; }
        if (!dst || !size || !guest_range_ok(dst, size)) { c.r[0] = 0; return; }
        uint32_t n = 0;
        for (; n < size - 1 && n < s.size(); ++n) w8(dst + n, uint8_t(s[n] < 0x100 ? s[n] : filler));
        w8(dst + n, 0);
        c.r[0] = n;
    };

    // ---- intlFormatDate(locale, spec, GregorianDate*, result, resultSize): "MM/DD/YYYY HH:MM:SS"
    // (the DateSpec format codes are not interpreted)
    v[-2] = [this](Cpu*) {
        const uint32_t d = arg(2);
        if (!guest_range_ok(d, 12)) { c.r[0] = uint32_t(kErrBadPtr); return; }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%02u/%02u/%04u %02u:%02u:%02u", r32(d + 4), unsigned(r8(d + 8)), r32(d),
                      unsigned(r8(d + 9)), unsigned(r8(d + 10)), unsigned(r8(d + 11)));
        std::vector<uint16_t> s(buf, buf + std::char_traits<char>::length(buf));
        c.r[0] = uint32_t(write_ustr(arg(3), r32(c.r[13]), s));
    };

    // ---- intlFormatNumber(locale, spec, whole, frac, negative, doFrac, result, resultSize)
    // Groups and the decimal mark come from the spec (or the locale's). The
    // fraction is taken as billionths.
    v[-1] = [this](Cpu*) {
        uint32_t spec = arg(1);
        HleItem* li = item(int32_t(arg(0)));
        if (!spec && li) spec = li->addr + loc::Numbers;
        const uint32_t whole = arg(2), frac = arg(3);
        const uint32_t sp = c.r[13];
        const bool neg = r32(sp) != 0, do_frac = r32(sp + 4) != 0;
        const uint32_t result = r32(sp + 8), size = r32(sp + 12);
        const uint32_t base = neg ? ns::NegGroups : 0;
        const uint32_t groups = spec ? r32(spec + base + ns::PosGroups) : 0;
        const std::vector<uint16_t> sep = spec ? read_ustr(r32(spec + base + ns::PosGroupSep), 8) : std::vector<uint16_t>{','};
        const std::vector<uint16_t> radix = spec ? read_ustr(r32(spec + base + ns::PosRadix), 8) : std::vector<uint16_t>{'.'};
        const uint32_t min_frac = spec ? r32(spec + base + ns::PosMinFrac) : 0;
        const uint32_t max_frac = spec ? std::min<uint32_t>(9, r32(spec + base + ns::PosMaxFrac)) : 2;
        std::string digits = std::to_string(whole);
        std::vector<uint16_t> out;
        for (size_t i = 0; i < digits.size(); ++i) {
            const size_t pos_from_right = digits.size() - i;     // digit index counted from the radix
            out.push_back(uint16_t(digits[i]));
            if (pos_from_right > 1 && (groups & (1u << (pos_from_right - 1)))) out.insert(out.end(), sep.begin(), sep.end());
        }
        if (do_frac && max_frac) {
            char f[16];
            std::snprintf(f, sizeof f, "%09u", frac % 1000000000u);
            std::string fs(f, max_frac);
            while (fs.size() > min_frac && !fs.empty() && fs.back() == '0') fs.pop_back();
            if (!fs.empty()) { out.insert(out.end(), radix.begin(), radix.end()); for (char ch : fs) out.push_back(uint16_t(ch)); }
        }
        if (neg) out.insert(out.begin(), uint16_t('-'));
        c.r[0] = uint32_t(write_ustr(result, size, out));
    };

    make_folio("International", kSubsysIntl, 32, v);
}

}  // namespace tdort
