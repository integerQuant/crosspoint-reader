#!/bin/sh

set -eu

repository=${KNIETTY_REPOSITORY:-integerQuant/crosspoint-reader}
release_base=${KNIETTY_RELEASE_BASE_URL:-https://github.com/$repository/releases}
install_dir=${KNIETTY_INSTALL_DIR:-${HOME:?HOME is required}/.local/bin}
requested_version=
uninstall=false

usage() {
    cat <<'EOF'
Install the knietty host for the current user.

Usage: install.sh [OPTIONS]

Options:
  --version VERSION      Install a specific release (for example, 0.1.2)
  --install-dir PATH     Install into PATH (default: ~/.local/bin)
  --uninstall            Remove knietty from the selected install directory
  -h, --help             Show this help

Environment:
  KNIETTY_INSTALL_DIR          Alternative user-owned install directory
  KNIETTY_REPOSITORY           Alternative GitHub owner/repository
  KNIETTY_RELEASE_BASE_URL     Alternative release base URL
EOF
}

fail() {
    printf 'knietty installer: %s\n' "$*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --version)
            [ "$#" -ge 2 ] || fail '--version requires a value'
            requested_version=$2
            shift 2
            ;;
        --install-dir)
            [ "$#" -ge 2 ] || fail '--install-dir requires a value'
            install_dir=$2
            shift 2
            ;;
        --uninstall)
            uninstall=true
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

case "$install_dir" in
    /*) ;;
    *) fail '--install-dir must be an absolute path' ;;
esac

destination=$install_dir/knietty
if [ "$uninstall" = true ]; then
    if [ -e "$destination" ]; then
        rm -f "$destination"
        printf 'Removed %s\n' "$destination"
    else
        printf 'knietty is not installed at %s\n' "$destination"
    fi
    exit 0
fi

command -v curl >/dev/null 2>&1 || fail 'curl is required'
command -v tar >/dev/null 2>&1 || fail 'tar is required'

os=${_KNIETTY_UNAME_S:-$(uname -s)}
machine=${_KNIETTY_UNAME_M:-$(uname -m)}
os_release_file=${_KNIETTY_OS_RELEASE_FILE:-/etc/os-release}

case "$os:$machine" in
    Darwin:arm64 | Darwin:aarch64)
        target=aarch64-apple-darwin
        ;;
    Darwin:x86_64 | Darwin:amd64)
        fail 'macOS Intel is not available in current knietty releases'
        ;;
    Linux:x86_64 | Linux:amd64)
        if [ -r "$os_release_file" ] &&
            grep -Eiq '^(ID|ID_LIKE)=.*(arch|manjaro|endeavour|garuda)' "$os_release_file"; then
            target=archlinux-x86_64-unknown-linux-gnu
        elif [ -r "$os_release_file" ] && grep -Eiq '^ID=(alpine|postmarketos)$' "$os_release_file"; then
            fail 'musl-based Linux is not available in current knietty releases'
        else
            target=x86_64-unknown-linux-gnu
        fi
        ;;
    Linux:*)
        fail "Linux architecture $machine is not available in current knietty releases"
        ;;
    *)
        fail "unsupported platform: $os $machine"
        ;;
esac

if [ -n "$requested_version" ]; then
    version=$requested_version
else
    latest_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "$release_base/latest") ||
        fail 'could not resolve the latest knietty release'
    version=${latest_url%/}
    version=${version##*/}
    version=${version#knietty-v}
fi

case "$version" in
    '' | *[!0-9A-Za-z._-]*) fail "invalid release version: $version" ;;
esac

asset=knietty-host-$version-$target.tar.gz
download_base=${release_base%/}/download/knietty-v$version

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/knietty-install.XXXXXX") ||
    fail 'could not create a temporary directory'
cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

printf 'Downloading knietty %s for %s...\n' "$version" "$target"
curl -fsSL "$download_base/$asset" -o "$tmp_dir/$asset" ||
    fail "could not download $asset"
curl -fsSL "$download_base/$asset.sha256" -o "$tmp_dir/$asset.sha256" ||
    fail "could not download $asset.sha256"

expected=$(awk 'NR == 1 { print $1 }' "$tmp_dir/$asset.sha256")
case "$expected" in
    '' | *[!0-9A-Fa-f]*) fail 'release checksum is malformed' ;;
esac
[ "${#expected}" -eq 64 ] || fail 'release checksum is malformed'

if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp_dir/$asset" | awk '{ print $1 }')
elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$tmp_dir/$asset" | awk '{ print $1 }')
else
    fail 'sha256sum or shasum is required'
fi

[ "$actual" = "$expected" ] || fail "checksum verification failed for $asset"

mkdir -p "$tmp_dir/unpacked"
tar -xzf "$tmp_dir/$asset" -C "$tmp_dir/unpacked"
[ -f "$tmp_dir/unpacked/knietty" ] || fail 'release archive does not contain knietty'
chmod 0755 "$tmp_dir/unpacked/knietty"
"$tmp_dir/unpacked/knietty" --version >/dev/null 2>&1 ||
    fail 'downloaded knietty binary did not start successfully'

mkdir -p "$install_dir"
staged=$install_dir/.knietty.install.$$
trap 'rm -f "$staged"; cleanup' EXIT HUP INT TERM
cp "$tmp_dir/unpacked/knietty" "$staged"
chmod 0755 "$staged"
mv -f "$staged" "$destination"

printf 'Installed knietty %s at %s\n' "$version" "$destination"
case ":${PATH:-}:" in
    *:"$install_dir":*) ;;
    *)
        printf '\nAdd %s to PATH, then open a new shell:\n' "$install_dir"
        # Print a command whose PATH is expanded by the user's next shell.
        # shellcheck disable=SC2016
        printf '  export PATH="%s:$PATH"\n' "$install_dir"
        ;;
esac
printf '\nDiscover an X4:  knietty list\n'
printf 'Connect:         knietty --host auto\n'
