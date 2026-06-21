# com.unity.webrtc (VEL fork) — agent guide

This is **velaboratory's fork** of Unity's [com.unity.webrtc](https://github.com/Unity-Technologies/com.unity.webrtc),
published to the VEL npm registry (`npm.ugavel.com`) as **`com.unity.webrtc`** (not scoped). Read this
before touching anything — the branch/version situation is genuinely confusing.

## Start here (the 30-second version)
- **Canonical branch = `main`. Current published version = `3.0.3`.** That's the source of truth, and
  it is the **complete, working package** (zero-copy video + spatial audio + the CI).
- In a local clone, **`origin` may point at UPSTREAM Unity** (`Unity-Technologies/com.unity.webrtc`).
  The FORK is the `velaboratory/com.unity.webrtc` remote (often added as `velab`). The 100+
  `release/*`, `dw24-*`, `experimental/*`, `fix/*`, `ci/*` branches are **upstream's — ignore them.**
- **Publish = bump `version` in `package.json` + push `main`** → `.github/workflows/publish_upm.yml`
  runs `npm publish` to `npm.ugavel.com`. The CI **skips if that version already exists**, so a
  docs/maintenance push to `main` is safe (no spurious republish, no red CI).

## What VEL added (vs. upstream)
The fork's value over vanilla `com.unity.webrtc` is **zero-copy video** + **spatial audio**, plus the
publish CI. All of it is in `main` / 3.0.3:

- **Zero-copy video** (in `main`'s base): `VideoStreamTrack.RendererId` + an Android AHB zero-copy
  receive path. ⚠️ **velshareunity's Android decoder REQUIRES `RendererId`** — a webrtc build without
  it fails to compile on Android (`VideoStreamTrack does not contain RendererId`). Don't drop it.
- **Spatial audio** (added in the 3.0.3 commit `2d107ae`): `Runtime/Scripts/SpatialAudioReceiver.cs` +
  the `SetTrackSpatial` extension. Upstream's `SetTrack` injects PCM via an `OnAudioFilterRead` filter
  that runs *after* Unity's spatializer (so the stream is effectively 2D). This drains the track's
  native sink on the **DSP thread** and **pans manually** from the AudioListener pose, writing a 2D
  source so Unity doesn't double-spatialize. velshareunity's `WebRTCReceiver.spatialAudio` /
  `ToggleSpatialAudio` uses it.
- **Custom `Runtime/Plugins/Android/libwebrtc.aar`** (~60 MB, added in `2d107ae`): arm64 build
  exporting `GetAhbDisplayMode` (+ its `DllImport` in `WebRTC.cs`) for the Android AHB path. It is
  rebuilt **out-of-band** — this repo's CI does NOT build it; replacing it is the real native work.
- **`.github/workflows/publish_upm.yml`** — auto-publish on `main`.

### Version-history reality (important)
**3.0.0–3.0.2 were published MANUALLY and the source had drifted out of git** — for a while the
spatial-audio receiver lived only in a *consumer's* PackageCache. **3.0.3 is the first version published
properly from git** (the ~7-file VEL delta was reconstructed onto `main`). Treat anything ≤ 3.0.2 as
unreliable source-wise; `main` @ 3.0.3 is the real thing.

## The branches & stray copies that cause confusion
| Thing | What it is | Do |
|---|---|---|
| **`main` (3.0.3)** | Canonical, published, **complete** fork: zero-copy video + spatial audio + libwebrtc.aar + CI. | **Start here. Cut releases from here.** |
| **`ahb-zerocopy-h264` (3.0.0 base)** | A **separate, UNMERGED experimental branch** pushing the zero-copy receive path further (MediaCodec direct-write, a `rendererId→decoder` map, off-screen decode pause). Diverged from `main` on an *older 3.0.0 base*, and it has churned the `RendererId` path velshareunity depends on. Tip = `c629b98`. | **Do NOT cut releases from this branch** (it has broken the Android decoder before). WIP only — build the package from `main`. |
| `origin/*` (100+ branches) | Upstream Unity-Technologies branches. | Ignore. |
| Sibling dir `com.unity.webrtc-3.0.1/` (not a git repo, version 3.0.2) | A manual-publish-era loose export. | Ignore — not source. |

## Consumers
Apps pin `com.unity.webrtc` by version through the `npm.ugavel.com` registry (coannotate whitelists the
exact name `com.unity.webrtc` in its scoped-registry `scopes`). As of this writing **coannotate pins
`3.0.2`** even though the fork's latest is `3.0.3` — they're functionally equivalent for coannotate's
use, so there's no rush to bump; a new publish never disturbs a pinned consumer.

## Working on this package
- **Edit + test:** clone the fork, `file:`-reference it from a consuming project's `Packages/manifest.json`
  (`"com.unity.webrtc": "file:../com.unity.webrtc"`), do the work, test in a real Android build (the AHB
  / `RendererId` paths only matter there). Never commit a consumer on a `file:` ref.
- **Publish:** bump `package.json` `version`, update `CHANGELOG.md`, push `main`. The CI publishes to
  `npm.ugavel.com` (org secret `VERDACCIO_TOKEN`, inherited — not repo-level). Re-pin the consumer after.
- **Don't** rebase/merge the upstream `origin/*` branches or `ahb-zerocopy-h264` into `main` casually —
  the VEL value (RendererId + the spatial receiver + the custom .aar) is easy to clobber.
