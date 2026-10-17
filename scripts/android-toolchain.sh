#!/bin/bash
# User-local Android build toolchain for android/ (no sudo, nothing system-wide).
# Installs JDK 17, Android command-line tools + SDK platform/build-tools, and
# Gradle into $DESKCLIP_ANDROID_TOOLS (default ~/.local/share/deskclip-android).
# Running it again only fills in what is missing. Remove that directory to uninstall.
# Note: installing SDK packages accepts the Android SDK license (sdkmanager --licenses).
set -euo pipefail
ROOT="${DESKCLIP_ANDROID_TOOLS:-$HOME/.local/share/deskclip-android}"
JDK_URL_MAC="https://api.adoptium.net/v3/binary/latest/17/ga/mac/aarch64/jdk/hotspot/normal/eclipse"
JDK_URL_MAC_X64="https://api.adoptium.net/v3/binary/latest/17/ga/mac/x64/jdk/hotspot/normal/eclipse"
JDK_URL_LINUX="https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"
TOOLS_MAC="https://dl.google.com/android/repository/commandlinetools-mac-11076708_latest.zip"
TOOLS_LINUX="https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
GRADLE_VERSION=8.7
GRADLE_URL="https://services.gradle.org/distributions/gradle-$GRADLE_VERSION-bin.zip"
mkdir -p "$ROOT/download"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) JDK_URL=$JDK_URL_MAC; TOOLS_URL=$TOOLS_MAC ;;
  Darwin-*) JDK_URL=$JDK_URL_MAC_X64; TOOLS_URL=$TOOLS_MAC ;;
  *) JDK_URL=$JDK_URL_LINUX; TOOLS_URL=$TOOLS_LINUX ;;
esac
if [[ ! -d "$ROOT/jdk" ]]; then
  echo "== JDK 17"
  curl -fL --retry 3 -o "$ROOT/download/jdk.tar.gz" "$JDK_URL"
  rm -rf "$ROOT/jdk.tmp" && mkdir "$ROOT/jdk.tmp"
  tar -xzf "$ROOT/download/jdk.tar.gz" -C "$ROOT/jdk.tmp" --strip-components 1
  mv "$ROOT/jdk.tmp" "$ROOT/jdk"
fi
JAVA_HOME="$ROOT/jdk"; [[ -d "$JAVA_HOME/Contents/Home" ]] && JAVA_HOME="$JAVA_HOME/Contents/Home"
export JAVA_HOME
if [[ ! -x "$ROOT/sdk/cmdline-tools/latest/bin/sdkmanager" ]]; then
  echo "== Android command-line tools"
  curl -fL --retry 3 -o "$ROOT/download/cmdline-tools.zip" "$TOOLS_URL"
  rm -rf "$ROOT/sdk/cmdline-tools" && mkdir -p "$ROOT/sdk/cmdline-tools"
  unzip -q "$ROOT/download/cmdline-tools.zip" -d "$ROOT/sdk/cmdline-tools"
  mv "$ROOT/sdk/cmdline-tools/cmdline-tools" "$ROOT/sdk/cmdline-tools/latest"
fi
if [[ ! -d "$ROOT/sdk/platforms/android-34" || ! -d "$ROOT/sdk/build-tools/34.0.0" ]]; then
  echo "== SDK platform 34 + build-tools 34.0.0"
  yes | "$ROOT/sdk/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$ROOT/sdk" --licenses >/dev/null || true
  "$ROOT/sdk/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$ROOT/sdk" "platforms;android-34" "build-tools;34.0.0" "platform-tools"
fi
if [[ ! -x "$ROOT/gradle/bin/gradle" ]]; then
  echo "== Gradle $GRADLE_VERSION"
  curl -fL --retry 3 -o "$ROOT/download/gradle.zip" "$GRADLE_URL"
  rm -rf "$ROOT/gradle-$GRADLE_VERSION" && unzip -q "$ROOT/download/gradle.zip" -d "$ROOT"
  rm -rf "$ROOT/gradle" && mv "$ROOT/gradle-$GRADLE_VERSION" "$ROOT/gradle"
fi
rm -rf "$ROOT/download"
echo "toolchain ready in $ROOT ($(du -sh "$ROOT" | cut -f1))"
