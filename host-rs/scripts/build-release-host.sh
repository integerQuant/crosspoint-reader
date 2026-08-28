#!/bin/sh

set -eu

[ "$#" -eq 3 ] || {
    printf 'usage: %s VERSION FLAVOR OUTPUT_DIR\n' "$0" >&2
    exit 2
}

version=$1
flavor=$2
output_dir=$3
toolchain=1.80.0
target_dir=${CARGO_TARGET_DIR:-target}

cargo +$toolchain fmt --check
cargo +$toolchain test --locked
cargo +$toolchain clippy --locked --all-targets -- -D warnings
cargo +$toolchain build --locked --release

host_triple=$(rustc +$toolchain -vV | sed -n 's/^host: //p')
[ -n "$host_triple" ]
if [ -n "$flavor" ]; then
    asset=knietty-host-$version-$flavor-$host_triple.tar.gz
else
    asset=knietty-host-$version-$host_triple.tar.gz
fi

mkdir -p "$output_dir"
tar -C "$target_dir/release" -czf "$output_dir/$asset" knietty
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$output_dir" && sha256sum "$asset" > "$asset.sha256")
else
    (cd "$output_dir" && shasum -a 256 "$asset" > "$asset.sha256")
fi

if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ] && command -v chown >/dev/null 2>&1; then
    chown "$HOST_UID:$HOST_GID" "$output_dir/$asset" "$output_dir/$asset.sha256" 2>/dev/null || true
fi

printf 'Built %s\n' "$output_dir/$asset"
