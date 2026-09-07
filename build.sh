#!/bin/zsh
set -euo pipefail

project_dir="${0:A:h}"
build_dir="$project_dir/build"
app_name="Delay Lama Standalone.app"
app_dir="$build_dir/$app_name"
sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
deployment="13.0"

rm -rf "$build_dir"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources"

build_arch() {
    local arch="$1"
    local arch_dir="$build_dir/$arch"
    mkdir -p "$arch_dir"

    for source in "$project_dir"/DSP/*.c "$project_dir/Sources/standalone_bridge.c"; do
        local object="$arch_dir/${source:t:r}.o"
        xcrun clang -arch "$arch" -isysroot "$sdk_path" -mmacosx-version-min="$deployment" \
            -O3 -DNDEBUG -std=c11 -I"$project_dir/DSP" -I"$project_dir/Sources" \
            -c "$source" -o "$object"
    done

    xcrun swiftc -target "$arch-apple-macosx$deployment" -O -whole-module-optimization \
        -sdk "$sdk_path" \
        -module-cache-path "$arch_dir/module-cache" \
        -import-objc-header "$project_dir/Sources/DelayLama-Bridging.h" \
        -Xcc -fmodules-cache-path="$arch_dir/module-cache" \
        -Xcc -I"$project_dir/Sources" -Xcc -I"$project_dir/DSP" \
        "$project_dir/Sources/main.swift" "$arch_dir"/*.o \
        -framework AppKit -framework AVFoundation -framework CoreMIDI \
        -o "$arch_dir/DelayLamaStandalone"
}

build_arch arm64
build_arch x86_64
lipo -create "$build_dir/arm64/DelayLamaStandalone" "$build_dir/x86_64/DelayLamaStandalone" \
    -output "$app_dir/Contents/MacOS/DelayLamaStandalone"

cp "$project_dir/Info.plist" "$app_dir/Contents/Info.plist"
cp "$project_dir/Resources/"* "$app_dir/Contents/Resources/"
cp "$project_dir/MONKSYNTH-LICENSE.txt" "$app_dir/Contents/Resources/"

xattr -cr "$app_dir"
codesign --force --deep --sign - "$app_dir"
codesign --verify --deep --strict "$app_dir"

echo "$app_dir"
