#include <deque>
#include <iterator>
#include <sstream>
// hle_file.cpp - File folio replacement. Serves the extracted disc (data/)
// through OpenDiskFile + IOReq/DoIO with block-based CMD_READ, CMD_STATUS,
// FILECMD_READDIR and FILECMD_GETPATH.
//
// Paths: "/cd-rom/x", "/<anything>/x" and "$boot/x" all name the disc root;
// relative paths resolve against the current directory. Matching is
// case-insensitive, as on the 3DO.
#include "hle_os.h"
#include "tdort/aif.h"
#include "tdort/host_name.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace tdort {

using namespace abi;

static constexpr uint32_t kBlock = 2048;
// Stream structure (filestream.h) fields kept up to date for the guest.
static constexpr uint32_t kStSize = 56, kStBlockSize = 28, kStBlockCount = 32, kStFileLength = 36, kStCursor = 40;

static std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : p) {
        if (ch == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

bool Portfolio::resolve(const std::string& guest, std::string& host, std::string& normalized, bool& is_dir) const {
    // Path parsing as the file folio does it: "$name" may begin any component
    // and is replaced by the alias's value; a value starting with "/" (as
    // $boot's, "/cd-rom") goes back to the root, as does an empty component
    // ("//"). After the root comes the filesystem name (cd-rom, a volume
    // label), which is skipped. So "$boot/HEGAMES/$boot/HEGAMES/puttmoon.he0"
    // is /cd-rom/HEGAMES/puttmoon.he0 (Putt-Putt Goes to the Moon asks for it).
    // CreateAlias defines aliases; "$boot" (the volume the game booted from)
    // and "$progdir" (the running program's directory, the disc's root for a
    // game) are always there.
    auto alias_value = [&](const std::string& name, std::string& value) {
        std::string key = name;
        for (auto& ch : key) ch = char(std::tolower((unsigned char)ch));
        if (auto it = aliases_.find(key); it != aliases_.end()) { value = it->second; return true; }
        if (key == "boot" || key == "progdir") { value = "/cd-rom"; return true; }
        return false;
    };
    std::vector<std::string> clean;
    std::deque<std::string> todo;
    bool skip_fs_name = false;
    auto queue = [&](const std::string& text, bool front) {
        // components of text, with "" for the root ("/x" -> "", "x")
        std::vector<std::string> comps;
        size_t i = 0;
        if (!text.empty() && text[0] == '/') { comps.push_back(""); i = 1; }
        while (i <= text.size()) {
            size_t j = text.find('/', i);
            if (j == std::string::npos) j = text.size();
            std::string c = text.substr(i, j - i);
            if (!c.empty()) comps.push_back(c);
            else if (j < text.size() && i > 0) comps.push_back("");   // "//": back to the root
            i = j + 1;
        }
        if (front) todo.insert(todo.begin(), comps.begin(), comps.end());
        else todo.insert(todo.end(), comps.begin(), comps.end());
    };
    if (guest.empty() || (guest[0] != '/' && guest[0] != '$'))
        for (auto& c : split(cwd_ref())) clean.push_back(c);
    queue(guest, false);
    for (int expansions = 0; !todo.empty();) {
        std::string c = todo.front();
        todo.pop_front();
        if (c.empty()) { clean.clear(); skip_fs_name = true; continue; }
        if (c[0] == '$') {
            std::string value;
            if (++expansions > 32 || !alias_value(c.substr(1), value)) return false;
            queue(value, true);
            continue;
        }
        if (skip_fs_name) { skip_fs_name = false; continue; }
        if (c == ".") continue;
        if (c == ".." || c == "^") { if (!clean.empty()) clean.pop_back(); continue; }
        clean.push_back(c);
    }
    // case-insensitive walk of the host tree, on the disc in the drive
    std::error_code ec;
    auto walk = [&](const fs::path& root, fs::path& found, std::string& norm) {
        fs::path cur = root;
        norm.clear();
        for (auto& comp : clean) {
            fs::path next;
            const std::string stored = encode_host_name(comp);
            if (fs::exists(cur / fs::u8path(stored), ec)) next = cur / fs::u8path(stored);
            else {
                for (auto& de : fs::directory_iterator(cur, ec)) {
                    const std::string n = de.path().filename().u8string();
                    if (ieq(decode_host_name(n), comp) || ieq(n, comp)) { next = de.path(); break; }
                }
            }
            if (next.empty()) return false;
            norm += "/" + decode_host_name(next.filename().u8string());
            cur = next;
        }
        if (norm.empty()) norm = "/";
        found = cur;
        return fs::exists(cur, ec);
    };
    fs::path cur;
    bool ok = walk(fs::u8path(disc_root()), cur, normalized);
    // A multi-disc game asking for a file that only another disc has: that
    // disc goes in, as when the player swaps discs at the game's prompt.
    if (!ok && !clean.empty())
        for (size_t k = 0; k < discs_.size() && !ok; ++k) {
            if (k == disc_) continue;
            std::string norm;
            if (walk(fs::u8path(discs_[k]), cur, norm)) {
                ok = true;
                normalized = norm;
                disc_ = k;
                rt.log("disc %zu is now in the drive (the game asked for %s)", k + 1, guest.c_str());
            }
        }
    if (!ok) return false;
    is_dir = fs::is_directory(cur, ec);
    host = cur.u8string();
    return true;
}

// F2 in the game window: the next disc goes in (after the last, disc 1 again).
void Portfolio::next_disc() {
    disc_root();
    if (discs_.size() < 2) return;
    disc_ = (disc_ + 1) % discs_.size();
    rt.log("disc %zu is now in the drive (F2)", disc_ + 1);
}

// The disc in the drive: data for disc 1, data_disc2, data_disc3, ... beside it.
const std::string& Portfolio::disc_root() const {
    if (discs_.empty()) {
        discs_.push_back(rt.cfg.data_dir);
        const fs::path base = fs::u8path(rt.cfg.data_dir);
        std::error_code ec;
        for (int k = 2; k <= 9; ++k) {
            const fs::path d = base.parent_path() / fs::u8path(base.filename().u8string() + "_disc" + std::to_string(k));
            if (!fs::is_directory(d, ec)) break;
            discs_.push_back(d.u8string());
        }
        if (discs_.size() > 1) rt.log("multi-disc game: %zu discs", discs_.size());
    }
    return discs_[disc_];
}

// A DirectoryEntry for a host file or folder (what FILECMD_READDIR and
// ReadDirectory return).
void Portfolio::fill_dir_entry(const std::string& host, uint32_t index, uint8_t* e) {
    std::memset(e, 0, direntry::Size);
    std::error_code ec;
    const fs::path p = fs::u8path(host);
    auto put = [&](uint32_t off, uint32_t v) { store_be32(e + off, v); };
    const bool dir = fs::is_directory(p, ec);
    const uint64_t bytes = dir ? 0 : fs::file_size(p, ec);
    put(direntry::Flags, dir ? 0x07 : 0x02);
    put(direntry::UniqueId, index);
    std::memcpy(e + direntry::Type, dir ? "*dir" : "    ", 4);
    put(direntry::BlockSize, kBlock);
    put(direntry::ByteCount, uint32_t(bytes));
    put(direntry::BlockCount, uint32_t((bytes + kBlock - 1) / kBlock));
    put(direntry::Burst, 1);
    put(direntry::AvatarCount, 1);
    const std::string nm = decode_host_name(p.filename().u8string()).substr(0, direntry::FileNameLen - 1);
    std::memcpy(e + direntry::FileName, nm.data(), nm.size());
}

// NVRAM (the 3DO's battery-backed save memory) is a writable folder "nvram"
// next to the game's data folder. Files there use 1-byte blocks.
bool Portfolio::nvram_path(const std::string& guest, std::string& host) const {
    std::string p = guest;
    if (p.size() < 7 || !ieq(p.substr(0, 7), "/nvram/")) return false;
    std::string name = p.substr(7);
    for (auto& ch : name) if (ch == '/') ch = '_';
    name = encode_host_name(name);
    std::error_code ec;
    fs::path dir = fs::u8path(rt.cfg.data_dir).parent_path() / "nvram";
    if (fs::u8path(rt.cfg.data_dir).parent_path().empty()) dir = fs::u8path("nvram");
    fs::create_directories(dir, ec);
    // case-insensitive match against existing files
    for (auto& de : fs::directory_iterator(dir, ec))
        if (ieq(de.path().filename().u8string(), name)) { host = de.path().u8string(); return true; }
    host = (dir / fs::u8path(name)).u8string();
    return true;
}

int32_t Portfolio::create_nvram_file(const std::string& path) {
    std::string host;
    if (!nvram_path(path, host)) return kErrNotSupported;
    if (FILE* f = std::fopen(host.c_str(), "ab")) std::fclose(f);
    else return kErrNotSupported;
    rt.log("NVRAM: created %s", path.c_str());
    return open_file(path, "");
}

int32_t Portfolio::delete_nvram_file(const std::string& path) {
    std::string host;
    if (!nvram_path(path, host)) return kErrNotSupported;
    std::error_code ec;
    if (!fs::remove(fs::u8path(host), ec)) return kErrFsNoFile;
    rt.log("NVRAM: deleted %s", path.c_str());
    return 0;
}

int32_t Portfolio::open_file(const std::string& path, const std::string& base_dir) {
    std::string nv;
    if (nvram_path(path, nv)) {
        std::error_code ec;
        if (!fs::exists(fs::u8path(nv), ec)) { rt.log("OpenDiskFile(\"%s\"): not found (NVRAM)", path.c_str()); return kErrFsNoFile; }
        HleItem& f = new_item(ItemKind::File, kSubsysFile, kNodeFile, device::Size, decode_host_name(fs::u8path(nv).filename().u8string()));
        f.guest_path = path; f.host_path = nv; f.writable = true; f.block_size = 1;
        f.file_size = fs::file_size(fs::u8path(nv), ec);
#if defined(_WIN32)
        f.fp = _wfopen(fs::u8path(nv).wstring().c_str(), L"r+b");
#else
        f.fp = std::fopen(nv.c_str(), "r+b");
#endif
        return f.num;
    }
    const std::string saved = cwd_ref();
    if (!base_dir.empty()) cwd_ref() = base_dir;
    std::string host, norm;
    bool dir = false;
    bool ok = resolve(path, host, norm, dir);
    cwd_ref() = saved;
    if (!ok) { rt.log("OpenDiskFile(\"%s\"): not found", path.c_str()); return kErrFsNoFile; }

    HleItem& f = new_item(ItemKind::File, kSubsysFile, kNodeFile, device::Size, fs::u8path(norm).filename().u8string());
    f.guest_path = norm;
    f.host_path = host;
    f.is_dir = dir;
    std::error_code ec;
    f.file_size = dir ? 0 : fs::file_size(fs::u8path(host), ec);
    if (!dir) {
#if defined(_WIN32)
        f.fp = _wfopen(fs::u8path(host).wstring().c_str(), L"rb");
#else
        f.fp = std::fopen(host.c_str(), "rb");
#endif
    }
    if (rt.cfg.trace) rt.log("OpenDiskFile(\"%s\") -> %d (%s, %llu bytes)", path.c_str(), f.num, norm.c_str(),
                             (unsigned long long)f.file_size);
    return f.num;
}

// The boot scripts a 3DO runs before the game: the system's STARTOPERA, then
// the game's own AppStartup. Their "alias name value" lines define the "$name"
// shortcuts games use: Burning Soldier finds its fonts through "$genki", which
// only its AppStartup defines. The rest of the scripts (starting daemons and
// folios) is the runtime's own business. A value may give alternatives,
// "{/rom2/x|$boot/x}": the one on the disc is taken. "^" is the game's folder.
void Portfolio::load_boot_aliases() {
    auto load = [this](const fs::path& script) {
        std::ifstream in(script, std::ios::binary);
        if (!in) return;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (auto& ch : text) if (ch == '\r') ch = '\n';
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
            std::istringstream w(line);
            std::string cmd, name, value;
            if (!(w >> cmd >> name >> value)) continue;
            for (auto& ch : cmd) ch = char(std::tolower((unsigned char)ch));
            if (cmd != "alias") continue;
            if (value.size() > 2 && value.front() == '{' && value.back() == '}') {
                std::string body = value.substr(1, value.size() - 2), pick;
                std::istringstream alts(body);
                std::string alt;
                while (std::getline(alts, alt, '|'))
                    if (pick.empty() && (alt[0] == '$' || alt[0] == '^')) pick = alt;
                if (pick.empty()) { std::istringstream a2(body); std::getline(a2, pick, '|'); }
                value = pick;
            }
            if (!value.empty() && value[0] == '^') value = "$boot" + value.substr(1);
            if (!value.empty() && value.back() == '/' && value.size() > 1) value.pop_back();
            std::string key = name;
            for (auto& ch : key) ch = char(std::tolower((unsigned char)ch));
            aliases_[key] = value;
            rt.log("boot alias $%s = %s (%s)", key.c_str(), value.c_str(), script.filename().string().c_str());
        }
    };
    const fs::path root = fs::u8path(disc_root());
    // Names on the disc may differ in case.
    auto find_ci = [](const fs::path& dir, const std::vector<std::string>& parts) -> fs::path {
        fs::path cur = dir;
        for (const std::string& want : parts) {
            std::error_code ec; bool found = false;
            for (auto& e : fs::directory_iterator(cur, ec)) {
                std::string n = e.path().filename().string(), a = n, b = want;
                for (auto& ch : a) ch = char(std::tolower((unsigned char)ch));
                for (auto& ch : b) ch = char(std::tolower((unsigned char)ch));
                if (a == b) { cur = e.path(); found = true; break; }
            }
            if (!found) return {};
        }
        return cur;
    };
    if (fs::path p = find_ci(root, {"System", "Scripts", "STARTOPERA"}); !p.empty()) load(p);
    if (fs::path p = find_ci(root, {"AppStartup"}); !p.empty()) load(p);
}

void Portfolio::install_file() {
    load_boot_aliases();
    // The File folio. Vector -6 is LoadProgramPrio(char *cmdline, int32 prio)
    // (identified from Gex launching its movie player). Running a second
    // executable is not supported yet: report it and fail, so games skip it
    // (typically intro movies).
    std::map<int32_t, HleFn> fv;
    // Vector -5 is LoadProgram(char *cmdline): the same thing at the default
    // priority (Slam 'n Jam '95 calls it at start-up, then waits for
    // SIGF_DEADTASK).
    auto load_program = [this](const char* fn, int32_t prio) {
        std::string cmd = rstr(arg(0));
        std::string prog = cmd.substr(0, cmd.find(' '));
        std::string host, norm; bool dir = false;
        bool exists = resolve(prog, host, norm, dir) && !dir;
        // System utilities from the disc's System folder (lmadm, which sets up
        // the save memory, and the like) are the OS's job, done by the runtime
        // itself: they are reported as finished and not run.
        bool system_tool = false;
        if (exists) {
            std::error_code ec;
            fs::path rel = fs::relative(fs::u8path(host), fs::u8path(disc_root()), ec);
            if (!ec && !rel.empty()) {
                std::string first = rel.begin()->string();
                for (auto& ch : first) ch = char(std::tolower((unsigned char)ch));
                system_tool = first == "system";
            }
        }
        if (system_tool) exists = false;
        // Report success with a task that has already finished: the caller
        // gets the usual "child task died" signal and carries on (for Gex:
        // past its intro movies).
        if (exists) {
            std::string why;
            int32_t t = start_program(cmd, host, prio, why);
            if (t > 0) {
                // LoadProgram starts the program in the directory its file is
                // in (FILETASK_TAG_CURRENTDIRECTORY), not its creator's.
                // Lemmings changes to /CD-ROM/LemmingsData, then starts its
                // movie players from the disc's root folder, which open
                // LogoEA.str and Intro.Film there by bare name.
                if (!std::getenv("TDORT_OLD_PROGRAM_CWD"))
                    if (HleTask* nt = task_by_item(t)) {
                        const size_t slash = norm.find_last_of('/');
                        nt->cwd = (slash == std::string::npos || slash == 0) ? std::string() : norm.substr(0, slash);
                    }
                c.r[0] = uint32_t(t);
                return;
            }
            rt.log("%s(\"%s\"): cannot run it here (%s)", fn, cmd.c_str(), why.c_str());
        }
        rt.log("%s(\"%s\", %d): %s; not run, reported as finished", fn, cmd.c_str(), prio,
               system_tool ? "a system utility the runtime replaces" : exists ? "found on disc but could not start" : "not on disc");
        // Behave like a program that starts and exits a moment later: the
        // task item exists now (games wait for it to appear), and a few
        // frames later it is gone and the parent gets SIGF_DEADTASK (games
        // wait for that too).
        HleItem& t = new_item(ItemKind::Generic, kSubsysKernel, kNodeTask, task::Size, prog);
        phantom_children_.push_back({t.num, cur().item, frame_no_ + 3});
        c.r[0] = uint32_t(t.num);
    };
    fv[-5] = [load_program](Cpu*) { load_program("LoadProgram", 100); };
    rt.register_swi(kFileSwi + 2, "FileDaemonInternals", [this](Cpu*) { c.r[0] = uint32_t(kErrNotSupported); });
    rt.register_swi(kFileSwi + 3, "File SWI 3", [this](Cpu*) { c.r[0] = uint32_t(kErrNotSupported); });       // unused by the OS
    rt.register_swi(kFileSwi + 12, "LoadOverlay", [this](Cpu*) { c.r[0] = uint32_t(kErrNotSupported); });
    rt.register_swi(kFileSwi + 14, "CreateLink", [this](Cpu*) { c.r[0] = uint32_t(kErrNotSupported); });       // the disc is read-only

    // ---- code modules: LoadCode / ExecuteAsSubroutine / ExecuteAsThread / UnloadCode
    // LoadCode(path, CodeHandle *code): the AIF file in memory, 16 bytes in
    // (the allocation size in front, for UnloadCode). The handle is the image.
    fv[-11] = [this](Cpu*) {
        const std::string path = rstr(arg(0));
        const uint32_t out = arg(1);
        std::string host, norm; bool dir = false;
        if (!resolve(path, host, norm, dir) || dir) { rt.log("LoadCode(\"%s\"): not found", path.c_str()); c.r[0] = uint32_t(kErrFsNoFile); return; }
        std::ifstream f(fs::u8path(host), std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        AifImage img; std::string why;
        if (!parse_aif(file, img, why)) { rt.log("LoadCode(\"%s\"): %s", path.c_str(), why.c_str()); c.r[0] = uint32_t(kErrNotSupported); return; }
        const uint64_t need = std::max<uint64_t>(img.load_size(), uint64_t(img.ro_size) + img.rw_size + img.zi_size) + 64;
        const uint32_t total = uint32_t(need) + 16;
        const uint32_t mem = alloc(total, kMemFromTop);
        if (!mem) { c.r[0] = uint32_t(kErrNoMem); return; }
        w32(mem, total);
        const uint32_t code = mem + 16;
        rt.mem.write_block(code, img.bytes.data(), img.load_size());
        if (out) w32(out, code);
        const bool translated = rt.map_module(fnv1a64_hex(file), code, img.load_size(), code + (img.entry - img.image_base), path);
        rt.log("LoadCode(\"%s\") -> %08X (%u bytes)%s", path.c_str(), code, img.load_size(), translated ? ", translated" : "");
        c.r[0] = 0;
    };
    // UnloadCode(code)
    fv[-12] = [this](Cpu*) {
        if (arg(0)) { rt.unmap_modules(arg(0), arg(0) + r32(arg(0) - 16)); free_mem(arg(0) - 16); }
        c.r[0] = 0;
    };
    // ExecuteAsSubroutine(code, argc, argv) -> main()'s result. As the OS does:
    // the AIF header's exit instruction becomes "ldmfd sp!, {r1-r12, r14, r15}",
    // those registers are pushed, and the module runs from its header; when
    // its main() returns, the caller continues with main()'s result in r0.
    fv[-13] = [this](Cpu*) {
        const uint32_t code = arg(0), argc = arg(1), argv = arg(2);
        if (!code) { c.r[0] = uint32_t(kErrNoMem); return; }
        w32(code + 0x10, 0xE8BDDFFEu);
        const uint32_t ret = c.r[14];
        uint32_t sp = c.r[13] - 14 * 4;
        for (int r = 1; r <= 12; ++r) w32(sp + uint32_t((r - 1) * 4), c.r[r]);
        w32(sp + 48, ret);                // r14
        w32(sp + 52, ret);                // r15: back to the caller
        c.r[13] = sp;
        // As the file folio enters a module: r0 = the code, r1/r2 = argc/argv,
        // and r5/r6/r7 = argc, argv and KernelBase, which the module's start-up
        // code (like a program's) takes them from. Immercenary's Cinepak
        // module crashed without KernelBase.
        c.r[0] = code; c.r[1] = argc; c.r[2] = argv;
        c.r[kRegArgc] = argc; c.r[kRegArgv] = argv; c.r[kRegKernelBase] = kbase_;
        c.r[15] = code;
    };
    // ExecuteAsThread(code, argc, argv, name, priority): the module as a
    // thread of this task, with the stack size from its 3DO header.
    fv[-14] = [this](Cpu*) {
        const uint32_t code = arg(0);
        if (!code) { c.r[0] = uint32_t(kErrNoMem); return; }
        const uint32_t name_p = r32(c.r[13]);
        const int32_t prio = int32_t(r32(c.r[13] + 4));
        uint32_t stack = r32(code + 128 + 40);          // _3DOBinHeader._3DO_Stack
        if (stack < 1024 || stack > 1024 * 1024) stack = 4096;
        const uint32_t tags = alloc(5 * 8, 0);
        if (!tags) { c.r[0] = uint32_t(kErrNoMem); return; }
        const uint32_t tv[10] = {10 /*PC*/, code, 12 /*stack size*/, stack, 13 /*argc*/, arg(1), 14 /*argp*/, arg(2), 0, 0};
        for (int i = 0; i < 10; ++i) w32(tags + uint32_t(i * 4), tv[i]);
        const uint8_t pri = uint8_t(prio <= 0 ? cur().pri : std::min(prio, 254));
        c.r[0] = uint32_t(create_task(name_p ? rstr(name_p) : "thread", tags, pri));
        free_mem(tags);
    };

    // Directories (directory.h): OpenDirectoryPath / OpenDirectoryItem return
    // a Directory handle; ReadDirectory fills a DirectoryEntry per call, then
    // fails at the end. Slam 'n Jam lists /NVRAM/ to find its save file.
    auto open_dir = [this](const std::string& guest, const std::string& host_in) -> uint32_t {
        std::string host = host_in;
        if (host.empty()) {
            std::string norm; bool dir = false;
            if (nvram_path(guest, host)) host = fs::u8path(host).parent_path().u8string() + (guest.size() > 7 && guest.back() != '/' ? "/" + fs::u8path(host).filename().u8string() : "");
            else if (!resolve(guest, host, norm, dir) || !dir) { rt.log("OpenDirectory(\"%s\"): not found", guest.c_str()); return 0; }
        }
        std::error_code ec;
        std::vector<std::string> ents;
        for (auto& de : fs::directory_iterator(fs::u8path(host), ec)) ents.push_back(de.path().u8string());
        std::sort(ents.begin(), ents.end());
        const uint32_t d = alloc(64, kMemFromTop);
        if (!d) return 0;
        for (uint32_t k = 0; k < 64; k += 4) w32(d + k, 0);
        dirs_[d] = {ents, 0};
        rt.log("OpenDirectory(\"%s\") -> %08X, %zu entries", guest.c_str(), d, ents.size());
        return d;
    };
    fv[-8] = [this, open_dir](Cpu*) { c.r[0] = open_dir(rstr(arg(0)), ""); };           // OpenDirectoryPath(path)
    fv[-7] = [this, open_dir](Cpu*) {                                                   // OpenDirectoryItem(file item)
        HleItem* f = item(int32_t(arg(0)));
        c.r[0] = f && f->is_dir ? open_dir(f->guest_path, f->host_path) : 0;
    };
    fv[-9] = [this](Cpu*) {                                                             // ReadDirectory(dir, entry)
        auto it = dirs_.find(arg(0));
        if (it == dirs_.end()) { c.r[0] = uint32_t(kErrBadPtr); return; }
        auto& d = it->second;
        if (d.next >= d.entries.size()) { c.r[0] = uint32_t(kErrEof); return; }
        uint8_t e[direntry::Size];
        fill_dir_entry(d.entries[d.next], uint32_t(d.next + 1), e);
        ++d.next;
        if (guest_range_ok(arg(1), direntry::Size)) rt.mem.write_block(arg(1), e, direntry::Size);
        c.r[0] = 0;
    };
    fv[-10] = [this](Cpu*) {                                                            // CloseDirectory(dir)
        if (dirs_.erase(arg(0))) free_mem(arg(0));
        c.r[0] = 0;
    };

    // Byte streams over disc files (filestream.h): Real Pinball reads its
    // table descriptions (zeus.dat.N) this way. The guest gets a Stream
    // structure whose position and length fields are kept up to date.
    fv[-1] = [this](Cpu*) {                                  // OpenDiskStream(name, bufferSize)
        const std::string path = rstr(arg(0));
        std::string host, norm; bool dir = false;
        if (!resolve(path, host, norm, dir) || dir) {
            rt.log("OpenDiskStream(\"%s\"): not found", path.c_str());
            c.r[0] = 0; return;
        }
        FILE* fp = std::fopen(host.c_str(), "rb");
        if (!fp) { c.r[0] = 0; return; }
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        const uint32_t st = alloc(kStSize, kMemFromTop);
        if (!st) { std::fclose(fp); c.r[0] = 0; return; }
        for (uint32_t k = 0; k < kStSize; k += 4) w32(st + k, 0);
        w32(st + kStBlockSize, kBlock);
        w32(st + kStBlockCount, uint32_t((size + kBlock - 1) / kBlock));
        w32(st + kStFileLength, uint32_t(size));
        streams_[st] = {fp, uint32_t(size), 0};
        rt.log("OpenDiskStream(\"%s\") -> %08X (%ld bytes)", path.c_str(), st, size);
        c.r[0] = st;
    };
    fv[-2] = [this](Cpu*) {                                  // ReadDiskStream(stream, buffer, nBytes)
        auto it = streams_.find(arg(0));
        if (it == streams_.end()) { c.r[0] = uint32_t(kErrBadPtr); return; }
        auto& s = it->second;
        const uint32_t buf = arg(1);
        uint32_t n = std::min<uint32_t>(arg(2), s.size > s.pos ? s.size - s.pos : 0);
        if (!guest_range_ok(buf, n)) { c.r[0] = uint32_t(kErrBadPtr); return; }
        std::vector<uint8_t> tmp(n);
        std::fseek(s.fp, long(s.pos), SEEK_SET);
        n = uint32_t(std::fread(tmp.data(), 1, n, s.fp));
        rt.mem.write_block(buf, tmp.data(), n);
        s.pos += n;
        w32(arg(0) + kStCursor, s.pos);
        c.r[0] = n;
    };
    fv[-3] = [this](Cpu*) {                                  // SeekDiskStream(stream, offset, origin)
        auto it = streams_.find(arg(0));
        if (it == streams_.end()) { c.r[0] = uint32_t(kErrBadPtr); return; }
        auto& s = it->second;
        const int32_t off = int32_t(arg(1));
        int64_t base = arg(2) == 2 ? int64_t(s.pos) : arg(2) == 3 ? int64_t(s.size) : 0;   // 1 SET, 2 CUR, 3 END
        const int64_t np = base + off;
        if (np < 0 || np > int64_t(s.size)) { c.r[0] = uint32_t(kErrBadPtr); return; }
        s.pos = uint32_t(np);
        w32(arg(0) + kStCursor, s.pos);
        c.r[0] = s.pos;
    };
    fv[-4] = [this](Cpu*) {                                  // CloseDiskStream(stream)
        auto it = streams_.find(arg(0));
        if (it != streams_.end()) { std::fclose(it->second.fp); free_mem(it->first); streams_.erase(it); }
        c.r[0] = 0;
    };
    fv[-6] = [this, load_program](Cpu*) { load_program("LoadProgramPrio", int32_t(arg(1))); };
    make_folio("File", kSubsysFile, 128, fv);

    auto F = [this](uint32_t n, const char* name, HleFn fn) { rt.register_swi(kFileSwi + n, name, std::move(fn)); };
    F(kOpenDiskFile, "OpenDiskFile", [this](Cpu*) { c.r[0] = uint32_t(open_file(rstr(arg(0)), "")); });
    F(kOpenDiskFileInDir, "OpenDiskFileInDir", [this](Cpu*) {
        HleItem* d = item(int32_t(arg(0)));
        if (!d || d->kind != ItemKind::File) { c.r[0] = uint32_t(kErrBadItem); return; }
        c.r[0] = uint32_t(open_file(rstr(arg(1)), d->guest_path));
    });
    F(kCloseDiskFile, "CloseDiskFile", [this](Cpu*) { c.r[0] = uint32_t(delete_item(int32_t(arg(0)))); });
    F(kCreateFile, "CreateFile", [this](Cpu*) {
        std::string p = rstr(arg(0));
        std::string host;
        if (nvram_path(p, host)) { c.r[0] = uint32_t(create_nvram_file(p)); return; }
        stub("CreateFile(" + p + ") on the read-only disc", uint32_t(kErrNotSupported));
    });
    F(kDeleteFile, "DeleteFile", [this](Cpu*) {
        std::string p = rstr(arg(0));
        std::string host;
        if (nvram_path(p, host)) { c.r[0] = uint32_t(delete_nvram_file(p)); return; }
        stub("DeleteFile(" + p + ") on the read-only disc", uint32_t(kErrNotSupported));
    });
    F(kCreateAlias, "CreateAlias", [this](Cpu*) {
        // CreateAlias(name, value): "$name" in later paths expands to value.
        std::string name = rstr(arg(0)), value = rstr(arg(1));
        if (!name.empty() && name[0] == '$') name.erase(0, 1);
        std::string key = name;
        for (auto& ch : key) ch = char(std::tolower((unsigned char)ch));
        if (key.empty()) { c.r[0] = uint32_t(kErrBadPtr); return; }
        aliases_[key] = value;
        HleItem& a = new_item(ItemKind::Generic, kSubsysFile, kNodeFile, 32, "$" + name);
        rt.log("CreateAlias(\"%s\", \"%s\") -> item %d", name.c_str(), value.c_str(), a.num);
        c.r[0] = uint32_t(a.num);
    });
    F(kChangeDirectory, "ChangeDirectory", [this](Cpu*) {
        std::string host, norm; bool dir = false;
        if (!resolve(rstr(arg(0)), host, norm, dir) || !dir) { c.r[0] = uint32_t(kErrFsNoFile); return; }
        cwd_ref() = norm == "/" ? "" : norm;
        c.r[0] = uint32_t(open_file(norm.empty() ? "/cd-rom" : "/cd-rom" + norm, ""));
    });
    F(kGetDirectory, "GetDirectory", [this](Cpu*) {
        std::string p = "/cd-rom" + cwd_ref();
        uint32_t buf = arg(0), len = arg(1);
        if (buf && len) {
            uint32_t n = std::min<uint32_t>(uint32_t(p.size()), len - 1);
            if (guest_range_ok(buf, n + 1)) { rt.mem.write_block(buf, p.data(), n); w8(buf + n, 0); }
        }
        c.r[0] = uint32_t(open_file(p, ""));
    });
    for (auto [n, name] : std::vector<std::pair<uint32_t, const char*>>{
             {kMountFileSystem, "MountFileSystem"}, {kMountMacFileSystem, "MountMacFileSystem"},
             {kDismountFileSystem, "DismountFileSystem"}}) {
        std::string nm = name;
        F(n, name, [this, nm](Cpu*) { stub(nm + " (disc is read-only)", uint32_t(kErrNotSupported)); });
    }
}

void Portfolio::file_io(HleItem& ior, HleItem& f, uint32_t info) {
    if (f.writable) {
        const uint8_t cmd = r8(info + ioinfo::Command);
        const int32_t offset = int32_t(r32(info + ioinfo::Offset));
        const uint32_t sbuf = r32(info + ioinfo::SendBuf), slen = r32(info + ioinfo::SendLen);
        const uint32_t rbuf = r32(info + ioinfo::RecvBuf), rlen = r32(info + ioinfo::RecvLen);
        auto resize = [&](uint64_t n) {
            std::error_code ec;
            std::fflush(f.fp);
            fs::resize_file(fs::u8path(f.host_path), n, ec);
            f.file_size = n;
        };
        switch (cmd) {
        case kCmdWrite: {
            if (!guest_range_ok(sbuf, slen) || offset < 0) { complete_io(ior, kErrBadPtr, 0); return; }
            std::fseek(f.fp, offset, SEEK_SET);
            std::fwrite(&rt.mem.ram[sbuf], 1, slen, f.fp);
            std::fflush(f.fp);
            if (uint64_t(offset) + slen > f.file_size) f.file_size = uint64_t(offset) + slen;
            complete_io(ior, 0, slen);
            return;
        }
        case kCmdRead: {
            if (!guest_range_ok(rbuf, rlen) || offset < 0) { complete_io(ior, kErrBadPtr, 0); return; }
            if (uint64_t(offset) > f.file_size) { complete_io(ior, kErrEof, 0); return; }
            uint32_t n = uint32_t(std::min<uint64_t>(rlen, f.file_size - uint64_t(offset)));
            std::vector<uint8_t> buf(n);
            std::fseek(f.fp, offset, SEEK_SET);
            size_t got = n ? std::fread(buf.data(), 1, n, f.fp) : 0;
            rt.mem.write_block(rbuf, buf.data(), uint32_t(got));
            complete_io(ior, 0, uint32_t(got));
            return;
        }
        case kCmdStatus: {
            uint8_t st[filestatus::Size] = {};
            store_be32(st + devstatus::MaxStatusSize, filestatus::Size);
            store_be32(st + devstatus::BlockSize, 1);
            store_be32(st + devstatus::BlockCount, uint32_t(f.file_size));
            store_be32(st + filestatus::ByteCount, uint32_t(f.file_size));
            uint32_t n = std::min<uint32_t>(rlen, filestatus::Size);
            if (guest_range_ok(rbuf, n)) rt.mem.write_block(rbuf, st, n);
            complete_io(ior, 0, n);
            return;
        }
        case 6: /* FILECMD_ALLOCBLOCKS: grow (or shrink) by ioi_Offset blocks */
            resize(uint64_t(std::max<int64_t>(0, int64_t(f.file_size) + offset)));
            complete_io(ior, 0, 0);
            return;
        case 7: /* FILECMD_SETEOF: logical size in bytes */
            resize(uint64_t(std::max(0, offset)));
            complete_io(ior, 0, 0);
            return;
        case 10: /* FILECMD_SETTYPE: a 4-character type code; not kept */
            complete_io(ior, 0, 0);
            return;
        default: break;   // others handled below like disc files
        }
    }
    const uint8_t cmd = r8(info + ioinfo::Command);
    const uint32_t offset = r32(info + ioinfo::Offset);
    const uint32_t rbuf = r32(info + ioinfo::RecvBuf), rlen = r32(info + ioinfo::RecvLen);
    const uint32_t blocks = uint32_t((f.file_size + kBlock - 1) / kBlock);

    switch (cmd) {
    case kCmdRead: {
        if (f.is_dir) {
            // A directory's blocks: the extracted disc has no raw directory
            // data, so this reads as zeros. Escape from Monster Manor reads a
            // block of "$progdir" now and then ("El Kabong", to keep the drive
            // busy) and throws it away; a failure there stops the game.
            const uint32_t rbuf = r32(info + ioinfo::RecvBuf), rlen = r32(info + ioinfo::RecvLen);
            if (rlen && guest_range_ok(rbuf, rlen)) { std::vector<uint8_t> z(rlen, 0); rt.mem.write_block(rbuf, z.data(), rlen); }
            complete_io(ior, 0, rlen);
            return;
        }
        if (!f.fp) { complete_io(ior, kErrBadCommand, 0); return; }
        if (!guest_range_ok(rbuf, rlen)) { complete_io(ior, kErrBadPtr, 0); return; }
        if (rbuf < 0x1000 && std::getenv("TDORT_NULLREAD")) {
            std::string chain;
            uint32_t fp = c.r[11];
            for (int k = 0; k < 8 && fp > 16 && fp + 4 <= kRamEnd; ++k) {
                char b[24]; std::snprintf(b, sizeof b, " %08X", r32(fp - 4)); chain += b;
                fp = r32(fp - 12);
            }
            rt.log("read into %08X: lr %08X, frame chain%s; r4-r10 %08X %08X %08X %08X %08X %08X %08X", rbuf, c.r[14], chain.c_str(),
                   c.r[4], c.r[5], c.r[6], c.r[7], c.r[8], c.r[9], c.r[10]);
        }
        const uint64_t start = uint64_t(offset) * kBlock;
        if (start > f.file_size) { complete_io(ior, kErrEof, 0); return; }
        // The OS reads whole blocks straight off the disc and does not stop at the
        // end of the file: a read past it transfers the full length (on a real
        // disc, whatever follows the file). Movie players rely on getting the full
        // length back (Creature Shock waits forever on a short last read). Past
        // EOF here reads as zero.
        uint64_t avail_blocks_bytes = uint64_t(blocks) * kBlock - start;
        uint32_t len = uint32_t(std::min<uint64_t>(rlen, avail_blocks_bytes));
        std::vector<uint8_t> buf(rlen, 0);
#if defined(_WIN32)
        _fseeki64(f.fp, int64_t(start), SEEK_SET);
#else
        fseeko(f.fp, off_t(start), SEEK_SET);
#endif
        size_t got = len ? std::fread(buf.data(), 1, len, f.fp) : 0;
        (void)got;
        rt.mem.write_block(rbuf, buf.data(), rlen);   // invalidates any recompiled code it overwrites
        recent_reads_.push_back({f.guest_path, offset, rlen, rbuf, frame_no_});
        if (std::getenv("TDORT_LOG_READS"))
            rt.log("read %s block %u, %u bytes -> %08X (task %s)", f.guest_path.c_str(), offset, rlen, rbuf, cur().name.c_str());
        if (recent_reads_.size() > 8) recent_reads_.pop_front();
        if (rt.cfg.trace || std::getenv("TDORT_TRACE_READS"))
            rt.log("read %s block %u: %u bytes (%u from the file) -> %08X-%08X", f.guest_path.c_str(), offset, rlen, len, rbuf, rbuf + rlen);
        // A CD drive moves about 300 KB a second (5 KB a frame), one request
        // at a time. An asynchronous read finishes when the drive would be
        // done with it. Streaming code depends on that pace: Twisted's stream
        // reaches a "wait" chunk only after the subscribers have answered
        // its start command; with reads finishing at once it got there first,
        // cleared the pending answers and the opening was abandoned.
        // TDORT_CD_SPEED=<bytes per frame> (0: instant, the old behaviour).
        static const uint32_t cd_bpf = [] {
            const char* e = std::getenv("TDORT_CD_SPEED");
            return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 5120u;
        }();
        // The drive's time is counted in bytes (cd_bpf a frame), so several
        // small reads can finish in the same frame, as they would on the drive.
        // A synchronous read (DoIO) makes its task wait for the drive too
        // (TDORT_CD_SYNC=0: synchronous reads are instant, as before).
        static const bool sync_wait = [] { const char* e = std::getenv("TDORT_CD_SYNC"); return !(e && e[0] == '0'); }();
        const bool timed = cd_bpf && !old_io() && (!io_quick_ || (sync_wait && multitasking()));
        if (timed) {
            const uint64_t now = uint64_t(frame_no_) * cd_bpf;
            const uint64_t begin = std::max(cd_free_pos_, now);
            cd_free_pos_ = begin + rlen;
            const uint32_t due = std::max<uint32_t>(frame_no_ + 1, uint32_t((cd_free_pos_ + cd_bpf - 1) / cd_bpf));
            if (!io_quick_) {
                w32(ior.addr + ioreq::Flags, r32(ior.addr + ioreq::Flags) & ~kIoDone);
                vbl_io_.push_back({ior.num, cur().item, 0, rlen, due});
                return;
            }
            complete_io(ior, 0, rlen);
            // Short reads the drive finishes within this frame cost no wait.
            if (cd_free_pos_ > now + cd_bpf) {
                static bool said = false;
                if (!said) { said = true; rt.log("CD: a synchronous read (%u bytes of %s) waits for the drive until frame %u (TDORT_CD_SYNC=0: no wait)", rlen, f.guest_path.c_str(), due); }
                sleep_until(due);
            }
            return;
        }
        complete_io(ior, 0, rlen);
        return;
    }
    case kCmdStatus: {
        uint8_t st[filestatus::Size] = {};
        auto put = [&](uint32_t off, uint32_t v) { store_be32(st + off, v); };
        put(devstatus::MaxStatusSize, filestatus::Size);
        put(devstatus::BlockSize, kBlock);
        put(devstatus::BlockCount, blocks);
        put(filestatus::ByteCount, uint32_t(f.file_size));
        uint32_t n = std::min<uint32_t>(rlen, filestatus::Size);
        if (!guest_range_ok(rbuf, n)) { complete_io(ior, kErrBadPtr, 0); return; }
        rt.mem.write_block(rbuf, st, n);
        complete_io(ior, 0, n);
        return;
    }
    case kFileCmdReadDir: {
        // ioi_Offset selects the entry, counting from 1.
        if (!f.is_dir) { complete_io(ior, kErrBadCommand, 0); return; }
        std::vector<fs::directory_entry> ents;
        std::error_code ec;
        for (auto& de : fs::directory_iterator(fs::u8path(f.host_path), ec)) ents.push_back(de);
        std::sort(ents.begin(), ents.end(), [](auto& a, auto& b) { return a.path().filename() < b.path().filename(); });
        if (offset == 0 || offset > ents.size()) { complete_io(ior, kErrEof, 0); return; }
        uint8_t e[direntry::Size];
        fill_dir_entry(ents[offset - 1].path().u8string(), offset, e);
        uint32_t n = std::min<uint32_t>(rlen, direntry::Size);
        if (!guest_range_ok(rbuf, n)) { complete_io(ior, kErrBadPtr, 0); return; }
        rt.mem.write_block(rbuf, e, n);
        complete_io(ior, 0, n);
        return;
    }
    case kFileCmdGetPath: {
        std::string p = "/cd-rom" + (f.guest_path == "/" ? std::string() : f.guest_path);
        uint32_t n = std::min<uint32_t>(rlen ? rlen - 1 : 0, uint32_t(p.size()));
        if (rlen && guest_range_ok(rbuf, n + 1)) { rt.mem.write_block(rbuf, p.data(), n); w8(rbuf + n, 0); }
        complete_io(ior, 0, n);
        return;
    }
    case kCmdWrite:
        stub("file write (disc is read-only)");
        complete_io(ior, kErrNotSupported, 0);
        return;
    default:
        stub("file cmd " + std::to_string(cmd));
        complete_io(ior, kErrBadCommand, 0);
    }
}


void Portfolio::cd_load_wait(uint32_t bytes, const std::string& what) {
    static const uint32_t cd_bpf = [] {
        const char* e = std::getenv("TDORT_CD_SPEED");
        return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 5120u;
    }();
    static const bool on = [] {
        const char* e = std::getenv("TDORT_CD_SYNC"); if (e && e[0] == '0') return false;
        return std::getenv("TDORT_OLD_LOADWAIT") == nullptr;
    }();
    if (!on || !cd_bpf || old_io() || !multitasking()) return;
    const uint64_t now = uint64_t(frame_no_) * cd_bpf;
    cd_free_pos_ = std::max(cd_free_pos_, now) + bytes;
    if (cd_free_pos_ <= now + cd_bpf) return;               // finished within this frame
    const uint32_t due = uint32_t((cd_free_pos_ + cd_bpf - 1) / cd_bpf);
    static bool said = false;
    if (!said) { said = true; rt.log("CD: loading %s (%u bytes) waits for the drive until frame %u (TDORT_OLD_LOADWAIT=1: no wait)", what.c_str(), bytes, due); }
    sleep_until(due);
}

} // namespace tdort
