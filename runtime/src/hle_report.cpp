// hle_report.cpp - tdort_report.txt: why a game stopped, stalled or crashed.
//
// Written when a run ends (window closed, frame limit, exit or crash). It
// holds a one-line VERDICT the batch tester copies into its summary, then the
// evidence: every thread's state, what it is waiting on and its last OS
// calls, unread messages, memory, and the problems the log reported. Most
// stalls can be explained from it without the game's disc.
#include "hle_os.h"
#include "tdort/gfx.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace tdort {

void Portfolio::note_call(const char* name, const uint32_t* r, uint32_t site) {
    if (cur_ >= tasks_.size()) return;
    // reading a controller listener's port counts as reading input
    if (std::strcmp(name, "GetMsg") == 0 || std::strcmp(name, "GetThisMsg") == 0 || std::strcmp(name, "WaitPort") == 0)
        for (const Listener& l : listeners_) if (int32_t(r[0]) == l.port) { last_input_frame_ = frame_no_; break; }
    auto& ring = tasks_[cur_].calls;
    if (!ring.empty()) {
        CallRec& last = ring.back();
        if (last.site == site && std::strncmp(last.name, name, sizeof last.name - 1) == 0) {
            if (tasks_[cur_].waiting) return;      // a blocked wait being retried: still the same call
            ++last.count;
            std::memcpy(last.a, r, sizeof last.a);
            last.frame = frame_no_;
            return;
        }
    }
    CallRec rec;
    std::snprintf(rec.name, sizeof rec.name, "%s", name);
    std::memcpy(rec.a, r, sizeof rec.a);
    rec.site = site; rec.frame = frame_no_; rec.count = 1;
    ring.push_back(rec);
    if (ring.size() > 10) ring.pop_front();
}

static std::string call_text(const CallRec& c) {
    char b[160];
    std::snprintf(b, sizeof b, "%s(%08X, %08X, %08X, %08X) at %08X, frame %u%s", c.name, c.a[0], c.a[1], c.a[2], c.a[3],
                  c.site, c.frame, c.count > 1 ? (" (x" + std::to_string(c.count) + ")").c_str() : "");
    return b;
}

// What a waiting thread waits for, in words: its message ports and cues that
// own a signal in its wait mask.
std::string Portfolio::describe_wait(const HleTask& t, uint32_t mask) {
    std::string out;
    // WaitPort(port, message): its first argument is a port, not a signal mask
    if (!t.calls.empty() && std::strcmp(t.calls.back().name, "WaitPort") == 0) {
        const int32_t port = int32_t(t.calls.back().a[0]), msg = int32_t(t.calls.back().a[1]);
        HleItem* p = item(port);
        char b[128];
        std::snprintf(b, sizeof b, "%s on port %d (%zu unread)", msg ? ("the reply to message " + std::to_string(msg)).c_str() : "a message",
                      port, p ? p->queue.size() : size_t(0));
        out = b;
        if (msg) {   // where that message is now
            if (HleItem* m = item(msg)) {
                if (m->in_port) out += ", which sits unread on port " + std::to_string(m->in_port);
                else out += ", which another thread has taken and not yet replied to";
            }
        }
        return out;
    }
    for (const HleItem& it : items_) {
        if (!it.alive || it.kind != ItemKind::MsgPort || it.owner_task != t.item || !(it.signal & mask)) continue;
        char b[96];
        std::snprintf(b, sizeof b, "%sport %d%s%s (%zu unread)", out.empty() ? "" : ", ", it.num,
                      it.name.empty() ? "" : " ", it.name.c_str(), it.queue.size());
        out += b;
    }
    for (auto& [cue, sig] : cue_signals_) {
        if (!(sig & mask)) continue;
        out += (out.empty() ? "" : ", ") + std::string("audio cue ") + std::to_string(cue);
    }
    if (mask & 0x40000000u) out += out.empty() ? "a timer or I/O completion" : ", a timer or I/O completion";
    uint32_t claimed = 0x40000000u;
    for (const HleItem& it : items_) if (it.alive && it.kind == ItemKind::MsgPort && it.owner_task == t.item) claimed |= it.signal;
    for (auto& [cue, sig] : cue_signals_) claimed |= sig;
    if (const uint32_t rest = mask & ~claimed) {
        char b[80];
        std::snprintf(b, sizeof b, "%ssignal %08X from another thread", out.empty() ? "" : ", ", rest);
        out += b;
    }
    return out.empty() ? "nothing (empty signal mask)" : out;
}

std::string Portfolio::write_report(const std::string& ended, uint32_t screen_change_frame) {
    static const char* st[] = {"ready", "blocked", "sleeping", "dead"};
    const uint32_t frame = frame_no_;
    std::ostringstream ev;

    // ---- threads
    std::vector<const HleTask*> live;
    for (const HleTask& t : tasks_) if (t.st != HleTask::St::Dead) live.push_back(&t);
    size_t dead = tasks_.size() - live.size();
    ev << "Threads (" << live.size() << " running";
    if (dead) ev << ", " << dead << " ended";
    ev << "):\n";
    auto last_call = [](const HleTask& t) { return t.calls.empty() ? std::string("no OS calls") : call_text(t.calls.back()); };
    for (const HleTask* tp : live) {
        const HleTask& t = *tp;
        const size_t idx = size_t(tp - &tasks_[0]);
        const uint32_t pc = (idx == cur_ && !idle_) ? c.r[15] : t.r[15];
        char b[200];
        std::snprintf(b, sizeof b, "  %s (item %d, priority %u): %s at pc %08X", t.name.c_str(), t.item, t.pri, st[int(t.st)], pc);
        ev << b;
        if (t.st == HleTask::St::Blocked) {
            const uint32_t mask = t.r[0];
            ev << ", waiting since frame " << t.blocked_since << " for " << describe_wait(t, mask);
            std::snprintf(b, sizeof b, " (signal mask %08X, pending %08X)", mask, t.pending);
            ev << b;
        } else if (t.st == HleTask::St::Sleeping) {
            ev << ", sleeping until frame " << t.wake_frame;
        }
        ev << "\n";
        for (const CallRec& cr : t.calls) ev << "      " << call_text(cr) << "\n";
    }

    // ---- unread messages
    bool any = false;
    for (const HleItem& it : items_) {
        if (!it.alive || it.kind != ItemKind::MsgPort || it.queue.empty()) continue;
        if (!any) { ev << "\nMessage ports with unread messages:\n"; any = true; }
        std::string owner = "?";
        for (const HleTask& t : tasks_) if (t.item == it.owner_task) owner = t.name + (t.st == HleTask::St::Dead ? " (ended)" : "");
        ev << "  port " << it.num << (it.name.empty() ? "" : " " + it.name) << " owned by " << owner << ": " << it.queue.size() << " unread\n";
    }

    // ---- disc reads
    if (!recent_reads_.empty()) {
        ev << "\nLast disc reads:\n";
        for (const ReadRec& r : recent_reads_) {
            char b[200];
            std::snprintf(b, sizeof b, "  frame %u: %s block %u, %u bytes -> %08X-%08X", r.frame, r.path.c_str(), r.block, r.bytes, r.dest, r.dest + r.bytes);
            ev << b << "\n";
        }
    }

    // ---- memory
    auto stats = [](const std::map<uint32_t, uint32_t>& fl, uint32_t& tot, uint32_t& big) {
        tot = big = 0; for (auto& [a, n] : fl) { tot += n; big = std::max(big, n); } };
    uint32_t dt, db, vt, vb; stats(free_dram_, dt, db); stats(free_vram_, vt, vb);
    ev << "\nMemory free: main " << dt / 1024 << " KB (largest block " << db / 1024 << " KB), video "
       << vt / 1024 << " KB (largest block " << vb / 1024 << " KB)\n";

    // ---- problems the log reported
    if (!rt.problems.empty()) {
        ev << "\nProblems reported in tdort.log (times seen, first frame):\n";
        for (auto& p : rt.problems) ev << "  " << p.second.first << "x  frame " << p.second.second << "  " << p.first << "\n";
    }
    ev << "\nPicture last changed at frame " << screen_change_frame << ". Cels drawn: " << rt.gfx->cels_drawn << ".\n";

    // ---- verdict
    std::string verdict;
    if (rt.crashed) {
        verdict = "CRASH: " + rt.crash_reason + " (thread " + (cur_ < tasks_.size() ? tasks_[cur_].name : "?") + ")";
    } else if (ended == "exited") {
        verdict = "EXITED: the game ended itself with code " + std::to_string(c.r[0]);
    } else {
        const bool frozen = frame >= screen_change_frame + 300;
        bool all_blocked = !live.empty();
        for (const HleTask* t : live) if (t->st != HleTask::St::Blocked) all_blocked = false;
        if (!frozen) {
            verdict = "RUNNING: the picture was still changing at the end";
        } else if (all_blocked && timed_signals_.empty() && vbl_io_.empty()) {
            verdict = "DEADLOCK: every thread is waiting and nothing will wake them:";
            for (const HleTask* t : live) verdict += " " + t->name + " in " + (t->calls.empty() ? "?" : t->calls.back().name) + ";";
        } else {
            // Threads waiting 5 seconds or more. Helper threads (sound, loaders)
            // often idle like that; a program's main thread rarely does, so
            // it is named first.
            std::vector<const HleTask*> waiting;
            for (const HleTask* t : live)
                if (t->st == HleTask::St::Blocked && frame >= t->blocked_since + 300) waiting.push_back(t);
            std::stable_sort(waiting.begin(), waiting.end(), [](const HleTask* a, const HleTask* b) {
                if (a->program != b->program) return a->program;
                return a->blocked_since < b->blocked_since;
            });
            // only a stuck main (program) thread is blamed; helpers are listed
            const HleTask* worst = (!waiting.empty() && waiting.front()->program) ? waiting.front() : nullptr;
            const bool input = frame < last_input_frame_ + 300 && last_input_frame_ > 0;
            verdict = input ? "WAITING FOR INPUT?: picture unchanged since frame " + std::to_string(screen_change_frame) +
                                  ", but the game read the controller at frame " + std::to_string(last_input_frame_)
                            : "STALLED: picture unchanged since frame " + std::to_string(screen_change_frame);
            if (worst && !input) {
                verdict += "; " + worst->name + " has waited since frame " + std::to_string(worst->blocked_since) + " in " +
                           last_call(*worst) + " for " + describe_wait(*worst, worst->r[0]);
                if (waiting.size() > 1) {
                    verdict += " (also waiting:";
                    for (size_t i = 1; i < waiting.size() && i < 5; ++i) verdict += " " + waiting[i]->name;
                    verdict += ")";
                }
            }
            if (input) worst = live.front();          // skip the busy-thread note below
            else {
                const HleTask* busy = nullptr;
                for (const HleTask* t : live) if (t->st == HleTask::St::Ready && (!busy || t->pri > busy->pri)) busy = t;
                if (busy) verdict += "; the main program is not stuck waiting, " + busy->name + " keeps running (last call " + last_call(*busy) + ")";
                // what the long-waiting threads wait for, oldest wait first
                std::vector<const HleTask*> byage = waiting;
                std::stable_sort(byage.begin(), byage.end(), [](const HleTask* a, const HleTask* b) { return a->blocked_since < b->blocked_since; });
                for (size_t i = 0; i < byage.size() && i < 3; ++i) {
                    const HleTask* t = byage[i];
                    verdict += "; " + t->name + " waits since frame " + std::to_string(t->blocked_since) + " in " +
                               (t->calls.empty() ? std::string("?") : std::string(t->calls.back().name)) + " for " + describe_wait(*t, t->r[0]);
                }
                if (byage.size() > 3) verdict += "; " + std::to_string(byage.size() - 3) + " more threads waiting";
            }
        }
        // the problems most likely to matter, in short
        std::string notes;
        for (auto& p : rt.problems) {
            if (p.first.find("not found") == std::string::npos && p.first.find("out of memory") == std::string::npos &&
                p.first.find("unimplemented") == std::string::npos && p.first.find("not supported") == std::string::npos) continue;
            if (notes.size() > 200) { notes += " ..."; break; }
            notes += (notes.empty() ? "" : " | ") + p.first.substr(0, 90);
        }
        if (!notes.empty() && verdict.rfind("RUNNING", 0) != 0) verdict += ". Log problems: " + notes;
    }

    std::FILE* f = std::fopen("tdort_report.txt", "w");
    if (f) {
        std::fprintf(f, "3DO to PC diagnosis report\nEnded: %s at frame %u\nVERDICT: %s\n\n%s", ended.c_str(), frame,
                     verdict.c_str(), ev.str().c_str());
        std::fclose(f);
    }
    return verdict;
}

}  // namespace tdort
