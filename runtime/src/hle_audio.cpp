#include <cstdlib>
// hle_audio.cpp - the audio folio: instruments, knobs, samples, attachments,
// envelopes and cues, on top of the host audio engine (tdort/audio.h).
//
// Vector numbers are the ones the SDK's audio link library uses; tag values
// follow the SDK's numbering (AF_TAG_* = 10 + n).
#include "hle_os.h"
#include "tdort/audio.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace tdort {

using namespace abi;

namespace {
enum : uint8_t { kAuTemplate = 1, kAuInstrument = 2, kAuKnob = 3, kAuSample = 4, kAuCue = 5, kAuEnvelope = 6,
                 kAuAttachment = 7, kAuTuning = 8 };
enum : uint32_t {
    kAfAmplitude = 10, kAfRate = 11, kAfName = 12, kAfPitch = 14, kAfVelocity = 15, kAfTemplate = 16,
    kAfInstrument = 17, kAfWidth = 22, kAfChannels = 23, kAfFrames = 24, kAfBaseNote = 25, kAfDetune = 26,
    kAfSustainBegin = 31, kAfSustainEnd = 32, kAfReleaseBegin = 33, kAfReleaseEnd = 34, kAfNumBytes = 35,
    kAfAddress = 36, kAfSample = 37, kAfSetFlags = 40, kAfClearFlags = 41, kAfFrequency = 42, kAfEnvelope = 43,
    kAfHookName = 44, kAfStartAt = 45, kAfSampleRate = 46, kAfCompressionRatio = 47, kAfCompressionType = 48,
    kAfNumBits = 49, kAfBaseFreq = 51, kAfDataOffset = 55, kAfDataSize = 56, kAfStatus = 60,
    kAfImageAddress = 63, kAfImageLength = 64, kAfLeaveInPlace = 65,
};
constexpr uint32_t kFourccSdx2 = 0x53445832u, kFourccAdp4 = 0x41445034u;   // 'SDX2', 'ADP4'

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

double ieee_extended(const uint8_t* p) {
    const int exp = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
    if (!exp && !mant) return 0;
    const double v = std::ldexp(double(mant), exp - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}
} // namespace

// Parse an AIFF / AIFC sound file. Fills format, loops and the sound data's
// position within the file.
bool Portfolio::parse_aiff(const uint8_t* d, size_t n, Audio::Sample& s, size_t& data_off, size_t& data_len,
                           std::string& why) {
    if (n < 12 || be32(d) != 0x464F524Du /*FORM*/) { why = "not an AIFF file"; return false; }
    const uint32_t form = be32(d + 8);
    if (form != 0x41494646u /*AIFF*/ && form != 0x41494643u /*AIFC*/) { why = "not an AIFF file"; return false; }
    std::map<uint16_t, uint32_t> marks;
    int sus[3] = {0, 0, 0}, rel[3] = {0, 0, 0};
    bool comm = false;
    data_off = data_len = 0;
    for (size_t p = 12; p + 8 <= n;) {
        const uint32_t id = be32(d + p), len = be32(d + p + 4);
        const uint8_t* c = d + p + 8;
        const size_t avail = std::min<size_t>(len, n - p - 8);
        if (id == 0x434F4D4Du /*COMM*/ && avail >= 18) {
            s.channels = be16(c);
            s.frames = be32(c + 2);
            s.bits = be16(c + 6);
            s.rate = ieee_extended(c + 8);
            s.codec = Audio::Codec::Pcm;
            if (form == 0x41494643u && avail >= 22) {
                const uint32_t comp = be32(c + 18);
                if (comp == kFourccSdx2) s.codec = Audio::Codec::Sdx2;
                else if (comp == kFourccAdp4) s.codec = Audio::Codec::Adp4;
            }
            comm = true;
        } else if (id == 0x53534E44u /*SSND*/ && avail >= 8) {
            data_off = p + 8 + 8 + be32(c);
            data_len = avail - 8 - be32(c);
        } else if (id == 0x4D41524Bu /*MARK*/ && avail >= 2) {
            const uint16_t count = be16(c);
            size_t q = 2;
            for (uint16_t i = 0; i < count && q + 7 <= avail; ++i) {
                marks[be16(c + q)] = be32(c + q + 2);
                const size_t slen = c[q + 6];
                q += 6 + ((1 + slen + 1) & ~size_t(1));
            }
        } else if (id == 0x494E5354u /*INST*/ && avail >= 20) {
            s.base_note = c[0];
            s.detune = int8_t(c[1]);
            sus[0] = be16(c + 8);  sus[1] = be16(c + 10); sus[2] = be16(c + 12);
            rel[0] = be16(c + 14); rel[1] = be16(c + 16); rel[2] = be16(c + 18);
        }
        p += 8 + ((len + 1) & ~1u);
    }
    if (!comm || !data_off) { why = "AIFF file without sound data"; return false; }
    if (sus[0] && marks.count(uint16_t(sus[1])) && marks.count(uint16_t(sus[2]))) {
        s.sus_begin = marks[uint16_t(sus[1])]; s.sus_end = marks[uint16_t(sus[2])];
    }
    if (rel[0] && marks.count(uint16_t(rel[1])) && marks.count(uint16_t(rel[2]))) {
        s.rel_begin = marks[uint16_t(rel[1])]; s.rel_end = marks[uint16_t(rel[2])];
    }
    // Bytes the frames occupy (the SSND chunk may be padded).
    Audio::Sample t = s;
    const uint32_t ch = uint32_t(std::max(1, s.channels));
    uint64_t need = s.codec == Audio::Codec::Sdx2 ? uint64_t(s.frames) * ch
                  : s.codec == Audio::Codec::Adp4 ? (uint64_t(s.frames) * ch + 1) / 2
                  : uint64_t(s.frames) * ch * uint32_t(std::max(1, s.bits / 8));
    data_len = size_t(std::min<uint64_t>(need, data_len));
    s.bytes = uint32_t(data_len);
    (void)t;
    return true;
}

int32_t Portfolio::audio_new(uint8_t type, const std::string& name) {
    return new_item(ItemKind::Generic, kSubsysAudio, type, 64, name).num;
}

// vpp4.dsp's lookup tables, from the game's own copy of the file: its DINI
// chunk holds { count, ?, resource, ? } + count words per table, and its
// DNMS chunk names the resources (UVQuantVals, YQuantVals, ...).
void Portfolio::load_vpp_tables(int32_t ins, const std::string& name) {
    std::string host, norm; bool dir = false;
    const std::string tries[] = {name, "$audio/dsp/" + name, "/remote/System/Audio/dsp/" + name};
    std::vector<uint8_t> f;
    for (const auto& t : tries) {
        if (!resolve(t, host, norm, dir) || dir) continue;
        FILE* fp = std::fopen(host.c_str(), "rb");
        if (!fp) continue;
        uint8_t b[4096]; size_t n = std::fread(b, 1, sizeof b, fp); std::fclose(fp);
        f.assign(b, b + n);
        break;
    }
    if (f.empty()) return;
    auto be = [&](size_t o) { return o + 4 <= f.size() ? load_be32(&f[o]) : 0u; };
    std::vector<std::string> names;
    size_t dini = 0, dini_len = 0;
    for (size_t o = 0; o + 8 <= f.size();) {
        const uint32_t id = be(o), len = be(o + 4);
        if (id == 0x464F524Du) { o += 12; continue; }            // FORM: step into it
        if (id == 0x444E4D53u)                                   // DNMS
            for (size_t k = o + 8; k < o + 8 + len && k < f.size();) {
                std::string sname;
                while (k < f.size() && f[k]) sname += char(f[k++]);
                ++k;
                if (!sname.empty()) names.push_back(sname);
            }
        if (id == 0x44494E49u) { dini = o + 8; dini_len = len; }   // DINI
        o += 8 + len + (len & 1);
    }
    uint16_t uv[16], y[16];
    bool got_uv = false, got_y = false;
    for (size_t o = dini; dini && o + 16 <= dini + dini_len;) {
        const uint32_t count = be(o), rsc = be(o + 8);
        if (count == 0 || count > 256) break;
        const std::string rn = rsc < names.size() ? names[rsc] : "";
        if (count == 16 && (rn == "UVQuantVals" || rn == "YQuantVals")) {
            uint16_t* t = rn == "YQuantVals" ? y : uv;
            for (int k = 0; k < 16; ++k) t[k] = uint16_t(be(o + 16 + 4 * size_t(k)));
            (rn == "YQuantVals" ? got_y : got_uv) = true;
        }
        o += 16 + 4 * size_t(count);
    }
    if (got_uv && got_y) {
        rt.audio->set_vpp_tables(ins, uv, y);
        rt.log("vpp: tables from %s", host.c_str());
    }
}

int32_t Portfolio::audio_instrument(int32_t tmpl) {
    auto it = templates_.find(tmpl);
    if (it == templates_.end()) return kErrBadItem;
    const int32_t ins = audio_new(kAuInstrument, it->second);
    rt.audio->add_instrument(ins, it->second);
    if (Audio::Instrument* ai = rt.audio->instrument(ins); ai && ai->vpp) load_vpp_tables(ins, it->second);
    ins_template_[ins] = tmpl;
    // Samples and envelopes attached to the template come with each instrument.
    if (auto ta = template_attach_.find(tmpl); ta != template_attach_.end())
        for (const TemplateAttach& a : ta->second)
            attach(ins, a.sample, a.envelope, a.fifo, a.flags, a.start_at);
    return ins;
}

int32_t Portfolio::audio_template(const std::string& name) {
    const int32_t t = audio_new(kAuTemplate, name);
    templates_[t] = name;
    return t;
}

void Portfolio::apply_sample_tags(Audio::Sample& s, uint32_t tags) {
    bool bytes_set = false, frames_set = false;
    each_tag(tags, [&](uint32_t t, uint32_t a) {
        switch (t) {
        case kAfAddress:       s.addr = a; break;
        case kAfNumBytes:      s.bytes = a; bytes_set = true; break;
        case kAfFrames:        s.frames = a; frames_set = true; break;
        case kAfChannels:      s.channels = int(std::max<uint32_t>(1, a)); break;
        case kAfWidth:         s.bits = int(a) * 8; break;
        case kAfNumBits:       s.bits = int(a); break;
        case kAfSampleRate:    s.rate = a / 65536.0; break;
        case kAfBaseNote:      s.base_note = int(a); break;
        case kAfDetune:        s.detune = int32_t(a); break;
        case kAfSustainBegin:  s.sus_begin = int32_t(a); break;
        case kAfSustainEnd:    s.sus_end = int32_t(a); break;
        case kAfReleaseBegin:  s.rel_begin = int32_t(a); break;
        case kAfReleaseEnd:    s.rel_end = int32_t(a); break;
        case kAfCompressionType:
            s.codec = a == kFourccSdx2 ? Audio::Codec::Sdx2 : a == kFourccAdp4 ? Audio::Codec::Adp4 : Audio::Codec::Pcm;
            break;
        default: break;
        }
    });
    if (bytes_set && !frames_set) s.recompute_frames();
    else if (frames_set && !bytes_set) {
        const uint32_t ch = uint32_t(std::max(1, s.channels));
        s.bytes = s.codec == Audio::Codec::Sdx2 ? s.frames * ch
                : s.codec == Audio::Codec::Adp4 ? (s.frames * ch + 1) / 2
                : s.frames * ch * uint32_t(std::max(1, s.bits / 8));
    }
}

int32_t Portfolio::load_sample(const std::string& path, bool scan_only, uint32_t scan_bytes) {
    std::string host, norm; bool dir = false;
    if (!resolve(path, host, norm, dir) || dir) { rt.log("LoadSample(\"%s\"): not found", path.c_str()); return kErrNotFound; }
    std::ifstream f(host, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Audio::Sample s;
    size_t off = 0, len = 0;
    std::string why;
    if (!parse_aiff(file.data(), file.size(), s, off, len, why)) {
        rt.log("LoadSample(\"%s\"): %s", path.c_str(), why.c_str());
        return kErrNotSupported;
    }
    s.data_offset = uint32_t(off);
    s.data_size = uint32_t(len);
    // ScanSample reads only the header plus the first scan_bytes of sound
    // (streaming players read the rest themselves, at data_offset).
    const size_t keep = scan_only ? std::min<size_t>(len, scan_bytes) : len;
    if (keep) {
        s.addr = alloc(uint32_t(scan_only ? scan_bytes : len), kMemFromTop);
        if (!s.addr) return kErrNoMem;
        s.host_alloc = s.addr;
        rt.mem.write_block(s.addr, file.data() + off, uint32_t(keep));
        if (scan_only) { s.bytes = uint32_t(keep); s.recompute_frames(); }
    } else { s.addr = 0; s.bytes = 0; }
    const int32_t item = audio_new(kAuSample, path);
    rt.audio->add_sample(item, s);
    // The file comes off the disc, so the caller waits for the drive, as for
    // any synchronous read. Return Fire's music thread fades only when time
    // has passed since its last pass, and Choose loads its sounds between
    // starting the music and fading it out; done at once, both fell in the
    // same frame and the fade (which Choose waits for) never began. The
    // caller waits after its result is set (see the LoadSample vectors).
    last_load_bytes_ = uint32_t(off + keep);
    rt.log("LoadSample(\"%s\") -> %d: %d ch, %d bits%s, %.0f Hz, %u frames%s", path.c_str(), item, s.channels, s.bits,
           s.codec == Audio::Codec::Sdx2 ? " SDX2" : s.codec == Audio::Codec::Adp4 ? " ADP4" : "", s.rate, s.frames,
           s.sus_end > s.sus_begin ? ", looped" : "");
    return item;
}

int32_t Portfolio::make_sample(uint32_t numbytes, uint32_t tags) {
    Audio::Sample s;
    s.bits = 16; s.channels = 1; s.rate = 44100;
    s.bytes = numbytes;
    // An AIFF file image already in memory (AF_TAG_IMAGE_ADDRESS/LENGTH): its
    // sound is used where it is with AF_TAG_LEAVE_IN_PLACE, else copied.
    uint32_t image = 0, image_len = 0, in_place = 0;
    tag_value(tags, kAfImageAddress, image); tag_value(tags, kAfImageLength, image_len); tag_value(tags, kAfLeaveInPlace, in_place);
    if (image && image_len && guest_range_ok(image, image_len)) {
        size_t off = 0, len = 0;
        std::string why;
        if (!parse_aiff(&rt.mem.ram[image], image_len, s, off, len, why)) {
            rt.log("CreateSample(image %08X, %u bytes): %s", image, image_len, why.c_str());
            return kErrNotSupported;
        }
        s.data_offset = uint32_t(off);
        s.data_size = uint32_t(len);
        s.bytes = uint32_t(len);
        if (in_place) s.addr = image + uint32_t(off);
        else if (len) {
            s.addr = alloc(uint32_t(len), kMemFromTop);
            if (!s.addr) return kErrNoMem;
            s.host_alloc = s.addr;
            std::vector<uint8_t> tmp(rt.mem.ram.begin() + image + off, rt.mem.ram.begin() + image + off + len);
            rt.mem.write_block(s.addr, tmp.data(), uint32_t(len));
        }
        s.recompute_frames();
    }
    apply_sample_tags(s, tags);
    if (!s.addr && s.bytes) {
        s.addr = alloc(s.bytes, kMemFromTop);
        if (!s.addr) return kErrNoMem;
        s.host_alloc = s.addr;
    }
    if (!s.frames) s.recompute_frames();
    const int32_t item = audio_new(kAuSample, "");
    rt.audio->add_sample(item, s);
    return item;
}

int32_t Portfolio::attach(int32_t ins, int32_t sample, int32_t envelope, const std::string& fifo, uint32_t flags,
                          uint32_t start_at) {
    if (!rt.audio->instrument(ins)) {
        // An attachment to an instrument template applies to every instrument
        // later made from it (Dragon Lore attaches its sound buffer this way).
        if (!templates_.count(ins)) return kErrBadItem;
        const int32_t item = audio_new(kAuAttachment, fifo);
        template_attach_[ins].push_back({item, sample, envelope, fifo, flags, start_at});
        return item;
    }
    Audio::Attachment a;
    a.ins = ins; a.sample = sample; a.envelope = envelope; a.fifo = fifo; a.flags = flags; a.start_at = start_at;
    const int32_t item = audio_new(kAuAttachment, fifo);
    rt.audio->add_attachment(item, a);
    if (rt.cfg.trace) {
        const Audio::Sample* s = rt.audio->sample(sample);
        const Audio::Instrument* in = rt.audio->instrument(ins);
        rt.log("attach %d: instrument %d (%s) <- sample %d: addr %08X, %u bytes, %u frames, %d ch, %d bits, loop %lld-%lld, flags %X",
               item, ins, in ? in->tmpl.c_str() : "?", sample, s ? s->addr : 0, s ? s->bytes : 0, s ? s->frames : 0,
               s ? s->channels : 0, s ? s->bits : 0, (long long)(s ? s->sus_begin : -1), (long long)(s ? s->sus_end : -1), flags);
    }
    return item;
}

int32_t Portfolio::make_envelope(uint32_t points, uint32_t count, int32_t sus_b, int32_t sus_e) {
    Audio::Envelope e;
    for (uint32_t i = 0; i < count && i < 1024; ++i)          // { uint32 time; int32 data; }
        e.points.push_back({int32_t(r32(points + i * 8 + 4)), int32_t(r32(points + i * 8))});
    e.sus_begin = sus_b; e.sus_end = sus_e;
    const int32_t item = audio_new(kAuEnvelope, "");
    rt.audio->add_envelope(item, e);
    return item;
}

int32_t Portfolio::create_audio_item(uint8_t type, uint32_t tags, const std::string& name) {
    switch (type) {
    case kAuTemplate: {
        std::string n = name;
        uint32_t a = 0;
        if (n.empty() && tag_value(tags, kAfName, a)) n = rstr(a);
        return audio_template(n);
    }
    case kAuInstrument: {
        uint32_t t = 0;
        tag_value(tags, kAfTemplate, t);
        return audio_instrument(int32_t(t));
    }
    case kAuKnob: {
        uint32_t ins = 0;
        tag_value(tags, kAfInstrument, ins);
        const int32_t k = audio_new(kAuKnob, name);
        knobs_[k] = {int32_t(ins), name};
        return k;
    }
    case kAuSample: {
        uint32_t nb = 0;
        tag_value(tags, kAfNumBytes, nb);
        return make_sample(0, tags);
    }
    case kAuCue: {
        const int32_t q = audio_new(kAuCue, name);
        cue_owner_[q] = cur().item;
        return q;
    }
    case kAuEnvelope: {
        uint32_t pts = 0, cnt = 0, sb = uint32_t(-1), se = uint32_t(-1);
        tag_value(tags, kAfAddress, pts); tag_value(tags, kAfFrames, cnt);
        tag_value(tags, kAfSustainBegin, sb); tag_value(tags, kAfSustainEnd, se);
        return make_envelope(pts, cnt, int32_t(sb), int32_t(se));
    }
    case kAuAttachment: {
        uint32_t ins = 0, smp = 0, env = 0, hook = 0, flags = 0, start = 0;
        tag_value(tags, kAfInstrument, ins); tag_value(tags, kAfSample, smp); tag_value(tags, kAfEnvelope, env);
        if (!ins) tag_value(tags, kAfTemplate, ins);
        tag_value(tags, kAfHookName, hook); tag_value(tags, kAfSetFlags, flags); tag_value(tags, kAfStartAt, start);
        return attach(int32_t(ins), int32_t(smp), int32_t(env), hook ? rstr(hook) : "", flags, start);
    }
    default:
        return audio_new(type, name);   // tunings and anything else: a plain item
    }
}

void Portfolio::audio_deleted(HleItem& it) {
    if (Audio::Sample* s = rt.audio->sample(it.num)) if (s->host_alloc) free_mem(s->host_alloc);
    rt.audio->remove(it.num);
    knobs_.erase(it.num);
    templates_.erase(it.num);
    template_attach_.erase(it.num);
    ins_template_.erase(it.num);
    abandoned_.erase(it.num);
    for (auto& [t, list] : template_attach_)
        list.erase(std::remove_if(list.begin(), list.end(), [&](const TemplateAttach& a) { return a.item == it.num; }), list.end());
    cue_owner_.erase(it.num);
}

void Portfolio::install_audio(std::map<int32_t, HleFn>& av) {
    Audio& au = *rt.audio;
    au.trace = rt.cfg.trace;
    au.on_cue = [this](int32_t cue) {
        auto o = cue_owner_.find(cue);
        const int32_t task = o != cue_owner_.end() ? o->second : tasks_[0].item;
        uint32_t sig;
        if (auto s = cue_signals_.find(cue); s != cue_signals_.end()) sig = s->second;
        else {
            HleTask* t = task_by_item(task);
            sig = 0;
            if (t) for (uint32_t b = 30; b >= kSigFirstUser; --b) if (!(t->allocated & (1u << b))) { sig = 1u << b; break; }
            if (t) t->allocated |= sig;
            cue_signals_[cue] = sig;
        }
        if (rt.cfg.trace) rt.log("audio cue %d -> task %d signal %08X", cue, task, sig);
        raise_signal(task, sig);
    };

    // ---- instruments
    av[-10] = [this](Cpu*) {                                 // LoadInstrument(name, device, pri)
        const std::string name = rstr(arg(0));
        const int32_t t = audio_template(name);
        c.r[0] = uint32_t(audio_instrument(t));
        rt.log("LoadInstrument(\"%s\") -> %d", name.c_str(), int32_t(c.r[0]));
    };
    av[-1]  = [this](Cpu*) { c.r[0] = uint32_t(audio_template(rstr(arg(0)))); };            // LoadInsTemplate
    av[-2]  = [this](Cpu*) { c.r[0] = uint32_t(audio_instrument(int32_t(arg(0)))); };       // AllocInstrument
    av[-24] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // UnloadInstrument
    av[-23] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // UnloadInsTemplate
    av[-25] = [this](Cpu*) {                                                                // FindAudioDevice
        if (!audio_device_) audio_device_ = audio_new(0, "audio");
        c.r[0] = uint32_t(audio_device_);
    };
    av[-26] = [this](Cpu*) { c.r[0] = uint32_t(audio_template(arg(3) ? rstr(arg(3)) : "custom")); };   // DefineInsTemplate
    av[-45] = [this](Cpu*) {                                                                // CreateInsTemplate(tags)
        uint32_t a = 0; std::string n = "custom";
        if (tag_value(arg(0), kAfName, a) || tag_value(arg(0), kTagItemName, a)) n = rstr(a);
        c.r[0] = uint32_t(audio_template(n));
    };
    av[-28] = [this](Cpu*) { c.r[0] = 1; };                                                 // GetNumInstruments
    av[-47] = [this](Cpu*) { c.r[0] = uint32_t(audio_instrument(int32_t(arg(0)))); };       // CreateInstrument(template, tags)
    av[-48] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DeleteInstrument
    // DSP resource queries (deprecated in the SDK): nothing is reported as used.
    av[-13] = [this](Cpu*) { c.r[0] = 0; };                                                 // DSPGetRsrcAlloc
    av[-21] = [this](Cpu*) { c.r[0] = 0; };                                                 // DSPGetInsRsrcUsed
    av[-22] = [this](Cpu*) { c.r[0] = 0; };                                                 // DSPGetTotalRsrcUsed
    av[-27] = [this](Cpu*) { c.r[0] = 0; };                                                 // (reserved)
    av[-46] = [this](Cpu*) { c.r[0] = 0; };                                                 // GetAudioFolioInfo(tags): defaults
    // Probes read a value inside an instrument; the DSP's internals are not
    // modelled, so a probe exists but reads 0 (ReadProbe, SWI 36).
    av[-49] = [this](Cpu*) {                                                                // CreateProbe(ins, output, tags)
        HleItem& p = new_item(ItemKind::Generic, kSubsysAudio, 12, 64, arg(1) ? rstr(arg(1)) : "probe", false);
        c.r[0] = uint32_t(p.num);
    };
    av[-50] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DeleteProbe

    // ---- knobs
    av[-4] = [this](Cpu*) {                                  // GrabKnob(ins, name)
        if (!rt.audio->instrument(int32_t(arg(0)))) { c.r[0] = uint32_t(kErrBadItem); return; }
        const std::string n = rstr(arg(1));
        const int32_t k = audio_new(kAuKnob, n);
        knobs_[k] = {int32_t(arg(0)), n};
        if (rt.cfg.trace) rt.log("GrabKnob(%d, \"%s\") -> %d", int32_t(arg(0)), n.c_str(), k);
        c.r[0] = uint32_t(k);
    };
    av[-7] = [this](Cpu*) {                                  // GetNumKnobs(ins)
        Audio::Instrument* in = rt.audio->instrument(int32_t(arg(0)));
        c.r[0] = in ? uint32_t(Audio::knob_names(in->tmpl).size()) : 0;
    };
    av[-8] = [this](Cpu*) {                                  // GetKnobName(ins, n)
        Audio::Instrument* in = rt.audio->instrument(int32_t(arg(0)));
        c.r[0] = 0;
        if (!in) return;
        auto names = Audio::knob_names(in->tmpl);
        if (arg(1) >= names.size()) return;
        auto& cache = knob_name_strings_[names[arg(1)]];
        if (!cache) cache = guest_string(names[arg(1)]);
        c.r[0] = cache;
    };

    // ---- samples
    av[-3]  = [this](Cpu*) { last_load_bytes_ = 0; const std::string n = rstr(arg(0)); c.r[0] = uint32_t(load_sample(n, false, 0)); if (last_load_bytes_) cd_load_wait(last_load_bytes_, n); };     // LoadSample
    av[-38] = [this](Cpu*) { last_load_bytes_ = 0; const std::string n = rstr(arg(0)); c.r[0] = uint32_t(load_sample(n, false, 0)); if (last_load_bytes_) cd_load_wait(last_load_bytes_, n); };     // LoadSampleHere
    av[-12] = [this](Cpu*) { last_load_bytes_ = 0; const std::string n = rstr(arg(0)); c.r[0] = uint32_t(load_sample(n, true, arg(1))); if (last_load_bytes_) cd_load_wait(last_load_bytes_, n); }; // ScanSample(name, bufsize)
    av[-41] = [this](Cpu*) {                                 // DefineSampleHere(image, numbytes, alloc, free)
        const uint32_t img = arg(0), nb = arg(1);
        if (!guest_range_ok(img, nb)) { c.r[0] = uint32_t(kErrBadPtr); return; }
        Audio::Sample s; size_t off = 0, len = 0; std::string why;
        if (!parse_aiff(&rt.mem.ram[img], nb, s, off, len, why)) {
            rt.log("DefineSampleHere: %s", why.c_str()); c.r[0] = uint32_t(kErrNotSupported); return;
        }
        s.addr = img + uint32_t(off);
        const int32_t item = audio_new(kAuSample, "");
        rt.audio->add_sample(item, s);
        c.r[0] = uint32_t(item);
    };
    av[-14] = [this](Cpu*) { c.r[0] = uint32_t(make_sample(arg(0), arg(1))); };             // MakeSample(bytes, tags)
    av[-44] = [this](Cpu*) {                                                                // CreateSample(tags)
        if (std::getenv("TDORT_SLEEP_TRACE"))
            each_tag(arg(0), [&](uint32_t t, uint32_t v) { rt.log("  CreateSample tag %u = %08X", t, v); });
        c.r[0] = uint32_t(make_sample(0, arg(0)));
    };
    av[-39] = [this](Cpu*) {                                 // CreateDelayLine(bytes, channels, loop)
        Audio::Sample s; s.bits = 16; s.channels = int(std::max<uint32_t>(1, arg(1))); s.bytes = arg(0);
        s.addr = s.host_alloc = alloc(std::max<uint32_t>(16, arg(0)), kMemFromTop);
        s.recompute_frames();
        if (arg(2)) { s.sus_begin = 0; s.sus_end = s.frames; }
        const int32_t item = audio_new(kAuSample, "delay line");
        rt.audio->add_sample(item, s);
        c.r[0] = uint32_t(item);
    };
    av[-40] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DeleteDelayLine
    av[-11] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // UnloadSample
    av[-6]  = [this](Cpu*) { c.r[0] = 0; };                                                 // DebugSample

    // ---- attachments and envelopes
    av[-36] = [this](Cpu*) {                                 // AttachSample(ins, sample, fifo)
        c.r[0] = uint32_t(attach(int32_t(arg(0)), int32_t(arg(1)), 0, arg(2) ? rstr(arg(2)) : "", 0, 0));
    };
    av[-37] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DetachSample
    av[-32] = [this](Cpu*) {                                 // AttachEnvelope(ins, env, name)
        c.r[0] = uint32_t(attach(int32_t(arg(0)), 0, int32_t(arg(1)), arg(2) ? rstr(arg(2)) : "Env", 0, 0));
    };
    av[-33] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DetachEnvelope
    av[-34] = [this](Cpu*) {                                 // CreateEnvelope(points, n, susBegin, susEnd)
        c.r[0] = uint32_t(make_envelope(arg(0), arg(1), int32_t(arg(2)), int32_t(arg(3))));
    };
    av[-35] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DeleteEnvelope

    // ---- tuning and information
    av[-29] = [this](Cpu*) { c.r[0] = 0x10000; };                                           // GetMasterTuning
    av[-30] = [this](Cpu*) { c.r[0] = uint32_t(audio_new(kAuTuning, "")); };                // CreateTuning
    av[-31] = [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); };            // DeleteTuning
    av[-43] = [this](Cpu*) {                                 // Convert12TET_F16(semitones, cents, frac16 *out)
        const double r = std::pow(2.0, (int32_t(arg(0)) + int32_t(arg(1)) / 100.0) / 12.0);
        if (arg(2)) w32(arg(2), uint32_t(r * 65536.0));
        c.r[0] = 0;
    };
    av[-9] = [this](Cpu*) {                                  // GetAudioItemInfo(item, tags)
        const int32_t id = int32_t(arg(0));
        Audio::Sample* s = rt.audio->sample(id);
        uint32_t p = arg(1);
        for (int guard = 0; p && guard < 256; ++guard, p += 8) {
            const uint32_t t = r32(p);
            if (t == kTagEnd) break;
            if (t == kTagJump) { p = r32(p + 4) - 8; continue; }
            uint32_t v = r32(p + 4);
            if (t == kAfStatus) v = uint32_t(rt.audio->status(id));
            else if (s) switch (t) {
                case kAfAddress: v = s->addr; break;
                case kAfNumBytes: v = s->bytes; break;
                case kAfFrames: v = s->frames; break;
                case kAfChannels: v = uint32_t(s->channels); break;
                case kAfWidth: v = s->codec == Audio::Codec::Pcm ? uint32_t(std::max(1, s->bits / 8)) : 1; break;
                case kAfNumBits: v = uint32_t(s->bits); break;
                case kAfSampleRate: v = uint32_t(s->rate * 65536.0); break;
                case kAfBaseNote: v = uint32_t(s->base_note); break;
                case kAfDetune: v = uint32_t(s->detune); break;
                case kAfSustainBegin: v = uint32_t(s->sus_begin); break;
                case kAfSustainEnd: v = uint32_t(s->sus_end); break;
                case kAfReleaseBegin: v = uint32_t(s->rel_begin); break;
                case kAfReleaseEnd: v = uint32_t(s->rel_end); break;
                case kAfDataOffset: v = s->data_offset; break;
                case kAfDataSize: v = s->data_size; break;
                case kAfBaseFreq: v = uint32_t(440.0 * std::pow(2.0, (s->base_note - 69) / 12.0) * 65536.0); break;
                case kAfCompressionType:
                    v = s->codec == Audio::Codec::Sdx2 ? kFourccSdx2 : s->codec == Audio::Codec::Adp4 ? kFourccAdp4 : 0; break;
                case kAfCompressionRatio:
                    v = s->codec == Audio::Codec::Sdx2 ? 2 : s->codec == Audio::Codec::Adp4 ? 4 : 1; break;
                default: break;
            }
            w32(p + 4, v);
        }
        c.r[0] = 0;
    };
}

void Portfolio::install_audio_swis() {
    auto A = [this](uint32_t n, const char* name, HleFn fn) { rt.register_swi(kAudioSwi + n, std::string("audio:") + name, std::move(fn)); };
    A(31, "IncrementGlobalIndex", [this](Cpu*) { c.r[0] = 0; });
    A(34, "EnableAudioInput", [this](Cpu*) { c.r[0] = 0; });                 // no audio input: accepted, stays silent
    A(35, "SetAudioFolioInfo", [this](Cpu*) { c.r[0] = 0; });
    A(36, "ReadProbe", [this](Cpu*) { if (guest_range_ok(arg(1), 4)) w32(arg(1), 0); c.r[0] = 0; });
    auto knob_tweak = [this](Cpu*) {
        auto k = knobs_.find(int32_t(arg(0)));
        if (k == knobs_.end()) { c.r[0] = uint32_t(kErrBadItem); return; }
        rt.audio->set_knob(k->second.first, k->second.second, int32_t(arg(1)));
        rt.audio->last_tweak = int32_t(arg(1));
        c.r[0] = 0;
    };
    A(0, "TweakKnob", knob_tweak);
    A(17, "TweakRawKnob", knob_tweak);
    A(1, "StartInstrument", [this](Cpu*) {
        int32_t amp = -1, rate = -1, pitch = -1, vel = -1; uint32_t freq = 0;
        each_tag(arg(1), [&](uint32_t t, uint32_t a) {
            if (rt.cfg.trace) rt.log("  StartInstrument(%d) tag %u = %08X", int32_t(arg(0)), t, a);
            if (t == kAfAmplitude) amp = int32_t(a);
            else if (t == kAfRate) rate = int32_t(a);
            else if (t == kAfPitch) pitch = int32_t(a);
            else if (t == kAfVelocity) vel = int32_t(a);
            else if (t == kAfFrequency) freq = a;
        });
        if (!rt.audio->instrument(int32_t(arg(0)))) { c.r[0] = uint32_t(kErrBadItem); return; }
        rt.audio->start(int32_t(arg(0)), amp, rate, pitch, vel, freq);
        c.r[0] = 0;
    });
    A(2, "ReleaseInstrument", [this](Cpu*) { rt.audio->release(int32_t(arg(0))); c.r[0] = 0; });
    A(3, "StopInstrument", [this](Cpu*) { rt.audio->stop(int32_t(arg(0))); c.r[0] = 0; });
    A(4, "TuneInsTemplate", [this](Cpu*) { c.r[0] = 0; });
    A(5, "TuneInstrument", [this](Cpu*) { c.r[0] = 0; });
    A(8, "ConnectInstruments", [this](Cpu*) {
        rt.audio->connect(int32_t(arg(0)), rstr(arg(1)), int32_t(arg(2)), rstr(arg(3)));
        if (rt.cfg.trace) rt.log("Connect %d.\"%s\" -> %d.\"%s\"", int32_t(arg(0)), rstr(arg(1)).c_str(), int32_t(arg(2)), rstr(arg(3)).c_str());
        c.r[0] = 0;
    });
    A(12, "DisconnectInstruments", [this](Cpu*) {
        rt.audio->disconnect(int32_t(arg(0)), rstr(arg(1)), int32_t(arg(2)), rstr(arg(3)));
        c.r[0] = 0;
    });
    A(9, "TraceAudio", [this](Cpu*) { c.r[0] = 0; });
    // Audio SWI 7 with tag 102 reads a DSP register back. Slam 'n Jam downloads
    // a table into its DSP program through two knobs, handshaking by polling
    // this until the register echoes the knob value it just wrote. Nothing runs
    // on a DSP here, so the echo is that last written value.
    A(7, "ReadDSPRegister", [this](Cpu*) {
        uint32_t reg = 0;
        c.r[0] = tag_value(arg(0), 102, reg) ? uint32_t(rt.audio->last_tweak) : 0;
    });
    A(10, "AllocAmplitude", [this](Cpu*) { c.r[0] = arg(0); });
    A(11, "FreeAmplitude", [this](Cpu*) { c.r[0] = 0; });
    A(15, "SetAudioRate", [this](Cpu*) { set_audio_rate(arg(1)); c.r[0] = 0; });            // (owner, frac16 Hz)
    A(16, "SetAudioDuration", [this](Cpu*) {                                               // (owner, sample frames per tick)
        if (arg(1)) set_audio_rate(uint32_t((44100ull << 16) / arg(1)));
        c.r[0] = 0;
    });
    A(18, "StartAttachment", [this](Cpu*) { rt.audio->start_attachment(int32_t(arg(0))); c.r[0] = 0; });
    A(19, "ReleaseAttachment", [this](Cpu*) { rt.audio->release_attachment(int32_t(arg(0))); c.r[0] = 0; });
    A(20, "StopAttachment", [this](Cpu*) { rt.audio->stop_attachment(int32_t(arg(0))); c.r[0] = 0; });
    A(21, "LinkAttachments", [this](Cpu*) {
        Audio::Attachment* a = rt.audio->attachment(int32_t(arg(0)));
        if (!a) { c.r[0] = uint32_t(kErrBadItem); return; }
        a->next = int32_t(arg(1));
        c.r[0] = 0;
    });
    A(22, "MonitorAttachment", [this](Cpu*) {               // (attachment, cue, index)
        Audio::Attachment* a = rt.audio->attachment(int32_t(arg(0)));
        if (!a) { c.r[0] = uint32_t(kErrBadItem); return; }
        a->cue = int32_t(arg(1)); a->cue_at = int32_t(arg(2));
        c.r[0] = 0;
    });
    A(23, "SetMasterTuning", [this](Cpu*) { c.r[0] = 0; });
    // AbandonInstrument stops an instrument and leaves it for AdoptInstrument,
    // which hands back an abandoned instrument of that template, or 0 when
    // there is none (the music library then makes a new one).
    A(24, "AbandonInstrument", [this](Cpu*) {
        const int32_t ins = int32_t(arg(0));
        if (!rt.audio->instrument(ins)) { c.r[0] = uint32_t(kErrBadItem); return; }
        rt.audio->stop(ins);
        abandoned_.insert(ins);
        c.r[0] = 0;
    });
    A(25, "AdoptInstrument", [this](Cpu*) {
        const int32_t tmpl = int32_t(arg(0));
        if (!templates_.count(tmpl)) { c.r[0] = uint32_t(kErrBadItem); return; }
        if (std::getenv("TDORT_OLD_ADOPT")) { c.r[0] = uint32_t(audio_instrument(tmpl)); return; }
        for (auto it = abandoned_.begin(); it != abandoned_.end(); ++it) {
            auto t = ins_template_.find(*it);
            if (t != ins_template_.end() && t->second == tmpl) { c.r[0] = uint32_t(*it); abandoned_.erase(it); return; }
        }
        c.r[0] = 0;
    });
    A(26, "ScavengeInstrument", [this](Cpu*) { c.r[0] = uint32_t(audio_instrument(int32_t(arg(0)))); });
    A(27, "SetAudioItemInfo", [this](Cpu*) {
        if (std::getenv("TDORT_SLEEP_TRACE"))
            each_tag(arg(1), [&](uint32_t t, uint32_t v) { rt.log("  SetAudioItemInfo(%d) tag %u = %08X", int32_t(arg(0)), t, v); });
        if (Audio::Sample* s = rt.audio->sample(int32_t(arg(0)))) apply_sample_tags(*s, arg(1));
        else if (Audio::Attachment* a = rt.audio->attachment(int32_t(arg(0)))) {
            each_tag(arg(1), [&](uint32_t t, uint32_t v) {
                if (t == kAfSetFlags) a->flags |= v;
                else if (t == kAfClearFlags) a->flags &= ~v;
                else if (t == kAfStartAt) a->start_at = v;
                else if (t == kAfSample) a->sample = int32_t(v);
            });
        }
        c.r[0] = 0;
    });
    A(28, "PauseInstrument", [this](Cpu*) {
        if (Audio::Instrument* in = rt.audio->instrument(int32_t(arg(0)))) in->paused = true;
        c.r[0] = 0;
    });
    A(29, "ResumeInstrument", [this](Cpu*) {
        if (Audio::Instrument* in = rt.audio->instrument(int32_t(arg(0)))) in->paused = false;
        c.r[0] = 0;
    });
    A(30, "WhereAttachment", [this](Cpu*) {
        Audio::Attachment* a = rt.audio->attachment(int32_t(arg(0)));
        c.r[0] = a ? uint32_t(a->pos) : uint32_t(kErrBadItem);
    });
    A(32, "BendInstrumentPitch", [this](Cpu*) {
        if (Audio::Instrument* in = rt.audio->instrument(int32_t(arg(0)))) in->bend = arg(1) / 65536.0;
        c.r[0] = 0;
    });
    A(38, "GetAudioFrameCount", [this](Cpu*) { c.r[0] = uint32_t(rt.audio->frames_out()); });
    A(39, "GetAudioCyclesUsed", [this](Cpu*) { c.r[0] = 0; });
}

} // namespace tdort
