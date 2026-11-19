// audio.cpp - host implementation of the 3DO audio DSP instruments (see audio.h).
#include "tdort/audio.h"
#include "tdort/memory.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tdort {

// ---------------------------------------------------------------------------
// instruments by name
// ---------------------------------------------------------------------------

static std::string base_name(const std::string& path) {
    std::string n = path;
    if (size_t s = n.find_last_of("/\\"); s != std::string::npos) n = n.substr(s + 1);
    for (auto& ch : n) ch = char(std::tolower(uint8_t(ch)));
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".dsp") == 0) n.resize(n.size() - 4);
    return n;
}

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

Audio::Kind Audio::classify(const std::string& tmpl, bool* fixed, bool* half, bool* stereo, int* inputs) {
    const std::string n = base_name(tmpl);
    *fixed = has(n, "fixed"); *half = has(n, "half"); *stereo = has(n, "stereo"); *inputs = 0;
    if (has(n, "mixer")) {
        // mixerNxM: N inputs, M outputs.
        size_t p = n.find("mixer") + 5;
        *inputs = std::max(1, std::atoi(n.c_str() + p));
        *stereo = has(n, "x2");
        return Kind::Mixer;
    }
    if (has(n, "directout") || has(n, "dacout") || n == "output") { *inputs = 2; *stereo = true; return Kind::Output; }
    // Sample players: sampler, varmono8/16, fixedmono8/16, fixedstereo8/16,
    // halfmono8/16, halfstereo8/16, the SDX2 / ADPCM decoders...
    if (has(n, "sampl") || has(n, "mono") || has(n, "stereo") || has(n, "dcsq") || has(n, "sqxd") ||
        has(n, "sqs2") || has(n, "adpcm") || has(n, "sdx") || has(n, "spool")) {
        if (*half) *fixed = true;     // half-rate players run at a fixed 22050 Hz
        return Kind::Sampler;
    }
    // ezflix225.dsp is not a sound player: the EZFlix movie library decodes its
    // video with it (see vpp_run). TDORT_NO_VPP=1: the old silent sampler.
    if (has(n, "ezflix") && std::getenv("TDORT_NO_VPP")) { *fixed = true; *half = true; return Kind::Sampler; }
    if (has(n, "envelope")) return Kind::Envelope;
    if (has(n, "saw") || has(n, "triangle") || has(n, "pulse") || has(n, "square") || has(n, "noise") ||
        has(n, "sine") || has(n, "osc"))
        return Kind::Osc;
    return Kind::Effect;
}

std::vector<std::string> Audio::knob_names(const std::string& tmpl) {
    bool f, h, s; int in;
    switch (classify(tmpl, &f, &h, &s, &in)) {
    case Kind::Sampler: return f ? std::vector<std::string>{"Amplitude"} : std::vector<std::string>{"Frequency", "Amplitude"};
    case Kind::Osc:     return {"Frequency", "Amplitude"};
    case Kind::Mixer: {
        std::vector<std::string> k;
        for (int i = 0; i < in; ++i) {
            if (s) { k.push_back("LeftGain" + std::to_string(i)); k.push_back("RightGain" + std::to_string(i)); }
            else k.push_back("Gain" + std::to_string(i));
        }
        return k;
    }
    default: return {};
    }
}

void Audio::Sample::recompute_frames() {
    const uint32_t ch = uint32_t(std::max(1, channels));
    switch (codec) {
    case Codec::Sdx2: frames = bytes / ch; break;
    case Codec::Adp4: frames = bytes * 2 / ch; break;
    default:          frames = bytes / (ch * uint32_t(std::max(1, bits / 8))); break;
    }
}

void Audio::add_instrument(int32_t item, const std::string& tmpl_name) {
    Instrument in;
    in.tmpl = base_name(tmpl_name);
    in.kind = classify(tmpl_name, &in.fixed_rate, &in.half_rate, &in.stereo, &in.inputs);
    // TDORT_EZFLIX_SILENT=1: the old behaviour (EZFlix movie sound muted).
    in.silent = has(base_name(tmpl_name), "ezflix");
    if ((has(in.tmpl, "vpp") || has(in.tmpl, "ezflix")) && !std::getenv("TDORT_NO_VPP")) {
        // The tables vpp4.dsp ships with (its DINI chunk); the loader replaces
        // them with the game's own file when it can read it.
        static const uint16_t uv[16] = {0xF2F2, 0xF2FE, 0xF204, 0xF210, 0xFEF2, 0xFEFE, 0xFE04, 0xFE10,
                                        0x04F2, 0x04FE, 0x0404, 0x0410, 0x10F2, 0x10FE, 0x1004, 0x1010};
        static const uint16_t y[16]  = {0xE8E8, 0xE8FC, 0xE806, 0xE818, 0xFCE8, 0xFCFC, 0xFC06, 0xFC18,
                                        0x06E8, 0x06FC, 0x0606, 0x0618, 0x18E8, 0x18FC, 0x1806, 0x1818};
        in.vpp = true;
        std::copy(uv, uv + 16, in.vpp_tab[0]);
        std::copy(y, y + 16, in.vpp_tab[1]);
    }
    if (in.kind == Kind::Sampler) {
        const std::string& n = in.tmpl;
        if (has(n, "duck")) {             // movie sound: fixed 22050 or 44100 Hz stereo
            in.data_duck = true; in.stereo = true; in.fixed_rate = true; in.half_rate = has(n, "22");
        }
        else if (has(n, "dcsq") || has(n, "sqxd") || has(n, "sdx")) in.data_sdx2 = true;
        else if (has(n, "adpcm")) in.data_adp4 = true;
        // Variable-rate players (adpcmvarmono, varmono8, ...): Frequency 0x8000
        // is the recorded speed. Slam 'n Jam starts its 22 kHz commentary at
        // 0x44D8; read on the 0x4000 scale it played twice too fast.
        if (has(n, "var")) in.freq_unit = 0x8000;
        else if (n.size() >= 2 && n.compare(n.size() - 2, 2, "16") == 0) in.data_bits = 16;
        else if (!n.empty() && n.back() == '8') in.data_bits = 8;
    }
    ins_[item] = in;
}

void Audio::add_attachment(int32_t item, const Attachment& a) {
    atts_[item] = a;
    if (Instrument* in = instrument(a.ins)) in->atts.push_back(item);
}

void Audio::remove(int32_t item) {
    if (auto it = atts_.find(item); it != atts_.end()) {
        if (Instrument* in = instrument(it->second.ins))
            in->atts.erase(std::remove(in->atts.begin(), in->atts.end(), item), in->atts.end());
        for (auto& [id, a] : atts_) if (a.next == item) a.next = 0;
        atts_.erase(it);
        return;
    }
    if (ins_.erase(item)) {
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [&](const Conn& c) { return c.src == item || c.dst == item; }), conns_.end());
        for (auto it = atts_.begin(); it != atts_.end();) it = it->second.ins == item ? atts_.erase(it) : std::next(it);
        return;
    }
    if (samples_.erase(item)) {
        for (auto& [id, a] : atts_) if (a.sample == item) { a.sample = 0; a.active = false; }
        return;
    }
    envelopes_.erase(item);
}

// ---------------------------------------------------------------------------
// control
// ---------------------------------------------------------------------------

int32_t Audio::knob(const Instrument& in, const char* name, int32_t def) const {
    if (auto m = in.mod.find(name); m != in.mod.end()) return m->second;
    auto it = in.knobs.find(name);
    return it == in.knobs.end() ? def : it->second;
}

void Audio::set_knob(int32_t ins, const std::string& k, int32_t value) {
    if (Instrument* in = instrument(ins)) {
        in->knobs[k] = value;
        if (k == "Frequency" || k == "Rate") in->pitch_step = -1;   // an explicit rate overrides note tags
    }
}

static void reset_play(Audio::Attachment& a) {
    a.active = true; a.released = false;
    a.pos = a.start_at;
    a.dec_frame = int64_t(a.start_at) - 1;
    a.dec_val[0] = a.dec_val[1] = 0;
    a.adp_index[0] = a.adp_index[1] = 0;
    a.env_ms = 0;
}

void Audio::start(int32_t id, int32_t amplitude, int32_t rate, int32_t pitch, int32_t velocity, uint32_t freq16) {
    Instrument* in = instrument(id);
    if (!in) return;
    in->st = Instrument::St::Started;
    in->paused = false;
    in->phase = 0;
    if (amplitude >= 0) in->knobs["Amplitude"] = amplitude;
    else if (velocity >= 0) in->knobs["Amplitude"] = std::min(0x7FFF, velocity * 0x7FFF / 127);
    // AF_TAG_RATE and AF_TAG_FREQUENCY both set the player's Frequency knob
    // (instrument units: 0x4000 = one sample per 44.1 kHz output frame).
    if (rate >= 0) { in->knobs["Frequency"] = rate; in->pitch_step = -1; }
    else if (freq16) { in->knobs["Frequency"] = int32_t(freq16); in->pitch_step = -1; freq16 = 0; }

    // Auto-start attachments: per FIFO, the first one not marked NOAUTOSTART.
    std::map<std::string, bool> fifo_started;
    for (int32_t aid : in->atts) {
        Attachment& a = atts_[aid];
        a.active = false;
        if (a.envelope) { reset_play(a); continue; }
        if ((a.flags & 1) || fifo_started[a.fifo]) continue;
        fifo_started[a.fifo] = true;
        reset_play(a);
    }

    // A note (pitch / frequency) sets the rate relative to the sample's own.
    if ((pitch >= 0 || freq16) && in->kind == Kind::Sampler && !in->fixed_rate) {
        const Sample* s = nullptr;
        for (int32_t aid : in->atts) if ((s = sample(atts_[aid].sample))) break;
        const double srate = s ? s->rate : 22050.0;
        const int base = s ? s->base_note : 60;
        const double det = s ? s->detune / 1200.0 : 0.0;
        double ratio;
        if (pitch >= 0) ratio = std::pow(2.0, (pitch - base) / 12.0 - det);
        else ratio = (freq16 / 65536.0) / (440.0 * std::pow(2.0, (base - 69) / 12.0));
        in->pitch_step = ratio * srate / kRate;
    }
    if (trace) std::fprintf(stderr, "[audio] start %d (%s)\n", id, in->tmpl.c_str());
}

void Audio::release(int32_t id) {
    Instrument* in = instrument(id);
    if (!in || in->st == Instrument::St::Stopped) return;
    in->st = Instrument::St::Released;
    for (int32_t aid : in->atts) atts_[aid].released = true;
}

void Audio::stop(int32_t id) {
    Instrument* in = instrument(id);
    if (!in) return;
    in->st = Instrument::St::Stopped;
    for (int32_t aid : in->atts) atts_[aid].active = false;
}

void Audio::start_attachment(int32_t id) {
    Attachment* a = attachment(id);
    if (!a) return;
    if (Instrument* in = instrument(a->ins))   // one sample per FIFO at a time
        for (int32_t o : in->atts) if (o != id && atts_[o].fifo == a->fifo && !a->envelope) atts_[o].active = false;
    reset_play(*a);
}

void Audio::release_attachment(int32_t id) { if (Attachment* a = attachment(id)) a->released = true; }
void Audio::stop_attachment(int32_t id)    { if (Attachment* a = attachment(id)) a->active = false; }

void Audio::connect(int32_t src, const std::string& sp, int32_t dst, const std::string& dp) {
    disconnect(src, sp, dst, dp);
    conns_.push_back({src, sp, dst, dp});
}

void Audio::disconnect(int32_t src, const std::string& sp, int32_t dst, const std::string& dp) {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [&](const Conn& c) {
        return c.src == src && c.dst == dst && c.src_port == sp && c.dst_port == dp; }), conns_.end());
}

int Audio::status(int32_t item) const {
    if (auto it = ins_.find(item); it != ins_.end())
        return it->second.st == Instrument::St::Started ? 3 : it->second.st == Instrument::St::Released ? 2 : 1;
    if (auto it = atts_.find(item); it != atts_.end())
        return !it->second.active ? 1 : it->second.released ? 2 : 3;
    return 0;
}

// ---------------------------------------------------------------------------
// sample decoding
// ---------------------------------------------------------------------------

static const int kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

int32_t Audio::decode(Attachment& a, const Sample& s, int64_t frame, int ch) {
    const auto& ram = mem_.ram;
    const int chs = std::max(1, s.channels);
    auto rd8 = [&](uint64_t addr) -> int32_t { return addr < kRamEnd ? int32_t(int8_t(ram[size_t(addr)])) : 0; };
    if (s.codec == Codec::Pcm) {
        if (s.bits <= 8) return rd8(s.addr + uint64_t(frame) * chs + ch) * 256;
        const uint64_t at = s.addr + (uint64_t(frame) * chs + ch) * 2;
        return at + 1 < kRamEnd ? int32_t(int16_t(uint16_t(ram[size_t(at)] << 8 | ram[size_t(at + 1)]))) : 0;
    }
    // Compressed: decode sequentially; a jump backwards (loop) restarts there
    // from the current values, which is what a streaming decoder does anyway.
    // The frame just decoded is asked for again for its second channel: answer
    // from the decoder state. Only an earlier frame (a loop) means decoding again.
    if (frame == a.dec_frame) return a.dec_val[std::min(ch, 1)];
    if (frame < a.dec_frame) { a.dec_frame = frame - 1; a.duck_left = 0; a.duck_skip = 0; }
    if (frame - a.dec_frame > 1 << 20) a.dec_frame = frame - 1;   // never spin on a wild position
    while (a.dec_frame < frame) {
        ++a.dec_frame;
        if (s.codec == Codec::Duck) {
            const uint64_t at = s.addr + uint64_t(a.dec_frame);
            const uint8_t b0 = at < kRamEnd ? ram[size_t(at)] : 0, b1 = at + 1 < kRamEnd ? ram[size_t(at + 1)] : 0;
            if (a.duck_skip > 0) { --a.duck_skip; continue; }          // rest of a block header: hold the sample
            if (a.duck_left <= 0 && b0 == 0xF7 && b1 == 0x7F) {         // block header: new step indexes
                a.adp_index[0] = std::min<int>(88, at + 2 < kRamEnd ? ram[size_t(at + 2)] : 0);
                a.adp_index[1] = std::min<int>(88, at + 3 < kRamEnd ? ram[size_t(at + 3)] : 0);
                a.duck_left = 1508; a.duck_skip = 3;
                continue;
            }
            if (a.duck_left > 0) --a.duck_left;
            for (int c = 0; c < 2; ++c) {
                const int code = c == 0 ? (b0 >> 4) : (b0 & 15);
                const int step = kImaStep[a.adp_index[c]];
                int diff = step >> 3;
                if (code & 4) diff += step;
                if (code & 2) diff += step >> 1;
                if (code & 1) diff += step >> 2;
                a.dec_val[c] = std::clamp(a.dec_val[c] + ((code & 8) ? -diff : diff), -32768, 32767);
                a.adp_index[c] = std::clamp(a.adp_index[c] + kImaIndex[code], 0, 88);
            }
            continue;
        }
        for (int c = 0; c < chs && c < 2; ++c) {
            if (s.codec == Codec::Sdx2) {
                const int32_t b = rd8(s.addr + uint64_t(a.dec_frame) * chs + c);
                const int32_t sq = (b * b * 2) * (b < 0 ? -1 : 1);
                int32_t v = (b & 1) ? a.dec_val[c] + sq : sq;
                a.dec_val[c] = std::clamp(v, -32768, 32767);
            } else {   // ADP4: IMA ADPCM, high nibble first
                const uint64_t nib = uint64_t(a.dec_frame) * chs + c;
                const uint64_t addr = s.addr + nib / 2;
                const uint8_t byte = addr < kRamEnd ? ram[size_t(addr)] : 0;
                static const bool low_first = std::getenv("TDORT_ADP_LOWFIRST") != nullptr;
                const bool second = (nib & 1) != 0;
                const int code = (second != low_first) ? (byte & 15) : (byte >> 4);
                const int step = kImaStep[a.adp_index[c]];
                int diff = step >> 3;
                if (code & 4) diff += step;
                if (code & 2) diff += step >> 1;
                if (code & 1) diff += step >> 2;
                int32_t v = a.dec_val[c] + ((code & 8) ? -diff : diff);
                a.dec_val[c] = std::clamp(v, -32768, 32767);
                a.adp_index[c] = std::clamp(a.adp_index[c] + kImaIndex[code], 0, 88);
            }
        }
    }
    return a.dec_val[std::min(ch, 1)];
}

void Audio::set_vpp_tables(int32_t ins, const uint16_t* uv, const uint16_t* y) {
    if (Instrument* in = instrument(ins)) {
        std::copy(uv, uv + 16, in->vpp_tab[0]);
        std::copy(y, y + 16, in->vpp_tab[1]);
    }
}

// vpp4.dsp: translate every started InFIFO/OutFIFO pair at once, then end
// those attachments so the cue the player waits on (end of InFIFO1) fires.
void Audio::vpp_run(Instrument& in) {
    if (in.st == Instrument::St::Stopped || in.paused) return;
    std::vector<int32_t> done;
    // ezflix225.dsp loads its tables from InFIFO0 first: 16 pairs of
    // (YQuantVals[i], UVQuantVals[i]).
    for (int32_t aid : in.atts) {
        auto it = atts_.find(aid);
        if (it == atts_.end() || !it->second.active || it->second.fifo != "InFIFO0") continue;
        if (const Sample* t = sample(it->second.sample); t && t->addr && t->bytes >= 64 && uint64_t(t->addr) + 64 <= kRamEnd)
            for (int i = 0; i < 16; ++i) {
                const uint32_t at = t->addr + uint32_t(i) * 4;
                in.vpp_tab[1][i] = uint16_t(mem_.ram[at] << 8 | mem_.ram[at + 1]);
                in.vpp_tab[0][i] = uint16_t(mem_.ram[at + 2] << 8 | mem_.ram[at + 3]);
            }
        done.push_back(aid);
    }
    for (int k = 1; k <= 3; ++k) {
        int32_t ia = 0, oa = 0;
        for (int32_t aid : in.atts) {
            auto it = atts_.find(aid);
            if (it == atts_.end() || !it->second.active) continue;
            if (it->second.fifo == "InFIFO" + std::to_string(k)) ia = aid;
            else if (it->second.fifo == "OutFIFO" + std::to_string(k)) oa = aid;
        }
        if (!ia || !oa) continue;
        const Sample* si = sample(atts_[ia].sample);
        const Sample* so = sample(atts_[oa].sample);
        if (si && so && si->addr && so->addr) {
            const uint16_t* tab = in.vpp_tab[k == 3 ? 0 : 1];
            const uint32_t words = std::min(si->bytes / 2, so->bytes / 8);
            std::vector<uint8_t> out(size_t(words) * 8);
            for (uint32_t i = 0; i < words; ++i) {
                const uint64_t at = uint64_t(si->addr) + i * 2;
                const uint16_t w = at + 1 < kRamEnd ? uint16_t(mem_.ram[size_t(at)] << 8 | mem_.ram[size_t(at + 1)]) : 0;
                for (int j = 0; j < 4; ++j) {
                    const uint16_t v = tab[(w >> (12 - 4 * j)) & 15];
                    out[i * 8 + j * 2] = uint8_t(v >> 8);
                    out[i * 8 + j * 2 + 1] = uint8_t(v);
                }
            }
            if (!out.empty() && uint64_t(so->addr) + out.size() <= kRamEnd) mem_.write_block(so->addr, out.data(), uint32_t(out.size()));
            if (trace) std::fprintf(stderr, "[audio] vpp fifo %d: %u words %08X -> %08X\n", k, words, si->addr, so->addr);
        }
        done.push_back(ia);
        done.push_back(oa);
    }
    for (int32_t aid : done) attachment_ended(aid, atts_[aid], in);
}

void Audio::attachment_ended(int32_t id, Attachment& a, Instrument& in) {
    if (trace && in.data_duck) std::fprintf(stderr, "[audio] movie attachment %d ended at %.0f\n", id, a.pos);
    a.active = false;
    if (a.cue && a.cue_at == -2 && on_cue) { ++cues_fired; on_cue(a.cue); }
    if (a.next) if (Attachment* n = attachment(a.next)) {
        const double carry = 0;
        reset_play(*n);
        n->pos += carry;
    }
    if (a.flags & 2) stop(a.ins);
    (void)id; (void)in;
}

// One output frame from one attachment. Returns false when nothing played.
bool Audio::sampler_frame(Instrument& in, Attachment& a, float& l, float& r) {
    const Sample* orig = sample(a.sample);
    if (!orig) return false;
    // A sample with no sound (no address or no length, as a streaming
    // player's buffer between fills) plays nothing and its attachment ends at
    // once, firing its cue and starting the linked one, as on the DSP. Waiting
    // here stalled Twisted's opening stream.
    if (!orig->addr || !orig->bytes) { if (!std::getenv("TDORT_OLD_EMPTY")) a.ended_flag = true; return false; }
    Sample eff;
    const Sample* s = orig;
    if (in.data_bits || in.data_sdx2 || in.data_adp4 || in.data_duck) {   // the instrument decides the format
        eff = *orig;
        if (in.data_duck) { eff.codec = Codec::Duck; eff.channels = 2; eff.frames = eff.bytes; }
        else if (in.data_sdx2) eff.codec = Codec::Sdx2;
        else if (in.data_adp4) eff.codec = Codec::Adp4;
        else { eff.codec = Codec::Pcm; eff.bits = in.data_bits; }
        if (in.stereo || (in.data_adp4 && std::getenv("TDORT_ADP_STEREO"))) eff.channels = 2;
        if (!in.data_duck) eff.recompute_frames();
        // Bytes per frame as the sample declares it (its width and channels,
        // whatever its compression) and as this instrument reads the data.
        const double decl_bpf = double(std::max(1, orig->bits / 8) * std::max(1, orig->channels));
        double eff_bpf;
        switch (eff.codec) {
        case Codec::Duck: eff_bpf = 1.0; break;
        case Codec::Sdx2: eff_bpf = double(std::max(1, eff.channels)); break;
        case Codec::Adp4: eff_bpf = 0.5 * std::max(1, eff.channels); break;
        default:          eff_bpf = double(std::max(1, eff.bits / 8) * std::max(1, eff.channels)); break;
        }
        if (decl_bpf != eff_bpf) {
            // Loop points were given in the sample's own frames (the folio turns
            // them into byte addresses with the sample's width); when the
            // instrument reads the data in another format, rescale them so they
            // land on the same bytes. Without this a streaming ring loops at its
            // middle: Slam 'n Jam's movie sound and Madden's intro music, which
            // then played twice, a moment apart.
            const double k = decl_bpf / eff_bpf;
            auto sc = [&](int64_t v) { return v < 0 ? v : int64_t(double(v) * k + 0.5); };
            eff.sus_begin = sc(eff.sus_begin); eff.sus_end = sc(eff.sus_end);
            eff.rel_begin = sc(eff.rel_begin); eff.rel_end = sc(eff.rel_end);
        }
        if (eff.sus_end > int64_t(eff.frames)) eff.sus_end = -1;
        if (eff.rel_end > int64_t(eff.frames)) eff.rel_end = -1;
        s = &eff;
    }
    if (!s->frames) { if (!std::getenv("TDORT_OLD_EMPTY")) a.ended_flag = true; return false; }
    double step;
    if (in.fixed_rate) step = in.half_rate ? 0.5 : 1.0;
    else if (in.pitch_step >= 0) step = in.pitch_step;
    else {
        // Frequency knob: 0x4000 = 1.0 (one sample per 44.1 kHz frame). Real
        // Pinball plays its 22 kHz effects with 0x2000.
        int32_t k = knob(in, "Frequency", knob(in, "Rate", in.freq_unit));
        step = std::max(0, k) / double(in.freq_unit);
    }
    step *= in.bend;

    // Loops: sustain while held, release loop after release.
    const bool held = !a.released && in.st == Instrument::St::Started;
    const int64_t lb = held ? s->sus_begin : s->rel_begin, le = held ? s->sus_end : s->rel_end;
    if (lb >= 0 && le > lb && le <= int64_t(s->frames) && a.pos >= double(le)) {
        a.pos -= double(le - lb);
        if (a.cue && a.cue_at == -1 && on_cue) on_cue(a.cue);
    }
    if (a.pos >= double(s->frames)) { a.ended_flag = true; return false; }

    const int64_t f = int64_t(a.pos);
    const int chs = std::max(1, s->channels);
    float v0 = decode(a, *s, f, 0) / 32768.0f;
    float v1 = chs > 1 ? decode(a, *s, f, 1) / 32768.0f : v0;
    if (s->codec == Codec::Pcm && f + 1 < int64_t(s->frames)) {   // linear interpolation
        const float t = float(a.pos - double(f));
        v0 += (decode(a, *s, f + 1, 0) / 32768.0f - v0) * t;
        if (chs > 1) v1 += (decode(a, *s, f + 1, 1) / 32768.0f - v1) * t; else v1 = v0;
    }
    l += v0; r += v1;

    const double old = a.pos;
    a.pos += step;
    if (a.cue && a.cue_at >= 0 && old < a.cue_at && a.pos >= a.cue_at && on_cue) on_cue(a.cue);
    return true;
}

float Audio::envelope_gain(Instrument& in) {
    float g = 1.0f;
    for (int32_t aid : in.atts) {
        Attachment& a = atts_[aid];
        if (!a.envelope || !a.active) continue;
        auto it = envelopes_.find(a.envelope);
        if (it == envelopes_.end() || it->second.points.empty()) continue;
        const Envelope& e = it->second;
        const bool held = !a.released && in.st == Instrument::St::Started;
        double t = a.env_ms;
        if (held && e.sus_end >= 0 && e.sus_end < int(e.points.size()) && t > e.points[size_t(e.sus_end)].second)
            t = e.points[size_t(e.sus_end)].second;           // hold at the sustain point
        else if (!held && e.sus_end >= 0 && e.sus_end < int(e.points.size()) && t < e.points[size_t(e.sus_end)].second)
            a.env_ms = t = e.points[size_t(e.sus_end)].second; // released: continue from it
        float v = float(e.points.back().first);
        for (size_t i = 0; i + 1 < e.points.size(); ++i) {
            const auto& p0 = e.points[i]; const auto& p1 = e.points[i + 1];
            if (t >= p0.second && t <= p1.second) {
                const double span = std::max(1, p1.second - p0.second);
                v = float(p0.first + (p1.first - p0.first) * (t - p0.second) / span);
                break;
            }
        }
        if (t < e.points.front().second) v = float(e.points.front().first);
        g *= std::clamp(v / 32767.0f, 0.0f, 1.0f);
        a.env_ms += 1000.0 / kRate;
        // Past the last point and not holding at a sustain point: the
        // envelope has finished, which ends its attachment (and fires a
        // CUE_AT_END cue; Creature Shock waits for one after its intro).
        const bool holding = held && e.sus_end >= 0 && e.sus_end < int(e.points.size());
        if (!holding && t >= e.points.back().second) attachment_ended(aid, a, in);
    }
    return g;
}

bool Audio::has_active_envelope(const Instrument& in) const {
    for (int32_t aid : in.atts) {
        auto it = atts_.find(aid);
        if (it != atts_.end() && it->second.envelope && it->second.active) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// mixing
// ---------------------------------------------------------------------------

static int port_index(const std::string& port) {
    size_t i = port.size();
    while (i > 0 && std::isdigit(uint8_t(port[i - 1]))) --i;
    return i < port.size() ? std::atoi(port.c_str() + i) : 0;
}

void Audio::eval(int32_t id, Instrument& in) {
    if (in.stamp == stamp_) return;
    if (in.busy) return;                 // feedback loop: use last frame's value
    in.busy = true;
    float l = 0, r = 0;
    const bool running = in.st != Instrument::St::Stopped && !in.paused;

    // Connections into a knob (any port that isn't an "Input"): the source's
    // output drives that knob. Games wire envelope.dsp into a mixer's gain
    // knobs to fade sound in and out (Real Pinball's movie player does).
    for (const Conn& c : conns_) {
        if (c.dst != id || has(c.dst_port, "Input")) continue;
        auto it = ins_.find(c.src);
        if (it == ins_.end()) continue;
        eval(c.src, it->second);
        in.mod[c.dst_port] = int32_t(std::clamp(it->second.out_l * 32767.0f, -32768.0f, 32767.0f));
    }
    switch (in.kind) {
    case Kind::Sampler:
        if (running) {
            for (int32_t aid : in.atts) {
                Attachment& a = atts_[aid];
                if (!a.active || a.envelope) continue;
                if (!sampler_frame(in, a, l, r) && a.ended_flag) { a.ended_flag = false; attachment_ended(aid, a, in); }
            }
            const float amp = in.silent ? 0.0f : std::clamp(knob(in, "Amplitude", 0x7FFF), 0, 0x7FFF) / 32767.0f * envelope_gain(in);
            l *= amp; r *= amp;
            if (in.data_duck) {
                // Movie sound never resets its sample values, so any seam in the
                // streamed data (playback briefly out of step with the movie
                // player) leaves a lasting offset. A one-pole high-pass (about
                // 7 Hz, inaudible) removes the drift.
                constexpr float kPole = 0.999f;
                float* io[2] = {&l, &r};
                for (int c = 0; c < 2; ++c) {
                    const float x = *io[c];
                    const float y = x - in.dc_in[c] + kPole * in.dc_out[c];
                    in.dc_in[c] = x; in.dc_out[c] = y; *io[c] = y;
                }
            }
        }
        break;
    case Kind::Osc:
        if (running) {
            const double hz = std::max(0, knob(in, "Frequency", 0x0800)) * double(kRate) / 65536.0;
            in.phase += hz / kRate;
            in.phase -= std::floor(in.phase);
            float v;
            const std::string& n = in.tmpl;
            if (has(n, "noise")) { in.noise = in.noise * 1664525u + 1013904223u; v = int32_t(in.noise) / 2147483648.0f; }
            else if (has(n, "tri")) v = float(in.phase < 0.5 ? in.phase * 4 - 1 : 3 - in.phase * 4);
            else if (has(n, "pulse") || has(n, "square")) v = in.phase < 0.5 ? 1.0f : -1.0f;
            else if (has(n, "sine")) v = float(std::sin(in.phase * 6.283185307179586));
            else v = float(in.phase * 2 - 1);
            v *= std::clamp(knob(in, "Amplitude", 0x7FFF), 0, 0x7FFF) / 32767.0f * envelope_gain(in);
            l = r = v;
        }
        break;
    case Kind::Envelope:
        // envelope.dsp with an envelope attached: the output follows the
        // envelope over time. Otherwise the level moves toward Env.request
        // by Env.incr per frame (0 = at once); the output is that level.
        if (running && has_active_envelope(in)) {
            in.env = envelope_gain(in) * 32767.0;
            l = r = float(in.env / 32767.0);
        } else if (running) {
            const double target = knob(in, "Env.request", knob(in, "Request", 0x7FFF));
            const double incr = std::max(0, knob(in, "Env.incr", knob(in, "Incr", 0)));
            if (incr <= 0 || std::fabs(target - in.env) <= incr) in.env = target;
            else in.env += target > in.env ? incr : -incr;
            l = r = float(in.env / 32767.0);
        }
        break;
    default:     // Mixer, Output, Effect: sum the inputs
        if (running || in.kind == Kind::Effect) {
            for (const Conn& c : conns_) {
                if (c.dst != id || !has(c.dst_port, "Input")) continue;
                auto it = ins_.find(c.src);
                if (it == ins_.end()) continue;
                eval(c.src, it->second);
                const Instrument& s = it->second;
                float sl, sr;
                if (has(c.src_port, "Left")) sl = sr = s.out_l;
                else if (has(c.src_port, "Right")) sl = sr = s.out_r;
                else { sl = s.out_l; sr = s.out_r; }
                float gl = 1, gr = 1;
                if (in.kind == Kind::Mixer) {
                    const std::string n = std::to_string(port_index(c.dst_port));
                    if (in.stereo) {
                        gl = knob(in, ("LeftGain" + n).c_str(), knob(in, ("Gain" + n).c_str(), 0)) / 32767.0f;
                        gr = knob(in, ("RightGain" + n).c_str(), knob(in, ("Gain" + n).c_str(), 0)) / 32767.0f;
                    } else gl = gr = knob(in, ("Gain" + n).c_str(), 0) / 32767.0f;
                }
                if (has(c.dst_port, "Left")) { l += (sl + sr) * 0.5f * gl; continue; }
                if (has(c.dst_port, "Right")) { r += (sl + sr) * 0.5f * gr; continue; }
                l += sl * gl; r += sr * gr;
            }
            if (in.kind == Kind::Mixer && !in.stereo) r = l;
        }
        break;
    }
    in.out_l = l; in.out_r = r;
    in.stamp = stamp_;
    in.busy = false;
}

std::string Audio::describe() const {
    std::string s;
    char b[96];
    for (const auto& [id, in] : ins_) {
        if (in.st == Instrument::St::Stopped) continue;
        int active = 0;
        for (int32_t a : in.atts) { auto it = atts_.find(a); if (it != atts_.end() && it->second.active) ++active; }
        std::snprintf(b, sizeof b, " %d:%s%s%s(%d att, out %.2f)", id, in.tmpl.c_str(),
                      in.st == Instrument::St::Released ? "/rel" : "", in.paused ? "/pause" : "", active,
                      std::max(std::fabs(in.out_l), std::fabs(in.out_r)));
        s += b;
    }
    return s;
}

void Audio::mix(int16_t* out, int frames) {
    // DAC: every running mixer / output instrument whose output goes nowhere else.
    std::vector<int32_t> dac;
    for (auto& [id, in] : ins_) {
        // Mixers and output instruments feed the DAC, and so do the "duck"
        // movie players (adpcmduck22S.dsp), which have no output port.
        const bool direct = in.kind == Kind::Sampler && in.data_duck;
        if (in.kind != Kind::Mixer && in.kind != Kind::Output && !direct) continue;
        bool feeds = false;
        for (const Conn& c : conns_) if (c.src == id) { feeds = true; break; }
        if (!feeds) dac.push_back(id);
    }
    for (auto& [id, in] : ins_) if (in.vpp) vpp_run(in);
    for (int i = 0; i < frames; ++i) {
        ++stamp_;
        float l = 0, r = 0;
        for (int32_t id : dac) {
            Instrument& in = ins_[id];
            eval(id, in);
            l += in.out_l; r += in.out_r;
        }
        // Instruments nobody listens to still advance (their cues must fire).
        for (auto& [id, in] : ins_) if (in.stamp != stamp_ && in.kind == Kind::Sampler) eval(id, in);
        out[2 * i]     = int16_t(std::clamp(l * 32767.0f, -32768.0f, 32767.0f));
        out[2 * i + 1] = int16_t(std::clamp(r * 32767.0f, -32768.0f, 32767.0f));
    }
    frames_out_ += uint64_t(frames);
}

} // namespace tdort
