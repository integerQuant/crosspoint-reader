#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
output_dir=
skip_firmware=false
skip_linux=false

usage() {
    cat <<'EOF'
Build a complete knietty release matrix on the local machine.

Usage: scripts/build-knietty-release-local.sh [OPTIONS]

Options:
  --output PATH       Output directory (default: release-dist/knietty-VERSION)
  --skip-firmware     Do not build the X4 firmware
  --skip-linux        Do not build Ubuntu and Arch artifacts in containers
  -h, --help          Show this help

The default build requires rustup, PlatformIO, and Docker or Podman. It builds
the native host, Ubuntu x86_64 host, Arch x86_64 host, firmware, installer, and
SHA-256 files without using GitHub Actions.
EOF
}

fail() {
    printf 'knietty release build: %s\n' "$*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --output)
            [ "$#" -ge 2 ] || fail '--output requires a value'
            output_dir=$2
            shift 2
            ;;
        --skip-firmware)
            skip_firmware=true
            shift
            ;;
        --skip-linux)
            skip_linux=true
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *) fail "unknown option: $1" ;;
    esac
done

firmware_version=$(sed -n '/^\[knietty\]$/,/^\[/s/^version = //p' "$repo_root/platformio.ini")
host_version=$(sed -n '/^\[package\]$/,/^\[/s/^version = "\([^"]*\)"/\1/p' "$repo_root/host-rs/Cargo.toml")
[ -n "$firmware_version" ] || fail 'could not read the firmware version'
[ "$firmware_version" = "$host_version" ] ||
    fail "firmware version $firmware_version does not match host version $host_version"
version=$firmware_version

if [ -z "$output_dir" ]; then
    output_dir=$repo_root/release-dist/knietty-$version
fi
case "$output_dir" in
    /*) ;;
    *) output_dir=$repo_root/$output_dir ;;
esac

if [ -d "$output_dir" ] && [ -n "$(find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    fail "output directory is not empty: $output_dir"
fi

command -v rustup >/dev/null 2>&1 || fail 'rustup is required'

container_engine=
if [ "$skip_linux" = false ]; then
    if command -v docker >/dev/null 2>&1; then
        container_engine=docker
    elif command -v podman >/dev/null 2>&1; then
        container_engine=podman
    else
        fail 'Docker or Podman is required for Linux release artifacts (or pass --skip-linux)'
    fi
fi

pio=
if [ "$skip_firmware" = false ]; then
    if [ -x "$repo_root/.venv/bin/pio" ]; then
        pio=$repo_root/.venv/bin/pio
    elif command -v pio >/dev/null 2>&1; then
        pio=$(command -v pio)
    else
        fail 'PlatformIO is required for firmware (or pass --skip-firmware)'
    fi
fi

mkdir -p "$output_dir"
cp "$repo_root/host-rs/install.sh" "$output_dir/knietty-install.sh"
chmod 0755 "$output_dir/knietty-install.sh"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$output_dir" && sha256sum knietty-install.sh > knietty-install.sh.sha256)
else
    (cd "$output_dir" && shasum -a 256 knietty-install.sh > knietty-install.sh.sha256)
fi

printf 'Building native host...\n'
rustup toolchain install 1.80.0 --profile minimal --component rustfmt,clippy
(cd "$repo_root/host-rs" && ./scripts/build-release-host.sh "$version" '' "$output_dir")

if [ "$skip_linux" = false ]; then
    host_uid=$(id -u)
    host_gid=$(id -g)
    printf 'Building Ubuntu x86_64 host with %s...\n' "$container_engine"
    # RELEASE_VERSION and HOME expand inside the container.
    # shellcheck disable=SC2016
    "$container_engine" run --rm --platform linux/amd64 \
        -e HOST_UID="$host_uid" -e HOST_GID="$host_gid" \
        -e RELEASE_VERSION="$version" \
        -e CARGO_TARGET_DIR=/tmp/knietty-target \
        -v "$repo_root:/src:ro" -v "$output_dir:/dist" -w /src \
        ubuntu:24.04 sh -c '
            set -eu
            apt-get update
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential ca-certificates curl pkg-config
            curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none
            . "$HOME/.cargo/env"
            rustup toolchain install 1.80.0 --profile minimal --component rustfmt,clippy
            cd host-rs
            ./scripts/build-release-host.sh "$RELEASE_VERSION" "" /dist
        '

    printf 'Building Arch x86_64 host with %s...\n' "$container_engine"
    # RELEASE_VERSION expands inside the container.
    # shellcheck disable=SC2016
    "$container_engine" run --rm --platform linux/amd64 \
        -e HOST_UID="$host_uid" -e HOST_GID="$host_gid" \
        -e RELEASE_VERSION="$version" \
        -e CARGO_TARGET_DIR=/tmp/knietty-target \
        -v "$repo_root:/src:ro" -v "$output_dir:/dist" -w /src \
        archlinux:base-devel sh -c '
            set -eu
            sed -i "s/^#DisableSandboxSyscalls/DisableSandboxSyscalls/" /etc/pacman.conf
            pacman -Syu --noconfirm --needed ca-certificates git rustup
            rustup toolchain install 1.80.0 --profile minimal --component rustfmt,clippy
            cd host-rs
            ./scripts/build-release-host.sh "$RELEASE_VERSION" archlinux /dist
        '
fi

if [ "$skip_firmware" = false ]; then
    printf 'Building X4 firmware...\n'
    (cd "$repo_root" && "$pio" run -e knietty_async_window -j1)
    firmware_asset=knietty-$version.bin
    cp "$repo_root/.pio/build/knietty_async_window/firmware.bin" "$output_dir/$firmware_asset"
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$output_dir" && sha256sum "$firmware_asset" > "$firmware_asset.sha256")
    else
        (cd "$output_dir" && shasum -a 256 "$firmware_asset" > "$firmware_asset.sha256")
    fi
fi

printf '\nRelease artifacts are ready in %s\n' "$output_dir"
