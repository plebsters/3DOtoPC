// platform_win32.cpp - Win32 window (GDI blit), XInput pad, keyboard, frame pacing.
// Deliberately dependency-free: only Windows SDK libraries, nothing downloaded.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <mmsystem.h>
#include "tdort/platform.h"
#include <chrono>
#include <thread>
#include <vector>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "winmm.lib")

namespace tdort {

std::unique_ptr<Platform> make_null_platform();

namespace {

struct Win32Platform : Platform {
    HWND hwnd = nullptr;
    bool quit = false;
    uint32_t keys = 0;
    std::vector<uint32_t> last;
    int fw = 320, fh = 240;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Win32Platform*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
        case WM_CLOSE: if (self) self->quit = true; return 0;
        case WM_KEYDOWN: case WM_KEYUP:
            if (self) self->key(uint32_t(w), m == WM_KEYDOWN);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            if (self) self->blit(dc);
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
        }
        keys = down ? (keys | bit) : (keys & ~bit);
    }

    bool open(const char* title, int w, int h) override {
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"tdort"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        RECT r{0, 0, w * 3, h * 3};
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

    void blit(HDC dc) {
        if (last.empty()) return;
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = fw; bi.bmiHeader.biHeight = -fh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        RECT cr; GetClientRect(hwnd, &cr);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, cr.right, cr.bottom, 0, 0, fw, fh, last.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }

    void present(const uint32_t* px, int w, int h) override {
        fw = w; fh = h; last.assign(px, px + size_t(w) * h);
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
        if (next > now) std::this_thread::sleep_until(next); else next = now;
    }
};

} // namespace

std::unique_ptr<Platform> make_platform(bool headless) {
    if (headless) return make_null_platform();
    return std::make_unique<Win32Platform>();
}

} // namespace tdort
#endif
