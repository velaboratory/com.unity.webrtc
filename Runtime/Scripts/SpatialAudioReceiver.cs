using System;
using UnityEngine;

namespace Unity.WebRTC
{
    /// <summary>
    /// Extension for spatializable playback of a received audio track.
    /// </summary>
    public static class SpatialAudioExtension
    {
        /// <summary>
        /// Plays a received <see cref="AudioStreamTrack"/> through <paramref name="source"/> with
        /// real head-relative panning, by draining the decoded audio on the DSP thread and panning
        /// it toward the source's world position (relative to the AudioListener).
        ///
        /// The default <see cref="AudioSourceExtension.SetTrack"/> injects PCM via an
        /// OnAudioFilterRead filter that runs AFTER Unity's spatializer, so the stream only gets
        /// distance attenuation, never panning. This keeps the same correct DSP-thread drain (so the
        /// native resampler stays rate-locked — no corruption) but does the panning itself, so the
        /// audio actually moves with your head.
        /// </summary>
        /// <param name="source">The AudioSource that will play the spatialized audio.</param>
        /// <param name="track">A receiver-side AudioStreamTrack.</param>
        public static void SetTrackSpatial(this AudioSource source, AudioStreamTrack track)
        {
            if (source == null)
                throw new ArgumentNullException(nameof(source));
            if (track == null || track._streamRenderer == null)
                throw new InvalidOperationException("SetTrackSpatial requires a receiver-side AudioStreamTrack.");
            var recv = source.gameObject.GetComponent<SpatialAudioReceiver>();
            if (recv == null)
                recv = source.gameObject.AddComponent<SpatialAudioReceiver>();
            recv.Begin(source, track);
        }
    }

    /// <summary>
    /// Drives spatializable playback for <see cref="SpatialAudioExtension.SetTrackSpatial"/>.
    ///
    /// The decoded audio is pulled from the track's native sink inside <c>OnAudioFilterRead</c> —
    /// the DSP thread, the SAME place and cadence the built-in receive filter drains it, so the
    /// sink's resampler stays rate-locked and the audio does not drift or corrupt (the earlier
    /// Update()-poll and streaming-clip versions failed: one drifted, one ran on a thread where the
    /// native pull returns silence). We then pan it ourselves from the AudioListener pose and write
    /// the result straight to the output buffer, so the source is 2D (spatialBlend 0) — Unity must
    /// not also spatialize on top of our pan.
    /// </summary>
    [DisallowMultipleComponent]
    public class SpatialAudioReceiver : MonoBehaviour
    {
        [Tooltip("How wide the virtual stereo image is (0 = mono/centered, 1 = full L/R swing).")]
        [Range(0f, 1f)] public float stereoWidth = 0.9f;

        [Tooltip("When false, play flat 2D (centered mono, no panning or distance falloff).")]
        public bool spatial = true;

        [Tooltip("Log a one-line audio heartbeat (blocks/sec + peak level) for diagnosis.")]
        public bool debugLog = false;

        /// <summary>Toggle 3D spatial panning vs flat 2D playback; returns the new state.</summary>
        public bool ToggleSpatial() { spatial = !spatial; return spatial; }

        AudioSource _out;
        IntPtr _sink;
        int _rate;
        float[] _stereo;
        volatile bool _ready;

        // Listener/source pose, cached on the main thread for the audio thread to read.
        AudioListener _listener;
        Vector3 _srcPos, _listPos;
        Quaternion _listInv = Quaternion.identity;
        float _minDist = 1.5f, _maxDist = 20f;

        // diagnostics (written on the audio thread, read on the main thread)
        int _blocks;
        float _peak;
        float _nextLog;

        public void Begin(AudioSource output, AudioStreamTrack track)
        {
            Stop();

            _out = output;
            _sink = track._streamRenderer.self;
            _rate = AudioSettings.outputSampleRate;

            _out.clip = null;
            _out.spatialBlend = 0f;       // we pan manually; Unity must not spatialize on top
            _out.loop = false;
            _out.playOnAwake = false;
            _minDist = Mathf.Max(0.01f, _out.minDistance);
            _maxDist = Mathf.Max(_minDist + 0.01f, _out.maxDistance);

            CachePose();
            _ready = true;
            _out.Play();                  // clip-less Play keeps OnAudioFilterRead firing (as the built-in filter does)
        }

        void Update()
        {
            if (!_ready || _out == null) return;
            // We pan manually; never let Unity also spatialize (the toolbar "Spatial" button may
            // still flip spatialBlend to 1 — snap it back so we don't double-process).
            if (_out.spatialBlend != 0f) _out.spatialBlend = 0f;
            CachePose();

            if (debugLog && Time.unscaledTime >= _nextLog)
            {
                int b = _blocks; _blocks = 0;
                float dt = _nextLog == 0 ? 2f : 2f;
                _nextLog = Time.unscaledTime + 2f;
                Debug.Log($"[SpatialAudio] blocks/2s={b} peak={_peak:F3} sink={(_sink != IntPtr.Zero)} listener={(_listener != null)} rate={_rate}");
            }
        }

        void CachePose()
        {
            if (_listener == null) _listener = FindFirstObjectByType<AudioListener>();
            if (_out != null) _srcPos = _out.transform.position;
            if (_listener != null)
            {
                var lt = _listener.transform;
                _listPos = lt.position;
                _listInv = Quaternion.Inverse(lt.rotation);
            }
        }

        void OnDestroy() => Stop();

        void Stop()
        {
            _ready = false;
            if (_out != null) _out.Stop();
        }

        // DSP thread. Pull `frames` of stereo from the sink (resampled to _rate), downmix to mono,
        // pan toward the source from the listener's view, write to the output buffer.
        void OnAudioFilterRead(float[] data, int channels)
        {
            if (!_ready || channels <= 0) return;
            IntPtr sink = _sink;
            if (sink == IntPtr.Zero) return;

            int frames = data.Length / channels;
            if (frames <= 0) return;
            if (_stereo == null || _stereo.Length < frames * 2) _stereo = new float[frames * 2];

            NativeMethods.AudioTrackSinkProcessAudio(sink, _stereo, frames * 2, 2, _rate);

            float gL, gR;
            if (spatial)
            {
                Vector3 toSrc = _srcPos - _listPos;
                float dist = toSrc.magnitude;
                float atten = dist <= _minDist ? 1f : Mathf.Clamp(_minDist / dist, _minDist / _maxDist, 1f);
                Vector3 local = dist > 1e-4f ? _listInv * (toSrc / dist) : Vector3.forward;
                float pan = Mathf.Clamp(local.x, -1f, 1f) * Mathf.Clamp01(stereoWidth);
                float ang = (pan * 0.5f + 0.5f) * (Mathf.PI * 0.5f);   // equal-power
                gL = Mathf.Cos(ang) * atten;
                gR = Mathf.Sin(ang) * atten;
            }
            else
            {
                gL = gR = 0.70710678f;   // flat 2D: centered mono, no distance falloff
            }

            float gC = (gL + gR) * 0.5f;   // center/extra channels
            float peak = 0f;
            for (int i = 0; i < frames; i++)
            {
                float m = (_stereo[i * 2] + _stereo[i * 2 + 1]) * 0.5f;
                int o = i * channels;
                data[o] = m * gL;
                if (channels > 1) data[o + 1] = m * gR;
                for (int c = 2; c < channels; c++) data[o + c] = m * gC;
                float a = m < 0 ? -m : m;
                if (a > peak) peak = a;
            }
            _blocks++;
            _peak = peak;
        }
    }
}
