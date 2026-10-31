// Slipframe Low Latency: NVIDIA's low latency mode for Unity's D3D12 players, on the public NVAPI.
//
// It needs no proxy DLLs and no Streamline. Unity already hands a native plugin its D3D12 device, and NVAPI's latency calls
// take that device, so this is a normal plugin in a normal Plugins folder.
//
//   main thread    the sleep at the start of the frame, the simulation markers
//   render thread  the render-submit markers, from plugin events
//   present        Present and Present1 are wrapped on the swap chain's v-table, for the present markers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"

#include "nvapi.h"

namespace
{
IUnityInterfaces* s_unity;
IUnityGraphics* s_graphics;
IUnityGraphicsD3D12v7* s_d3d;
ID3D12Device* s_device;
bool s_available;
char s_reason[128] = "NOT STARTED";
std::atomic<int> s_wantMode{1};      // 0 off, 1 on, 2 on + boost
int s_appliedMode = -1;
std::atomic<uint32_t> s_minimumIntervalUs{0};   // a frame cap the driver paces to; 0: none
uint32_t s_appliedInterval = 0;
std::atomic<uint64_t> s_submitted{0};   // the last frame whose render submit ended: the one Present shows

enum Event : int { kRenderSubmitStart = 0x534C, kRenderSubmitEnd };

void Mark(uint64_t frame, NV_LATENCY_MARKER_TYPE type)
{
    if (!s_available) return;
    NV_LATENCY_MARKER_PARAMS p{};
    p.version = NV_LATENCY_MARKER_PARAMS_VER;
    p.frameID = frame;
    p.markerType = type;
    NvAPI_D3D_SetLatencyMarker(s_device, &p);
}

void ApplyMode()
{
    int mode = s_wantMode.load();
    uint32_t interval = s_minimumIntervalUs.load();
    if (mode == s_appliedMode && interval == s_appliedInterval) return;
    NV_SET_SLEEP_MODE_PARAMS p{};
    p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    p.bLowLatencyMode = mode > 0;
    p.bLowLatencyBoost = mode == 2;
    p.minimumIntervalUs = interval;
    p.bUseMarkersToOptimize = true;
    if (NvAPI_D3D_SetSleepMode(s_device, &p) == NVAPI_OK) { s_appliedMode = mode; s_appliedInterval = interval; }
}

// ---- the present -----------------------------------------------------------------------------------------------------

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn s_present;
Present1Fn s_present1;
bool s_hooked;
void** s_vtable;

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    uint64_t frame = s_submitted.load();
    Mark(frame, PRESENT_START);
    HRESULT hr = s_present(sc, sync, flags);
    Mark(frame, PRESENT_END);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    uint64_t frame = s_submitted.load();
    Mark(frame, PRESENT_START);
    HRESULT hr = s_present1(sc, sync, flags, p);
    Mark(frame, PRESENT_END);
    return hr;
}

/// Unity owns the swap chain and has no present callback, so the markers go in through its v-table: Present is slot 8,
/// Present1 slot 22. Once, on the render thread.
void HookPresentOnce()
{
    if (s_hooked || !s_d3d) return;
    IDXGISwapChain* sc = s_d3d->GetSwapChain();
    if (!sc) return;   // not made yet: the next event tries again
    s_hooked = true;
    void** vt = *reinterpret_cast<void***>(sc);
    s_vtable = vt;
    DWORD old = 0;
    if (VirtualProtect(&vt[8], sizeof(void*) * 15, PAGE_EXECUTE_READWRITE, &old))
    {
        s_present = reinterpret_cast<PresentFn>(vt[8]);
        s_present1 = reinterpret_cast<Present1Fn>(vt[22]);
        vt[8] = reinterpret_cast<void*>(&HookPresent);
        vt[22] = reinterpret_cast<void*>(&HookPresent1);
        VirtualProtect(&vt[8], sizeof(void*) * 15, old, &old);
    }
}

/// Everything back as it was before Unity takes the device down: no marker reaches a dead device, the driver stops pacing,
/// and the swap chain gets its own Present back.
void Shutdown()
{
    if (!s_device) return;
    if (s_available)
    {
        s_available = false;
        NV_SET_SLEEP_MODE_PARAMS p{};
        p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        NvAPI_D3D_SetSleepMode(s_device, &p);
    }
    if (s_present && s_vtable)
    {
        DWORD old = 0;
        if (VirtualProtect(&s_vtable[8], sizeof(void*) * 15, PAGE_EXECUTE_READWRITE, &old))
        {
            s_vtable[8] = reinterpret_cast<void*>(s_present);
            s_vtable[22] = reinterpret_cast<void*>(s_present1);
            VirtualProtect(&s_vtable[8], sizeof(void*) * 15, old, &old);
        }
        s_present = nullptr;
    }
    s_device = nullptr;
}

void UNITY_INTERFACE_API OnEvent(int id, void* data)
{
    if (!s_available || !data) return;
    HookPresentOnce();
    uint64_t frame = *static_cast<const uint64_t*>(data);
    if (id == kRenderSubmitStart) Mark(frame, RENDERSUBMIT_START);
    else if (id == kRenderSubmitEnd) { Mark(frame, RENDERSUBMIT_END); s_submitted.store(frame); }
}

void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType e)
{
    if (e == kUnityGfxDeviceEventShutdown) { Shutdown(); return; }
    if (e != kUnityGfxDeviceEventInitialize || s_device) return;
    if (s_graphics->GetRenderer() != kUnityGfxRendererD3D12) { strcpy_s(s_reason, "NEEDS DIRECTX 12"); return; }
    s_d3d = s_unity->Get<IUnityGraphicsD3D12v7>();
    if (!s_d3d) { strcpy_s(s_reason, "UNITY D3D12 INTERFACE MISSING"); return; }
    UnityD3D12PluginEventConfig cfg{};
    cfg.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
    cfg.ensureActiveRenderTextureIsBound = false;
    s_d3d->ConfigureEvent(kRenderSubmitStart, &cfg);
    s_d3d->ConfigureEvent(kRenderSubmitEnd, &cfg);
    s_device = s_d3d->GetDevice();
    if (!s_device) { strcpy_s(s_reason, "NO D3D12 DEVICE"); return; }
    if (NvAPI_Initialize() != NVAPI_OK) { strcpy_s(s_reason, "NEEDS AN NVIDIA GPU"); return; }
    NV_GET_SLEEP_STATUS_PARAMS status{};
    status.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
    if (NvAPI_D3D_GetSleepStatus(s_device, &status) != NVAPI_OK) { strcpy_s(s_reason, "NOT SUPPORTED BY THIS GPU OR DRIVER"); return; }
    s_available = true;
    strcpy_s(s_reason, "");
}
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unity)
{
    s_unity = unity;
    s_graphics = unity->Get<IUnityGraphics>();
    s_graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);   // the device already exists when a plugin loads late
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    Shutdown();
    if (s_graphics) s_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
}

extern "C" UNITY_INTERFACE_EXPORT int SlipframeLL_Available() { return s_available ? 1 : 0; }
extern "C" UNITY_INTERFACE_EXPORT const char* SlipframeLL_Reason() { return s_reason; }
extern "C" UNITY_INTERFACE_EXPORT void SlipframeLL_SetMode(int mode) { s_wantMode.store(mode < 0 ? 0 : mode > 2 ? 2 : mode); }
extern "C" UNITY_INTERFACE_EXPORT void SlipframeLL_SetMinimumInterval(uint32_t us) { s_minimumIntervalUs.store(us); }
extern "C" UNITY_INTERFACE_EXPORT UnityRenderingEventAndData SlipframeLL_EventFunc() { return OnEvent; }

/// First in the frame, on the main thread: apply the mode, sleep until the frame should start, mark it. NVIDIA wants the
/// sleep and the markers every frame, whatever the mode.
extern "C" UNITY_INTERFACE_EXPORT void SlipframeLL_FrameStart(uint64_t frame)
{
    if (!s_available) return;
    ApplyMode();
    NvAPI_D3D_Sleep(s_device);
    Mark(frame, SIMULATION_START);
}

extern "C" UNITY_INTERFACE_EXPORT void SlipframeLL_SimulationEnd(uint64_t frame) { Mark(frame, SIMULATION_END); }

/// PC latency in ms - simulation start to the GPU finishing the frame, the driver's own reports averaged - or -1 with no
/// report yet. It reads 64 reports: call it a couple of times a second, not per frame.
extern "C" UNITY_INTERFACE_EXPORT float SlipframeLL_LatencyMs()
{
    if (!s_available) return -1.f;
    static NV_LATENCY_RESULT_PARAMS report{};   // big: not on the stack
    report.version = NV_LATENCY_RESULT_PARAMS_VER;
    if (NvAPI_D3D_GetLatency(s_device, &report) != NVAPI_OK) return -1.f;
    double sum = 0;
    int n = 0;
    for (const auto& r : report.frameReport)
        if (r.simStartTime > 0 && r.gpuRenderEndTime > r.simStartTime) { sum += double(r.gpuRenderEndTime - r.simStartTime); n++; }
    return n > 0 ? float(sum / n / 1000.0) : -1.f;
}
