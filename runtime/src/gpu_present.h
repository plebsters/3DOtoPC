// gpu_present.h - shows the game picture through Direct3D 11 (Windows only).
//
// The picture (320x240, or finer with the detail setting) is uploaded as one
// texture each frame and the graphics card scales it into the picture box,
// with the chosen look (sharp, smooth or scanlines); the on-screen message and
// frame counter are small textures blended on top. That replaces scaling and
// filtering every screen pixel on the CPU and the GDI copy, which at 1440p or
// 4K was a large part of each frame.
//
// Nothing is linked: d3d11.dll and d3dcompiler_47.dll are loaded at run time,
// and if anything is missing or fails (old Windows, no driver, a device lost
// later) the caller falls back to its GDI path.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tdort {

class GpuPresenter {
public:
    struct Overlay {                       // premultiplied BGRA, w x h, drawn at (x, y) of the client area
        const uint32_t* px = nullptr;
        int w = 0, h = 0, x = 0, y = 0;
        uint64_t version = 0;              // changes when the pixels change
        float fade = 1.0f;
    };

    ~GpuPresenter() { release(); }

    // false: no Direct3D 11 here; use GDI.
    bool init(HWND hwnd) {
        hwnd_ = hwnd;
        HMODULE d3d = LoadLibraryW(L"d3d11.dll");
        HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!d3d || !comp) return false;
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
            reinterpret_cast<void*>(GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain")));
        compile_ = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(comp, "D3DCompile")));
        if (!create || !compile_) return false;

        RECT cr; GetClientRect(hwnd, &cr);
        bw_ = std::max<LONG>(1, cr.right); bh_ = std::max<LONG>(1, cr.bottom);
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = UINT(bw_); sd.BufferDesc.Height = UINT(bh_);
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got{};
        // Flip model (Windows 10 and later): no copy through the desktop compositor.
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &swap_, &dev_, &got, &ctx_);
        if (FAILED(hr)) {                  // Windows 7 / 8: the older blit model
            sd.BufferCount = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &swap_, &dev_, &got, &ctx_);
        }
        if (FAILED(hr) || !swap_ || !dev_ || !ctx_) { release(); return false; }
        // Alt+Enter is the window's own fullscreen toggle, not DXGI's.
        IDXGIFactory* fac = nullptr;
        if (SUCCEEDED(swap_->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&fac))) && fac) {
            fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            fac->Release();
        }
        if (!make_target() || !make_pipeline()) { release(); return false; }
        return true;
    }

    // Draw a frame. box: the picture's rectangle in the client area. look:
    // 0 sharp, 1 smooth, 2 scanlines. false: the device failed; use GDI from now on.
    bool render(const uint32_t* px, int fw, int fh, int bx, int by, int bw, int bh, int look,
                const Overlay* overlays, int n_overlays) {
        if (!dev_) return false;
        RECT cr; GetClientRect(hwnd_, &cr);
        const LONG cw = std::max<LONG>(1, cr.right), ch = std::max<LONG>(1, cr.bottom);
        if (cw != bw_ || ch != bh_) {      // the window changed size
            if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            if (FAILED(swap_->ResizeBuffers(0, UINT(cw), UINT(ch), DXGI_FORMAT_UNKNOWN, 0))) { release(); return false; }
            bw_ = cw; bh_ = ch;
            if (!make_target()) { release(); return false; }
        }
        const float black[4] = {0, 0, 0, 1};
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        ctx_->ClearRenderTargetView(rtv_, black);
        if (px && fw > 0 && fh > 0 && bw > 0 && bh > 0) {
            if (!upload(frame_, frame_srv_, fw_, fh_, px, fw, fh)) { release(); return false; }
            Consts c{};
            c.box[0] = float(bx); c.box[1] = float(by); c.box[2] = float(bw); c.box[3] = float(bh);
            c.info[0] = float(look); c.info[1] = 1.0f;
            draw(frame_srv_, look == 1 ? linear_ : point_, ps_picture_, bx, by, bw, bh, c, false);
        }
        for (int i = 0; i < n_overlays; ++i) {
            const Overlay& o = overlays[i];
            if (!o.px || o.w <= 0 || o.h <= 0) continue;
            Tex& t = ovl_[i < 2 ? i : 1];
            if (t.version != o.version || t.w != o.w || t.h != o.h) {
                if (!upload(t.tex, t.srv, t.w, t.h, o.px, o.w, o.h)) { release(); return false; }
                t.version = o.version;
            }
            Consts c{};
            c.box[0] = float(o.x); c.box[1] = float(o.y); c.box[2] = float(o.w); c.box[3] = float(o.h);
            c.info[1] = o.fade;
            draw(t.srv, point_, ps_overlay_, o.x, o.y, o.w, o.h, c, true);
        }
        const HRESULT hr = swap_->Present(0, 0);
        if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) { release(); return false; }
        return true;
    }

    bool ok() const { return dev_ != nullptr; }

private:
    struct Consts { float box[4]; float info[4]; };   // box: x, y, w, h; info: look, fade
    struct Tex { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; uint64_t version = ~0ull; };

    HWND hwnd_ = nullptr;
    pD3DCompile compile_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_picture_ = nullptr;
    ID3D11PixelShader* ps_overlay_ = nullptr;
    ID3D11SamplerState* point_ = nullptr;
    ID3D11SamplerState* linear_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11Buffer* cbuf_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11Texture2D* frame_ = nullptr;
    ID3D11ShaderResourceView* frame_srv_ = nullptr;
    int fw_ = 0, fh_ = 0;
    Tex ovl_[2];
    LONG bw_ = 0, bh_ = 0;                 // back buffer size

    template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void release() {
        for (Tex& t : ovl_) { rel(t.srv); rel(t.tex); t.version = ~0ull; t.w = t.h = 0; }
        rel(frame_srv_); rel(frame_); fw_ = fh_ = 0;
        rel(raster_); rel(cbuf_); rel(blend_); rel(linear_); rel(point_);
        rel(ps_overlay_); rel(ps_picture_); rel(vs_); rel(rtv_);
        if (ctx_) ctx_->ClearState();
        rel(ctx_); rel(swap_); rel(dev_);
    }

    bool make_target() {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
        const HRESULT hr = dev_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
        return SUCCEEDED(hr);
    }

    ID3DBlob* compile(const char* src, const char* entry, const char* target) {
        ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
        const HRESULT hr = compile_(src, std::strlen(src), "tdort", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (err) err->Release();
        if (FAILED(hr)) { if (code) code->Release(); return nullptr; }
        return code;
    }

    bool make_pipeline() {
        // One triangle covering the viewport; the viewport is the picture box
        // (or an overlay's rectangle), so uv runs 0..1 across it.
        static const char* kSrc = R"(
cbuffer C : register(b0) { float4 box; float4 info; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_picture(V i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;
    // Scanlines: the last quarter of each 3DO line at half brightness (needs
    // at least 2 screen lines per 3DO line to show).
    if (info.x == 2 && box.w >= 480) {
        float y = floor(i.pos.y - box.y);
        float fl = floor(y * 960.0 / box.w);
        if (fl - 4.0 * floor(fl / 4.0) >= 3.0) c *= 0.5;
    }
    return float4(c, 1);
}
float4 ps_overlay(V i) : SV_Target { return tex.Sample(smp, i.uv) * info.y; }
)";
        ID3DBlob* vsb = compile(kSrc, "vs", "vs_4_0");
        ID3DBlob* psb = compile(kSrc, "ps_picture", "ps_4_0");
        ID3DBlob* pob = compile(kSrc, "ps_overlay", "ps_4_0");
        bool ok = vsb && psb && pob
            && SUCCEEDED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))
            && SUCCEEDED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_picture_))
            && SUCCEEDED(dev_->CreatePixelShader(pob->GetBufferPointer(), pob->GetBufferSize(), nullptr, &ps_overlay_));
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        if (pob) pob->Release();
        if (!ok) return false;

        D3D11_SAMPLER_DESC s{};
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(dev_->CreateSamplerState(&s, &point_))) return false;
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(dev_->CreateSamplerState(&s, &linear_))) return false;

        D3D11_BLEND_DESC b{};             // premultiplied alpha
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev_->CreateBlendState(&b, &blend_))) return false;

        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
        if (FAILED(dev_->CreateRasterizerState(&r, &raster_))) return false;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Consts); cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev_->CreateBuffer(&cb, nullptr, &cbuf_));
    }

    // Copy w x h pixels into tex (made or remade at that size).
    bool upload(ID3D11Texture2D*& tex, ID3D11ShaderResourceView*& srv, int& tw, int& th, const uint32_t* px, int w, int h) {
        if (!tex || tw != w || th != h) {
            rel(srv); rel(tex);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = UINT(w); d.Height = UINT(h); d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex))) return false;
            if (FAILED(dev_->CreateShaderResourceView(tex, nullptr, &srv))) return false;
            tw = w; th = h;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        for (int y = 0; y < h; ++y)
            std::memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, px + size_t(y) * w, size_t(w) * 4);
        ctx_->Unmap(tex, 0);
        return true;
    }

    void draw(ID3D11ShaderResourceView* srv, ID3D11SamplerState* smp, ID3D11PixelShader* ps,
              int x, int y, int w, int h, const Consts& c, bool blend) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cbuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { std::memcpy(m.pData, &c, sizeof c); ctx_->Unmap(cbuf_, 0); }
        D3D11_VIEWPORT vp{float(x), float(y), float(w), float(h), 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(raster_);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_, nullptr, 0);
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cbuf_);
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->PSSetSamplers(0, 1, &smp);
        const float f[4] = {0, 0, 0, 0};
        ctx_->OMSetBlendState(blend ? blend_ : nullptr, f, 0xFFFFFFFF);
        ctx_->Draw(3, 0);
    }
};

} // namespace tdort
