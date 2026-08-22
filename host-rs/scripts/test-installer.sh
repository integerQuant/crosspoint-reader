#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
installer=$script_dir/../install.sh
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/knietty-installer-test.XXXXXX")
cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

version=9.8.7
release_dir=$tmp_dir/releases/download/knietty-v$version
mkdir -p "$release_dir"

checksum() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" > "$1.sha256"
    else
        shasum -a 256 "$1" > "$1.sha256"
    fi
}

make_asset() {
    target=$1
    marker=$2
    staging=$tmp_dir/staging-$marker
    mkdir -p "$staging"
    printf '#!/bin/sh\nprintf "knietty 9.8.7 %s\\n"\n' "$marker" > "$staging/knietty"
    chmod 0755 "$staging/knietty"
    archive=$release_dir/knietty-host-$version-$target.tar.gz
    tar -C "$staging" -czf "$archive" knietty
    checksum "$archive"
}

make_asset aarch64-apple-darwin mac-arm
make_asset x86_64-unknown-linux-gnu linux
make_asset archlinux-x86_64-unknown-linux-gnu arch

run_install() {
    home=$1
    os=$2
    machine=$3
    os_release=$4
    KNIETTY_RELEASE_BASE_URL=file://$tmp_dir/releases \
        KNIETTY_INSTALL_DIR=$home/bin \
        _KNIETTY_UNAME_S=$os \
        _KNIETTY_UNAME_M=$machine \
        _KNIETTY_OS_RELEASE_FILE=$os_release \
        sh "$installer" --version "$version"
}

printf 'ID=ubuntu\n' > "$tmp_dir/ubuntu-release"
printf 'ID=arch\n' > "$tmp_dir/arch-release"

run_install "$tmp_dir/mac-home" Darwin arm64 "$tmp_dir/missing"
[ "$("$tmp_dir/mac-home/bin/knietty" --version)" = 'knietty 9.8.7 mac-arm' ]

run_install "$tmp_dir/linux-home" Linux x86_64 "$tmp_dir/ubuntu-release"
[ "$("$tmp_dir/linux-home/bin/knietty" --version)" = 'knietty 9.8.7 linux' ]

run_install "$tmp_dir/arch-home" Linux x86_64 "$tmp_dir/arch-release"
[ "$("$tmp_dir/arch-home/bin/knietty" --version)" = 'knietty 9.8.7 arch' ]

printf '#!/bin/sh\nprintf "preserved\\n"\n' > "$tmp_dir/linux-home/bin/knietty"
chmod 0755 "$tmp_dir/linux-home/bin/knietty"
printf '%064d  bad\n' 0 > "$release_dir/knietty-host-$version-x86_64-unknown-linux-gnu.tar.gz.sha256"
if run_install "$tmp_dir/linux-home" Linux x86_64 "$tmp_dir/ubuntu-release" >/dev/null 2>&1; then
    printf 'bad checksum unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$("$tmp_dir/linux-home/bin/knietty")" = preserved ]

if run_install "$tmp_dir/arm-home" Linux aarch64 "$tmp_dir/ubuntu-release" >/dev/null 2>&1; then
    printf 'unsupported Linux architecture unexpectedly succeeded\n' >&2
    exit 1
fi

KNIETTY_INSTALL_DIR=$tmp_dir/mac-home/bin sh "$installer" --uninstall
[ ! -e "$tmp_dir/mac-home/bin/knietty" ]

printf 'installer tests passed\n'
