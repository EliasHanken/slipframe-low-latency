using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

namespace Slipframe
{
    public enum LatencyMode { Off = 0, On = 1, OnPlusBoost = 2 }

    /// <summary>Takes over the low latency mode. Slipframe's frame generation does, because DLSS-G has to own it. A game
    /// keeps calling SlipframeLowLatency either way.</summary>
    public interface ILowLatencyProvider
    {
        bool Available { get; }
        string Reason { get; }
        void SetMode(LatencyMode mode);
        float LatencyMs();
    }

    /// NVIDIA's low latency mode for Unity's D3D12 players. It starts by itself in a Windows player: set Mode from your
    /// settings, and read Available and Reason to grey the option out. In the editor, or without an NVIDIA GPU, nothing
    /// per frame runs.
    public static class SlipframeLowLatency
    {
        private const string Dll = "SlipframeLowLatency";
        private const int RenderSubmitStart = 0x534C, RenderSubmitEnd = 0x534D;   // the plugin's render events

        [DllImport(Dll)] private static extern int SlipframeLL_Available();
        [DllImport(Dll)] private static extern IntPtr SlipframeLL_Reason();
        [DllImport(Dll)] private static extern void SlipframeLL_SetMode(int mode);
        [DllImport(Dll)] private static extern void SlipframeLL_SetMinimumInterval(uint microseconds);
        [DllImport(Dll)] private static extern IntPtr SlipframeLL_EventFunc();
        [DllImport(Dll)] private static extern void SlipframeLL_FrameStart(ulong frame);
        [DllImport(Dll)] private static extern void SlipframeLL_SimulationEnd(ulong frame);
        [DllImport(Dll)] private static extern float SlipframeLL_LatencyMs();

        private static bool _probed, _native, _available;
        private static string _reason = "NOT IN THE EDITOR";
        private static IntPtr _event;
        private static LatencyMode _mode = LatencyMode.On;
        private static ILowLatencyProvider _provider;

        /// <summary>Hands the mode to a provider. Call it before the first scene loads.</summary>
        public static void UseProvider(ILowLatencyProvider provider) => _provider = provider;

        /// It can run here: an NVIDIA GPU, a driver that supports it, D3D12.
        public static bool Available { get { if (_provider != null) return _provider.Available; Probe(); return _available; } }
        /// Why not, or null when it can.
        public static string Reason { get { if (_provider != null) return _provider.Reason; Probe(); return _available ? null : _reason; } }

        public static LatencyMode Mode
        {
            get => _mode;
            set
            {
                _mode = value;
                if (_provider != null) { _provider.SetMode(value); return; }
                Probe();
                if (_native) SlipframeLL_SetMode((int)value);
            }
        }

        /// A frame cap the driver paces to, in frames a second, 0 for none. Lower latency than Application.targetFrameRate.
        public static void SetFrameCap(float framesPerSecond)
        {
            Probe();
            if (_native) SlipframeLL_SetMinimumInterval(framesPerSecond > 0f ? (uint)(1000000f / framesPerSecond) : 0u);
        }

        /// PC latency in ms, simulation start to the GPU finishing the frame, or -1 with no report yet. It reads 64 reports,
        /// so call it a couple of times a second, not every frame.
        public static float LatencyMs() => _provider != null ? _provider.LatencyMs() : _available ? SlipframeLL_LatencyMs() : -1f;

        private static void Probe()
        {
            if (_probed) return;
            _probed = true;
            if (Application.isEditor) return;   // the editor's swap chains aren't one game's frames
            try
            {
                _available = SlipframeLL_Available() == 1;
                _reason = Marshal.PtrToStringAnsi(SlipframeLL_Reason()) ?? "";
                _event = SlipframeLL_EventFunc();
                _native = true;
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
            {
                _reason = "PLUGIN NOT FOUND";
            }
        }

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Boot()
        {
            if (_provider != null) { _provider.SetMode(_mode); return; }
            Probe();
            if (!_native || !_available) return;
            SlipframeLL_SetMode((int)_mode);
            var go = new GameObject("Slipframe Low Latency");
            UnityEngine.Object.DontDestroyOnLoad(go);
            go.hideFlags = HideFlags.HideInHierarchy;
            go.AddComponent<FrameStart>();
            go.AddComponent<FrameEnd>();
            RenderPipelineManager.beginContextRendering += (ctx, cams) => Submit(RenderSubmitStart);
            RenderPipelineManager.endContextRendering += (ctx, cams) => Submit(RenderSubmitEnd);
        }

        private static ulong Frame => (ulong)Time.frameCount;

        // The render events go straight to the render thread, never onto the render context: endContextRendering fires after
        // the pipeline has submitted it, so a buffer queued there never runs.
        private static readonly CommandBuffer _cmd = new CommandBuffer { name = "Slipframe Low Latency" };
        private const int Slots = 8;
        private static readonly IntPtr _frames = Marshal.AllocHGlobal(Slots * 8);
        private static int _slot;

        private static void Submit(int ev)
        {
            var p = _frames + 8 * (_slot = (_slot + 1) % Slots);
            Marshal.WriteInt64(p, (long)Frame);
            _cmd.Clear();
            _cmd.IssuePluginEventAndData(_event, ev, p);
            Graphics.ExecuteCommandBuffer(_cmd);
            _cmd.Clear();
        }

        [DefaultExecutionOrder(-32000)]
        private sealed class FrameStart : MonoBehaviour
        {
            private void Update() => SlipframeLL_FrameStart(Frame);
        }

        [DefaultExecutionOrder(32000)]
        private sealed class FrameEnd : MonoBehaviour
        {
            private void LateUpdate() => SlipframeLL_SimulationEnd(Frame);
        }
    }
}
