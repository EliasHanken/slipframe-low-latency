# Slipframe Low Latency: design (2026-10-31)

The free half of Slipframe: NVIDIA's low latency mode for Unity HDRP. It's a small public MIT package, and the plugin
talks to the driver through NVIDIA's public NVAPI, not the Reflex SDK or Streamline.

## Why this shape

Frame generation needs Streamline between Unity and the GPU, which is why it needs proxy DLLs. The low latency calls
(`NvAPI_D3D_SetSleepMode`, `NvAPI_D3D_Sleep`, `NvAPI_D3D_SetLatencyMarker`, `NvAPI_D3D_GetLatency`) only need the D3D12
device, and Unity gives that to any native plugin. So this is a normal plugin in a normal Plugins folder.

## The parts

| Part | Where | What |
|---|---|---|
| `SlipframeLowLatency.dll` | `Runtime/Plugins/x86_64` | the sleep, the markers, the latency report |
| `SlipframeLowLatency` | `Runtime` | the C# side: mode, availability, frame timing |

The frame: sleep and `SIMULATION_START` first in `Update`, `SIMULATION_END` last in `LateUpdate`, the render-submit
markers from plugin events, the present markers from a wrapped `Present`.

Unity has no present callback, so the plugin wraps slots 8 and 22 of the swap chain's v-table. The events go through
`Graphics.ExecuteCommandBuffer`, because `endContextRendering` fires after the render context has been submitted.

## Budget

One native call at each end of the simulation, two plugin events, two markers around Present. Under 0.1 ms on each
thread. NVIDIA's sleep is the point of the feature, so it doesn't count as cost. Not measured yet.

## Measured

In a blank HDRP scene held GPU-bound at about 75 fps, 1280x720 windowed, RTX 5080: 26.3 / 26.4 ms off, 16.3 / 16.5 ms on,
15.5 ms on + boost, at the same frame rate. The player first hung at exit with the mode on, so the plugin now turns the
mode off and restores `Present` when Unity shuts the device down. The plugin's own CPU budget isn't measured.

## Retirement path

If Unity ships a low latency mode for HDRP, this package is deprecated in its README and left up for older versions.

## Not in the first version

Flash indicator, input sample markers, a settings UI, URP and Built-in testing, the Reflex verification report.

## Names

The package says "NVIDIA's low latency mode" in plain text. It doesn't use NVIDIA's logos, or put its marks in the name.
