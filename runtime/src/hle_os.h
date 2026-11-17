// hle_os.h - internal: the high-level Portfolio OS replacement.
//
// The game is a single Portfolio task. OS objects ("items") live in two
// places: the guest-visible structure (in guest memory, so game code that
// pokes at fields keeps working) and host-side bookkeeping (HleItem).
#pragma once
#include "tdort/audio.h"
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "tdort/aif.h"
#include "tdort/portfolio_abi.h"
#include "tdort/runtime.h"

namespace tdort {

enum class ItemKind { Generic, Folio, Task, Device, File, IOReq, MsgPort, Message, ScreenGroup, Screen, Bitmap, Semaphore };

struct HleItem {
    int32_t  num = 0;
    uint32_t addr = 0, size = 0;
    uint8_t  subsys = 0, type = 0;
    ItemKind kind = ItemKind::Generic;
    std::string name;
    bool     alive = true;
    bool     heap_owned = false;      // structure allocated from the game heap

    // File
    std::string guest_path, host_path;
    uint64_t file_size = 0;
    bool     is_dir = false;
    FILE*    fp = nullptr;
    // IOReq
    int32_t  device = 0, reply_port = 0, msg_item = 0;
    // MsgPort
    std::deque<int32_t> queue;
    uint32_t signal = 0;
    // Message
    int32_t  in_port = 0;
    // Screen group / screen / bitmap
    std::vector<int32_t> children;
    uint32_t buffer = 0;
    // Semaphore
    int32_t  sem_owner = 0;        // task item holding it
    uint32_t sem_count = 0;
    // owning task (ports: whose signal to raise)
    int32_t  owner_task = 0;
    // NVRAM file (writable, 1-byte blocks)
    bool     writable = false;
    uint32_t block_size = 2048;
};

// One Portfolio task or thread. The CPU registers of the task that is not
// running are kept here; switching tasks swaps them with the live Cpu.
struct CallRec {                    // one OS call, for the diagnosis report
    char     name[32] = {};
    uint32_t a[4] = {}, site = 0, frame = 0, count = 0;
};

struct HleTask {
    int32_t  item = 0;
    uint32_t addr = 0;              // guest Task structure
    uint32_t r[16] = {};
    uint32_t n = 0, z = 0, c = 0, v = 0, ctl = 0x10;
    uint32_t pending = 0, allocated = 0xFF;
    uint8_t  pri = 100;
    enum class St { Ready, Blocked, Sleeping, Dead } st = St::Ready;
    uint32_t wake_frame = 0;        // Sleeping: until this frame
    bool     waiting = false;       // inside a wait that has not completed yet
    uint32_t blocked_since = 0;     // frame that wait started
    int32_t  parent = 0;            // task that loaded this program (gets SIGF_DEADTASK when it ends)
    uint32_t owned_stack = 0;       // stack memory to free when the task dies
    bool     program = false;       // a separately loaded executable, not a thread
    std::string cwd;                // current directory, guest-normalized ("/Data"); each task has its own
    std::string name;
    std::deque<CallRec> calls;      // its last OS calls (repeats counted)
};

class Portfolio {
public:
    Portfolio(Runtime& rt, const AifImage& img);
    ~Portfolio();

    Runtime& rt;
    Cpu& c;

    // ---- guest memory helpers
    uint32_t r32(uint32_t a) const;
    void     w32(uint32_t a, uint32_t v);
    uint8_t  r8(uint32_t a) const;
    void     w8(uint32_t a, uint8_t v);
    std::string rstr(uint32_t a, size_t max = 512) const;
    uint32_t guest_string(const std::string& s);          // allocated in the HLE heap
    uint32_t arg(int n) const;                            // APCS: r0-r3, then stack
    bool     guest_range_ok(uint32_t a, uint32_t len) const;

    // ---- items
    HleItem& new_item(ItemKind k, uint8_t subsys, uint8_t type, uint32_t struct_size,
                      const std::string& name, bool from_game_heap = true);
    HleItem* item(int32_t num);
    HleItem* find_item(uint32_t ctype, const std::string& name);
    int32_t  delete_item(int32_t num);

    // ---- game heap
    uint32_t alloc(uint32_t size, uint32_t flags);
    void     update_free_page_bits();
    void     plot(int32_t bitmap_item, int32_t x, int32_t y, uint16_t pen);
    int32_t  draw_text(uint32_t gc, int32_t bitmap_item, const std::vector<uint32_t>& text);
    uint32_t audio_rate_ = 240u << 16, audio_rate_ticks_ = 0, audio_rate_frame_ = 0;   // the audio clock
    void     set_audio_rate(uint32_t rate16);
    uint32_t font_ = 0;                           // the Font structure GetCurrentFont returns
    // Compression folio (hle_compress.cpp)
    struct Codec;
    std::map<uint32_t, std::shared_ptr<Codec>> codecs_;   // by Compressor/Decompressor handle
    uint32_t deliver_code_ = 0, final_word_ = 0;
    void     install_compression();
    void     install_jstring();
    // diagnosis report (hle_report.cpp)
    void        note_call(const char* name, const uint32_t* r, uint32_t site);
    std::string describe_wait(const HleTask& t, uint32_t mask);
    std::string write_report(const std::string& ended, uint32_t screen_change_frame);
    uint32_t    last_input_frame_ = 0;    // last time the game handed back a controller event
    std::vector<uint32_t> read_sjis(uint32_t a);
    int32_t  write_sjis(const std::vector<uint32_t>& t, uint32_t out, uint32_t size, uint8_t filler, bool ascii);
    void     codec_emit_byte(Codec& d, uint8_t c, std::vector<uint32_t>& out);
    void     codec_deliver(Codec& d, const std::vector<uint32_t>& words, int32_t result);
    void     compress_some(Codec& d, bool flush, std::vector<uint32_t>& out);
    static constexpr uint32_t kGcFGPen = 20, kGcBGPen = 24, kGcPenX = 28, kGcPenY = 32;
    void     delete_task_items(const HleTask& t);
    void     free_mem(uint32_t addr);

    // ---- guest lists (Exec-style: head / null / tailpred anchor)
    void     list_init(uint32_t l, uint32_t name_ptr);
    void     list_add_head(uint32_t l, uint32_t n);
    void     list_add_tail(uint32_t l, uint32_t n);
    uint32_t list_rem_head(uint32_t l);
    uint32_t list_rem_tail(uint32_t l);
    void     node_remove(uint32_t n);
    void     list_insert_from_tail(uint32_t l, uint32_t n);
    void     list_insert_from_head(uint32_t l, uint32_t n);
    uint32_t list_find_named(uint32_t l, const std::string& name);

    // ---- tags: calls fn(tag, arg) for each tag, following TAG_JUMP
    template <class F> void each_tag(uint32_t tags, F fn) const {
        for (int guard = 0; tags && guard < 4096; ++guard) {
            uint32_t t = r32(tags), a = r32(tags + 4);
            if (t == abi::kTagEnd) return;
            if (t == abi::kTagJump) { tags = a; continue; }
            if (t != abi::kTagNop) fn(t, a);
            tags += 8;
        }
    }
    bool tag_value(uint32_t tags, uint32_t tag, uint32_t& out) const;

    // ---- stubs: log the first few calls of anything not implemented
    void stub(const std::string& name, uint32_t result = 0);

    uint32_t kernel_base() const { return kbase_; }

    // ---- tasks (cooperative scheduler, time-sliced among equal priority)
    HleTask& cur() { return tasks_[cur_]; }
    HleTask* task_by_item(int32_t item);
    bool     multitasking() const { return live_tasks() > 1; }
    size_t   live_tasks() const;
    void     preempt();                       // called by the runtime between blocks
    uint64_t slice_start = 0;
    bool     mt = false;                      // more than one live task

    // Called once per host frame with the host pad state (PadBit mask).
    void frame(uint32_t host_pad);
    std::string status_line(uint32_t frame);     // where every task is (for the log)
    uint64_t usec() const;

private:
    void install_kernel_swis();
    void install_kernel_vectors();
    void install_file();
    void install_graphics();
    void install_other_folios();
    // ---- audio folio (hle_audio.cpp)
    void    install_audio(std::map<int32_t, HleFn>& vectors);
    void    install_audio_swis();
    int32_t create_audio_item(uint8_t type, uint32_t tags, const std::string& name);
    void    audio_deleted(HleItem& it);
    int32_t audio_new(uint8_t type, const std::string& name);
    int32_t audio_template(const std::string& name);
    int32_t audio_instrument(int32_t tmpl);
    int32_t load_sample(const std::string& path, bool scan_only, uint32_t scan_bytes);
    int32_t make_sample(uint32_t numbytes, uint32_t tags);
    void    apply_sample_tags(Audio::Sample& s, uint32_t tags);
    int32_t attach(int32_t ins, int32_t sample, int32_t envelope, const std::string& fifo, uint32_t flags, uint32_t start_at);
    int32_t make_envelope(uint32_t points, uint32_t count, int32_t sus_b, int32_t sus_e);
    static bool parse_aiff(const uint8_t* d, size_t n, Audio::Sample& s, size_t& data_off, size_t& data_len, std::string& why);
    std::map<int32_t, std::string> templates_;                       // instrument templates by item
    std::map<int32_t, std::pair<int32_t, std::string>> knobs_;      // knob item -> (instrument, knob name)
    std::map<int32_t, int32_t> cue_owner_;                          // cue item -> task that created it
    std::map<std::string, uint32_t> knob_name_strings_;
    int32_t audio_device_ = 0;
    uint32_t pixel_address(int32_t item, int32_t x, int32_t y);
    std::map<int32_t, uint32_t> vdl_buffer_;          // VDL item -> frame buffer it shows
    std::map<int32_t, int32_t>  screen_vdl_;          // screen -> VDL set on it
    std::map<int32_t, uint32_t> screen_vdl_buffer_;   // screen -> frame buffer from its VDL
    struct DiskStream { FILE* fp; uint32_t size, pos; };
    struct DirList { std::vector<std::string> entries; size_t next; };
    std::map<uint32_t, DirList> dirs_;                              // OpenDirectory*: guest Directory* -> listing
    void fill_dir_entry(const std::string& host, uint32_t index, uint8_t* e);
    std::map<uint32_t, DiskStream> streams_;                        // OpenDiskStream: guest Stream* -> host file
    uint32_t make_folio(const std::string& name, uint8_t node_subsys, uint32_t nvec,
                        const std::map<int32_t, HleFn>& impl, int32_t* item_out = nullptr);
    void set_vector(uint32_t base, int32_t index, const std::string& name, HleFn fn);

    int32_t create_item(uint32_t ctype, uint32_t tags, uint32_t size);
    int32_t do_io(int32_t ior_num, uint32_t ioinfo, bool* quick);
    void    file_io(HleItem& ior, HleItem& dev, uint32_t info);
    void    timer_io(HleItem& ior, uint32_t info);
    void    sport_io(HleItem& ior, uint32_t info);
    void    complete_io(HleItem& ior, int32_t err, uint32_t actual, int32_t task_item = 0);
    struct VblIo { int32_t ior, task, err; uint32_t actual; uint32_t due_frame = 0; };
    std::vector<VblIo> vbl_io_;                       // requests that finish later: SPORT at the next vertical blank, timers when due
    void    defer_io(HleItem& ior, uint32_t frames);
    static bool old_io();
    void    send_message(HleItem& port, HleItem& msg);
    void    wait_port(int32_t port, int32_t msg);
    void    unqueue(HleItem& msg);

    // files
    bool    resolve(const std::string& guest, std::string& host, std::string& normalized, bool& is_dir) const;
    bool    nvram_path(const std::string& guest, std::string& host) const;   // true if /NVRAM/...
    int32_t open_file(const std::string& path, const std::string& base_dir);

    // scheduling internals
    void    save_ctx(HleTask& t);
    void    load_ctx(HleTask& t);
    bool    switch_to_next(bool include_current);     // false: nothing else can run
    void    block_retry();                            // re-run this wait SWI when resumed
    void    wait_done();                              // a wait completed
    void    park_idle();
    void    wake_tasks();                             // once per frame
    void    sleep_until(uint32_t frame);              // resume after this SWI, at that frame
    void    task_exit(HleTask& t, const char* why);
    void    raise_signal(int32_t task_item, uint32_t mask);
    int32_t create_task(const std::string& name, uint32_t tags, uint8_t pri);
    void    install_tasks();
    // deques, not vectors: references to items and tasks stay valid while new
    // ones are added. With a vector, code holding one item while creating
    // another (a device, then its IOReq) read freed memory whenever the list
    // grew past its capacity (Captain Quazar crashed around item 8192).
    std::deque<HleTask> tasks_;
    size_t  cur_ = 0;
    uint32_t blocked_streak_ = 0;
    uint32_t thread_exit_trap_ = 0, idle_trap_ = 0;
    bool     io_quick_ = false;                       // current request completes synchronously
    bool     idle_ = false;                           // CPU holds no task's registers

    // NVRAM
    std::string nvram_dir_;
    int32_t create_nvram_file(const std::string& path);
    int32_t delete_nvram_file(const std::string& path);
    std::string debug_line_;
    struct Phantom { int32_t item, parent; uint32_t end_frame; };
    std::vector<Phantom> phantom_children_;
public:
    // Load another executable from the disc (LoadProgram) and run it as a
    // task of its own. Returns the task item, or a negative error with `why`.
    int32_t start_program(const std::string& cmdline, const std::string& host_path, int32_t prio, std::string& why);
    // exit()/AIF exit from the current task: ends the whole game for the
    // first program, only that task for any other.
    void    exit_current(const char* why);
private:   // programs we could not run, "exiting" soon
    // audio clock
    struct TimedSignal { int32_t task; uint32_t mask, at_ticks; };
    std::vector<TimedSignal> timed_signals_;
    std::map<int32_t, uint32_t> cue_signals_;
    uint32_t audio_ticks() const;
    uint32_t cue_signal(int32_t cue);
    void     audio_sleep_until(uint32_t ticks);

    // event broker (controller input)
    struct Listener { int32_t port = 0; uint32_t trigger = 0, capture = 0; bool fresh = true; };
    void    install_input();
    bool    broker_receive(HleItem& msg, uint32_t data, uint32_t size);   // game -> broker
    void    broker_post(Listener& l, uint32_t pad);
    bool    blocking_port(int32_t port) const;    // something can still arrive here
    uint32_t listener_signals() const;
    int32_t broker_port_ = 0;
    std::vector<Listener> listeners_;
    std::vector<int32_t> broker_free_;            // idle broker-owned event messages
    std::map<int32_t, uint32_t> broker_buf_;      // event message -> its guest buffer
    std::map<int32_t, std::pair<uint32_t, uint32_t>> read_address_;   // bitmap -> SetReadAddress(buffer, width)
    uint32_t disc_os_version_ = 0;
    int32_t  locale_item_ = 0;                    // the International folio's locale
    int32_t  broker_focus_ = 0;                   // EB_SetFocus listener
    mutable std::vector<std::string> discs_;      // disc folders (data, data_disc2, ...)
    mutable size_t disc_ = 0;                     // the disc in the drive
    const std::string& disc_root() const;
public:
    void     next_disc();
private:
    void     install_international();
    std::vector<uint16_t> read_ustr(uint32_t a, uint32_t max_chars = 4096);
    int32_t  write_ustr(uint32_t a, uint32_t bytes, const std::vector<uint16_t>& s);
    uint32_t alloc_ustr(const char* s);
    std::map<int32_t, int32_t> broker_dest_;      // event message -> listener port it was sent to
    void    drop_dead_listeners();
    uint32_t last_pad_ = 0, frame_no_ = 0;

    // graphics
    int32_t create_screen_group(uint32_t screen_items_out, uint32_t tags);
    int32_t display_screen(int32_t s0, int32_t s1);
    int32_t draw_cels(int32_t bitmap, uint32_t ccb);

    uint32_t kbase_ = 0, memhdr_dram_ = 0, memhdr_vram_ = 0, graphics_base_ = 0;
    std::deque<HleItem> items_;
    std::map<std::string, int> stub_counts_;
    std::map<uint32_t, uint32_t> free_dram_, free_vram_, allocs_;
    std::map<uint32_t, int32_t> alloc_owner_;     // allocation -> item of the task that owns it (threads: their owner)
    int32_t owning_task(const HleTask& t);        // the task a thread belongs to, or the task itself
    void free_task_memory(int32_t owner);         // everything a task owned, when it ends
    void install_math_swis();
    void load_boot_aliases();
    std::map<std::string, std::string> aliases_;   // CreateAlias: lower-case name (no '$') -> value
    std::string& cwd_ref() { static std::string none; return tasks_.empty() ? none : cur().cwd; }   // the running task's current directory
    const std::string& cwd_ref() const { static const std::string none; return tasks_.empty() ? none : tasks_[cur_].cwd; }
};

} // namespace tdort
