// platform_win32.cpp - Win32 window (Direct3D 11, or a GDI blit), XInput pad, keyboard, frame pacing.
// Deliberately dependency-free: only Windows SDK libraries, nothing downloaded.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <mmsystem.h>
#include "tdort/platform.h"
#include "png_write.h"
#include "gpu_present.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "winmm.lib")

namespace tdort {

std::unique_ptr<Platform> make_null_platform();

namespace {

// F12 detail level for a first run (no display.cfg line yet).
constexpr int kDefaultDetail = 2;

struct Win32Platform : Platform {
    HWND hwnd = nullptr;
    bool quit = false;
    uint32_t keys = 0;
    std::vector<uint32_t> last;
    int fw = 320, fh = 240;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();

    // ---- display settings, changed in-game with hotkeys and kept between runs
    // (one file for every converted game: %APPDATA%\3DO to PC\display.cfg).
    //   F7  window size 2x..6x        F8  picture shape: 4:3 / pixel-perfect / stretch
    //   F9  look: sharp / smooth / scanlines     F11 or Alt+Enter  fullscreen
    //   F1  show these keys          F2  show the button mapping (again to hide)
    //   F4  put in the next disc of a multi-disc game
    //   F10 screenshot: the game picture (320x240, or finer with F12) as a PNG in screenshots\ next to the game
    //   F12 detail: draw the game at 1x..4x its resolution (smoother 3D and scaled sprites)
    // Save states: F5 save, F6 load, F3 choose the slot (1..9).
    enum Fit { Fit43, FitInteger, FitStretch, kFits };
    enum Look { LookSharp, LookSmooth, LookScanlines, kLooks };
    int scale = 3, fit = Fit43, look = LookSharp;
    int detail = kDefaultDetail;          // internal resolution, 1..4
    bool show_fps = false;                // Ctrl+F frame counter
    std::string fps_text;
    // Frame counter: frames shown, picture changes and work time in the
    // current second (work: from the end of one frame's wait to the start of
    // the next, i.e. running the game and drawing its picture).
    std::chrono::steady_clock::time_point fps_since = std::chrono::steady_clock::now(), woke = std::chrono::steady_clock::now();
    int fps_frames = 0, fps_changes = 0;
    double fps_work = 0, fps_work_max = 0;
    uint64_t last_hash = 0;
    bool fullscreen = false;
    int state_slot = 1, state_req = 0;    // save states: slot, and 1 = save / 2 = load asked for
    WINDOWPLACEMENT saved_place{sizeof(WINDOWPLACEMENT)};
    std::vector<uint32_t> out;            // the scaled picture, client-sized box
    std::string osd;                      // message shown over the picture
    int osd_tab = 6;                      // tab stops for the message, in average characters
    bool controls_shown = false;          // F2 panel is up
    std::chrono::steady_clock::time_point osd_until{};

    static std::wstring settings_path() {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        std::wstring dir;
        if (n && n < MAX_PATH) { dir = std::wstring(buf) + L"\\3DO to PC"; CreateDirectoryW(dir.c_str(), nullptr); }
        else {
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            dir = buf; size_t k = dir.find_last_of(L"\\/"); dir = k == std::wstring::npos ? L"." : dir.substr(0, k);
        }
        return dir + L"\\display.cfg";
    }
    void load_settings() {
        FILE* f = _wfopen(settings_path().c_str(), L"r");
        if (!f) return;
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            int v = 0;
            if (std::sscanf(line, "scale=%d", &v) == 1) scale = std::clamp(v, 1, 8);
            else if (std::sscanf(line, "fit=%d", &v) == 1) fit = std::clamp(v, 0, int(kFits) - 1);
            else if (std::sscanf(line, "look=%d", &v) == 1) look = std::clamp(v, 0, int(kLooks) - 1);
            else if (std::sscanf(line, "fullscreen=%d", &v) == 1) fullscreen = v != 0;
            else if (std::sscanf(line, "detail=%d", &v) == 1) detail = std::clamp(v, 1, 4);
            else if (std::sscanf(line, "fps=%d", &v) == 1) show_fps = v != 0;
        }
        std::fclose(f);
    }
    void save_settings() {
        FILE* f = _wfopen(settings_path().c_str(), L"w");
        if (!f) return;
        std::fprintf(f, "scale=%d\nfit=%d\nlook=%d\nfullscreen=%d\ndetail=%d\nfps=%d\n", scale, fit, look, fullscreen ? 1 : 0, detail, show_fps ? 1 : 0);
        std::fclose(f);
    }
    void say(const std::string& text, int ms = 2000, int tab = 6) {
        osd = text; osd_tab = tab; controls_shown = false;
        osd_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        redraw();
    }
    // F2: which key or controller button plays each 3DO button. Stays up until
    // F2 is pressed again (or another message replaces it).
    static const char* controls_text() {
        return "Controls  \xC2\xB7  F2 to close\n"
               "3DO pad\tKeyboard\tController\n"
               "D-pad\tArrow keys\tD-pad or left stick\n"
               "A\tZ\tA\n"
               "B\tX\tB\n"
               "C\tC\tX\n"
               "L  /  R\tA  /  S\tLB  /  RB\n"
               "Play (Start)\tEnter\tStart\n"
               "Stop (X)\tBackspace\tBack\n"
               "Next disc\tF4\n"
               "Screenshot\tF10\n"
               "Other keys\tF1";
    }
    // F10: the current game picture, as the game drew it (no message panel, no
    // scaling), saved as screenshots\<game>_<date>_<time>.png beside the .exe.
    void screenshot() {
        if (last.empty()) { say("Screenshot: no picture yet"); return; }
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring exe(buf, n), dir = L".", name = L"game";
        const size_t k = exe.find_last_of(L"\\/");
        if (k != std::wstring::npos) { dir = exe.substr(0, k); name = exe.substr(k + 1); }
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        const std::wstring folder = dir + L"\\screenshots";
        CreateDirectoryW(folder.c_str(), nullptr);
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf(stamp, 64, L"_%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring file = folder + L"\\" + name + stamp + L".png";
        for (int i = 2; GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && i < 100; ++i)   // two in one second
            file = folder + L"\\" + name + stamp + L"_" + std::to_wstring(i) + L".png";
        FILE* f = _wfopen(file.c_str(), L"wb");
        const bool ok = f && tdort::png_write_file(f, last.data(), fw, fh);
        if (f) std::fclose(f);
        const std::wstring shown = file.substr(dir.size() + 1);
        const int len = WideCharToMultiByte(CP_UTF8, 0, shown.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u8(size_t(std::max(1, len)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, shown.c_str(), -1, &u8[0], len, nullptr, nullptr);
        u8.resize(std::strlen(u8.c_str()));
        say(ok ? "Screenshot saved  \xC2\xB7  " + u8 : "Screenshot could not be saved (" + u8 + ")", 2500);
    }
    void toggle_controls() {
        if (controls_shown && !osd.empty()) { osd.clear(); controls_shown = false; redraw(); return; }
        say(controls_text(), 3600 * 1000, 11);
        controls_shown = true;
    }
    void redraw() { InvalidateRect(hwnd, nullptr, FALSE); }

    // Window size for the current scale, kept inside the screen's work area.
    void apply_scale() {
        if (fullscreen) return;
        RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int s = scale;
        for (;;) {
            RECT r{0, 0, 320 * s, 240 * s};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (s <= 1 || (r.right - r.left <= work.right - work.left && r.bottom - r.top <= work.bottom - work.top)) {
                if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                scale = s;
                return;
            }
            --s;
        }
    }
    // Fullscreen is a borderless window over the whole monitor (no display mode
    // change, so switching is instant and alt-tab is safe).
    void set_fullscreen(bool on) {
        if (on == fullscreen && on == ((GetWindowLongW(hwnd, GWL_STYLE) & WS_POPUP) != 0)) return;
        fullscreen = on;
        if (on) {
            GetWindowPlacement(hwnd, &saved_place);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            if (saved_place.length) SetWindowPlacement(hwnd, &saved_place);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        redraw();
    }
    static const char* fit_name(int f) { return f == Fit43 ? "4:3" : f == FitInteger ? "Pixel perfect" : "Stretch to window"; }
    static const char* look_name(int l) { return l == LookSharp ? "Sharp pixels" : l == LookSmooth ? "Smooth" : "Scanlines"; }

    // Display hotkeys; true if the key was one of them.
    bool display_key(uint32_t vk) {
        if (vk == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            show_fps = !show_fps; save_settings();
            say(show_fps ? "Frame counter on (Ctrl+F to hide)" : "Frame counter off");
            return true;
        }
        switch (vk) {
        case VK_F1:
            say("F2\tButton mapping\nF5\tSave state (slot " + std::to_string(state_slot) + ")\nF6\tLoad state\nF3\tChange save slot\n"
                "F4\tNext disc\nF7\tWindow size\nF8\tPicture shape\nF9\tLook\nF10\tScreenshot\nF11\tFullscreen\nF12\tDetail (resolution)\nCtrl+F\tFrame counter", 6000);
            return true;
        case VK_F2:
            toggle_controls();
            return true;
        case VK_F3:
            state_slot = state_slot % 9 + 1;
            say("Save slot " + std::to_string(state_slot));
            return true;
        case VK_F5: state_req = 1; return true;
        case VK_F6: state_req = 2; return true;
        case VK_F7:
            if (fullscreen) { say("Window size: leave fullscreen first (F11)"); return true; }
            scale = scale >= 6 ? 2 : scale + 1;
            apply_scale(); save_settings();
            say("Window size  " + std::to_string(scale) + "\xC3\x97   (" + std::to_string(320 * scale) + " \xC3\x97 " + std::to_string(240 * scale) + ")");
            return true;
        case VK_F8:
            fit = (fit + 1) % kFits; save_settings();
            say(std::string("Shape  \xC2\xB7  ") + fit_name(fit));
            return true;
        case VK_F9:
            look = (look + 1) % kLooks; save_settings();
            say(std::string("Look  \xC2\xB7  ") + look_name(look));
            return true;
        case VK_F12:
            detail = detail % 4 + 1; save_settings();
            say("Detail  " + std::to_string(detail) + "\xC3\x97   (" + std::to_string(320 * detail) + " \xC3\x97 " + std::to_string(240 * detail) +
                (detail == 1 ? ", as on the console)" : ")"));
            return true;
        case VK_F11:
            set_fullscreen(!fullscreen); save_settings();
            say(fullscreen ? "Fullscreen (F11 or Alt+Enter to leave)" : "Windowed");
            return true;
        }
        return false;
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Win32Platform*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
        case WM_CLOSE: if (self) self->quit = true; return 0;
        case WM_KEYDOWN:
            if (self && !(l & (1 << 30)) && self->display_key(uint32_t(w))) return 0;
            if (self) self->key(uint32_t(w), true);
            return 0;
        case WM_KEYUP:
            if (self) self->key(uint32_t(w), false);
            return 0;
        case WM_SYSKEYDOWN:
            if (self && w == VK_RETURN && (l & (1 << 29))) {      // Alt+Enter
                if (!(l & (1 << 30))) { self->set_fullscreen(!self->fullscreen); self->save_settings();
                                        self->say(self->fullscreen ? "Fullscreen (F11 or Alt+Enter to leave)" : "Windowed"); }
                return 0;
            }
            if (self && w == VK_F10) {                            // F10: screenshot (and no menu-bar mode)
                if (!(l & (1 << 30))) self->screenshot();
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (self && self->fullscreen && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
            break;
        case WM_ERASEBKGND: return 1;                             // blit paints every pixel
        case WM_SIZE: if (self) self->redraw(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            if (self && !self->gpu_frame()) self->blit(dc);
            EndPaint(h, &ps); return 0;
        }
        }
        return DefWindowProcW(h, m, w, l);
    }

    void key(uint32_t vk, bool down) {
        uint32_t bit = 0;
        switch (vk) {
        case VK_UP: bit = PadUp; break;       case VK_DOWN: bit = PadDown; break;
        case VK_LEFT: bit = PadLeft; break;   case VK_RIGHT: bit = PadRight; break;
        case 'Z': bit = PadA; break;          case 'X': bit = PadB; break;
        case 'C': bit = PadC; break;          case 'A': bit = PadL; break;
        case 'S': bit = PadR; break;          case VK_BACK: bit = PadX; break;
        case VK_RETURN: bit = PadP; break;
        case VK_F4: bit = PadNextDisc; break;
        }
        keys = down ? (keys | bit) : (keys & ~bit);
    }

    bool open(const char* title, int w, int h) override {
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"tdort"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        load_settings();
        (void)w; (void)h;
        RECT r{0, 0, 320 * scale, 240 * scale};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        wchar_t wt[256]; MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, 256);
        hwnd = CreateWindowW(L"tdort", wt, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(this));
        // Take keyboard focus: when started from the console script the new
        // window can open behind it, and key presses would go to the console.
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        apply_scale();
        if (!std::getenv("TDORT_GDI")) gpu_on = gpu.init(hwnd);
        if (fullscreen) { fullscreen = false; set_fullscreen(true); }
        say("F1  \xC2\xB7  keys      F2  \xC2\xB7  controls", 3000);
        timeBeginPeriod(1);
        return true;
    }

    bool pump() override {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return !quit;
    }

    uint32_t pad() override {
        uint32_t b = keys;
        XINPUT_STATE st{};
        if (XInputGetState(0, &st) == ERROR_SUCCESS) {
            WORD g = st.Gamepad.wButtons;
            if (g & XINPUT_GAMEPAD_DPAD_UP) b |= PadUp;       if (g & XINPUT_GAMEPAD_DPAD_DOWN) b |= PadDown;
            if (g & XINPUT_GAMEPAD_DPAD_LEFT) b |= PadLeft;   if (g & XINPUT_GAMEPAD_DPAD_RIGHT) b |= PadRight;
            if (g & XINPUT_GAMEPAD_A) b |= PadA;              if (g & XINPUT_GAMEPAD_B) b |= PadB;
            if (g & XINPUT_GAMEPAD_X) b |= PadC;
            if (g & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= PadL;  if (g & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= PadR;
            if (g & XINPUT_GAMEPAD_BACK) b |= PadX;           if (g & XINPUT_GAMEPAD_START) b |= PadP;
            if (st.Gamepad.sThumbLY > 16000) b |= PadUp;      if (st.Gamepad.sThumbLY < -16000) b |= PadDown;
            if (st.Gamepad.sThumbLX < -16000) b |= PadLeft;   if (st.Gamepad.sThumbLX > 16000) b |= PadRight;
        }
        return b;
    }

    // The picture box inside the client area for the current shape.
    void box(int cw, int ch, int& bx, int& by, int& bw, int& bh) const {
        if (fit == FitStretch) { bx = by = 0; bw = cw; bh = ch; return; }
        if (fit == FitInteger && cw >= 320 && ch >= 240) {
            const int k = std::min(cw / 320, ch / 240);
            bw = 320 * k; bh = 240 * k;
        } else if (int64_t(cw) * 3 > int64_t(ch) * 4) { bh = ch; bw = ch * 4 / 3; }
        else { bw = cw; bh = cw * 3 / 4; }
        bx = (cw - bw) / 2; by = (ch - bh) / 2;
    }

    // Scale the frame into out (bw x bh), rows split over a few threads.
    std::vector<uint32_t> hrows;          // smooth: source rows already widened to bw
    template <class F> static void parallel_rows(int n_rows, size_t work, F&& f) {
        const int n = std::clamp(int(std::thread::hardware_concurrency()), 1, 4);
        if (n == 1 || work < 400000) { f(0, n_rows); return; }
        std::vector<std::thread> pool;
        for (int i = 1; i < n; ++i) pool.emplace_back(f, n_rows * i / n, n_rows * (i + 1) / n);
        f(0, n_rows / n);
        for (auto& t : pool) t.join();
    }
    static uint32_t lerp(uint32_t p, uint32_t q, uint32_t t) {     // t in 0..256
        const uint32_t rb = ((p & 0xFF00FF) * (256 - t) + (q & 0xFF00FF) * t) >> 8 & 0xFF00FF;
        const uint32_t g = ((p & 0x00FF00) * (256 - t) + (q & 0x00FF00) * t) >> 8 & 0x00FF00;
        return rb | g;
    }
    void scale_into(int bw, int bh) {
        out.resize(size_t(bw) * bh);
        const uint32_t* src = last.data();
        const int sw = fw, sh = fh;
        if (look == LookSmooth) {
            // Bilinear in two passes: widen each source row once, then blend
            // two widened rows per screen line.
            std::vector<int> x0(static_cast<size_t>(bw)), wx(static_cast<size_t>(bw));
            for (int x = 0; x < bw; ++x) {
                const int fx = std::max(0, int(((int64_t(x) * 2 + 1) * sw * 128) / bw) - 128);   // 1/256 px
                x0[size_t(x)] = std::min(fx >> 8, sw - 1); wx[size_t(x)] = fx & 255;
            }
            hrows.resize(size_t(sh) * bw);
            parallel_rows(sh, size_t(sh) * bw, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    const uint32_t* r = src + size_t(y) * sw; uint32_t* d = &hrows[size_t(y) * bw];
                    for (int x = 0; x < bw; ++x) {
                        const int a = x0[size_t(x)], b = std::min(a + 1, sw - 1);
                        d[x] = lerp(r[a], r[b], uint32_t(wx[size_t(x)]));
                    }
                }
            });
            parallel_rows(bh, size_t(bw) * bh, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    const int fy = std::max(0, int(((int64_t(y) * 2 + 1) * sh * 128) / bh) - 128);
                    const int ya = std::min(fy >> 8, sh - 1), yb = std::min(ya + 1, sh - 1);
                    const uint32_t t = uint32_t(fy & 255);
                    const uint32_t* ra = &hrows[size_t(ya) * bw]; const uint32_t* rb = &hrows[size_t(yb) * bw];
                    uint32_t* d = &out[size_t(y) * bw];
                    if (t == 0) std::memcpy(d, ra, size_t(bw) * 4);
                    else for (int x = 0; x < bw; ++x) d[x] = lerp(ra[x], rb[x], t);
                }
            });
            return;
        }
        std::vector<int> xi(static_cast<size_t>(bw));
        for (int x = 0; x < bw; ++x) xi[size_t(x)] = std::min(int(int64_t(x) * sw / bw), sw - 1);
        parallel_rows(bh, size_t(bw) * bh, [&](int y0, int y1) {
            int prev = -1;
            for (int y = y0; y < y1; ++y) {
                const int64_t fy = int64_t(y) * sh * 4 / bh;                 // quarter source lines
                const int64_t fl = int64_t(y) * 240 * 4 / bh;                // quarter 3DO lines (the picture may be drawn finer)
                const int sy = std::min(int(fy / 4), sh - 1);
                uint32_t* d = &out[size_t(y) * bw];
                if (sy == prev && y > y0) std::memcpy(d, d - bw, size_t(bw) * 4);   // same source line as above
                else { const uint32_t* r = src + size_t(sy) * sw; for (int x = 0; x < bw; ++x) d[x] = r[xi[size_t(x)]]; }
                prev = sy;
                // Scanlines: the last quarter of each source line at half brightness
                // (needs at least 2 screen lines per source line to show).
                if (look == LookScanlines && bh >= 480 && (fl & 3) == 3) {
                    for (int x = 0; x < bw; ++x) d[x] = (d[x] >> 1) & 0x7F7F7F;
                    prev = -1;                                           // this line is darkened: don't copy it
                }
            }
        });
    }

    // ---- the on-screen message: anti-aliased Windows text (Segoe UI) on a
    // rounded, see-through panel in the top-left corner, fading out at the end.
    // The text is drawn once per message and size into a coverage mask, then
    // blended over the picture every frame.
    struct TextMask { std::string text; int px = 0, w = 0, h = 0, tab = 6; std::vector<uint8_t> m; };   // text coverage, 0..255
    TextMask osd_mask, fps_mask;

    void build_mask(TextMask& tm, const std::string& text, int px, int tab) {
        tm.text = text; tm.px = px; tm.tab = tab; tm.w = tm.h = 0; tm.m.clear();
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
        std::wstring w(size_t(std::max(0, n)), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), &w[0], n);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return;
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ old_font = SelectObject(dc, font);
        DRAWTEXTPARAMS tabs{sizeof tabs, tab, 0, 0, 0};    // tab stops every `tab` average characters
        const UINT flags = DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS | DT_TABSTOP;
        RECT r{0, 0, 4096, 0};
        DrawTextExW(dc, &w[0], int(w.size()), &r, flags | DT_CALCRECT, &tabs);
        tm.w = std::max(1, int(r.right)); tm.h = std::max(1, int(r.bottom));
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = tm.w; bi.bmiHeader.biHeight = -tm.h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            HGDIOBJ old_bmp = SelectObject(dc, bmp);
            std::memset(bits, 0, size_t(tm.w) * tm.h * 4);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT tr{0, 0, tm.w, tm.h};
            DrawTextExW(dc, &w[0], int(w.size()), &tr, flags, &tabs);
            GdiFlush();
            const uint32_t* p = static_cast<const uint32_t*>(bits);
            tm.m.resize(size_t(tm.w) * tm.h);
            for (size_t k = 0; k < tm.m.size(); ++k) {     // grey text: any channel is the coverage
                const uint32_t v = p[k];
                tm.m[k] = uint8_t(std::max({v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF}));
            }
            SelectObject(dc, old_bmp);
        }
        if (bmp) DeleteObject(bmp);
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
    }

    // A rounded, see-through panel with white text at (x0, y0) of the picture.
    void draw_panel(const TextMask& tm, int bw, int bh, int x0, int y0, double fade) {
        const int px = tm.px;
        const int pad_x = px * 3 / 4, pad_y = px / 2;
        const int radius = std::max(4, px / 2);
        if (x0 < 0 || y0 < 0) return;
        const int pw = std::min(tm.w + pad_x * 2, bw - x0), ph = std::min(tm.h + pad_y * 2, bh - y0);
        if (pw <= 0 || ph <= 0) return;
        const int panel_a = int(190 * fade);                             // about 75% black
        const int text_a = int(255 * fade);
        for (int y = 0; y < ph; ++y) {
            uint32_t* row = &out[size_t(y0 + y) * bw + x0];
            for (int x = 0; x < pw; ++x) {
                // Rounded corners with a one-pixel soft edge.
                int cov = 256;
                const int cx = x < radius ? radius - x : x >= pw - radius ? x - (pw - radius - 1) : 0;
                const int cy = y < radius ? radius - y : y >= ph - radius ? y - (ph - radius - 1) : 0;
                if (cx && cy) {
                    const double d = std::sqrt(double(cx * cx + cy * cy)) - radius;
                    if (d >= 0.5) continue;
                    if (d > -0.5) cov = int((0.5 - d) * 256);
                }
                const uint32_t a = uint32_t(panel_a * cov >> 8);
                uint32_t p = row[x];
                p = (((p & 0xFF00FF) * (256 - a)) >> 8 & 0xFF00FF) | (((p & 0x00FF00) * (256 - a)) >> 8 & 0x00FF00);
                const int tx = x - pad_x, ty = y - pad_y;
                if (tx >= 0 && ty >= 0 && tx < tm.w && ty < tm.h) {
                    const uint32_t t = uint32_t(tm.m[size_t(ty) * tm.w + tx]) * uint32_t(text_a) / 255;
                    if (t) p = (((p & 0xFF00FF) * (256 - t) + 0xFF00FF * t) >> 8 & 0xFF00FF)
                             | (((p & 0x00FF00) * (256 - t) + 0x00FF00 * t) >> 8 & 0x00FF00);
                }
                row[x] = p;
            }
        }
    }
    void draw_osd(int bw, int bh) {
        if (osd.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= osd_until) { osd.clear(); return; }
        const double left_ms = std::chrono::duration<double, std::milli>(osd_until - now).count();
        const double fade = std::min(1.0, left_ms / 300.0);            // fades out over the last 0.3 s
        const int px = std::clamp(bh / 32, 13, 34);                      // text size follows the picture
        if (osd != osd_mask.text || px != osd_mask.px || osd_tab != osd_mask.tab) build_mask(osd_mask, osd, px, osd_tab);
        if (osd_mask.m.empty()) return;
        const int margin = std::max(6, px / 2);
        draw_panel(osd_mask, bw, bh, margin, margin, fade);
    }
    // Ctrl+F: frames per second (the window's, and how often the game's
    // picture changed) and the time each frame took to make, top right.
    void draw_fps(int bw, int bh) {
        if (!show_fps || fps_text.empty()) return;
        const int px = std::clamp(bh / 40, 11, 26);
        if (fps_text != fps_mask.text || px != fps_mask.px) build_mask(fps_mask, fps_text, px, 6);
        if (fps_mask.m.empty()) return;
        const int margin = std::max(6, px / 2);
        const int pw = fps_mask.w + (px * 3 / 4) * 2;
        draw_panel(fps_mask, bw, bh, bw - margin - pw, margin, 1.0);
    }

    // ---- the picture through Direct3D 11 (gpu_present.h), when it works.
    // TDORT_GDI=1: always the GDI path below.
    GpuPresenter gpu;
    bool gpu_on = false;
    std::vector<uint32_t> osd_img, fps_img;   // the panels as premultiplied pictures
    int osd_w = 0, osd_h = 0, fps_w = 0, fps_h = 0;
    uint64_t osd_ver = 0, fps_ver = 0;
    std::string osd_key, fps_key;

    // A panel (draw_panel's look) as a premultiplied BGRA picture, w x h.
    void panel_image(const TextMask& tm, std::vector<uint32_t>& img, int& w, int& h) {
        const int px = tm.px;
        const int pad_x = px * 3 / 4, pad_y = px / 2;
        const int radius = std::max(4, px / 2);
        w = tm.w + pad_x * 2; h = tm.h + pad_y * 2;
        img.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int cov = 256;
                const int cx = x < radius ? radius - x : x >= w - radius ? x - (w - radius - 1) : 0;
                const int cy = y < radius ? radius - y : y >= h - radius ? y - (h - radius - 1) : 0;
                if (cx && cy) {
                    const double d = std::sqrt(double(cx * cx + cy * cy)) - radius;
                    if (d >= 0.5) continue;
                    if (d > -0.5) cov = int((0.5 - d) * 256);
                }
                const uint32_t a = uint32_t(190 * cov >> 8);          // panel, of 256
                const int tx = x - pad_x, ty = y - pad_y;
                const uint32_t t = (tx >= 0 && ty >= 0 && tx < tm.w && ty < tm.h) ? tm.m[size_t(ty) * tm.w + tx] : 0;   // text, of 255
                // Over a picture p: p * (1 - a) * (1 - t) + white * t.
                const uint32_t keep = (256 - a) * (255 - t) / 255;           // of 256
                const uint32_t alpha = std::min<uint32_t>(255, (256 - keep) * 255 / 256);
                const uint32_t c = std::min<uint32_t>(255, t);
                img[size_t(y) * w + x] = (alpha << 24) | (c << 16) | (c << 8) | c;
            }
    }

    // Draw a frame with the graphics card; false: use GDI.
    bool gpu_frame() {
        if (!gpu_on) return false;
        RECT cr; GetClientRect(hwnd, &cr);
        const int cw = cr.right, ch = cr.bottom;
        if (cw <= 0 || ch <= 0) return true;
        int bx = 0, by = 0, bw = 0, bh = 0;
        if (!last.empty()) box(cw, ch, bx, by, bw, bh);
        GpuPresenter::Overlay ov[2];
        int n = 0;
        if (!last.empty() && bw > 0 && bh > 0) {
            if (show_fps && !fps_text.empty()) {
                const int px = std::clamp(bh / 40, 11, 26);
                if (fps_text != fps_mask.text || px != fps_mask.px) build_mask(fps_mask, fps_text, px, 6);
                const std::string key = fps_text + "|" + std::to_string(px);
                if (!fps_mask.m.empty()) {
                    if (key != fps_key) { panel_image(fps_mask, fps_img, fps_w, fps_h); fps_key = key; ++fps_ver; }
                    const int margin = std::max(6, px / 2);
                    ov[n].px = fps_img.data(); ov[n].w = std::min(fps_w, bw); ov[n].h = std::min(fps_h, bh);
                    ov[n].x = bx + bw - margin - fps_w; ov[n].y = by + margin; ov[n].version = fps_ver;
                    if (ov[n].w == fps_w && ov[n].h == fps_h && ov[n].x >= bx) ++n;
                }
            }
            if (!osd.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= osd_until) osd.clear();
                else {
                    const double left_ms = std::chrono::duration<double, std::milli>(osd_until - now).count();
                    const int px = std::clamp(bh / 32, 13, 34);
                    if (osd != osd_mask.text || px != osd_mask.px || osd_tab != osd_mask.tab) build_mask(osd_mask, osd, px, osd_tab);
                    const std::string key = osd + "|" + std::to_string(px) + "|" + std::to_string(osd_tab);
                    if (!osd_mask.m.empty()) {
                        if (key != osd_key) { panel_image(osd_mask, osd_img, osd_w, osd_h); osd_key = key; ++osd_ver; }
                        const int margin = std::max(6, px / 2);
                        ov[n].px = osd_img.data(); ov[n].w = osd_w; ov[n].h = osd_h;
                        ov[n].x = bx + margin; ov[n].y = by + margin; ov[n].version = osd_ver;
                        ov[n].fade = float(std::min(1.0, left_ms / 300.0));    // fades out over the last 0.3 s
                        if (osd_w <= bw - margin && osd_h <= bh - margin) ++n;
                    }
                }
            }
        }
        const int lk = look == LookSmooth ? 1 : look == LookScanlines ? 2 : 0;
        if (gpu.render(last.empty() ? nullptr : last.data(), fw, fh, bx, by, bw, bh, lk, ov, n)) return true;
        gpu_on = false;                    // the device failed: GDI from now on
        return false;
    }

    void blit(HDC dc) {
        RECT cr; GetClientRect(hwnd, &cr);
        const int cw = cr.right, ch = cr.bottom;
        if (cw <= 0 || ch <= 0) return;
        HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (last.empty()) { FillRect(dc, &cr, black); return; }
        int bx, by, bw, bh;
        box(cw, ch, bx, by, bw, bh);
        if (bw <= 0 || bh <= 0) return;
        scale_into(bw, bh);
        draw_fps(bw, bh);
        draw_osd(bw, bh);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bw; bi.bmiHeader.biHeight = -bh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, bx, by, DWORD(bw), DWORD(bh), 0, 0, 0, UINT(bh), out.data(), &bi, DIB_RGB_COLORS);
        // Black bars around the picture.
        RECT bars[4] = {{0, 0, cw, by}, {0, by + bh, cw, ch}, {0, by, bx, by + bh}, {bx + bw, by, cw, by + bh}};
        for (const RECT& b : bars) if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
    }

    int take_state_request(int& slot) override {
        const int r = state_req; state_req = 0; slot = state_slot; return r;
    }
    void notify(const std::string& message) override { say(message); }
    int render_scale() override { return detail; }

    void present(const uint32_t* px, int w, int h) override {
        if (show_fps) {                   // did the game's picture change? (a sample of its pixels)
            uint64_t hsh = 1469598103934665603ull ^ uint64_t(w);
            const size_t n = size_t(w) * h, step = std::max<size_t>(1, n / 4096) | 1;
            for (size_t i = 0; i < n; i += step) hsh = (hsh ^ px[i]) * 1099511628211ull;
            if (hsh != last_hash) { last_hash = hsh; ++fps_changes; }
        }
        fw = w; fh = h; last.assign(px, px + size_t(w) * h);
        if (gpu_frame()) return;
        HDC dc = GetDC(hwnd); blit(dc); ReleaseDC(hwnd, dc);
    }

    // Sound: waveOut (winmm, part of every Windows install) with a small ring
    // of buffers. Each frame's mix is queued; if the device falls behind, the
    // oldest audio is dropped rather than letting latency grow.
    static constexpr int kBufs = 12;
    HWAVEOUT wave = nullptr;
    WAVEHDR hdr[kBufs] = {};
    std::vector<int16_t> pcm[kBufs];
    int next_buf = 0;
    bool wave_failed = false;

    void open_wave(int rate) {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = DWORD(rate);
        f.wBitsPerSample = 16; f.nBlockAlign = 4; f.nAvgBytesPerSec = DWORD(rate) * 4;
        if (waveOutOpen(&wave, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wave = nullptr; wave_failed = true; }
    }

    void queue_audio(const int16_t* stereo, int frames) override {
        if (!wave && !wave_failed) {
            open_wave(44100);
            // A little silence first so small timing hiccups don't click.
            std::vector<int16_t> zero(size_t(frames) * 2, 0);
            for (int i = 0; i < 3 && wave; ++i) queue_audio(zero.data(), frames);
        }
        if (!wave) return;
        WAVEHDR& h = hdr[next_buf];
        if (h.dwFlags & WHDR_PREPARED) {
            if (!(h.dwFlags & WHDR_DONE)) return;           // device is behind: drop this block
            waveOutUnprepareHeader(wave, &h, sizeof h);
        }
        pcm[next_buf].assign(stereo, stereo + size_t(frames) * 2);
        h = WAVEHDR{};
        h.lpData = reinterpret_cast<LPSTR>(pcm[next_buf].data());
        h.dwBufferLength = DWORD(frames * 4);
        if (waveOutPrepareHeader(wave, &h, sizeof h) == MMSYSERR_NOERROR) waveOutWrite(wave, &h, sizeof h);
        next_buf = (next_buf + 1) % kBufs;
    }

    ~Win32Platform() override {
        if (wave) {
            waveOutReset(wave);
            for (auto& h : hdr) if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wave, &h, sizeof h);
            waveOutClose(wave);
        }
    }

    void fatal(const char* message) override {
        MessageBoxA(hwnd, message, "3DO to PC", MB_OK | MB_ICONERROR);
    }

    void wait_frame(uint32_t hz) override {
        next += std::chrono::microseconds(1000000 / hz);
        auto now = std::chrono::steady_clock::now();
        const double work = std::chrono::duration<double, std::milli>(now - woke).count();
        ++fps_frames; fps_work += work; fps_work_max = std::max(fps_work_max, work);
        if (now - fps_since >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - fps_since).count();
            char b[160];
            std::snprintf(b, sizeof b, "%.0f fps  \xC2\xB7  game %.0f fps  \xC2\xB7  %.1f ms (max %.1f) of %.1f",
                          fps_frames / secs, fps_changes / secs, fps_work / std::max(1, fps_frames), fps_work_max, 1000.0 / hz);
            fps_text = std::string(b) + (gpu_on ? "  \xC2\xB7  D3D11" : "  \xC2\xB7  GDI");
            fps_since = now; fps_frames = fps_changes = 0; fps_work = fps_work_max = 0;
        }
        if (next > now) std::this_thread::sleep_until(next); else next = now;
        woke = std::chrono::steady_clock::now();
    }
};

} // namespace

std::unique_ptr<Platform> make_platform(bool headless) {
    if (headless) return make_null_platform();
    return std::make_unique<Win32Platform>();
}

} // namespace tdort
#endif
