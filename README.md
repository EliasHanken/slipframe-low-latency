# Slipframe Low Latency

A low latency mode for Unity's D3D12 Windows players on NVIDIA GPUs, tested on HDRP. Your input reaches the screen sooner, because the driver stops frames queueing up in front of the GPU.

It's one small native plugin and one C# file, with no proxy DLLs, no Streamline, and no changes to Unity's render pipeline.

| Measured | Latency | Frame rate |
|---|---|---|
| Off | 26.4 ms | 75 fps |
| On | 16.4 ms | 76 fps |
| On + Boost | 15.5 ms | 76 fps |

That's PC latency, from the start of the simulation to the GPU finishing the frame, as the driver reports it. It was measured in a blank HDRP scene held GPU-bound at about 75 fps on one PC: an RTX 5080, in a 1280x720 window. Off and On were each run twice. Your numbers will differ, and a game that isn't GPU-bound gains less.

## Install

In Unity 6.3, open the Package Manager, choose **Add package from git URL**, and enter this repo's URL.

## Use

It starts by itself in a Windows player, with the mode on. It does nothing in the editor, because the editor's frames aren't one game's frames.

```csharp
using Slipframe;

if (SlipframeLowLatency.Available)
    SlipframeLowLatency.Mode = LatencyMode.On;        // Off, On, OnPlusBoost
else
    Debug.Log(SlipframeLowLatency.Reason);            // "NEEDS AN NVIDIA GPU", ...

float ms = SlipframeLowLatency.LatencyMs();           // a couple of times a second, not every frame
SlipframeLowLatency.SetFrameCap(120f);                // optional: a cap the driver paces to
```

Boost also holds the GPU's clocks up, so it uses a little more power.

## Requirements

- Unity 6.3 on a scriptable render pipeline. Only HDRP 17.3 has been tested.
- Windows x64, Direct3D 12.
- An NVIDIA GPU with a current driver. On anything else, `Available` is false and nothing per frame runs.

## How it works

Unity gives a native plugin its D3D12 device, and NVIDIA's public NVAPI takes that device. So the plugin sleeps at the start of each frame and sends the driver markers for simulation, render submit and present. Unity has no present callback, so the plugin wraps `Present` on the swap chain's v-table. It puts that back at shutdown.

`native~/` holds the C++ source, and `Documentation~/design.md` the design.

## Known limits

- It has only run on one PC, an RTX 5080.
- It's only been tested with Unity's Mono scripting backend, on HDRP.
- Other tools that hook the swap chain could clash with the `Present` wrapper.
- It hasn't been through NVIDIA's verification tools, and it isn't affiliated with or endorsed by NVIDIA.
- There's no flash indicator or input sample marker yet.

## Licence

MIT. See `LICENSE` and `THIRD-PARTY-NOTICES.md`.
