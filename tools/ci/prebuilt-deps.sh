#!/usr/bin/env bash
# Builds LGPL FFmpeg with the pinned Kazarov configuration and fetches checksum-pinned cxxbridge-cmd.
#   tools/ci/prebuilt-deps.sh linux-x64|macos-arm64
set -euo pipefail
release=https://github.com/Plarpoon/Kazarov/releases/download/deps-1
# Kazarov's own build of the same FFmpeg (tools/windows/common.ps1 takes Windows' from it too). kz-ffmpeg keeps it
# when its stamp matches the configuration it would build, so ffmpeg.org is only asked when it doesn't.
ffmpeg_release=https://github.com/TheMoonPeople/Kazarov/releases/download/deps-ffmpeg-9.0.2-7a3806abd8eb-cxxbridge-1.0.205
case ${1:-} in
    linux-x64)
        cxxbridge_sha=0fd5a8d3de5ec5471d1535e89e021b2cc7ec1709625e58f00206185ac2b30de6
        ffmpeg_sha=7880ab780e7881c1b2033ec2f55199b9ab382964c978fe461809c4e41f07d525
        ffmpeg_dir=.kazarov/ffmpeg/linux-x64 ;;
    macos-arm64)
        cxxbridge_sha=a74d272155569f5d776f222457a7b3bbc7da6aece66e1ffc6eadb7516f8267b6
        ffmpeg_sha=82e80a8589a576fdbb883a5647a28152602bc6e7d407ec27e8c753c2ec9aac87
        ffmpeg_dir=.kazarov/ffmpeg/darwin-arm64 ;;
    *) echo "usage: $0 linux-x64|macos-arm64" >&2; exit 2 ;;
esac
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fetch() {
    curl -fsSL --retry 3 -o "$tmp/$1" "${3:-$release}/$1"
    echo "$2  $tmp/$1" | shasum -a 256 -c -
}

if [ ! -f "$root/$ffmpeg_dir/kz-ffmpeg.json" ] && fetch "kz-ffmpeg-9.0.2-$1.tar.gz" "$ffmpeg_sha" "$ffmpeg_release"; then
    mkdir -p "$root/$ffmpeg_dir"
    tar -xzf "$tmp/kz-ffmpeg-9.0.2-$1.tar.gz" -C "$root/$ffmpeg_dir"
fi
python3 "$root/tools/kazarov/scripts/build/kz-ffmpeg" --out "$root/$ffmpeg_dir"

fetch "cxxbridge-1.0.205-$1.tar.gz" "$cxxbridge_sha"
bin=$root/.kazarov/bin
mkdir -p "$bin"
tar -xzf "$tmp/cxxbridge-1.0.205-$1.tar.gz" -C "$bin"
if [ -n "${GITHUB_PATH:-}" ]; then echo "$bin" >> "$GITHUB_PATH"; fi
