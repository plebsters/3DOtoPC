// tdort/audio.h - high-level replacement for the 3DO audio DSP.
//
// The 3DO plays sound by loading small DSP programs ("instruments" such as
// sampler.dsp or mixer4x2.dsp), attaching sample data to their FIFOs,
// connecting them together and tweaking their knobs. Rather than running the
// DSP code, this engine recognises instruments by name and does the
// equivalent work on the host:
//   * sample players read sample data (PCM 8/16-bit, SDX2, ADP4) straight
//     from guest memory, so streamed audio (spoolers refilling buffers) works;
//   * mixers sum their inputs with per-channel gains and feed the DAC;
//   * simple oscillators, pass-through for effects we don't model.
// Everything runs on the emulation thread; mix() is called once per frame.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tdort {

struct Memory;

class Audio {
    friend struct StateIO;   // savestate.cpp
public:
    static constexpr int kRate = 44100;
    explicit Audio(Memory& mem) : mem_(mem) {}

    enum class Kind { Sampler, Mixer, Output, Osc, Envelope, Effect, Math };
    // Duck: the Duck/Crystal Dynamics movie sound (adpcmduck22S.dsp). Stereo
    // IMA ADPCM, one byte per frame (high nibble left, low right), in blocks of
    // 1512 bytes: 0xF7 0x7F, the left and right step indexes, 1508 data bytes.
    // A block resets the step indexes; the sample values carry on.
    enum class Codec { Pcm, Sdx2, Adp4, Duck };

    struct Sample {
        uint32_t addr = 0, bytes = 0, frames = 0;
        int      channels = 1, bits = 16;
        Codec    codec = Codec::Pcm;
        double   rate = 44100.0;            // recorded sample rate (Hz)
        int      base_note = 60, detune = 0;
        int64_t  sus_begin = -1, sus_end = -1, rel_begin = -1, rel_end = -1;
        uint32_t host_alloc = 0;            // guest memory we allocated (freed on delete)
        uint32_t data_offset = 0, data_size = 0;   // where the sound data sits in its file (streaming players read it)
        void     recompute_frames();        // from bytes, channels, codec
    };
    struct Envelope {
        std::vector<std::pair<int32_t, int32_t>> points;   // (value 0..0x7FFF, time ms)
        int sus_begin = -1, sus_end = -1;
    };
    struct Attachment {
        int32_t  ins = 0, sample = 0, envelope = 0;
        std::string fifo;
        uint32_t flags = 0;                 // 1 = no auto-start, 2 = stop instrument at end
        int32_t  next = 0;                  // LinkAttachments
        uint32_t start_at = 0;
        int32_t  cue = 0, cue_at = -2;      // MonitorAttachment: frame, -1 loop, -2 end
        // play state
        bool     active = false, released = false, ended_flag = false;
        double   pos = 0;
        int64_t  dec_frame = -1;            // codec: last decoded frame
        int32_t  dec_val[2] = {0, 0};
        int      adp_index[2] = {0, 0};
        int      duck_left = 0;             // Duck: data bytes left in the current block (0 = find a header)
        int      duck_skip = 0;             // Duck: header bytes still to pass
        double   env_ms = 0;
    };
    struct Instrument {
        std::string tmpl;
        Kind     kind = Kind::Effect;
        bool     fixed_rate = false, half_rate = false, stereo = false;
        bool     silent = false;          // a player whose sound format isn't decoded (EZFlix)
        // vpp4.dsp (EZFlix video): not a sound instrument. Each 16-bit word from
        // InFIFOk becomes four table entries (one per 4-bit code, top first) on
        // OutFIFOk; FIFOs 1 and 2 use YQuantVals, FIFO 3 UVQuantVals.
        bool     vpp = false;
        uint16_t vpp_tab[2][16] = {};     // [0] UVQuantVals, [1] YQuantVals
        int32_t  freq_unit = 0x4000;      // Frequency knob value for normal speed (0x8000 on var* players)
        // The DSP program decides how sample data is read: "...8" players read
        // 8-bit PCM, "...16" 16-bit, the dcsqxd players SDX2, adpcm ADP4.
        int      data_bits = 0;             // 0 = as the sample says
        bool     data_sdx2 = false, data_adp4 = false, data_duck = false;
        int      inputs = 0;
        // Math (multiply.dsp, timesplus.dsp, subtract.dsp, add.dsp): Output from the
        // InputA/B/C knobs, each driven by a connection or set by a tweak.
        enum class Op { Mul, TimesPlus, Sub, Add } op = Op::Mul;
        std::map<std::string, int32_t, std::less<>> knobs;   // values as last tweaked (std::less<>: look up by const char* without a temporary string)
        std::map<std::string, int32_t, std::less<>> mod;     // knobs driven by another instrument's output (ConnectInstruments to a knob)
        // Knob version: changes whenever a knob or a driven knob changes, so
        // values worked out from knobs can be kept until then (not saved).
        uint64_t kver = 0;
        int32_t  c_amp = 0, c_freq = 0;
        uint64_t c_amp_ver = ~0ull, c_freq_ver = ~0ull;
        int8_t   wave = -1;                     // oscillator shape from the template name (-1: not yet known)
        double   env = 0;                       // envelope.dsp: current level
        float    dc_in[2] = {0, 0}, dc_out[2] = {0, 0};   // DC blocker (movie players)
        enum class St { Stopped, Started, Released } st = St::Stopped;
        bool     paused = false;
        double   pitch_step = -1;           // set by StartInstrument pitch/frequency tags
        double   bend = 1.0;
        double   phase = 0;
        uint32_t noise = 0x12345678u;
        std::vector<int32_t> atts;
        // per-sample evaluation scratch
        uint64_t stamp = 0;
        float    out_l = 0, out_r = 0;
        bool     busy = false;
    };
    struct Conn {
        int32_t src; std::string src_port; int32_t dst; std::string dst_port;
        // Worked out once from the port names (prep_conn), not saved: the mixer
        // used to search the names, parse the port number and build the gain
        // knob names for every connection on every output sample.
        mutable bool    prepped = false, dst_input = false;
        mutable uint8_t src_side = 0, dst_side = 0;      // 0 both, 1 left, 2 right
        mutable int8_t  math_k = -1;                     // InputA/B/C -> 0/1/2
        mutable std::string g, lg, rg;                   // GainN, LeftGainN, RightGainN
        mutable float   gl = 1, gr = 1;                  // mixer gains for this input...
        mutable uint64_t gver = ~0ull;                   // ...as of this knob version of the mixer
    };

    // ---- object management (item numbers come from the kernel) ----
    void add_instrument(int32_t item, const std::string& tmpl_name);
    void add_sample(int32_t item, const Sample& s) { samples_[item] = s; }
    void add_envelope(int32_t item, const Envelope& e) { envelopes_[item] = e; }
    void add_attachment(int32_t item, const Attachment& a);
    void set_vpp_tables(int32_t ins, const uint16_t* uv, const uint16_t* y);
    void remove(int32_t item);              // any audio item
    // Bytes of sample data per playback frame of the attachment, as its
    // instrument reads the data (WhereAttachment reports bytes).
    double      attachment_bytes_per_frame(int32_t att);
    Sample*     sample(int32_t i)     { auto it = samples_.find(i); return it == samples_.end() ? nullptr : &it->second; }
    Instrument* instrument(int32_t i) { auto it = ins_.find(i); return it == ins_.end() ? nullptr : &it->second; }
    Attachment* attachment(int32_t i) { auto it = atts_.find(i); return it == atts_.end() ? nullptr : &it->second; }
    static Kind classify(const std::string& tmpl, bool* fixed, bool* half, bool* stereo, int* inputs);
    static std::vector<std::string> knob_names(const std::string& tmpl);

    // ---- operations ----
    void set_knob(int32_t ins, const std::string& knob, int32_t value);
    void start(int32_t ins, int32_t amplitude, int32_t rate, int32_t pitch, int32_t velocity, uint32_t freq16);
    void release(int32_t ins);
    void stop(int32_t ins);
    void start_attachment(int32_t att);
    void release_attachment(int32_t att);
    void stop_attachment(int32_t att);
    void connect(int32_t src, const std::string& sp, int32_t dst, const std::string& dp);
    void disconnect(int32_t src, const std::string& sp, int32_t dst, const std::string& dp);
    int  status(int32_t item) const;        // AF_STOPPED/RELEASED/STARTED for instruments and attachments
    uint64_t frames_out() const { return frames_out_; }
    uint64_t cues_fired = 0;
    int32_t  last_tweak = 0;                // value of the most recent TweakKnob (DSP register read-back)
    std::string describe() const;           // playing voices, for the status line

    // Raised when a monitored attachment reaches its cue point (cue item).
    std::function<void(int32_t cue)> on_cue;
    bool trace = false;

    // Mix `frames` stereo frames into out (interleaved L/R).
    void mix(int16_t* out, int frames);

private:
    Memory& mem_;
    std::map<int32_t, Instrument> ins_;
    std::map<int32_t, Sample> samples_;
    std::map<int32_t, Envelope> envelopes_;
    std::map<int32_t, Attachment> atts_;
    std::vector<Conn> conns_;
    uint64_t stamp_ = 0, frames_out_ = 0;

    void  eval(int32_t id, Instrument& in);
    bool  sampler_frame(Instrument& in, Attachment& a, float& l, float& r);
    int32_t decode(Attachment& a, const Sample& s, int64_t frame, int ch);
    void  attachment_ended(int32_t id, Attachment& a, Instrument& in);
    void  vpp_run(Instrument& in);
    float envelope_gain(Instrument& in);
    bool  has_active_envelope(const Instrument& in) const;
    int32_t knob(const Instrument& in, const char* name, int32_t def) const;
    uint64_t knob_gen_ = 0;
    void knobs_changed(Instrument& in) { in.kver = ++knob_gen_; }
    static void prep_conn(const Conn& c);
};

} // namespace tdort
