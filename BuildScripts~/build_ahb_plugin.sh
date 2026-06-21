#!/usr/bin/env bash
# Build the custom com.unity.webrtc native plugin (zero-copy H.264 AHB decoder, arm64)
# and ship the resulting libwebrtc.so into coannotate's embedded package .aar.
#
# Prereqs: the prebuilt libwebrtc must already be unpacked into Plugin~/webrtc/{lib,include}
# (BuildScripts~/build_plugin_android.sh fetches it from the M116-20250805 release once).
#
# API 26 is required (AImageReader / AImage_getHardwareBuffer / AHardwareBuffer are
# __INTRODUCED_IN(26)); the coannotate app targets API 34 so this is fine. Changing the API
# needs a clean reconfigure (rm -rf build-arm64).
set -euo pipefail

UNITY="/Applications/Unity/Hub/Editor/6000.0.49f1/PlaybackEngines/AndroidPlayer"
ANDROID_NDK="$UNITY/NDK"
CMAKE="$UNITY/SDK/cmake/3.22.1/bin/cmake"
PLUGIN_DIR="$(cd "$(dirname "$0")/.." && pwd)/Plugin~"
SO_OUT="$(cd "$(dirname "$0")/.." && pwd)/Runtime/Plugins/Android/libwebrtc.so"
AAR="$HOME/git_repo/coannotate/Packages/com.unity.webrtc/Runtime/Plugins/Android/libwebrtc.aar"

cd "$PLUGIN_DIR"
if [ ! -d build-arm64 ]; then
  echo "=== configure (arm64-v8a, API 26, Release) ==="
  "$CMAKE" . -B build-arm64 \
    -D CMAKE_SYSTEM_NAME=Android -D CMAKE_ANDROID_API_MIN=26 -D CMAKE_ANDROID_API=26 \
    -D CMAKE_ANDROID_ARCH_ABI=arm64-v8a -D CMAKE_ANDROID_NDK="$ANDROID_NDK" \
    -D CMAKE_BUILD_TYPE=Release -D CMAKE_ANDROID_STL_TYPE=c++_static
fi

echo "=== build WebRTCPlugin ==="
"$CMAKE" --build build-arm64 --target WebRTCPlugin -j8

echo "=== ship libwebrtc.so into the embedded .aar (jni/arm64-v8a) ==="
ls -la "$SO_OUT"
STAGE="$(mktemp -d)"; mkdir -p "$STAGE/jni/arm64-v8a"
cp "$SO_OUT" "$STAGE/jni/arm64-v8a/libwebrtc.so"
( cd "$STAGE" && zip -gq "$AAR" jni/arm64-v8a/libwebrtc.so )
rm -rf "$STAGE"
unzip -l "$AAR" | grep arm64-v8a/libwebrtc.so
echo "=== done. Rebuild the APK in Unity to pick up the new plugin. ==="
