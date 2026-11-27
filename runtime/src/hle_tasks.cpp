#include <cstdlib>
// hle_tasks.cpp - tasks and threads on one host thread.
//
// Portfolio is a priority-based preemptive kernel. Here tasks switch only at
// well-defined points, all between guest instructions:
//   * a wait that cannot be satisfied yet (WaitSignal, WaitPort, a semaphore
//     held by someone else): the task parks ON the wait SWI, so when it runs
//     again it simply re-executes the wait and checks once more;
//   * WaitVBL-style timer delays: the task sleeps until a later frame;
//   * Yield, task exit, and a time slice expiring (equal or higher priority
//     task ready), checked by the runtime between blocks.
// When nothing can run, the frame ends; at each new frame sleeping tasks
// whose time has come and all blocked tasks become ready again.
#include "hle_os.h"

#include <algorithm>
#include <set>
#include <algorithm>
#include <fstream>
#include <iterator>
#include "tdort/aif.h"
#include <cstdio>
#include "tdort/gfx.h"

namespace tdort {

using namespace abi;

static constexpr uint64_t kSliceCycles = 25000;    // ~1/8 of a frame at 12.5 MHz
static constexpr uint32_t kTagTaskPc = 10, kTagTaskStackSize = 12, kTagTaskArgc = 13, kTagTaskArgp = 14,
                          kTagTaskSp = 15, kTagTaskBase = 16,
                          kTagTaskAllocdThreadSp = 24;   // the thread's stack was allocated: free it when the thread dies

HleTask* Portfolio::task_by_item(int32_t item) {
    for (auto& t : tasks_) if (t.item == item && t.st != HleTask::St::Dead) return &t;
    return nullptr;
}

size_t Portfolio::live_tasks() const {
    size_t n = 0;
    for (auto& t : tasks_) if (t.st != HleTask::St::Dead) ++n;
    return n;
}

void Portfolio::save_ctx(HleTask& t) {
    for (int i = 0; i < 16; ++i) t.r[i] = c.r[i];
    t.n = c.n; t.z = c.z; t.c = c.c; t.v = c.v; t.ctl = c.ctl;
}

void Portfolio::load_ctx(HleTask& t) {
    for (int i = 0; i < 16; ++i) c.r[i] = t.r[i];
    static const char* watch = std::getenv("TDORT_DUMP_SWITCH");   // a task name: log its frame on each switch-in
    if (watch && t.name == watch)
        rt.log("switch-in %s: pc=%08X sp=%08X fp=%08X [fp-12..fp-4]=%08X %08X %08X", t.name.c_str(), t.r[15], t.r[13], t.r[11],
               t.r[11] >= 12 ? r32(t.r[11] - 12) : 0, t.r[11] >= 8 ? r32(t.r[11] - 8) : 0, t.r[11] >= 4 ? r32(t.r[11] - 4) : 0);
    c.n = t.n; c.z = t.z; c.c = t.c; c.v = t.v; c.ctl = t.ctl;
    w32(kbase_ + kbase::CurrentTask, t.addr);
    slice_start = c.cycles;
}

// Pick the highest-priority ready task (round-robin among equals).
bool Portfolio::switch_to_next(bool include_current) {
    int best = -1;
    const size_t n = tasks_.size();
    for (size_t k = 1; k <= n; ++k) {
        const size_t i = (cur_ + k) % n;
        if (i == cur_ && !include_current) continue;
        const HleTask& t = tasks_[i];
        if (t.st != HleTask::St::Ready) continue;
        if (best < 0 || t.pri > tasks_[size_t(best)].pri) best = int(i);
    }
    if (best < 0) return false;
    if (!idle_) save_ctx(cur());
    idle_ = false;
    if (size_t(best) != cur_ && rt.cfg.trace)
        rt.log("switch: %s -> %s", cur().name.c_str(), tasks_[size_t(best)].name.c_str());
    cur_ = size_t(best);
    load_ctx(cur());
    return true;
}

// Nothing can run: keep every task's state and spin on the idle trap, one
// check per frame, until a task becomes ready.
void Portfolio::park_idle() {
    if (!idle_) save_ctx(cur());
    idle_ = true;
    c.r[15] = idle_trap_;
    rt.request_vblank_wait();
}

void Portfolio::block_retry() {
    HleTask& t = cur();
    if (!t.waiting) { t.waiting = true; t.blocked_since = frame_no_; }
    t.st = HleTask::St::Blocked;
    c.r[15] = rt.retry_pc;              // point back at the wait (SWI or vector trap)
    if (!switch_to_next(false)) {
        // No one else is ready: the frame ends and this wait is retried next frame.
        rt.request_vblank_wait();
    }
}

void Portfolio::wait_done() {
    cur().waiting = false;
    cur().st = HleTask::St::Ready;
}

void Portfolio::sleep_until(uint32_t frame) {
    HleTask& t = cur();
    t.st = HleTask::St::Sleeping;
    t.wake_frame = frame;
    if (!switch_to_next(false)) park_idle();
}

void Portfolio::raise_signal(int32_t task_item, uint32_t mask) {
    if (!mask) return;
    HleTask* t = task_by_item(task_item);
    if (!t) t = &cur();
    t->pending |= mask;
    w32(t->addr + task::SigBits, t->pending);
    if (t->st == HleTask::St::Blocked) t->st = HleTask::St::Ready;
}

void Portfolio::task_exit(HleTask& t, const char* why) {
    rt.log("task \"%s\" ended (%s)", t.name.c_str(), why);
    t.st = HleTask::St::Dead;
    // A task's threads end with it, as on the 3DO. Left running, they go on
    // executing code in the program's freed memory: Jurassic Park Interactive's
    // intro exited with its movie-streaming threads still alive, and they
    // crashed once the next mini-game reused that memory.
    if (owning_task(t) == t.item) {
        std::vector<int32_t> threads;
        for (const HleTask& o : tasks_)
            if (o.item != t.item && o.st != HleTask::St::Dead && owning_task(o) == t.item) threads.push_back(o.item);
        for (int32_t th : threads)
            if (HleTask* o = task_by_item(th); o && o->st != HleTask::St::Dead) task_exit(*o, "its task ended");
    }
    // Programs it started go too, unless they were given to another task: a
    // task's items (a child task is one) are deleted when it ends, and
    // SetItemOwner moves one out of that list. Furopon World's logo program
    // leaves its BGMchild behind; the title program's BGMchild then made a
    // second port of the same name and the title, finding the old one, waited
    // forever. Samurai Shodown's pselect hands its fight program to LaunchMe
    // with SetItemOwner first, so that one keeps running.
    // TDORT_OLD_CHILD_PROGRAMS=1: programs always outlive their parent.
    static const bool keep_children = std::getenv("TDORT_OLD_CHILD_PROGRAMS") != nullptr;
    if (t.program && !keep_children) {
        std::vector<int32_t> kids;
        for (const HleTask& o : tasks_) {
            if (o.item == t.item || !o.program || o.st == HleTask::St::Dead) continue;
            const HleItem* oi = item(o.item);
            if (!oi || !oi->owner_task) continue;
            const HleTask* own = task_by_item(oi->owner_task);
            if (oi->owner_task == t.item || (own && own->item != t.item && owning_task(*own) == t.item))
                kids.push_back(o.item);
        }
        for (int32_t k : kids)
            if (HleTask* o = task_by_item(k); o && o->st != HleTask::St::Dead) task_exit(*o, "the program that owned it ended");
    }
    // Free the stack the kernel owns (allocated here, or handed over with
    // CREATETASK_TAG_ALLOCDTHREADSP by the C library's CreateThread). Left
    // behind, each thread's stack fragments the heap: Slam 'n Jam restarts its
    // sound thread every attract-mode cycle and soon can't get its 890 KB buffer.
    if (t.owned_stack) {
        uint32_t s = t.owned_stack;
        if (!allocs_.count(s)) {   // the tag may hold the top of the stack rather than its base
            // ...but only a block of the stack's size is the stack. Star
            // Control II's threads free their own stacks; one stack-size below
            // the tag's value lay a 1600-byte block holding its memory
            // manager's handle table, which this freed and handed out again.
            const uint32_t size = r32(t.addr + task::StackSize);
            auto b = allocs_.find(s - size);
            static const bool loose = std::getenv("TDORT_OLD_STACK_FREE") != nullptr;
            if (b != allocs_.end() && (loose || b->second == ((size + 15) & ~15u))) s -= size; else s = 0;
        }
        if (s) free_mem(s);
        t.owned_stack = 0;
    }
    if (t.program && t.image_hi) rt.unmap_modules(t.image_lo, t.image_hi);
    if (t.program) delete_task_items(t);
    if (t.program) free_task_memory(t.item);   // a loaded program: all it owned (threads belong to their owner)
    if (HleItem* it = item(t.item)) it->alive = false;
    // The creator learns that its child (program or thread) has gone, as the
    // kernel does: by the exit message it asked for, else SIGF_DEADTASK.
    // (Threads used to send nothing; Cyberia's movie player waits for its
    // streaming threads this way and hung after the intro.)
    if (t.parent && task_by_item(t.parent)) {
        HleItem* m = t.exit_msg ? item(t.exit_msg) : nullptr;
        HleItem* port = m ? item(m->reply_port) : nullptr;
        if (m && port) {
            w32(m->addr + message::Result, uint32_t(t.exit_status));
            w32(m->addr + message::DataPtr, uint32_t(t.item));
            w32(m->addr + message::DataSize, uint32_t(cur().item));
            send_message(*port, *m);
        } else raise_signal(t.parent, 0x10 /*SIGF_DEADTASK*/);
    }
    if (HleItem* it = item(t.item)) node_remove(it->addr);
    mt = live_tasks() > 1;
    if (&t == &cur()) {
        idle_ = true;                   // its registers no longer matter
        if (!switch_to_next(false)) {
            if (live_tasks() == 0) { c.halted = true; return; }
            park_idle();
        }
    }
}

// When a program ends, the items it and its threads created go with it, as on
// the 3DO: I/O requests, ports, messages, semaphores, open files, screens and
// sound items. Folios, tasks and devices stay (a device opened by a
// short-lived player program is still needed afterwards). Captain Quazar
// cycles menu -> level -> movie player -> menu; the leftover items of each
// round fragmented memory until its 590 KB stream buffer no longer fit.
void Portfolio::delete_task_items(const HleTask& t) {
    std::set<int32_t> owners{t.item};
    for (const HleTask& o : tasks_) if (o.item != t.item && owning_task(o) == t.item) owners.insert(o.item);
    std::vector<int32_t> doomed;
    for (const HleItem& it : items_)
        if (it.alive && it.num && it.owner_task && owners.count(it.owner_task) &&
            it.kind != ItemKind::Folio && it.kind != ItemKind::Task && it.kind != ItemKind::Device &&
            it.subsys != 9 /*the locale*/)
            doomed.push_back(it.num);
    for (int32_t n : doomed) {
        HleItem* it = item(n);
        if (!it || !it->alive) continue;
        if (it->kind == ItemKind::Message && it->in_port) {        // still queued somewhere
            if (HleItem* port = item(it->in_port)) {
                auto& q = port->queue;
                q.erase(std::remove(q.begin(), q.end(), n), q.end());
            }
            it->in_port = 0;
        }
        delete_item(n);
    }
    if (!doomed.empty()) rt.log("deleted %zu items owned by task %d", doomed.size(), t.item);
}

void Portfolio::wake_tasks() {
    if (!vbl_io_.empty()) {
        std::vector<VblIo> done, later;
        for (const VblIo& v : vbl_io_) (int32_t(frame_no_ - v.due_frame) >= 0 ? done : later).push_back(v);
        vbl_io_.swap(later);
        const bool q = io_quick_;
        io_quick_ = false;
        for (const VblIo& v : done)
            if (HleItem* ior = item(v.ior)) {
                if (std::getenv("TDORT_DEBUG_IO")) rt.log("complete deferred IO %d (reply port %d, msg %d) for task %d", v.ior, ior->reply_port, ior->msg_item, v.task);
                complete_io(*ior, v.err, v.actual, v.task);
            }
        io_quick_ = q;
    }
    for (auto& t : tasks_) {
        if (t.st == HleTask::St::Sleeping && int32_t(frame_no_ - t.wake_frame) >= 0) t.st = HleTask::St::Ready;
        else if (t.st == HleTask::St::Blocked) t.st = HleTask::St::Ready;   // retry its wait once per frame
    }
    // If the CPU is parked, or running a task that is waiting, prefer a ready one.
    if (idle_) switch_to_next(true);
}

void Portfolio::preempt() {
    if (idle_) return;
    const HleTask& me = cur();
    bool higher = false, equal = false;
    for (size_t i = 0; i < tasks_.size(); ++i) {
        if (i == cur_ || tasks_[i].st != HleTask::St::Ready) continue;
        if (tasks_[i].pri > me.pri) higher = true;
        else if (tasks_[i].pri == me.pri) equal = true;
    }
    if (higher || (equal && c.cycles - slice_start > kSliceCycles)) switch_to_next(false);
    else if (c.cycles - slice_start > kSliceCycles) slice_start = c.cycles;
}

int32_t Portfolio::create_task(const std::string& name, uint32_t tags, uint8_t pri, bool module) {
    uint32_t pc = 0, stack_size = 0, argc = 0, argp = 0, sp = 0, base = c.r[9], owned_stack = 0;
    int32_t exit_msg = 0;
    bool has_pc = false;
    uint32_t aif = 0, image_size = 0, cmdstr = 0;
    each_tag(tags, [&](uint32_t t, uint32_t a) {
        if (rt.cfg.trace) rt.log("CreateTask \"%s\" tag %u = %08X", name.c_str(), t, a);
        switch (t) {
        case kTagTaskPc: pc = a; has_pc = true; break;
        case kTagTaskStackSize: stack_size = a; break;
        case kTagTaskArgc: argc = a; break;
        case kTagTaskArgp: argp = a; break;
        case kTagTaskSp: sp = a; break;
        case kTagTaskBase: base = a; break;
        case kTagTaskAllocdThreadSp: owned_stack = a; break;
        case 25 /* CREATETASK_TAG_MSGFROMCHILD */: exit_msg = int32_t(a); break;
        case 18 /* CREATETASK_TAG_IMAGESZ */: image_size = a; break;
        case 19 /* CREATETASK_TAG_AIF */: aif = a; break;
        case 20 /* CREATETASK_TAG_CMDSTR */: cmdstr = a; break;
        default: break;
        }
    });
    // A program the caller loaded itself (CREATETASK_TAG_AIF, the image's size
    // and a command line): it runs where it is, as a task of its own, like
    // LoadProgram. Sangokushi IV starts "open.x" this way.
    // TDORT_OLD_TASK_AIF=1: refused.
    if (!has_pc && aif && image_size && !std::getenv("TDORT_OLD_TASK_AIF")) {
        if (!guest_range_ok(aif, image_size) || (aif & 3)) return kErrBadPtr;
        std::vector<uint8_t> bytes(image_size);
        for (uint32_t k = 0; k < image_size; ++k) bytes[k] = r8(aif + k);
        std::string cmd = cmdstr ? rstr(cmdstr) : name;
        if (cmd.empty()) cmd = name;
        std::string why;
        const int32_t t = start_program_image(cmd, bytes, pri, why, aif);
        if (t < 0) { rt.log("CreateItem(Task \"%s\") from an AIF at %08X: %s", name.c_str(), aif, why.c_str()); return t; }
        if (HleTask* nt = task_by_item(t)) nt->exit_msg = exit_msg;
        return t;
    }
    if (!has_pc) {
        rt.log("CreateItem(Task \"%s\"): no PC tag and no program image", name.c_str());
        return kErrNotSupported;
    }
    if (!stack_size) stack_size = 4096;
    // CREATETASK_TAG_ALLOCDTHREADSP names the thread's stack, for the kernel to
    // free when the thread dies. Star Control II (an older SDK) passes other
    // values with tag 24: code addresses, and once its memory manager's handle
    // table, which was then freed while still in use. Only a value inside the
    // thread's stack is taken as the stack. TDORT_OLD_STACK_FREE=1: any value.
    if (owned_stack && sp && std::getenv("TDORT_OLD_STACK_FREE") == nullptr &&
        !(owned_stack >= sp - stack_size && owned_stack <= sp)) {
        if (rt.cfg.trace) rt.log("CreateTask \"%s\": tag 24 value %08X is not its stack (%08X-%08X), ignored", name.c_str(), owned_stack, sp - stack_size, sp);
        owned_stack = 0;
    }
    if (!sp) {
        // Room below the stack: the real kernel grows a thread's stack into
        // free memory beneath it when the thread runs past its limit
        // (Super Street Fighter II Turbo starts a thread with 256 bytes).
        const uint32_t reserve = stack_size < 2048 ? 8192 - stack_size : 0;   // only for very small stacks
        uint32_t block = alloc(stack_size + reserve, kMemFromTop);   // keep OS memory out of the game's heap
        uint32_t stack = block + reserve;
        if (!block) { block = alloc(stack_size, kMemFromTop); stack = block; }
        if (!block) return kErrNoMem;
        owned_stack = block;
        sp = stack + stack_size;
    }
    HleItem& it = new_item(ItemKind::Task, kSubsysKernel, kNodeTask, task::Size, name);
    w8(it.addr + node::Priority, pri);
    const uint32_t stack_lo = sp - stack_size;
    w32(it.addr + task::StackBase, stack_lo);
    w32(it.addr + task::StackSize, stack_size);
    w32(it.addr + task::FreeMemoryLists, r32(tasks_[0].addr + task::FreeMemoryLists));   // threads share memory
    // A thread belongs to its creator's task (its owner, if the creator is a
    // thread too). Games reach the owner's item through it: Escape from Monster
    // Manor's sound spooler signals CurrentTask->t_ThreadTask->n_Item when it
    // pauses the music, and with 0 here it signalled garbage and the game hung.
    {
        const uint32_t owner = r32(cur().addr + task::ThreadTask);
        w32(it.addr + task::ThreadTask, owner ? owner : cur().addr);
    }
    w32(it.addr + task::AllocatedSigs, 0xFF);
    list_add_tail(r32(kbase_ + kbase::Tasks), it.addr);

    HleTask t;
    t.item = it.num; t.addr = it.addr; t.pri = pri; t.name = name;
    t.owned_stack = owned_stack;
    t.parent = cur().item;           // the creator gets SIGF_DEADTASK (or exit_msg) when this ends
    t.exit_msg = exit_msg;
    t.r[0] = argc; t.r[1] = argp;
    if (module && !old_exec_thread()) {   // as ExecuteAsSubroutine enters a module
        t.r[0] = pc; t.r[1] = argc; t.r[2] = argp;
        t.r[kRegArgc] = argc; t.r[kRegArgv] = argp; t.r[kRegKernelBase] = kbase_;
    }
    t.r[9] = base;
    t.r[10] = stack_lo + 128;          // APCS stack limit, as the kernel sets it
    t.r[13] = sp & ~3u;
    t.r[14] = thread_exit_trap_;       // returning from the thread function ends it
    t.r[15] = pc & ~3u;
    t.cwd = cur().cwd;              // a new task starts in its creator's directory
    tasks_.push_back(t);
    mt = true;
    rt.log("task \"%s\" created: item %d pc=%08X sp=%08X stack=%u pri=%u", name.c_str(), it.num, pc, sp, stack_size, pri);
    return it.num;
}

void Portfolio::exit_current(const char* why) {
    if (cur_ == 0 || idle_) { rt.log("%s from the main program: stopping", why); c.halted = true; return; }
    task_exit(cur(), why);
}

int32_t Portfolio::start_program(const std::string& cmdline, const std::string& host_path, int32_t prio,
                                 std::string& why) {
    std::ifstream f(host_path, std::ios::binary);
    if (!f) { why = "cannot open " + host_path; return kErrNotFound; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return start_program_image(cmdline, file, prio, why, 0);
}

// in_place: the image is already in guest memory at that address (CreateItem
// with CREATETASK_TAG_AIF: the caller loaded it), and runs there.
int32_t Portfolio::start_program_image(const std::string& cmdline, const std::vector<uint8_t>& file, int32_t prio,
                                       std::string& why, uint32_t in_place) {
    AifImage img;
    if (!parse_aif(file, img, why)) return kErrNotSupported;
    if (!img.self_relocating && img.image_base == 0) { why = "not relocatable (linked at address 0)"; return kErrNotSupported; }

    // Code + data + zero-init area, or the whole file (the relocation table
    // follows the image), whichever is larger. Taken from the top of memory,
    // as the real loader does, to keep the game's own heap in one piece.
    uint64_t need = std::max<uint64_t>(img.load_size(), uint64_t(img.ro_size) + img.rw_size + img.zi_size) + 64;
    uint32_t base = in_place ? in_place : img.self_relocating ? alloc(uint32_t(need), kMemFromTop) : img.image_base;
    if (!base) { why = "out of memory for " + std::to_string(need) + " bytes"; return kErrNoMem; }
    if (in_place && !img.self_relocating && img.image_base != in_place) { why = "not relocatable"; return kErrNotSupported; }
    if (!in_place) rt.mem.write_block(base, img.bytes.data(), img.load_size());

    // Arguments: argv[0] is the program path, then the rest of the command line.
    std::vector<std::string> args;
    { size_t i = 0; while (i < cmdline.size()) {
        while (i < cmdline.size() && cmdline[i] == ' ') ++i;
        size_t j = cmdline.find(' ', i); if (j == std::string::npos) j = cmdline.size();
        if (j > i) args.push_back(cmdline.substr(i, j - i));
        i = j; } }
    const uint32_t argv = alloc(uint32_t(4 * (args.size() + 1)), kMemFromTop);
    for (size_t k = 0; k < args.size(); ++k) w32(argv + uint32_t(4 * k), guest_string(args[k]));
    w32(argv + uint32_t(4 * args.size()), 0);

    const uint32_t stack_size = 16 * 1024;
    const uint32_t stack = alloc(stack_size, kMemFromTop);
    if (!stack) { why = "out of memory for the stack"; return kErrNoMem; }

    std::string name = args.empty() ? "program" : args[0];
    if (size_t s = name.find_last_of('/'); s != std::string::npos) name = name.substr(s + 1);
    const uint8_t pri = uint8_t(prio <= 0 ? 100 : std::min(prio, 254));
    HleItem& it = new_item(ItemKind::Task, kSubsysKernel, kNodeTask, task::Size, name);
    w8(it.addr + node::Priority, pri);
    w32(it.addr + task::StackBase, stack);
    w32(it.addr + task::StackSize, stack_size);
    w32(it.addr + task::FreeMemoryLists, r32(tasks_[0].addr + task::FreeMemoryLists));
    w32(it.addr + task::AllocatedSigs, 0xFF);
    list_add_tail(r32(kbase_ + kbase::Tasks), it.addr);

    // Same start-up state as the main program (see the constructor); the
    // AIF header code runs first and relocates the image to where it is.
    HleTask t;
    t.item = it.num; t.addr = it.addr; t.pri = pri; t.name = name;
    t.parent = cur().item; t.program = true;
    t.r[kRegArgc] = uint32_t(args.size());
    t.r[4] = uint32_t(args.size());    // as for the main program (see the Portfolio constructor)
    t.r[kRegArgv] = argv;
    t.r[kRegKernelBase] = kbase_;
    t.r[10] = stack + 512;
    t.r[13] = stack + stack_size - 16;
    t.r[14] = thread_exit_trap_;
    t.r[15] = base;
    t.cwd = cur().cwd;              // a new task starts in its creator's directory
    tasks_.push_back(t);
    for (uint32_t a : {base, argv, stack})
        if (allocs_.count(a) && !(in_place && a == base)) alloc_owner_[a] = it.num;   // an in-place image stays its loader's
    for (size_t k = 0; k < args.size(); ++k) if (uint32_t sa = r32(argv + uint32_t(4 * k))) if (allocs_.count(sa)) alloc_owner_[sa] = it.num;
    mt = true;
    const bool translated = rt.map_module(fnv1a64_hex(file), base, img.load_size(), base + (img.entry - img.image_base), name);
    tasks_.back().image_lo = base; tasks_.back().image_hi = base + uint32_t(need);
    rt.log("program \"%s\" loaded at %08X-%08X (%u bytes, entry %08X), task item %d, pri %u, %s",
           cmdline.c_str(), base, base + uint32_t(need), img.file_size, base + (img.entry - img.image_base), it.num, pri,
           translated ? "translated" : "interpreted (not translated by the converter)");
    return it.num;
}

std::string Portfolio::status_line(uint32_t frame) {
    static const char* st[] = {"ready", "blocked", "sleeping", "dead"};
    std::string out = "frame " + std::to_string(frame) + ":";
    size_t dead = 0;
    for (size_t i = 0; i < tasks_.size(); ++i) {
        const HleTask& t = tasks_[i];
        // ended tasks are only counted: a game that starts many threads made
        // this line long enough to lose the counters at its end
        if (t.st == HleTask::St::Dead) { ++dead; continue; }
        uint32_t pc = (i == cur_ && !idle_) ? c.r[15] : t.r[15];
        char b[128];
        std::snprintf(b, sizeof b, " [%s %s pc=%08X sig=%08X wait=%08X", t.name.c_str(), st[int(t.st)], pc, t.pending,
                      t.waiting ? ((i == cur_ && !idle_) ? c.r[0] : t.r[0]) : 0u);
        out += b;
        // APCS backtrace: fp points at the saved pc; lr is at fp-4, the caller's fp at fp-12.
        uint32_t fp = (i == cur_ && !idle_) ? c.r[11] : t.r[11];
        for (int depth = 0; depth < 6 && fp >= 12 && fp < kRamEnd - 3; ++depth) {
            uint32_t lr = r32(fp - 4), next = r32(fp - 12);
            std::snprintf(b, sizeof b, " <%08X", lr);
            out += b;
            if (next <= fp) break;
            fp = next;
        }
        out += "]";
    }
    char b[96];
    if (dead) { std::snprintf(b, sizeof b, " (+%zu ended)", dead); out += b; }
    std::snprintf(b, sizeof b, " cels=%llu pixels=%llu display=%08X", (unsigned long long)rt.gfx->cels_drawn,
                  (unsigned long long)rt.gfx->pixels_written, rt.gfx->display_buffer());
    out += b;
    {   // memory: free total / largest free block, main and video RAM
        auto stats = [](const std::map<uint32_t, uint32_t>& fl, uint32_t& tot, uint32_t& big) {
            tot = big = 0; for (auto& [a, n] : fl) { tot += n; big = std::max(big, n); } };
        uint32_t dt, db, vt, vb; stats(free_dram_, dt, db); stats(free_vram_, vt, vb);
        std::snprintf(b, sizeof b, " mem=%uK/%uK vram=%uK/%uK", dt / 1024, db / 1024, vt / 1024, vb / 1024);
        out += b;
    }
    std::snprintf(b, sizeof b, " audio_cues=%llu", (unsigned long long)rt.audio->cues_fired);
    if (std::getenv("TDORT_AUDIO_STATUS")) { out += b; out += " voices:" + rt.audio->describe(); b[0] = 0; }
    return out + b;
}

void Portfolio::install_tasks() {
    thread_exit_trap_ = rt.register_trap("thread exit", [this](Cpu*) { task_exit(cur(), "returned"); });
    idle_trap_ = rt.register_trap("idle", [this](Cpu*) {
        if (!switch_to_next(true)) { c.r[15] = idle_trap_; rt.request_vblank_wait(); }
    });
}

} // namespace tdort
