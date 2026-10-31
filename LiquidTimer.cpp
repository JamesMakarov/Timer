// Compilar no Developer Command Prompt for VS:
// cl /std:c++17 /O2 /EHsc LiquidTimer.cpp /link /SUBSYSTEM:WINDOWS

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Tamanhos lógicos (DIPs). O tamanho real em pixels = lógico * escala do monitor.
static constexpr float kPillW = 108.f, kPillH = 28.f;
static constexpr float kCardW = 212.f, kCardH = 178.f;
static constexpr float kAlarmW = 190.f, kAlarmH = 116.f;

// Pares de taps do gaussiano (cada par vira 1 amostra bilinear). Tem que ser igual a PAIRS no HLSL.
static constexpr int kBlurPairs = 10;

// ============================================================================
// SHADERS HLSL
//  Passo 1 (PS_BlurH): blur gaussiano horizontal do desktop -> textura intermediária
//  Passo 2 (PS_Main) : blur vertical + refração + brilho do vidro -> janela
// ============================================================================
const char* g_hlslShader = R"(
#define PAIRS 10

cbuffer GlassParams : register(b0) {
    float2 winPos;      // canto da janela no monitor (px)
    float2 winSize;     // tamanho da janela (px)
    float2 screenSize;  // tamanho da textura do desktop (px)
    float2 texSize;     // tamanho alocado da textura de blur
    float2 regionSize;  // parte da textura de blur em uso (janela + margem)
    float radius;
    float lensEdge;
    float alarmFlash;
    float padPx;        // margem em volta da janela dentro da textura de blur
    float sigma;        // desvio padrão do blur (px)
    float stepPx;       // espaçamento entre taps (px)
};

Texture2D bgTex   : register(t0);
Texture2D blurTex : register(t1);
SamplerState samLinear : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VS_Main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float gauss(float x) {
    float s = x / sigma;
    return exp(-0.5 * s * s);
}

// Junta os taps (2m-1) e (2m) numa única amostra bilinear: metade das leituras, mesmo resultado.
void PairTap(int m, out float off, out float wgt) {
    float o1 = (2.0 * m - 1.0) * stepPx;
    float o2 = (2.0 * m) * stepPx;
    float w1 = gauss(o1);
    float w2 = gauss(o2);
    wgt = w1 + w2;
    off = (o1 * w1 + o2 * w2) / wgt;
}

float4 PS_BlurH(VSOut i) : SV_Target {
    // pixel da região -> pixel correspondente no monitor
    float2 scr = winPos - padPx + i.uv * regionSize;
    float3 sum = bgTex.SampleLevel(samLinear, scr / screenSize, 0).rgb;
    float wsum = 1.0;
    [unroll] for (int m = 1; m <= PAIRS; m++) {
        float o, w;
        PairTap(m, o, w);
        float3 a = bgTex.SampleLevel(samLinear, (scr + float2( o, 0)) / screenSize, 0).rgb;
        float3 b = bgTex.SampleLevel(samLinear, (scr + float2(-o, 0)) / screenSize, 0).rgb;
        sum += (a + b) * w;
        wsum += 2.0 * w;
    }
    return float4(sum / wsum, 1.0);
}

float3 VBlur(float2 q) {
    float2 lo = float2(0.5, 0.5);
    float2 hi = regionSize - 0.5;
    float3 sum = blurTex.SampleLevel(samLinear, clamp(q, lo, hi) / texSize, 0).rgb;
    float wsum = 1.0;
    [unroll] for (int m = 1; m <= PAIRS; m++) {
        float o, w;
        PairTap(m, o, w);
        float3 a = blurTex.SampleLevel(samLinear, clamp(q + float2(0,  o), lo, hi) / texSize, 0).rgb;
        float3 b = blurTex.SampleLevel(samLinear, clamp(q + float2(0, -o), lo, hi) / texSize, 0).rgb;
        sum += (a + b) * w;
        wsum += 2.0 * w;
    }
    return sum / wsum;
}

float sdRoundBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float4 PS_Main(VSOut i) : SV_Target {
    float2 pixelPos = i.uv * winSize;
    float2 center = winSize * 0.5;
    float2 p = pixelPos - center;

    float dist = sdRoundBox(p, center - 1.0, radius);
    float alpha = 1.0 - smoothstep(-0.75, 0.75, dist);
    if (alpha <= 0.001) return float4(0, 0, 0, 0);

    float eps = 1.0;
    float dx = sdRoundBox(p + float2(eps, 0), center - 1.0, radius) - sdRoundBox(p - float2(eps, 0), center - 1.0, radius);
    float dy = sdRoundBox(p + float2(0, eps), center - 1.0, radius) - sdRoundBox(p - float2(0, eps), center - 1.0, radius);
    float2 normal = normalize(float2(dx, dy) + 1e-5);

    float edgeFactor = saturate(1.0 + dist / lensEdge);
    float lensCurve = pow(edgeFactor, 2.35);
    float2 refractPx = -normal * lensCurve * (lensEdge * 0.72);

    // Blur vertical lido na posição refratada (já em pixels da textura de blur)
    float3 glassColor = VBlur(pixelPos + padPx + refractPx);

    // Saturação leve, como o backdrop do iOS
    float luma = dot(glassColor, float3(0.299, 0.587, 0.114));
    glassColor = lerp(float3(luma, luma, luma), glassColor, 1.25);

    // Véu translúcido
    float3 tint = (alarmFlash > 0.5) ? float3(0.52, 0.06, 0.06) : float3(0.12, 0.14, 0.18);
    float tintStrength = (alarmFlash > 0.5) ? 0.30 : 0.22;
    glassColor = lerp(glassColor, tint, tintStrength);

    // Brilho Fresnel nas bordas curvas
    float fresnel = pow(edgeFactor, 3.8) * 0.40;
    float topHighlight = saturate(1.0 - i.uv.y * 2.2) * 0.15;
    glassColor += (fresnel + topHighlight);

    // Contorno especular
    float borderMask = 1.0 - smoothstep(0.0, 1.35, abs(dist + 0.6));
    float diagLight = saturate(1.0 - (i.uv.x * 0.4 + i.uv.y * 0.6));
    float3 borderCol = (alarmFlash > 0.5) ? float3(1.0, 0.75, 0.2) : float3(1.0, 1.0, 1.0);
    glassColor = lerp(glassColor, borderCol, borderMask * (0.35 + 0.45 * diagLight));

    // Dither mínimo contra faixas de cor em fundos com degradê suave
    float n = frac(sin(dot(i.pos.xy, float2(12.9898, 78.233))) * 43758.5453);
    glassColor += (n - 0.5) / 255.0;

    return float4(glassColor * alpha, alpha);
}
)";

// 16 floats = 64 bytes, espelha o cbuffer GlassParams
struct GlassCB {
    float winPos[2];
    float winSize[2];
    float screenSize[2];
    float texSize[2];
    float regionSize[2];
    float radius;
    float lensEdge;
    float alarmFlash;
    float padPx;
    float sigma;
    float stepPx;
};
static_assert(sizeof(GlassCB) == 64, "GlassCB deve ter 64 bytes");

// ============================================================================
// GERENCIADOR DE SONS EM CACHE LOCAL
// ============================================================================
struct SoundManager {
    std::wstring folder;
    std::vector<std::wstring> names = { L"Cristal Apple", L"Sino Suave", L"Pulso Digital", L"Alarme Duplo" };
    std::vector<std::wstring> files;
    int selectedIdx = 0;

    struct Note { float freq; float dur; };

    void Init() {
        wchar_t path[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
        folder = std::wstring(path) + L"\\LiquidTimer";
        CreateDirectoryW(folder.c_str(), nullptr);

        files = {
            folder + L"\\cristal.wav",
            folder + L"\\sino.wav",
            folder + L"\\pulso.wav",
            folder + L"\\duplo.wav"
        };

        GenerateWavIfMissing(files[0], { {880.f, 0.12f}, {1318.5f, 0.15f}, {1760.f, 0.35f} }, 5.0f);
        GenerateWavIfMissing(files[1], { {587.3f, 0.25f}, {880.f, 0.45f} }, 3.5f);
        GenerateWavIfMissing(files[2], { {1046.5f, 0.08f}, {0.f, 0.05f}, {1046.5f, 0.08f}, {0.f, 0.05f}, {1568.f, 0.25f} }, 8.0f);
        GenerateWavIfMissing(files[3], { {784.f, 0.12f}, {987.8f, 0.12f}, {784.f, 0.12f}, {987.8f, 0.25f} }, 6.0f);
    }

    void GenerateWavIfMissing(const std::wstring& filepath, const std::vector<Note>& notes, float decay) {
        if (GetFileAttributesW(filepath.c_str()) != INVALID_FILE_ATTRIBUTES) return;

        const int sr = 44100;
        std::vector<short> pcm;
        for (auto& n : notes) {
            int count = (int)(sr * n.dur);
            for (int i = 0; i < count; ++i) {
                float t = (float)i / sr;
                short val = 0;
                if (n.freq > 0.f) {
                    float env = std::exp(-decay * t);
                    float w = std::sin(2.f * 3.14159265f * n.freq * t) * 0.7f +
                              std::sin(4.f * 3.14159265f * n.freq * t) * 0.2f +
                              std::sin(6.f * 3.14159265f * n.freq * t) * 0.1f;
                    val = (short)std::clamp((int)(24000.f * env * w), -32767, 32767);
                }
                pcm.push_back(val);
            }
        }
        for (int i = 0; i < sr / 4; ++i) pcm.push_back(0);

        std::ofstream f(filepath, std::ios::binary);
        int dataSize = (int)pcm.size() * 2;
        int chunkSize = 36 + dataSize;
        f.write("RIFF", 4); f.write((char*)&chunkSize, 4); f.write("WAVEfmt ", 8);
        int fmtSize = 16; short audioFmt = 1, channels = 1, blockAlign = 2, bits = 16; int byteRate = sr * 2;
        f.write((char*)&fmtSize, 4); f.write((char*)&audioFmt, 2); f.write((char*)&channels, 2);
        f.write((char*)&sr, 4); f.write((char*)&byteRate, 4); f.write((char*)&blockAlign, 2); f.write((char*)&bits, 2);
        f.write("data", 4); f.write((char*)&dataSize, 4);
        f.write((char*)pcm.data(), dataSize);
    }

    void Play(bool loop) {
        DWORD flags = SND_FILENAME | SND_ASYNC;
        if (loop) flags |= SND_LOOP;
        PlaySoundW(files[selectedIdx].c_str(), nullptr, flags);
    }

    void Stop() {
        PlaySoundW(nullptr, nullptr, 0);
    }
};

// ============================================================================
// ESTADO DA APLICAÇÃO E INTERFACE
// ============================================================================
enum class AppState { Pill, Card, Alarm };
enum class TimerMode { Chrono, Timer };

struct AppController {
    HWND hwnd = nullptr;
    AppState state = AppState::Pill;
    TimerMode mode = TimerMode::Chrono;
    bool running = false;

    int elapsedSec = 0;
    int remainSec = 300;
    int inputMin = 5;
    int inputSec = 0;
    int editingField = 0;

    // Tamanhos em unidades lógicas (DIPs). Pixels reais = lógico * scale.
    float curW = kPillW, curH = kPillH;
    float targetW = kPillW, targetH = kPillH;

    // Escala do monitor onde a janela está (DPI efetivo / 96)
    float scale = 1.f;

    float bellPhase = 0.f;
    DWORD alarmStartTick = 0;
    const DWORD maxAlarmMs = 25000;

    bool dragging = false;
    bool movedDuringDrag = false;
    POINT dragStartMouse = { 0, 0 };
    POINT dragStartWin = { 0, 0 };

    SoundManager sounds;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dCtx;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11PixelShader> psBlur;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> sampler;

    // Textura intermediária do blur (resultado do passo horizontal)
    ComPtr<ID3D11Texture2D> blurTex;
    ComPtr<ID3D11RenderTargetView> blurRTV;
    ComPtr<ID3D11ShaderResourceView> blurSRV;
    int blurAllocW = 0, blurAllocH = 0;

    ComPtr<IDXGIOutputDuplication> deskDupl;
    ComPtr<ID3D11Texture2D> bgTexture;
    ComPtr<ID3D11ShaderResourceView> bgSRV;
    int bgW = 0, bgH = 0;   // tamanho real da captura do monitor atual
    HMONITOR currentMon = nullptr;
    RECT monRect = { 0, 0, 1920, 1080 };

    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dCtx;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> fontPill;
    ComPtr<IDWriteTextFormat> fontBig;
    ComPtr<IDWriteTextFormat> fontSmall;
    ComPtr<ID2D1SolidColorBrush> brushWhite;
    ComPtr<ID2D1SolidColorBrush> brushMuted;
    ComPtr<ID2D1SolidColorBrush> brushGreen;
    ComPtr<ID2D1SolidColorBrush> brushBlue;
    ComPtr<ID2D1SolidColorBrush> brushOrange;
    ComPtr<ID2D1SolidColorBrush> brushRed;
    ComPtr<ID2D1SolidColorBrush> brushGlassBtn;

    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
} g_app;

// ============================================================================
// ESCALA POR MONITOR
// ============================================================================
float GetMonitorScale(HMONITOR hMon) {
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx > 0)
        return dx / 96.f;
    return 1.f;
}

int PhysW() { return std::max(1, (int)std::lround(g_app.curW * g_app.scale)); }
int PhysH() { return std::max(1, (int)std::lround(g_app.curH * g_app.scale)); }

// Parâmetros do blur, proporcionais à escala (o blur tem o mesmo tamanho "visual" em qualquer monitor)
struct BlurLayout { float sigma; float step; int pad; };

BlurLayout ComputeBlurLayout(float scale) {
    BlurLayout L;
    L.sigma = 7.f * scale;
    L.step = std::max(1.f, L.sigma * 0.15f);
    float refractMax = 16.f * 0.72f * scale;                        // maior lensEdge * 0.72
    float blurReach = 2.f * kBlurPairs * L.step;                    // alcance vertical do gaussiano
    L.pad = (int)std::ceil(refractMax + blurReach) + 2;
    return L;
}

void EnsureBlurTexture(int needW, int needH) {
    if (g_app.blurTex && g_app.blurAllocW >= needW && g_app.blurAllocH >= needH) return;

    BlurLayout L = ComputeBlurLayout(g_app.scale);
    // Aloca já no tamanho do card para não recriar a cada frame da animação
    int w = std::max(needW, (int)std::ceil(kCardW * g_app.scale) + 2 * L.pad);
    int h = std::max(needH, (int)std::ceil(kCardH * g_app.scale) + 2 * L.pad);

    g_app.blurTex.Reset();
    g_app.blurRTV.Reset();
    g_app.blurSRV.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_app.d3dDevice->CreateTexture2D(&td, nullptr, &g_app.blurTex))) return;
    g_app.d3dDevice->CreateRenderTargetView(g_app.blurTex.Get(), nullptr, &g_app.blurRTV);
    g_app.d3dDevice->CreateShaderResourceView(g_app.blurTex.Get(), nullptr, &g_app.blurSRV);
    g_app.blurAllocW = w;
    g_app.blurAllocH = h;
}

// ============================================================================
// GERENCIAMENTO DE MÚLTIPLOS MONITORES E BORDAS DE TELA
// ============================================================================
void ClampWindowToCurrentMonitor(int targetW, int targetH, bool centerOnCurrent) {
    RECT wr;
    GetWindowRect(g_app.hwnd, &wr);
    POINT center = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };

    HMONITOR hMon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    RECT work = mi.rcWork;

    int newX = centerOnCurrent ? (center.x - targetW / 2) : wr.left;
    int newY = wr.top;
    const int margin = (int)std::lround(6.f * g_app.scale);

    if (newX < work.left + margin) newX = work.left + margin;
    if (newX + targetW > work.right - margin) newX = work.right - targetW - margin;
    if (newY < work.top + margin) newY = work.top + margin;
    if (newY + targetH > work.bottom - margin) newY = work.bottom - targetH - margin;

    SetWindowPos(g_app.hwnd, HWND_TOPMOST, newX, newY, targetW, targetH, SWP_NOACTIVATE);
}

void SetAppState(AppState s) {
    if (g_app.state == s) return;
    g_app.state = s;
    g_app.editingField = 0;

    if (s == AppState::Pill) {
        g_app.targetW = kPillW; g_app.targetH = kPillH;
    } else if (s == AppState::Card) {
        g_app.targetW = kCardW; g_app.targetH = kCardH;
        SetForegroundWindow(g_app.hwnd);
    } else if (s == AppState::Alarm) {
        g_app.targetW = kAlarmW; g_app.targetH = kAlarmH;
    }
}

// ============================================================================
// INICIALIZAÇÃO DO MOTOR GPU (D3D11 + DXGI + DIRECTCOMPOSITION + DIRECT2D)
// ============================================================================
void InitOutputDuplicationForMonitor(HMONITOR hMon) {
    g_app.deskDupl.Reset();
    g_app.bgTexture.Reset();
    g_app.bgSRV.Reset();
    g_app.bgW = g_app.bgH = 0;
    g_app.currentMon = hMon;

    ComPtr<IDXGIDevice> dxgiDev;
    g_app.d3dDevice.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);

    UINT i = 0;
    ComPtr<IDXGIOutput> output;
    while (adapter->EnumOutputs(i++, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);
        if (desc.Monitor == hMon) {
            g_app.monRect = desc.DesktopCoordinates;
            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(output.As(&out1))) {
                out1->DuplicateOutput(g_app.d3dDevice.Get(), &g_app.deskDupl);
            }
            break;
        }
        output.Reset();
    }
}

void ResizeSwapChain(int w, int h) {
    if (!g_app.swapChain || w <= 0 || h <= 0) return;

    // Solta toda referência ao back buffer antes do ResizeBuffers
    g_app.d3dCtx->OMSetRenderTargets(0, nullptr, nullptr);
    g_app.d2dCtx->SetTarget(nullptr);
    g_app.d2dTarget.Reset();
    g_app.rtv.Reset();
    g_app.d3dCtx->Flush();

    if (FAILED(g_app.swapChain->ResizeBuffers(2, (UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) return;

    ComPtr<ID3D11Texture2D> backBuf;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    g_app.d3dDevice->CreateRenderTargetView(backBuf.Get(), nullptr, &g_app.rtv);

    ComPtr<IDXGISurface> surf;
    backBuf.As(&surf);
    // DPI fixo em 96: a escala é aplicada por transformação no RenderFrame
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f
    );
    g_app.d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &g_app.d2dTarget);
    g_app.d2dCtx->SetTarget(g_app.d2dTarget.Get());
    g_app.d2dCtx->SetDpi(96.f, 96.f);
}

bool CompileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(g_hlslShader, strlen(g_hlslShader), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        std::string msg = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize())
                              : std::string("erro desconhecido");
        MessageBoxA(nullptr, msg.c_str(), entry, MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

bool InitGPUEngine(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &g_app.d3dDevice, &fl, &g_app.d3dCtx))) return false;

    ComPtr<IDXGIDevice> dxgiDev;
    g_app.d3dDevice.As(&dxgiDev);

    ComPtr<IDXGIDevice1> dxgiDev1;
    if (SUCCEEDED(dxgiDev.As(&dxgiDev1))) {
        dxgiDev1->SetMaximumFrameLatency(1);
    }

    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = (UINT)PhysW();
    scd.Height = (UINT)PhysH();
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    if (FAILED(factory->CreateSwapChainForComposition(g_app.d3dDevice.Get(), &scd, nullptr, &g_app.swapChain))) return false;

    DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&g_app.dcompDevice));
    g_app.dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &g_app.dcompTarget);
    g_app.dcompDevice->CreateVisual(&g_app.dcompVisual);
    g_app.dcompVisual->SetContent(g_app.swapChain.Get());
    g_app.dcompTarget->SetRoot(g_app.dcompVisual.Get());
    g_app.dcompDevice->Commit();

    ComPtr<ID3DBlob> vsBlob, psBlob, blurBlob;
    if (!CompileShader("VS_Main", "vs_4_0", vsBlob)) return false;
    if (!CompileShader("PS_Main", "ps_4_0", psBlob)) return false;
    if (!CompileShader("PS_BlurH", "ps_4_0", blurBlob)) return false;
    g_app.d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_app.vs);
    g_app.d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_app.ps);
    g_app.d3dDevice->CreatePixelShader(blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(), nullptr, &g_app.psBlur);

    D3D11_BUFFER_DESC cbd = { sizeof(GlassCB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    g_app.d3dDevice->CreateBuffer(&cbd, nullptr, &g_app.cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_app.d3dDevice->CreateSamplerState(&sd, &g_app.sampler);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &g_app.d2dFactory);
    g_app.d2dFactory->CreateDevice(dxgiDev.Get(), &g_app.d2dDevice);
    g_app.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_app.d2dCtx);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_app.dwrite);
    g_app.dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f, L"pt-BR", &g_app.fontPill);
    g_app.fontPill->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_app.fontPill->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    g_app.dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 26.f, L"pt-BR", &g_app.fontBig);
    g_app.fontBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_app.fontBig->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    g_app.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"pt-BR", &g_app.fontSmall);
    g_app.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_app.fontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.96f), &g_app.brushWhite);
    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.55f), &g_app.brushMuted);
    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.82f, 0.35f, 0.9f), &g_app.brushGreen);
    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.9f), &g_app.brushBlue);
    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.62f, 0.04f, 0.9f), &g_app.brushOrange);
    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.27f, 0.23f, 0.9f), &g_app.brushRed);
    g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.18f), &g_app.brushGlassBtn);

    ResizeSwapChain(PhysW(), PhysH());
    InitOutputDuplicationForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    return true;
}

void UpdateDesktopTextureGPU() {
    RECT wr;
    GetWindowRect(g_app.hwnd, &wr);
    HMONITOR hMon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
    if (hMon != g_app.currentMon || !g_app.deskDupl) {
        // Se a duplicação falhar, não tenta de novo a cada frame
        static DWORD lastTry = 0;
        DWORD now = GetTickCount();
        if (hMon != g_app.currentMon || now - lastTry > 500) {
            lastTry = now;
            InitOutputDuplicationForMonitor(hMon);
        }
    }
    if (!g_app.deskDupl) return;

    DXGI_OUTDUPL_FRAME_INFO fi;
    ComPtr<IDXGIResource> res;
    HRESULT hr = g_app.deskDupl->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        InitOutputDuplicationForMonitor(hMon);
        return;
    }
    if (SUCCEEDED(hr) && res) {
        ComPtr<ID3D11Texture2D> frameTex;
        if (SUCCEEDED(res.As(&frameTex))) {
            D3D11_TEXTURE2D_DESC desc;
            frameTex->GetDesc(&desc);
            if (!g_app.bgTexture) {
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                g_app.d3dDevice->CreateTexture2D(&desc, nullptr, &g_app.bgTexture);
                g_app.d3dDevice->CreateShaderResourceView(g_app.bgTexture.Get(), nullptr, &g_app.bgSRV);
                g_app.bgW = (int)desc.Width;
                g_app.bgH = (int)desc.Height;
            }
            g_app.d3dCtx->CopyResource(g_app.bgTexture.Get(), frameTex.Get());
        }
        g_app.deskDupl->ReleaseFrame();
    }
}

// ============================================================================
// RENDERIZAÇÃO DE CADA FRAME
// ============================================================================
std::wstring FormatClock(int totalSec) {
    int m = totalSec / 60;
    int s = totalSec % 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%02d:%02d", m, s);
    return buf;
}

// Garante que a janela tenha o tamanho em pixels correspondente ao tamanho lógico e à escala atual
void SyncWindowSize() {
    int pw = PhysW(), ph = PhysH();
    RECT wr;
    GetWindowRect(g_app.hwnd, &wr);
    if (wr.right - wr.left == pw && wr.bottom - wr.top == ph) return;
    ClampWindowToCurrentMonitor(pw, ph, true);
    ResizeSwapChain(pw, ph);
}

void RenderFrame() {
    // 1. Escala do monitor onde a janela está agora (muda ao arrastar para outra tela)
    HMONITOR hMonNow = MonitorFromWindow(g_app.hwnd, MONITOR_DEFAULTTONEAREST);
    float newScale = GetMonitorScale(hMonNow);
    if (std::abs(newScale - g_app.scale) > 0.001f) g_app.scale = newScale;

    // 2. Animação do tamanho lógico
    float dw = g_app.targetW - g_app.curW;
    float dh = g_app.targetH - g_app.curH;
    if (std::abs(dw) > 0.4f || std::abs(dh) > 0.4f) {
        g_app.curW += dw * 0.28f;
        g_app.curH += dh * 0.28f;
    } else {
        g_app.curW = g_app.targetW;
        g_app.curH = g_app.targetH;
    }

    // 3. Aplica tamanho em pixels (cobre animação e troca de escala)
    SyncWindowSize();

    UpdateDesktopTextureGPU();

    RECT wr;
    GetWindowRect(g_app.hwnd, &wr);
    const float s = g_app.scale;
    const float pw = (float)(wr.right - wr.left);   // pixels reais
    const float ph = (float)(wr.bottom - wr.top);
    const float w = pw / s;                         // unidades lógicas (usadas na UI)
    const float h = ph / s;

    BlurLayout L = ComputeBlurLayout(s);
    int regionW = (int)pw + 2 * L.pad;
    int regionH = (int)ph + 2 * L.pad;
    EnsureBlurTexture(regionW, regionH);

    if (!g_app.rtv || !g_app.blurRTV) return;

    float scrW = g_app.bgW > 0 ? (float)g_app.bgW : (float)(g_app.monRect.right - g_app.monRect.left);
    float scrH = g_app.bgH > 0 ? (float)g_app.bgH : (float)(g_app.monRect.bottom - g_app.monRect.top);

    bool isPill = (h <= 34.f);

    GlassCB cbData = {};
    cbData.winPos[0] = (float)(wr.left - g_app.monRect.left);
    cbData.winPos[1] = (float)(wr.top - g_app.monRect.top);
    cbData.winSize[0] = pw;
    cbData.winSize[1] = ph;
    cbData.screenSize[0] = scrW > 0 ? scrW : 1920.f;
    cbData.screenSize[1] = scrH > 0 ? scrH : 1080.f;
    cbData.texSize[0] = (float)g_app.blurAllocW;
    cbData.texSize[1] = (float)g_app.blurAllocH;
    cbData.regionSize[0] = (float)regionW;
    cbData.regionSize[1] = (float)regionH;
    cbData.radius = isPill ? (ph * 0.5f) : 20.f * s;
    cbData.lensEdge = (isPill ? 9.5f : 16.0f) * s;
    cbData.alarmFlash = (g_app.state == AppState::Alarm) ? 1.0f : 0.0f;
    cbData.padPx = (float)L.pad;
    cbData.sigma = L.sigma;
    cbData.stepPx = L.step;

    g_app.d3dCtx->UpdateSubresource(g_app.cb.Get(), 0, nullptr, &cbData, 0, 0);

    // Estado comum aos dois passos
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    g_app.d3dCtx->PSSetShaderResources(0, 2, nullSRVs);
    g_app.d3dCtx->VSSetShader(g_app.vs.Get(), nullptr, 0);
    g_app.d3dCtx->PSSetConstantBuffers(0, 1, g_app.cb.GetAddressOf());
    g_app.d3dCtx->PSSetSamplers(0, 1, g_app.sampler.GetAddressOf());
    g_app.d3dCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // --- Passo 1: blur horizontal do desktop -> textura intermediária ---
    g_app.d3dCtx->OMSetRenderTargets(1, g_app.blurRTV.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vpBlur = { 0.f, 0.f, (float)regionW, (float)regionH, 0.f, 1.f };
    g_app.d3dCtx->RSSetViewports(1, &vpBlur);
    g_app.d3dCtx->PSSetShader(g_app.psBlur.Get(), nullptr, 0);
    g_app.d3dCtx->PSSetShaderResources(0, 1, g_app.bgSRV.GetAddressOf());
    g_app.d3dCtx->Draw(3, 0);

    // --- Passo 2: blur vertical + refração + brilho -> janela ---
    g_app.d3dCtx->OMSetRenderTargets(1, g_app.rtv.GetAddressOf(), nullptr);
    float clearCol[4] = { 0.f, 0.f, 0.f, 0.f };
    g_app.d3dCtx->ClearRenderTargetView(g_app.rtv.Get(), clearCol);
    D3D11_VIEWPORT vp = { 0.f, 0.f, pw, ph, 0.f, 1.f };
    g_app.d3dCtx->RSSetViewports(1, &vp);
    g_app.d3dCtx->PSSetShader(g_app.ps.Get(), nullptr, 0);
    g_app.d3dCtx->PSSetShaderResources(1, 1, g_app.blurSRV.GetAddressOf());
    g_app.d3dCtx->Draw(3, 0);

    // --- Interface vetorial (Direct2D), desenhada em unidades lógicas ---
    const D2D1::Matrix3x2F scaleMat = D2D1::Matrix3x2F::Scale(s, s);
    g_app.d2dCtx->BeginDraw();
    g_app.d2dCtx->SetTransform(scaleMat);

    int displaySeconds = (g_app.mode == TimerMode::Chrono) ? g_app.elapsedSec : g_app.remainSec;
    std::wstring timeStr = FormatClock(displaySeconds);

    if (g_app.state == AppState::Pill && h < 45.f) {
        ID2D1SolidColorBrush* dotBrush = g_app.running
            ? (g_app.mode == TimerMode::Chrono ? g_app.brushGreen.Get() : g_app.brushBlue.Get())
            : g_app.brushOrange.Get();
        g_app.d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(16.f, h * 0.5f), 3.8f, 3.8f), dotBrush);
        g_app.d2dCtx->DrawTextW(timeStr.c_str(), (UINT32)timeStr.size(), g_app.fontPill.Get(),
            D2D1::RectF(22.f, 0.f, w - 8.f, h), g_app.brushWhite.Get());
    }
    else if (g_app.state == AppState::Card && h > 120.f) {
        D2D1_ROUNDED_RECT tabCrono = D2D1::RoundedRect(D2D1::RectF(12, 10, 86, 32), 7, 7);
        D2D1_ROUNDED_RECT tabTimer = D2D1::RoundedRect(D2D1::RectF(92, 10, 166, 32), 7, 7);
        D2D1_ROUNDED_RECT btnClose = D2D1::RoundedRect(D2D1::RectF(w - 32, 10, w - 12, 30), 10, 10);

        if (g_app.mode == TimerMode::Chrono) g_app.d2dCtx->FillRoundedRectangle(tabCrono, g_app.brushGlassBtn.Get());
        else g_app.d2dCtx->FillRoundedRectangle(tabTimer, g_app.brushGlassBtn.Get());
        g_app.d2dCtx->FillRoundedRectangle(btnClose, g_app.brushRed.Get());

        g_app.d2dCtx->DrawTextW(L"Crono", 5, g_app.fontSmall.Get(), tabCrono.rect, g_app.brushWhite.Get());
        g_app.d2dCtx->DrawTextW(L"Timer", 5, g_app.fontSmall.Get(), tabTimer.rect, g_app.brushWhite.Get());
        g_app.d2dCtx->DrawTextW(L"\u2715", 1, g_app.fontSmall.Get(), btnClose.rect, g_app.brushWhite.Get());

        g_app.d2dCtx->DrawTextW(timeStr.c_str(), (UINT32)timeStr.size(), g_app.fontBig.Get(),
            D2D1::RectF(12, 36, w - 12, 78), g_app.brushWhite.Get());

        if (g_app.mode == TimerMode::Timer) {
            D2D1_ROUNDED_RECT boxMin = D2D1::RoundedRect(D2D1::RectF(36, 80, 96, 102), 6, 6);
            D2D1_ROUNDED_RECT boxSec = D2D1::RoundedRect(D2D1::RectF(116, 80, 176, 102), 6, 6);
            g_app.d2dCtx->FillRoundedRectangle(boxMin, g_app.brushGlassBtn.Get());
            g_app.d2dCtx->FillRoundedRectangle(boxSec, g_app.brushGlassBtn.Get());

            if (g_app.editingField == 1) g_app.d2dCtx->DrawRoundedRectangle(boxMin, g_app.brushBlue.Get(), 1.5f);
            if (g_app.editingField == 2) g_app.d2dCtx->DrawRoundedRectangle(boxSec, g_app.brushBlue.Get(), 1.5f);

            wchar_t bMin[16], bSec[16];
            swprintf_s(bMin, L"%02d min", g_app.inputMin);
            swprintf_s(bSec, L"%02d seg", g_app.inputSec);
            g_app.d2dCtx->DrawTextW(bMin, (UINT32)wcslen(bMin), g_app.fontSmall.Get(), boxMin.rect, g_app.brushWhite.Get());
            g_app.d2dCtx->DrawTextW(L":", 1, g_app.fontSmall.Get(), D2D1::RectF(96, 80, 116, 102), g_app.brushWhite.Get());
            g_app.d2dCtx->DrawTextW(bSec, (UINT32)wcslen(bSec), g_app.fontSmall.Get(), boxSec.rect, g_app.brushWhite.Get());

            D2D1_ROUNDED_RECT boxSound = D2D1::RoundedRect(D2D1::RectF(12, 108, w - 40, 130), 6, 6);
            D2D1_ROUNDED_RECT btnPreview = D2D1::RoundedRect(D2D1::RectF(w - 34, 108, w - 12, 130), 6, 6);
            g_app.d2dCtx->FillRoundedRectangle(boxSound, g_app.brushGlassBtn.Get());
            g_app.d2dCtx->FillRoundedRectangle(btnPreview, g_app.brushGlassBtn.Get());

            // U+1F514 não cabe em um wchar_t: precisa do par substituto (\U vira 2 unidades UTF-16)
            std::wstring sndLabel = std::wstring(L"\U0001F514 ") + g_app.sounds.names[g_app.sounds.selectedIdx];
            g_app.d2dCtx->DrawTextW(sndLabel.c_str(), (UINT32)sndLabel.size(), g_app.fontSmall.Get(), boxSound.rect, g_app.brushWhite.Get());
            g_app.d2dCtx->DrawTextW(L"\u266A", 1, g_app.fontSmall.Get(), btnPreview.rect, g_app.brushWhite.Get());
        } else {
            g_app.d2dCtx->DrawTextW(L"Clique fora para retrair", 24, g_app.fontSmall.Get(),
                D2D1::RectF(12, 92, w - 12, 120), g_app.brushMuted.Get());
        }

        D2D1_ROUNDED_RECT btnStart = D2D1::RoundedRect(D2D1::RectF(12, h - 38, w * 0.5f - 4, h - 10), 8, 8);
        D2D1_ROUNDED_RECT btnReset = D2D1::RoundedRect(D2D1::RectF(w * 0.5f + 4, h - 38, w - 12, h - 10), 8, 8);

        g_app.d2dCtx->FillRoundedRectangle(btnStart, g_app.running ? g_app.brushOrange.Get() : g_app.brushGreen.Get());
        g_app.d2dCtx->FillRoundedRectangle(btnReset, g_app.brushGlassBtn.Get());

        const wchar_t* startTxt = g_app.running ? L"Pausar" : L"Iniciar";
        g_app.d2dCtx->DrawTextW(startTxt, (UINT32)wcslen(startTxt), g_app.fontSmall.Get(), btnStart.rect, g_app.brushWhite.Get());
        g_app.d2dCtx->DrawTextW(L"Zerar", 5, g_app.fontSmall.Get(), btnReset.rect, g_app.brushWhite.Get());
    }
    else if (g_app.state == AppState::Alarm) {
        g_app.bellPhase += 0.25f;
        float angle = std::sin(g_app.bellPhase) * 22.f;

        g_app.d2dCtx->DrawTextW(L"Tempo Esgotado!", 15, g_app.fontPill.Get(),
            D2D1::RectF(10, h - 72, w - 10, h - 46), g_app.brushWhite.Get());

        // Sino vetorial balançando (rotação no espaço lógico, depois escala)
        D2D1_POINT_2F bellPivot = D2D1::Point2F(w * 0.5f, 26.f);
        g_app.d2dCtx->SetTransform(D2D1::Matrix3x2F::Rotation(angle, bellPivot) * scaleMat);

        ComPtr<ID2D1PathGeometry> bellGeo;
        g_app.d2dFactory->CreatePathGeometry(&bellGeo);
        ComPtr<ID2D1GeometrySink> sink;
        bellGeo->Open(&sink);
        sink->BeginFigure(D2D1::Point2F(w * 0.5f, 12.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(w * 0.5f - 14.f, 12.f),
            D2D1::Point2F(w * 0.5f - 16.f, 32.f),
            D2D1::Point2F(w * 0.5f - 18.f, 36.f)
        ));
        sink->AddLine(D2D1::Point2F(w * 0.5f + 18.f, 36.f));
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(w * 0.5f + 16.f, 32.f),
            D2D1::Point2F(w * 0.5f + 14.f, 12.f),
            D2D1::Point2F(w * 0.5f, 12.f)
        ));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();

        ComPtr<ID2D1SolidColorBrush> brushGold, brushClapper;
        g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.84f, 0.0f, 0.95f), &brushGold);
        g_app.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.62f, 0.04f, 0.95f), &brushClapper);

        g_app.d2dCtx->FillGeometry(bellGeo.Get(), brushGold.Get());
        g_app.d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w * 0.5f, 39.f), 4.0f, 4.0f), brushClapper.Get());

        g_app.d2dCtx->SetTransform(scaleMat);

        D2D1_ROUNDED_RECT btnStop = D2D1::RoundedRect(D2D1::RectF(16, h - 42, w - 16, h - 12), 9, 9);
        g_app.d2dCtx->FillRoundedRectangle(btnStop, g_app.brushRed.Get());
        g_app.d2dCtx->DrawTextW(L"OK, Parar Alarme", 16, g_app.fontSmall.Get(), btnStop.rect, g_app.brushWhite.Get());

        if (GetTickCount() - g_app.alarmStartTick >= g_app.maxAlarmMs) {
            g_app.sounds.Stop();
            SetAppState(AppState::Pill);
        }
    }

    g_app.d2dCtx->EndDraw();
    g_app.swapChain->Present(1, 0);
}

// ============================================================================
// INTERAÇÃO DE MOUSE, TECLADO E TIMERS
// ============================================================================
bool PtInRectF(POINT pt, float l, float t, float r, float b) {
    return pt.x >= l && pt.x <= r && pt.y >= t && pt.y <= b;
}

// pt, w e h em unidades lógicas
void HandleCardClick(POINT pt, float w, float h) {
    if (g_app.state == AppState::Alarm) {
        if (PtInRectF(pt, 16, h - 42, w - 16, h - 12)) {
            g_app.sounds.Stop();
            g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
            SetAppState(AppState::Pill);
        }
        return;
    }

    if (PtInRectF(pt, w - 32, 10, w - 12, 30)) {
        g_app.sounds.Stop();
        PostQuitMessage(0);
        return;
    }
    if (PtInRectF(pt, 12, 10, 86, 32)) {
        g_app.mode = TimerMode::Chrono; g_app.running = false; g_app.editingField = 0; return;
    }
    if (PtInRectF(pt, 92, 10, 166, 32)) {
        g_app.mode = TimerMode::Timer; g_app.running = false;
        g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
        return;
    }
    if (g_app.mode == TimerMode::Timer) {
        if (PtInRectF(pt, 36, 80, 96, 102)) { g_app.editingField = 1; g_app.inputMin = 0; return; }
        if (PtInRectF(pt, 116, 80, 176, 102)) { g_app.editingField = 2; g_app.inputSec = 0; return; }
        if (PtInRectF(pt, 12, 108, w - 40, 130)) {
            g_app.sounds.selectedIdx = (g_app.sounds.selectedIdx + 1) % (int)g_app.sounds.names.size();
            return;
        }
        if (PtInRectF(pt, w - 34, 108, w - 12, 130)) {
            g_app.sounds.Play(false);
            return;
        }
    }
    if (PtInRectF(pt, 12, h - 38, w * 0.5f - 4, h - 10)) {
        g_app.editingField = 0;
        if (!g_app.running && g_app.mode == TimerMode::Timer && g_app.remainSec <= 0) {
            g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
        }
        g_app.running = !g_app.running;
        return;
    }
    if (PtInRectF(pt, w * 0.5f + 4, h - 38, w - 12, h - 10)) {
        g_app.running = false;
        g_app.editingField = 0;
        g_app.elapsedSec = 0;
        g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
        return;
    }

    SetAppState(AppState::Pill);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g_app.state == AppState::Card) {
            SetAppState(AppState::Pill);
        }
        // O DefWindowProc é quem dá o foco de teclado à janela (necessário para o WM_CHAR)
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_DPICHANGED:
        // O tamanho é ajustado no RenderFrame a partir da escala do monitor; ignora o retângulo sugerido
        return 0;

    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        g_app.dragging = true;
        g_app.movedDuringDrag = false;
        GetCursorPos(&g_app.dragStartMouse);
        RECT r; GetWindowRect(hwnd, &r);
        g_app.dragStartWin = { r.left, r.top };
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_app.dragging) {
            POINT cur; GetCursorPos(&cur);
            int dx = cur.x - g_app.dragStartMouse.x;
            int dy = cur.y - g_app.dragStartMouse.y;
            if (std::abs(dx) > 4 || std::abs(dy) > 4) g_app.movedDuringDrag = true;
            if (g_app.movedDuringDrag) {
                SetWindowPos(hwnd, HWND_TOPMOST, g_app.dragStartWin.x + dx, g_app.dragStartWin.y + dy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        ReleaseCapture();
        bool wasMoved = g_app.movedDuringDrag;
        g_app.dragging = false;
        g_app.movedDuringDrag = false;

        if (!wasMoved) {
            // clique em pixels -> unidades lógicas
            POINT pt = { (LONG)(GET_X_LPARAM(lp) / g_app.scale), (LONG)(GET_Y_LPARAM(lp) / g_app.scale) };
            if (g_app.state == AppState::Pill) SetAppState(AppState::Card);
            else HandleCardClick(pt, g_app.curW, g_app.curH);
        } else {
            ClampWindowToCurrentMonitor(PhysW(), PhysH(), false);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (g_app.state == AppState::Card && g_app.mode == TimerMode::Timer && !g_app.running) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
            POINT spt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &spt);
            POINT pt = { (LONG)(spt.x / g_app.scale), (LONG)(spt.y / g_app.scale) };
            if (PtInRectF(pt, 36, 80, 96, 102)) g_app.inputMin = std::clamp(g_app.inputMin + delta, 0, 999);
            else if (PtInRectF(pt, 116, 80, 176, 102)) g_app.inputSec = std::clamp(g_app.inputSec + delta, 0, 59);
            g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
        }
        return 0;
    }

    case WM_CHAR: {
        if (g_app.state == AppState::Card && g_app.mode == TimerMode::Timer && !g_app.running && g_app.editingField > 0) {
            if (wp >= '0' && wp <= '9') {
                int digit = (int)(wp - '0');
                if (g_app.editingField == 1) g_app.inputMin = std::clamp((g_app.inputMin * 10 + digit) % 1000, 0, 999);
                if (g_app.editingField == 2) g_app.inputSec = std::clamp((g_app.inputSec * 10 + digit) % 100, 0, 59);
                g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
            } else if (wp == VK_BACK) {
                if (g_app.editingField == 1) g_app.inputMin /= 10;
                if (g_app.editingField == 2) g_app.inputSec /= 10;
                g_app.remainSec = g_app.inputMin * 60 + g_app.inputSec;
            } else if (wp == VK_RETURN) {
                g_app.editingField = 0;
            }
        }
        return 0;
    }

    case WM_TIMER: {
        if (wp == 2 && g_app.running) {
            if (g_app.mode == TimerMode::Chrono) {
                g_app.elapsedSec++;
            } else {
                if (g_app.remainSec > 0) {
                    g_app.remainSec--;
                    if (g_app.remainSec == 0) {
                        g_app.running = false;
                        g_app.alarmStartTick = GetTickCount();
                        SetAppState(AppState::Alarm);
                        g_app.sounds.Play(true);
                    }
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        g_app.sounds.Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_app.sounds.Init();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LiquidGlassPillClass";
    RegisterClassExW(&wc);

    // Escala inicial: monitor primário (onde a pílula nasce)
    POINT origin = { 0, 0 };
    g_app.scale = GetMonitorScale(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int startX = (scrW - PhysW()) / 2;
    int startY = (int)std::lround(12.f * g_app.scale);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    g_app.hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"LiquidTimer", WS_POPUP,
        startX, startY, PhysW(), PhysH(), nullptr, nullptr, hInst, nullptr);

    SetWindowDisplayAffinity(g_app.hwnd, WDA_EXCLUDEFROMCAPTURE);

    if (!InitGPUEngine(g_app.hwnd)) return -1;

    ShowWindow(g_app.hwnd, SW_SHOW);
    SetTimer(g_app.hwnd, 2, 1000, nullptr);

    MSG msg = {};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        RenderFrame();
    }

    return 0;
}