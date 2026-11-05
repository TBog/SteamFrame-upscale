#!/usr/bin/env bash
# Tests get.sh's install / uninstall against a throwaway HOME, using a copy of this checkout as the
# source (no network: any fetch would fail). Also checks that `steamframe-passthrough-ctl uninstall`
# leaves exactly what `get.sh --uninstall` leaves -- they are two implementations of the same removal
# (one must work offline from the headset, the other is fetched with curl), so they must not drift.
# Usage: installer_test.sh <repo root>
# Exits 77 (ctest: skipped) when libvulkan.so.1 is absent, because get.sh itself refuses to run then.
set -u

repo="${1:?usage: installer_test.sh <repo root>}"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
fails=0

# The same library check get.sh makes.
have_vulkan() {
    ldconfig -p 2>/dev/null | grep -q libvulkan.so.1 && return 0
    local d
    for d in /usr/lib /usr/lib64 "/usr/lib/$(uname -m)-linux-gnu" /lib /lib64 /usr/local/lib; do
        [ -e "$d/libvulkan.so.1" ] && return 0
    done
    return 1
}
have_vulkan || { echo "SKIP: libvulkan.so.1 not found (get.sh needs it)"; exit 77; }

# A CR-free copy of the files get.sh reads from a checkout (a Windows checkout may hold CRLF copies).
src="$root/src"
mkdir -p "$src/scripts" "$src/contrib"
strip_cr() { tr -d '\r' <"$1" >"$2"; }
strip_cr "$repo/get.sh" "$src/get.sh"
strip_cr "$repo/config.ini" "$src/config.ini"
strip_cr "$repo/scripts/steamframe-passthrough-ctl" "$src/scripts/steamframe-passthrough-ctl"
for f in "$repo"/contrib/steamframe-passthrough-*.desktop; do strip_cr "$f" "$src/contrib/$(basename "$f")"; done
printf 'not a real library\n' >"$root/fake-layer.so"

getsh() { # home, args...
    local h="$1"
    shift
    env -u XDG_DATA_HOME HOME="$h" SF_LIB_PATH="$root/fake-layer.so" \
        SF_RAW_URL=http://127.0.0.1:9 SF_BASE_URL=http://127.0.0.1:9 bash "$src/get.sh" "$@" >/dev/null 2>&1
}
ctl_of() { # home, args... -- the *installed* copy of the script
    local h="$1"
    shift
    env -u XDG_DATA_HOME -u SF_CONFIG -u SF_LOG -u XDG_CACHE_HOME HOME="$h" \
        bash "$h/.local/bin/steamframe-passthrough-ctl" "$@" --no-wait
}
files() { (cd "$1" && find . -type f -not -name mimeinfo.cache | sort); }
fresh_home() { local h="$root/home$1"; rm -rf "$h"; mkdir -p "$h"; printf '%s\n' "$h"; }

check() { # description, command...
    local what="$1"
    shift
    if "$@"; then echo "ok   $what"; else echo "FAIL $what"; fails=$((fails + 1)); fi
}
same() { test "$1" = "$2"; }

expected_install="$(printf '%s\n' \
    ./.config/steamframe-passthrough/config.ini \
    ./.local/bin/steamframe-passthrough-ctl \
    ./.local/lib/openxr/libsteamframe_layer.so \
    ./.local/share/applications/steamframe-passthrough-status.desktop \
    ./.local/share/applications/steamframe-passthrough-toggle.desktop \
    ./.local/share/openxr/1/api_layers/implicit.d/steamframe_passthrough.json)"
expected_after_uninstall=./.config/steamframe-passthrough/config.ini

# --- install -------------------------------------------------------------------------------------
h="$(fresh_home 1)"
getsh "$h"
check "get.sh installs the library, manifest, script, launchers and config" same "$(files "$h")" "$expected_install"
manifest="$h/.local/share/openxr/1/api_layers/implicit.d/steamframe_passthrough.json"
check "the installed manifest has no enable_environment" test "$(grep -c enable_environment "$manifest")" = 0
check "the installed manifest points at the installed library" grep -q "\"library_path\": \"$h/.local/lib/openxr/libsteamframe_layer.so\"" "$manifest"
check "the launchers name the script by absolute path" grep -q "^Exec=konsole -e $h/.local/bin/steamframe-passthrough-ctl status\$" \
    "$h/.local/share/applications/steamframe-passthrough-status.desktop"
check "no @CTL@ placeholder is left in a launcher" test "$(grep -l '@CTL@' "$h"/.local/share/applications/*.desktop 2>/dev/null | wc -l)" = 0
check "the installed script runs" ctl_of "$h" status >/dev/null

# --- get.sh --uninstall --------------------------------------------------------------------------
getsh "$h" --uninstall
check "get.sh --uninstall leaves only the settings" same "$(files "$h")" "$expected_after_uninstall"

# --- ctl uninstall leaves exactly the same --------------------------------------------------------
h="$(fresh_home 2)"
getsh "$h"
ctl_of "$h" uninstall >/dev/null
check "ctl uninstall leaves only the settings, same as get.sh --uninstall" same "$(files "$h")" "$expected_after_uninstall"

# --- a disabled layer: uninstall clears it, a re-install supersedes it ---------------------------
h="$(fresh_home 3)"
getsh "$h"
ctl_of "$h" disable >/dev/null
check "disable leaves the layer out of the loader's sight" test ! -e "$h/.local/share/openxr/1/api_layers/implicit.d/steamframe_passthrough.json"
getsh "$h" --uninstall
check "get.sh --uninstall also clears a disabled manifest" same "$(files "$h")" "$expected_after_uninstall"

h="$(fresh_home 4)"
getsh "$h"
ctl_of "$h" disable >/dev/null
getsh "$h"
check "installing again re-enables a disabled layer" same "$(files "$h")" "$expected_install"

# --- the user's settings survive an uninstall + install ------------------------------------------
h="$(fresh_home 5)"
getsh "$h"
printf '[passthrough]\nenabled = 0\ntemporal_alpha = 0.31\n' >"$h/.config/steamframe-passthrough/config.ini"
getsh "$h" --uninstall
getsh "$h"
check "a customised config.ini survives uninstall and reinstall" grep -qx 'temporal_alpha = 0.31' "$h/.config/steamframe-passthrough/config.ini"

if [ "$fails" -eq 0 ]; then
    echo "OK"
else
    echo "$fails check(s) FAILED"
    exit 1
fi
