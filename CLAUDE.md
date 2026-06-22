# com.unity.webrtc (VEL fork) — agent guide

This is **velaboratory's fork** of Unity's [com.unity.webrtc](https://github.com/Unity-Technologies/com.unity.webrtc),
published to the VEL npm registry (`npm.ugavel.com`) as **`com.unity.webrtc`** (not scoped). Read this
before touching anything — the branch/version history is genuinely confusing.

## Start here (the 30-second version)
- **Canonical branch = `main`. Current published version = `3.0.4`.** As of 3.0.4 `main` is
  **self-contained**: the AHB native plugin **source**, the prebuilt `.aar`, the spatial-audio
  receiver, and the publish CI all live here.
- In a local clone, **`origin` may point at UPSTREAM Unity** (`Unity-Technologies/com.unity.webrtc`).
  The FORK is the `velaboratory/com.unity.webrtc` remote (often added as `velab`). The 100+
  `release/*`, `dw24-*`, `experimental/*`, `fix/*`, `ci/*` branches are **upstream's — ignore them.**
- **Publish = bump `version` in `package.json` + push `main`** → `.github/workflows/publish_upm.yml`
  runs `npm publish` to `npm.ugavel.com`. The CI **skips if that version already exists**, so a
  docs/maintenance push is safe (no spurious republish, no red CI).

## What VEL added (vs. upstream)
Two features — **zero-copy video** + **spatial audio** — plus the publish CI:

- **Zero-copy video (Android AHB).** Native plugin source under
  `Plugin~/WebRTCPlugin/Codec/AhbCodec/` (`AhbH264DecoderFactory`, `AhbDecodePipeline`,
  `AhbConvertPass`, `AhbMediaCodec`, the `ycbcr_to_rgba` compute shader) plus hooks in
  `UnityRenderEvent.cpp` (the `GetAhbDisplayMode` export), `CreateVideoCodecFactory`, the Vulkan
  init, and `UnityVideoRenderer`. On the C# side: `VideoStreamTrack.RendererId` + the
  `[DllImport] GetAhbDisplayMode` in `WebRTC.cs`. ⚠️ **velshareunity's Android decoder REQUIRES
  `RendererId`** — a webrtc build without it fails to compile on Android (`VideoStreamTrack does not
  contain RendererId`).
- **Spatial audio.** `Runtime/Scripts/SpatialAudioReceiver.cs` + the `SetTrackSpatial` extension.
  Upstream's `SetTrack` injects PCM via an `OnAudioFilterRead` filter that runs *after* Unity's
  spatializer (so the stream is effectively 2D). This drains the track's native sink on the **DSP
  thread** and **pans manually** from the AudioListener pose (writing a 2D source so Unity doesn't
  double-spatialize). velshareunity's `WebRTCReceiver.spatialAudio` / `ToggleSpatialAudio` uses it.
- **`.github/workflows/publish_upm.yml`** — auto-publish on `main`.

## Native plugin & rebuilding the `.aar`
The shipped `Runtime/Plugins/Android/libwebrtc.aar` (~60 MB) is a **prebuilt artifact** — git-tracked
but **regenerable from the `Plugin~/` source on this branch**. Its `arm64-v8a/libwebrtc.so` contains
exactly the AHB symbols (`GetAhbDisplayMode`, `AhbConvertPass`, `AhbDecodePipeline`,
`AhbDisplayBuffer`, `AhbH264DecoderFactory`, `AhbMediaCodec`) — i.e. it was built from this source.
**Verified 2026-06-21:** a fresh `arm64-v8a` / API-26 build from this source reproduces the shipped
`.so` — same AHB symbols, byte size within ~350 bytes (`--build-id` + embedded build paths only).

To rebuild it (needs the Unity **Android** module's NDK + CMake; macOS paths in the script):
1. `BuildScripts~/build_plugin_android.sh` — one-time: downloads the prebuilt **M116-20250805**
   libwebrtc release into `Plugin~/webrtc/` (the link base).
2. `BuildScripts~/build_ahb_plugin.sh` — CMake-builds the `WebRTCPlugin` target **arm64-v8a, API 26**
   (AHB needs `AImageReader`/`AHardwareBuffer`, `__INTRODUCED_IN(26)`), producing
   `Runtime/Plugins/Android/libwebrtc.so`, and zips it into the `.aar`. (Edit the hardcoded `UNITY`
   path / the `.aar` destination for your setup.)
3. The prereq download + build outputs (`Plugin~/webrtc/`, `Plugin~/build-arm64/`, `webrtc.zip`, the
   loose `.so`) are git-ignored — only the source + the final `.aar` are tracked.

**After rebuilding, device-test on Quest before shipping a new `.aar`** — there's no automated native
CI, and a regressed decoder is silent until runtime.

## Version-history reality (important)
**3.0.0–3.0.2 were published MANUALLY and the source drifted out of git** (the spatial receiver lived
only in a *consumer's* PackageCache for a while). **3.0.3 was the first proper git publish** — but it
shipped the custom `.aar` **without** the native source that built it (the source lived only on the
`ahb-zerocopy-h264` branch). **3.0.4 reconciles that**: the AHB native source was ported onto `main`,
so the binary is now reproducible from its own branch. 3.0.4 is **runtime-identical to 3.0.3/3.0.2**
(the added `Plugin~/` source is build-time only; Unity ignores `~` folders).

## The branches & stray copies
| Thing | What it is | Do |
|---|---|---|
| **`main` (3.0.4)** | Canonical, published, **self-contained**: AHB native source + prebuilt `.aar` + spatial audio + CI. | **Start here. Cut releases from here.** |
| **`ahb-zerocopy-h264` (3.0.0 base)** | The older branch that pioneered the AHB decoder. Its two commits touched **only native C++ + build scripts — zero C# files** (the "zero-copy receive decoder": MediaCodec → AHB → Vulkan compute → texture, + direct-write / off-screen-decode-pause refinements). **All of that native source is now on `main`** (3.0.4), verified byte-identical (the `main`↔branch diff shows no `Plugin~` changes). So nothing is stranded on it; it only *lacks* what `main` adds (the C# bindings, `SpatialAudioReceiver`, the working `.aar`, the CI). Its committed `.aar` is stale stock (no AHB symbols — strictly worse). | **Superseded by `main` — archive/ignore.** Don't release or build from it. |
| `origin/*` (100+ branches) | Upstream Unity-Technologies branches. | Ignore. |
| Sibling dir `com.unity.webrtc-3.0.1/` (not a git repo, version 3.0.2) | A manual-publish-era loose export. | Ignore — not source. |

## Consumers
Apps pin `com.unity.webrtc` by version through `npm.ugavel.com` (coannotate whitelists the exact name
`com.unity.webrtc` in its scoped-registry `scopes`). **coannotate pins `3.0.2`**; `3.0.3`/`3.0.4` are
runtime-identical to it, so there's no urgency to bump — and a new publish never disturbs a pinned
consumer. Bump to `3.0.4` when you want the consumer on a fully source-tracked version.

## Working on this package
- **Edit + test:** clone the fork, `file:`-reference it from a consuming project's `Packages/manifest.json`
  (`"com.unity.webrtc": "file:../com.unity.webrtc"`), test in a real Android build (the AHB /
  `RendererId` paths only matter there). Never commit a consumer on a `file:` ref.
- **Publish:** bump `package.json` `version`, update `CHANGELOG.md`, push `main`. CI publishes to
  `npm.ugavel.com` (org secret `VERDACCIO_TOKEN`, inherited — not repo-level). Re-pin the consumer after.
- **Don't** rebase the upstream `origin/*` branches or `ahb-zerocopy-h264` into `main` casually — the
  VEL value (the AHB source, `RendererId`, the spatial receiver, the custom `.aar`) is easy to clobber.
