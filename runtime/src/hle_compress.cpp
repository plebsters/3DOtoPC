// hle_compress.cpp - the Compression folio: LZSS with a 4 KB window.
//
// The stream format (what game data compressed with the 3DO tools holds):
// bits are read most significant first from 32-bit words. A 1 bit is
// followed by an 8-bit literal byte; a 0 bit by a 12-bit window position
// and a 4-bit length, copying length+3 bytes from the window. Position 0
// ends the stream. The window starts at position 1, filled with zeros.
// Output bytes are packed big-endian into 32-bit words, each handed to the
// caller's CompFunc(userData, word) once the next byte starts it (the last
// one when the decompressor is deleted).
//
// The callbacks are game code, so each call ends by jumping to a small ARM
// routine that calls the CompFunc for every word produced and then returns
// to the game with the call's result.
#include "hle_os.h"
#include "state_ar.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace tdort {
using namespace abi;

namespace {
constexpr uint32_t kWindow = 4096;
constexpr uint32_t kTagWorkBuffer = 9, kTagUserData = 10;
constexpr int32_t kErrCompBadPtr = int32_t(0xD53E7009), kErrCompDataRemains = int32_t(0xD53E7101),
                  kErrCompDataMissing = int32_t(0xD53E7102);
}  // namespace

struct Portfolio::Codec {
    bool compress = false;
    uint32_t cf = 0, user = 0;
    bool own_handle = false;
    uint32_t out_buf = 0, out_cap = 0;       // guest buffer the delivery routine reads
    std::array<uint8_t, kWindow> window{};
    uint32_t pos = 1;
    // decompressor
    uint64_t bits = 0; uint32_t nbits = 0;   // unread input bits (most significant first)
    uint32_t word = 0, bytes_left = 4;       // output word being filled
    bool ended = false, data_after_end = false;
    // compressor
    std::vector<uint8_t> pending;            // input not yet encoded
    std::vector<int32_t> head, prev;         // hash chains over window positions
    uint64_t obits = 0; uint32_t onbits = 0; // output bits not yet a full word
};

// Save states: every open compressor and decompressor, with its window.
void Portfolio::codecs_io(StateAr& a) {
    size_t n = a.count(codecs_.size());
    std::vector<uint32_t> keys;
    if (a.saving) for (auto& kv : codecs_) keys.push_back(kv.first);
    else { codecs_.clear(); keys.assign(n, 0); }
    for (size_t i = 0; i < keys.size() && !a.bad; ++i) {
        a(keys[i]);
        std::shared_ptr<Codec>& d = codecs_[keys[i]];
        if (!d) d = std::make_shared<Codec>();
        a(d->compress); a(d->cf); a(d->user); a(d->own_handle); a(d->out_buf); a(d->out_cap);
        a(d->window); a(d->pos); a(d->bits); a(d->nbits); a(d->word); a(d->bytes_left);
        a(d->ended); a(d->data_after_end); a(d->pending); a(d->head); a(d->prev); a(d->obits); a(d->onbits);
    }
}

void Portfolio::codec_emit_byte(Codec& d, uint8_t c, std::vector<uint32_t>& out) {
    if (d.bytes_left == 0) { out.push_back(d.word); d.word = c; d.bytes_left = 3; }
    else { d.word = (d.word << 8) | c; --d.bytes_left; }
}

// Ends the current call: hands `words` to the game's CompFunc through the
// ARM delivery routine, then returns `result` to the caller.
void Portfolio::codec_deliver(Codec& d, const std::vector<uint32_t>& words, int32_t result) {
    if (words.empty() || !d.cf) { c.r[0] = uint32_t(result); return; }
    const uint32_t need = uint32_t(words.size() * 4);
    if (need > d.out_cap) {
        if (d.out_buf) free_mem(d.out_buf);
        d.out_cap = std::max<uint32_t>(need, 1024);
        d.out_buf = alloc(d.out_cap, 0);
        if (!d.out_buf) { d.out_cap = 0; c.r[0] = uint32_t(kErrNoMem); return; }
    }
    for (size_t i = 0; i < words.size(); ++i) w32(d.out_buf + uint32_t(i * 4), words[i]);
    c.r[0] = d.cf; c.r[1] = d.user; c.r[2] = d.out_buf; c.r[3] = uint32_t(words.size());
    c.r[12] = uint32_t(result);
    c.r[15] = deliver_code_;                  // returns to the caller's lr when done
}

void Portfolio::install_compression() {
    // deliver(cf, user, words, count), result in r12:
    //   stmfd sp!,{r4-r8,lr}; r4..r7 = args, r8 = r12
    //   loop: while (count--) cf(user, *words++);  mov r0, r8; ldmfd sp!,{r4-r8,pc}
    static const uint32_t code[] = {
        0xE92D41F0, 0xE1A04000, 0xE1A05001, 0xE1A06002, 0xE1A07003, 0xE1A0800C,
        0xE3570000, 0x0A000005, 0xE1A00005, 0xE4961004, 0xE1A0E00F, 0xE1A0F004,
        0xE2477001, 0xEAFFFFF7, 0xE1A00008, 0xE8BD81F0,
    };
    deliver_code_ = rt.hle_code(code, sizeof code / 4);

    std::map<int32_t, HleFn> v;
    // Create(Compressor/Decompressor **out, CompFunc cf, tags)
    auto create = [this](bool compress) {
        const uint32_t out = arg(0), cf = arg(1), tags = arg(2);
        if (!out) { c.r[0] = uint32_t(kErrCompBadPtr); return; }
        uint32_t work = 0, user = 0;
        each_tag(tags, [&](uint32_t t, uint32_t a) { if (t == kTagWorkBuffer) work = a; else if (t == kTagUserData) user = a; });
        auto d = std::make_shared<Codec>();
        d->compress = compress; d->cf = cf; d->user = user;
        uint32_t handle = work;
        if (!handle) { handle = alloc(16, 0); d->own_handle = true; }
        if (!handle) { c.r[0] = uint32_t(kErrNoMem); return; }
        if (compress) { d->head.assign(1 << 12, -1); d->prev.assign(kWindow, -1); }
        codecs_[handle] = std::move(d);
        w32(out, handle);
        c.r[0] = 0;
    };
    auto find = [this](uint32_t h, bool compress) -> Codec* {
        auto it = codecs_.find(h);
        return (it != codecs_.end() && it->second->compress == compress) ? it->second.get() : nullptr;
    };

    // ---- decompressor
    v[-5] = [create](Cpu*) { create(false); };                            // CreateDecompressor
    v[-8] = [this](Cpu*) { c.r[0] = 4096 + 64; };                        // GetDecompressorWorkBufferSize
    v[-7] = [this, find](Cpu*) {                                          // FeedDecompressor(d, data, words)
        Codec* d = find(arg(0), false);
        if (!d) { c.r[0] = uint32_t(kErrCompBadPtr); return; }
        const uint32_t data = arg(1), n = arg(2);
        std::vector<uint32_t> out;
        for (uint32_t i = 0; i < n; ++i) {
            if (d->ended) { d->data_after_end = true; break; }
            d->bits = (d->bits << 32) | r32(data + i * 4);
            d->nbits += 32;
            while (!d->ended && d->nbits >= 1) {
                auto peek = [&](uint32_t k) { return uint32_t((d->bits >> (d->nbits - k)) & ((1ull << k) - 1)); };
                const bool literal = peek(1);
                const uint32_t need = literal ? 9 : 13;   // the length follows only a non-zero position
                if (d->nbits < need) break;
                if (literal) {
                    const uint8_t ch = uint8_t(peek(9) & 0xFF);
                    d->nbits -= 9;
                    codec_emit_byte(*d, ch, out);
                    d->window[d->pos] = ch;
                    d->pos = (d->pos + 1) & (kWindow - 1);
                    continue;
                }
                const uint32_t mpos = peek(13) & 0xFFF;
                if (mpos == 0) { d->nbits -= 13; d->ended = true; break; }
                if (d->nbits < 17) break;
                const uint32_t len = (peek(17) & 0xF) + 2;
                d->nbits -= 17;
                for (uint32_t k = mpos; k <= mpos + len; ++k) {
                    const uint8_t ch = d->window[k & (kWindow - 1)];
                    codec_emit_byte(*d, ch, out);
                    d->window[d->pos] = ch;
                    d->pos = (d->pos + 1) & (kWindow - 1);
                }
            }
            if (d->nbits <= 32) d->bits &= (d->nbits ? ((1ull << d->nbits) - 1) : 0);
        }
        codec_deliver(*d, out, 0);
    };
    v[-6] = [this, find](Cpu*) {                                          // DeleteDecompressor
        const uint32_t h = arg(0);
        Codec* d = find(h, false);
        if (!d) { c.r[0] = uint32_t(kErrCompBadPtr); return; }
        int32_t result = d->data_after_end ? kErrCompDataRemains : !d->ended ? kErrCompDataMissing : 0;
        std::vector<uint32_t> out;
        if (d->bytes_left == 0) out.push_back(d->word);
        // the last word goes out through a buffer that outlives this item
        if (!out.empty() && d->cf) {
            if (!final_word_) final_word_ = rt.hle_alloc(4);
            w32(final_word_, out[0]);
            c.r[0] = d->cf; c.r[1] = d->user; c.r[2] = final_word_; c.r[3] = 1; c.r[12] = uint32_t(result);
            c.r[15] = deliver_code_;
        } else c.r[0] = uint32_t(result);
        if (d->out_buf) free_mem(d->out_buf);
        if (d->own_handle) free_mem(h);
        codecs_.erase(h);
    };

    // ---- compressor
    v[-1] = [create](Cpu*) { create(true); };                             // CreateCompressor
    v[-4] = [this](Cpu*) { c.r[0] = 4096 + 4097 * 12 + 64; };            // GetCompressorWorkBufferSize
    v[-3] = [this, find](Cpu*) {                                          // FeedCompressor(c, data, words)
        Codec* d = find(arg(0), true);
        if (!d) { c.r[0] = uint32_t(kErrCompBadPtr); return; }
        for (uint32_t i = 0; i < arg(2); ++i) {
            const uint32_t w = r32(arg(1) + i * 4);
            for (int b = 24; b >= 0; b -= 8) d->pending.push_back(uint8_t(w >> b));
        }
        std::vector<uint32_t> out;
        compress_some(*d, false, out);
        codec_deliver(*d, out, 0);
    };
    v[-2] = [this, find](Cpu*) {                                          // DeleteCompressor: flush, end marker
        const uint32_t h = arg(0);
        Codec* d = find(h, true);
        if (!d) { c.r[0] = uint32_t(kErrCompBadPtr); return; }
        std::vector<uint32_t> out;
        compress_some(*d, true, out);
        // copy out before the item and its buffer go away
        if (!out.empty() && d->cf) {
            const uint32_t buf = alloc(uint32_t(out.size() * 4), 0);   // freed with the task (small)
            if (buf) {
                for (size_t i = 0; i < out.size(); ++i) w32(buf + uint32_t(i * 4), out[i]);
                c.r[0] = d->cf; c.r[1] = d->user; c.r[2] = buf; c.r[3] = uint32_t(out.size()); c.r[12] = 0;
                c.r[15] = deliver_code_;
            } else c.r[0] = uint32_t(kErrNoMem);
        } else c.r[0] = 0;
        if (d->out_buf) free_mem(d->out_buf);
        if (d->own_handle) free_mem(h);
        codecs_.erase(h);
    };

    make_folio("Compression", kSubsysKernel, 32, v);
}

// Encodes pending input (all of it when `flush`, else keeping 18 bytes of
// look-ahead), appending finished 32-bit words to `out`. Greedy longest
// match over hash chains of 3-byte strings; matches never overlap the bytes
// being encoded and never use window position 0 (the end marker).
void Portfolio::compress_some(Codec& d, bool flush, std::vector<uint32_t>& out) {
    auto put = [&](uint32_t value, uint32_t n) {
        d.obits = (d.obits << n) | (value & ((1ull << n) - 1));
        d.onbits += n;
        while (d.onbits >= 32) { out.push_back(uint32_t(d.obits >> (d.onbits - 32))); d.onbits -= 32; }
        d.obits &= d.onbits ? ((1ull << d.onbits) - 1) : 0;
    };
    auto hash = [&](uint32_t p) {
        const uint8_t a = d.window[p & (kWindow - 1)], b = d.window[(p + 1) & (kWindow - 1)], c3 = d.window[(p + 2) & (kWindow - 1)];
        return ((a << 4) ^ (b << 2) ^ c3) & 0xFFF;
    };
    auto add_byte = [&](uint8_t ch) {
        d.window[d.pos] = ch;
        // the string starting two bytes back is now complete: index it
        const uint32_t s = (d.pos - 2) & (kWindow - 1);
        if (s != 0 && s != d.pos) { const uint32_t hsh = hash(s); d.prev[s] = d.head[hsh]; d.head[hsh] = int32_t(s); }
        d.pos = (d.pos + 1) & (kWindow - 1);
    };
    size_t i = 0;
    const size_t keep = flush ? 0 : 18;
    while (d.pending.size() - i > keep) {
        const size_t avail = d.pending.size() - i;
        uint32_t best_len = 0, best_pos = 0;
        if (avail >= 3) {
            const uint32_t hsh = ((d.pending[i] << 4) ^ (d.pending[i + 1] << 2) ^ d.pending[i + 2]) & 0xFFF;
            int chain = 0;
            for (int32_t cand = d.head[hsh]; cand >= 0 && chain < 64; cand = d.prev[uint32_t(cand)], ++chain) {
                const uint32_t dist = (d.pos - uint32_t(cand)) & (kWindow - 1);
                if (cand == 0 || dist == 0) continue;
                const uint32_t max_len = uint32_t(std::min<size_t>({avail, 18, dist}));
                uint32_t l = 0;
                while (l < max_len && d.window[(uint32_t(cand) + l) & (kWindow - 1)] == d.pending[i + l]) ++l;
                if (l > best_len) { best_len = l; best_pos = uint32_t(cand); if (l == 18) break; }
            }
        }
        if (best_len >= 3) {
            put(0, 1); put(best_pos, 12); put(best_len - 3, 4);
            for (uint32_t k = 0; k < best_len; ++k) add_byte(d.pending[i + k]);
            i += best_len;
        } else {
            put(1, 1); put(d.pending[i], 8);
            add_byte(d.pending[i]);
            ++i;
        }
    }
    d.pending.erase(d.pending.begin(), d.pending.begin() + long(i));
    if (flush) {
        put(0, 1); put(0, 12);                       // end of stream
        if (d.onbits) put(0, 32 - d.onbits);         // pad the last word
    }
}

}  // namespace tdort
