# com.unity.webrtc (VEL fork) — agent guide

This is **velaboratory's fork** of Unity's [com.unity.webrtc](https://github.com/Unity-Technologies/com.unity.webrtc),
published to the VEL npm registry (`npm.ugavel.com`) as **`com.unity.webrtc`** (not scoped). Read this
before touching anything — the branch/version situation is genuinely confusing.

## Start here (the 30-second version)
- **Canonical branch = `main`. Current published version = `3.0.3`.** That's the source of truth.
- In a local clone, **`origin` may point at UPSTREAM Unity** (`Unity-Technologies/com.unity.webrtc`).
  The FORK is the `velaboratory/com.unity.webrtc` remote (often added as `velab`). The 100+
  `release/*`, `dw24-*`, `experimental/*`, `fix/*`, `ci/*` branches are **upstream's — ignore them.**
- **Publish = bump `version` in `package.json` + push `main`** → `.github/workflows/publish_upm.yml`
  runs `npm publish` to `npm.ugavel.com`. The CI **skips if that version already exists**, so a
  docs/maintenance push to `main` is safe (no spurious republish, no red CI).

## What VEL actually changed (vs. upstream) — in 3.0.3 / `main`
Everything VEL-specific is one commit on top of upstream 3.0.x (`2d107ae`):
- **`Runtime/Scripts/SpatialAudioReceiver.cs`** — spatializable playback of a received audio track.
  Upstream's `AudioSource.SetTrack` injects PCM via an `OnAudioFilterRead` filter that runs *after*
  Unity's spatializer (so the stream is effectively 2D). This adds `SetTrackSpatial(...)`: it drains
  the track's native sink on the **DSP thread** and **pans manually** from the AudioListener pose,
  writing a 2D source (Unity must not also spatialize on top). This is what velshareunity's
  `WebRTCReceiver.spatialAudio` / `ToggleSpatialAudio` path uses.
- **`Runtime/Plugins/Android/libwebrtc.aar`** — a custom arm64 libwebrtc build exporting
  `GetAhbDisplayMode` (+ its `DllImport`), for Android **AHB zero-copy _display_**.
- **`.github/workflows/publish_upm.yml`** — the auto-publish-on-`main` CI.

### Version-history reality (important)
**3.0.0–3.0.2 were published MANUALLY and the source had drifted out of git** — the audio receiver
lived only in a *consumer's* PackageCache for a while. **3.0.3 is the first version published properly
from git.** Treat anything ≤ 3.0.2 as unreliable source-wise; `main` @ 3.0.3 is the real thing.

## The branches & stray copies that cause confusion
| Thing | What it is | Do |
|---|---|---|
| **`main` (3.0.3)** | Canonical, published fork. Spatial audio + AHB-display libwebrtc + CI. | **Start here.** |
| **`ahb-zerocopy-h264` (3.0.0 base)** | A **separate, UNMERGED experimental branch**: a zero-copy H.264 **receive _decoder_** (MediaCodec/AHB → ycbcr → compute shader → Unity texture). Built on an older 3.0.0 base and **diverged** from `main` (lacks the 3.0.3 publish commit). NOT in the published package. | Don't confuse this "zero-copy receive decoder" with the "AHB zero-copy _display_" that *is* in 3.0.3. Treat as WIP. |
| `origin/*` (100+ branches) | Upstream Unity-Technologies branches. | Ignore. |
| A sibling dir `com.unity.webrtc-3.0.1/` (not a git repo, version 3.0.2) | A manual-publish-era loose export. | Ignore — not source. |

## Consumers
Apps pin `com.unity.webrtc` by version through the `npm.ugavel.com` registry (coannotate whitelists the
exact name `com.unity.webrtc` in its scoped-registry `scopes`). As of this writing **coannotate pins
`3.0.2`** even though the fork's latest is `3.0.3` — consumers lag until they choose to bump, and a new
publish never disturbs them.

## Working on this package
- **Edit + test:** clone the fork, `file:`-reference it from a consuming project's `Packages/manifest.json`
  (`"com.unity.webrtc": "file:../com.unity.webrtc"`), do the work, test in a real build. Never commit a
  consumer on a `file:` ref.
- **Publish:** bump `package.json` `version`, update `CHANGELOG.md`, push `main`. The CI publishes to
  `npm.ugavel.com` (org secret `VERDACCIO_TOKEN`, inherited). Then re-pin the consumer to the new version.
- **The big native binary:** `Runtime/Plugins/Android/libwebrtc.aar` is ~60 MB and is rebuilt out-of-band
  (it's not produced by this repo's CI). Replacing it = the real work behind the AHB features.
