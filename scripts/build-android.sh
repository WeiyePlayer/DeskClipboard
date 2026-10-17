#!/bin/bash
# Unit tests + debug APK for android/, using the user-local toolchain from
# scripts/android-toolchain.sh. Output: android/app/build/outputs/apk/debug/app-debug.apk
set -euo pipefail
ROOT="${DESKCLIP_ANDROID_TOOLS:-$HOME/.local/share/deskclip-android}"
[[ -x "$ROOT/gradle/bin/gradle" ]] || { echo "run scripts/android-toolchain.sh first" >&2; exit 1; }
JAVA_HOME="$ROOT/jdk"; [[ -d "$JAVA_HOME/Contents/Home" ]] && JAVA_HOME="$JAVA_HOME/Contents/Home"
export JAVA_HOME ANDROID_HOME="$ROOT/sdk"
cd "$(dirname "$0")/../android"
echo "sdk.dir=$ANDROID_HOME" > local.properties
"$ROOT/gradle/bin/gradle" --no-daemon testDebugUnitTest assembleDebug "$@"
ls -la app/build/outputs/apk/debug/app-debug.apk
