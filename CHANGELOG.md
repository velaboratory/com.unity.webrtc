# Changelog
All notable changes to the webrtc package will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](http://semver.org/spec/v2.0.0.html).

## [3.0.8] - 2026-06-27 (VEL fork)

### Added

- **macOS H.264 High advertised for VideoToolbox decode** (`UnityVideoDecoderFactory`). When the
  macOS/iOS VideoToolbox factory is present, advertise H.264 High (`640034`) and route an unmatched H.264
  `CreateVideoDecoder` request to VideoToolbox (it decodes High fine). Browser hardware H.264 encoders
  (Chrome/macOS VideoToolbox) emit only Baseline/Main/High — never Constrained Baseline — so without this
  an Editor viewer negotiated a hardware-encoded stream to nothing. No-op on Android (the AHB factory
  already advertises High).

### Changed

- **AHB decoder always decodes every subscribed stream** (removed off-screen decode-pause). Pausing the HW
  decoder on look-away and forcing a keyframe on resume caused a visible glitch on every glance in VR.
- **AHB recovery is codec-agnostic and `missing_frames`-independent.** On any decode failure the decoder
  self-resets (releases the HW codec, re-requires a keyframe) and returns ERROR so libwebrtc requests a
  keyframe; the codec reconfigures on the next keyframe. Replaces the `missing_frames`-driven path, which
  never fired for AV1 without the Dependency Descriptor — AV1 corrupted persistently until a natural
  keyframe (desktop recovered, Quest did not). Validated clean at 12×1080p60 AV1.

### Fixed

- **Hard codec-fault recovery** (e.g. framework ResourceManager reclaim). This M116 `VideoReceiveStream2`
  does not re-init a decoder on a returned ERROR — it only requests a keyframe — so a faulted HW codec was
  re-fed forever (permanent black board). The decoder now releases its codec on a decode fault so the next
  frame reconfigures it.
- **Input-buffer starvation no longer silently drops NALs.** `AhbMediaCodec::DecodeNal` waits up to 500 ms
  (stock `DEQUEUE_INPUT_TIMEOUT_US`) for an input buffer and signals failure instead of dropping the frame
  and returning OK (the old timeout-0 drop broke the reference chain with no recovery).
- **AImageReader pool 8→12** so the decoder keeps enough render buffers under 6×1440p / many concurrent
  1080p streams (the consumer holds ~5; 8 left only 3, which starved and flooded "Unsupported input buffer").
- **Reap-before-create at the HW session cap.** Opening a decode session right at the cap (avc=16) could
  transiently fail while a just-reset sibling's session was still reaping; `Configure` now retries with a
  short backoff (and frees its AImageReader on give-up) instead of erroring into a reset storm at 15–16 streams.
- **macOS 26 SDK build:** `libcxx/debug.cpp` is guarded with `#if __has_include(<__debug>)` (the obsolete
  libc++ debug header was removed in newer toolchains; the prebuilt libwebrtc references none of its symbols).

### Notes

- Decoder-output AHB memory scales with the pool: ~37 MB/stream at 1080p, ~66 MB at 1440p. The shipped and
  tested target is ≤12×1080p60; at a 1440p / 16-stream peak this is ~1 GB of AHBs alone on the shared 8 GB
  Quest 3 — tune the pool down if you push past that.

## [3.0.7] - 2026-06-24 (VEL fork)

### Fixed

- **Android Player build failed on registry installs.** The published package shipped the upstream
  `TestRunnerOptions.json` (and `.yamato/`) without `.meta` files; in an immutable PackageCache Unity
  can't generate them and logs *"asset has no meta file … will be ignored"* at **Error** level, which
  counts toward `Error building Player` and fails the build (it was silent on a `file:` ref because Unity
  generated the `.meta` locally). Both are now excluded via `.npmignore` so they aren't published.

## [3.0.6] - 2026-06-24 (VEL fork)

### Added

- **Multi-codec AHB zero-copy decode (VP9, AV1).** The zero-copy AHB path was H.264-only; it now drives
  the correct MediaCodec mime per codec (`video/avc`, `video/x-vnd.on2.vp9`, `video/av01`), so VP9 and AV1
  hardware-decode zero-copy on Quest just like H.264. (H.265 is unavailable — this libwebrtc build has no
  HEVC codec support — and VP8 has no Quest HW decoder, so it stays on libvpx; see below.)
- **Per-track display path.** Each `UnityVideoRenderer` records whether its decoder delivers native (AHB)
  frames (`LastFrameWasNative` / the `GetRendererFrameIsNative` export). C# now picks the zero-copy
  random-write `RenderTexture` only for those, and the standard `Texture2D` CPU upload for software decoders
  (e.g. libvpx VP8) whose I420 output the AHB compute display can't consume. Previously the display mode
  was global, so a software-decoded stream rendered nothing under the AHB path.

### Fixed

- **Decoder-driven resolution for codecs without `_encodedWidth`.** AV1's receive path never populates
  `_encodedWidth/Height`, so the AHB decoder couldn't size its `AImageReader` and produced no frames. It
  now seeds a size, learns the true dimensions from MediaCodec's `INFO_OUTPUT_FORMAT_CHANGED`, and resizes
  the reader in place via `AMediaCodec_setOutputSurface`.
- **Green video from known multi-planar AHB formats.** `AhbVkImporter` classified any non-`UNDEFINED`
  format as RGBA8 (no conversion). A known multi-planar YUV `VkFormat` (e.g. NV12) is now recognized and
  given the proper `VkSamplerYcbcrConversion` (using `fmtProps.format`, not the external-format path),
  instead of sampling the YUV planes as colour (green).

## [3.0.5] - 2026-06-24 (VEL fork)

### Fixed

- **Encoder crash (`EncoderQueue` SIGSEGV).** `VideoFrameAdapter::ConvertToVideoFrameBuffer` dereferenced
  a null GPU buffer — either `GetGpuMemoryBuffer()` was null, or `gmb->ToI420()` returned null on a failed
  `WaitSync` (the texture hadn't been uploaded yet) — when a frame reached the encoder before its GPU data
  was ready (e.g. an SFU forces an immediate keyframe the instant publishing starts while a subscriber is
  already watching). The release-stripped `RTC_DCHECK` never caught it. Now returns a transient black
  `I420Buffer` instead of dereferencing null.
- **Receive-side freeze on packet/keyframe loss.** The AHB H.264 decoder always returned
  `WEBRTC_VIDEO_CODEC_OK` and ignored `missing_frames`, so libwebrtc believed every frame decoded and
  never requested a keyframe — a lost reference froze the stream until an external watchdog poked the SFU.
  The decoder now honors `missing_frames` and returns `WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME` while it
  needs a keyframe, so libwebrtc emits the RTCP PLI itself (native NACK/PLI recovery, no watchdog).
- **Resume crash (`UnityMain` SIGSEGV).** `GetVideoRendererId` dereferenced a null `UnityVideoRenderer`
  when a C# `VideoStreamTrack` polled `RendererId` after the native renderer was torn down on app resume.
  Now null-safe (returns 0).

## [3.0.4] - 2026-06-21 (VEL fork)

### Added

- **AHB zero-copy native plugin source** ported onto `main`
  (`Plugin~/WebRTCPlugin/Codec/AhbCodec/*`, the `GetAhbDisplayMode` / Vulkan / renderer hooks, and
  `BuildScripts~/build_ahb_plugin.sh`). 3.0.3 shipped the custom `libwebrtc.aar` without the source
  that built it (the source lived only on the `ahb-zerocopy-h264` branch); the binary is now
  reproducible from this branch. **Verified:** a fresh arm64 / API-26 build reproduces the shipped
  `.so` (same AHB symbols, byte size within ~350 bytes).

### Notes

- **Runtime-identical to 3.0.3 / 3.0.2** — the added `Plugin~/` source is build-time only (Unity
  ignores `~` folders); the prebuilt `.aar` and all `Runtime/Scripts` are unchanged.
- (VEL-fork history: 3.0.0–3.0.2 were manual publishes with drifted source; 3.0.3 was the first
  proper git publish but lacked the native source; 3.0.4 reconciles it.)

## [3.0.0] - 2025-09-12

### Changed

- Update minimum Unity version.
- Upgrade libwebrtc to support 16kb pagesizes for Android.
- Change Info.plist format for iOS native plugin.
- Change license to Unity Companion License.

### Fixed
- VideoStreamTrack fails to start streaming on Android.

## [3.0.0-pre.8] - 2024-12-12

### Changed

- doc: improve the API doc of CameraExtension 
- doc: improve the API doc of RTCSessionDescription
- doc: improve the API doc of RTCDataChannel
- doc: Improve the API doc of RTCConfiguration
- doc: Improve the API doc of RTCRtpEncodingParameters
- doc: Improve the API doc of RTCRtpSender
- doc: Improve the API doc of RTCRtpTransceiver
- doc: Improve the API doc of WebRTC
- doc: Improve the API doc of AudioStreamTrack
- doc: Improve the API doc of VideoStreamTrack
- doc: Improve the API doc of MediaStreamTrack
- doc: Improve the API doc of MediaStream
- doc: Improve the API doc of RTCPeerConnection
- doc: Improve the API doc of RTCIceServer


## [3.0.0-pre.7] - 2023-10-20

### Added

- Add **Android x86_64** support experimentally.
- Add an property `.CanTrickleIceCandidates` in `RTCPeerConnection` class.
- Add an configurable logger in `WebRTC` class.
- Support scale resolution for NVIDIA H.264 codec.
- Support simulcast for NVIDIA H.264 codec.
- Add an property `SyncApplicationFramerate` in `RTCRtpSender` class to improve latency of video streaming.

### Changed

- Upgrade libwebrtc [M116](https://groups.google.com/g/discuss-webrtc/c/bEsO8Lz7psE).
- Change Android API minimum level to **23** because the API level **22** is obsoleted on Unity2020.3 LTS.
- Add `RTCDataChannel.onError` event.
- Change a constructor of `VideoStreamTrack` to make simple to flip video texture.

### Fixed

- Fix an issue when setting a scale factor of video encoding to a fractional scaling ratio.
- Fix an occasional crash issue when using hardware encoder with D3D11 graphic device.
- Fix an crash when `nvEncodeAPI64.dll` is not found on Windows.
- Optimize audio sampling when using mute.

## [3.0.0-pre.6] - 2023-07-16

### Added

- Add `AudioStreamTrack.onReceived` delegate which fetch audio buffers from other peers.

### Changed

- Upgrade libwebrtc [M112](https://groups.google.com/g/discuss-webrtc/c/V-XFau9W9gY).

### Fixed 

- Improve video encoding performance for DX11/DX12 which decrease the loads on the rendering thread.
- Fix a runtime error when replacing track with NvCodec.
-  Support calling `AudioStreamTrack.SetData` on worker thread 
- Fix an issue in a **VideoReceive** sample scene which a video streaming doesn't work from camera device on Android since Unity 2022.1.

### Removed

- Remove Obsolete methods in **WebRTC** class. 
  - `WebRTC.Initialize`
  - `WebRTC.Dispose`

- Remove Obsolete methods in **RTCConfiguration** class. 
  - `RTCConfiguration.enableDtlsSrtp`

## [3.0.0-pre.5] - 2023-04-28

### Added

- Add Encoded Transform API.
- Add `RTCRtpReceiver.GetContributingSources` method.
- Add **Metadata** and **Encrypt** into package sample.

### Changed

- Upgrade NVIDIA Codec SDK 12.0.
- Change to send SPS and PPS when using the encoder of NVIDIA video codec.

### Fixed

- Fix unhandled NVENCException's occurring during initialization & reconfigure calls when using NVIDIA video codec.
- Fix crash on dedicated Linux Server.
- Fix a performance issue of video streaming with Unity 2022.2

## [3.0.0-pre.4] - 2023-01-28

### Fixed

- Fix KeyNotFoundException in `RTCPeerConnection` when firing the callback after disposing the its instance.
- Fix crash when streaming high resolution video on Android device.
- Fix memory leak on macOS/iOS when streaming high resolution video.

## [3.0.0-pre.3] - 2022-12-16

### Fixed

- Fix the crash when launching Unity Editor on a command line with `-nographics` option.

## [3.0.0-pre.2] - 2022-12-09

### Changed

- Upgrade libwebrtc [M107](https://groups.google.com/g/discuss-webrtc/c/StVFkKuSRc8).
- Change that invoking initialization process automatically just after launching Unity Editor.
- Obsolete `WebRTC.Initialize` and `WebRTC.Initialize`.

### Fixed

- Fix not displaying profiling result of encoding thread on Unity Profiler on macOS.
- Fix stopping at first received frame for seconds when using H264 codec.
- Fix the crash when streaming video which resolution is small on macOS Apple Silicon.
- Fix the freeze when using over WQHD (2560×1440) resolution on Ubuntu 20.04.

### Removed

- Finish Bitcode support.
- Finish Unity 2019.4 support.

## [3.0.0-pre.1] - 2022-10-28

### Changed

- Add `AudioStreamTrack` constructor to set `AudioListener`.
- Change `WebRTC.Initialize` method to not throw exception when using OpenGL Core on Windows or macOS.
- Remove dependency *libc++.so.1* in Linux native plugin

### Fixed

- Fix `SetRemoteDescription` and `SetLocalDescription` method of `RTCPeerConnection` class to work correct when calling multiple at the same time.

## [2.4.0-exp.11] - 2022-09-28

### Changed

- Add **ValidationExceptions.json** to suppress warnings about the package validation.
- Stop to use flip shader so that removing `Resources` folder.
- Change parameter of `VideoStraamTrack.CaptureStreamTrack` method.

### Fixed

- Fix a bug when calling `GetStats` method multiple times at the same time.

## [2.4.0-exp.10] - 2022-08-09

### Fixed

- Fix crash when streaming video between different platforms.

## [2.4.0-exp.9] - 2022-08-01

### Fixed

- Fix the crash bug on Android devices like pixel4a when using Vulkan.

## [2.4.0-exp.8] - 2022-07-08

### Added

- Supported video encoding framerate control.
- Added the items in Profiler Window to show the CPU loads of video encoding/decoding thread.
- Add a new sample "ReplaceTrack" to demonstrate `RTCRtpSender.ReplaceTrack` method.

### Changed

- Changed arguments of `RTCPeerConnection.AddTransceiver` method to pass initial information of `RTCRtpTransceiver` when instanting it.

### Fixed 

- Fixed the crash because of the old version of NVIDIA graphics driver.
- Fixed the issue when garbage collected but the finalizer hasn't been called.
- Improved performance of video streaming when using Vulkan graphics API.

## [2.4.0-exp.7] - 2022-05-20

### Added

- Supported Unity 2021.3 LTS.
- Added clang-format file.
- Added CMakePresets.json file.
- Supported NVIDIA decoder for H264 codec.
- Added the codec selection option and video resolution option in the menu scene.

### Changed

- Moved CPU loads of the color conversion from the rendering thread to the worker thread.
- Upgraded NVIDIA Codec SDK 11.0.

### Fixed

- Removed unnecessary dependencies of the native plugin on Linux.

### Removed

- Removed an argument `EncoderType` for `WebRTC.Initialize` method.
- Removed `WebRTC.HardwareEncoderSupport` method.

## [2.4.0-exp.6] - 2022-02-24

### Fixed

- Fixed a crash bug where disposing `AudioStreamTrack` on a receiver side.

## [2.4.0-exp.5] - 2022-02-02

### Added

- Added the ability to execute pending native tasks manually from the main thread.
- Added support for sending basic native collections (`NativeArray<T>`, `NativeSlice<T>` and `NativeArray<T>.ReadOnly`) to `RTCDataChannel`.
- Added a property `RTCRtpTransceiver.Mid`.
- Added an argument `enableNativeLog` for `WebRTC.Initialize` method.
- Added "E2ELatency" scene in the sample.
- Added a constuctor without arguments in `AudioStreamTrack` class.

### Changed

- Upgraded libwebrtc [M92](https://groups.google.com/g/discuss-webrtc/c/hks5zneZJbo).
- Changed compiler for native plugin on Windows (MSVC to Clang).
- Changed to use `OnAudioFilterRead` method in `MonoBehaviour` for audio rendering.
- Changed resizing texture dynamically on receiver side when using a property `scaleResolutionDownBy` in `RTCRtpEncodingParameters` class.
- Changed the exception type `ObjectDisposedException` when accessing instance after call Dispose method. 

### Fixed

- Fixed a crash bug where initializing video streaming on Unity Editor on Apple Silicon.
- Fixed a crash bug where configuring OpenGL Core as a graphics API on Unity Editor on windows.
- Fixed a bug that sending stereo audio produces mono clip on some peers.
- Fixed a crach bug where using Full HD resolution with HWA enabled on macOS(Intel).
- Fixed a crash bug where using Full HD resolution with HWA enabled on Windows.

### Removed

- Removed iOS Simulator (x86_64) support for make simple building process for iOS.

## [2.4.0-exp.4] - 2021-08-19

### Added

- mac M1 architecture native support
- Audio stream rendering support
- Add two scenes ("Audio" and "MultiAudioReceive") into the package sample
- Add `RTCAudioSourceStats` and `VideoSourceStats` class

### Changed

- Add the audio waveform graph to "MultiplePeerConnections" scene in the sample

### Fixed

- Fix the crash on Windows with Vulkan API on the "VideoReceive" sample
- Fix the crash when calling `WebRTC.Initialize` twice
- Fix the error in the build process on `Unity2021.2`

## [2.4.0-exp.3] - 2021-06-08

### Changed

- Add options of the incoming video in "VideoReceive" sample to test video capture modules on the device

### Fixed

- Fix the validation for the color space of the incoming video
- Fix the crash when accessing the property `RTCRtpSender.Track`

## [2.4.0-exp.2] - 2021-05-21

### Fixed

- Fix the color space of the RenderTexture for streaming when using Vulkan API
- Fix sample scenes
- Add a short version string to info.plist of ios framework

### Changed

- Add the validation of the streaming texture size on Android
- Add the validation of the  streaming when using NvCodec
- Use the software video decoder when disabling hardware acceleration

## [2.4.0-exp.1] - 2021-04-23

### Added

- Android ARM64 platform support
- Added a sample scene "Menu" which developers can go back and forth between sample scenes
- Added a sample scene "PerfectNegotiation"
- Added the software encoder on Linux support when using OpenGL Core graphics API
- Added the `RestartIce` method to the `RTCPeerConnection` class
- Added the `Streams` property to the `RTCRtpReceiver` class

### Changed

- Unity 2020.3 support
- Upgrade libwebrtc [m89](https://groups.google.com/g/discuss-webrtc/c/Zrsn2hi8FV0/m/KIbn0EZPBQAJ)
- Changed the argument type of the `RTCPeerConnection.CreateOffer` method and the `RTCPeerConnection.CreateAnswer` method

### Fixed

- Fixed crash for accessing properties of `RTCDataChannel` instance
- Fixed crash when using the invalid graphics format to stream video on macOS

## [2.3.3-preview] - 2021-02-26

### Added

- Added `OnConnectionStateChange` event to the `RTCPeerConnection` class

### Fixed

- Fixed a crash bug that occurs when accessing `MediaStreamTrack` properties after disposing of `RTCPeerConnection`

## [2.3.2-preview] - 2021-02-12

### Changed

- Changed `Audio.CaptureStream` method to allow setting of audio track label.

### Fixed

- Fixed memory leaks in native code.
- Fixed a crash bug when access an instance after disposed of it.
- Fixed `MediaStream.GetVideoStreamTrack` method and `MediaStream.GetVideoStreamTrack` method to return a correct value.
- Fixed `RTCRtpTransceiver.Receiver` property and `RTCRtpTransceiver.Sender` property to return a correct value.

## [2.3.1-preview] - 2021-01-07

### Fixed

- Fixed `RTCIceCandidate.candidate` property in order to return a correct SDP formatted string.

## [2.3.0-preview] - 2020-12-28

### Added

- Supported iOS platform
- Supported H.264 HW decoder (VideoToolbox) on macOS
- Added `GetCapabilities` method to the `RTCRtpSender` class and the `RTCRtpReceiver` class
- Added `SetCodecPreferences` method to the `RTCRtpTransceiver` class
- Added two samples ("ChangeCodecs", "TrickleIce")
- Added properties to the `RTCIceCandidate` class
- Added properties tp the `RTCDataChannelInit` class

### Changed

- Changed `RTCIceCandidate` type from `struct` to `class`
- Changed `RTCIceCandidateInit` type from `struct` to `class`
- Changed `RTCDataChannelInit` type from `struct` to `class`
- Changed argumments of the `RTCPeerConnection.AddIceCandidate` method
```csharp
// old
public void AddIceCandidate(ref RTCIceCandidate candidate);
// new
public bool AddIceCandidate(RTCIceCandidate candidate);
```

- Changed arguments of the `RTCPeerConnection.CreateDataChannel` method
```csharp
// old
public RTCDataChannel CreateDataChannel(string label, ref RTCDataChannelInit options);
// new
public RTCDataChannel CreateDataChannel(string label RTCDataChannelInit options = null);
```

## [2.2.1-preview] - 2020-11-13

### Added

- Added a "Bandwidth" sample

### Fixed

- Fixed the receiver of video streaming with Vulkan API
- Fixed a crash bug when the application ended using Vulkan API
- Fixed a crash bug of the standalone build using Vulkan API
- Fixed bugs that occur on Linux not installed NVIDIA driver
- Fixed a bug of the "VideoReceive" sample

## [2.2.0-preview] - 2020-10-26

### Added

- Software decoder support
- Hardware encoder (VideoToolbox) support on macOS
- Vulkan API support on Linux and Windows
- Linux IL2CPP support
- Add WebRTC samples ("MultiplePeerConnections", "MultiVideoReceive", "MungeSDP", "VideoReceive")

### Changed

- Upgrade libwebrtc m85
- Upgrade NVIDIA Codec SDK 9.1
- Changed `RTCPeerConnection` behaviour to throw exceptions when pass invalid arguments to `SetLocalDescription`, `SetRemoteDescription`

## [2.1.3-preview] - 2020-09-28

### Changed

- Add "minBitrate" parameter to `RTCRtpEncodingParameters` class.

## [2.1.2-preview] - 2020-09-14

### Changed

- Erase Japanese documentation due to migrating to internal translation system.

## [2.1.1-preview] - 2020-09-11

### Fixed

- Fixed an issue where the `RTCRtpSender.SetParameters` API did not work properly
- Removed ZWSP(zero-width-space) in C# code

## [2.1.0-preview] - 2020-08-24

### Added

- Added statistics window in Unity editor to allow checking the operation of WebRTC
- Added `RTCPeerConnection.GetStats` API which collect statistics of WebRTC
- Added `RTCRtpSender.SetParameters` and `RTCRtpSender.GetParameters` to adjustment streaming video quality
- Added `RTCDataChannel.ReadyState` which shows the state of the channel

### Fixed

- Fixed a issue which video stream remains with bad quality after a short network degradation

## [2.0.5-preview] - 2020-07-30

### Fixed

- Upgrade libwebrtc m84 to fix security issue (https://bugs.chromium.org/p/project-zero/issues/detail?id=2034)

## [2.0.4-preview] - 2020-07-10

### Fixed

- Fix a crash bug when dispose a video track

## [2.0.3-preview] - 2020-06-05

### Fixed

- Fix the memory leak when using DirectX12

## [2.0.2-preview] - 2020-05-14

### Fixed

- Fix the crash when using the incorrect parameter to as the argument of `RTCDataChannel` constructor
- Fix the crash when initializing the hardware encoder failed
- Fix the editor freeze bug when recompiling scripts
- Fixed documents

## [2.0.1-preview] - 2020-05-01

### Fixed

- Fixed versioning issue

## [2.0.0-preview] - 2020-04-30

### Added

- Multi camera support
- DirectX 12 API support
- Published VideoStreamTrack API
- Published AudioStreamTrack API

## [1.1.2-preview] - 2020-03-19

### Fixed

- Fix OpenGL color order

## [1.1.1-preview] - 2020-02-28

### Fixed

- Fix DLL import error

## [1.1.0-preview] - 2020-02-25

### Added

- IL2CPP support
- Linux OpenGL hardware encoder support
- Mac OS Metal software encoder support
- Windows DirectX11 software encoder support

### Changed

- Changed `Audio.Update` method to public

## [1.0.1-preview] - 2019-09-22

### Fixed

- Fixed documents

## [1.0.0-preview] - 2019-08-22

### Added

- Added tooltips

### Changed

- Renamed sample folders

## [0.2.0-preview] - 2019-07-30

### Changed

- Output logs when NVCodec failed to initialize

## [0.1.0-preview] - 2019-07-02

- Initial Release
