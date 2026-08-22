#!/bin/sh

set -eu

replace_existing=false
draft=false

usage() {
    cat <<'EOF'
Publish locally built knietty artifacts to a GitHub release.

Usage: scripts/publish-knietty-release-local.sh [OPTIONS] VERSION DIST_DIR

Options:
  --draft               Create a draft release
  --replace-existing    Replace matching assets on an existing release
  -h, --help            Show this help

The exact knietty-vVERSION tag must already exist and point at HEAD. The script
verifies every adjacent SHA-256 file before making a GitHub change.
EOF
}

fail() {
    printf 'knietty release publish: %s\n' "$*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --draft)
            draft=true
            shift
            ;;
        --replace-existing)
            replace_existing=true
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        --) shift; break ;;
        -*) fail "unknown option: $1" ;;
        *) break ;;
    esac
done

[ "$#" -eq 2 ] || {
    usage >&2
    exit 2
}

version=$1
dist_dir=$2
tag=knietty-v$version

command -v gh >/dev/null 2>&1 || fail 'gh is required'
command -v git >/dev/null 2>&1 || fail 'git is required'
[ -d "$dist_dir" ] || fail "artifact directory does not exist: $dist_dir"

tag_commit=$(git rev-list -n 1 "$tag" 2>/dev/null) || fail "local tag does not exist: $tag"
head_commit=$(git rev-parse HEAD)
[ "$tag_commit" = "$head_commit" ] || fail "$tag does not point at HEAD"

firmware_version=$(sed -n '/^\[knietty\]$/,/^\[/s/^version = //p' platformio.ini)
host_version=$(sed -n '/^\[package\]$/,/^\[/s/^version = "\([^"]*\)"/\1/p' host-rs/Cargo.toml)
[ "$version" = "$firmware_version" ] || fail "tag version does not match firmware: $firmware_version"
[ "$version" = "$host_version" ] || fail "tag version does not match host: $host_version"

for required in \
    "knietty-$version.bin" \
    "knietty-host-$version-aarch64-apple-darwin.tar.gz" \
    "knietty-host-$version-x86_64-unknown-linux-gnu.tar.gz" \
    "knietty-host-$version-archlinux-x86_64-unknown-linux-gnu.tar.gz" \
    knietty-install.sh; do
    [ -f "$dist_dir/$required" ] || fail "missing release asset: $required"
    [ -f "$dist_dir/$required.sha256" ] || fail "missing checksum: $required.sha256"
done

for checksum_file in "$dist_dir"/*.sha256; do
    [ -f "$checksum_file" ] || fail 'no checksum files found'
    expected=$(awk 'NR == 1 { print $1 }' "$checksum_file")
    asset_name=$(basename "$checksum_file" .sha256)
    [ -f "$dist_dir/$asset_name" ] || fail "checksum has no asset: $asset_name"
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$dist_dir/$asset_name" | awk '{ print $1 }')
    else
        actual=$(shasum -a 256 "$dist_dir/$asset_name" | awk '{ print $1 }')
    fi
    [ "$actual" = "$expected" ] || fail "checksum failed: $asset_name"
done

repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
    [ "$replace_existing" = true ] ||
        fail "$tag already exists; pass --replace-existing to replace its matching assets"
    gh release upload "$tag" "$dist_dir"/* --clobber --repo "$repo"
else
    set -- gh release create "$tag" "$dist_dir"/* --repo "$repo" \
        --verify-tag --title "knietty $version" --generate-notes
    if [ "$draft" = true ]; then
        set -- "$@" --draft
    fi
    "$@"
fi

printf 'Published %s to %s\n' "$tag" "$repo"
