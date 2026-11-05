#!/usr/bin/env bash
# Tests scripts/steamframe-passthrough-ctl against a throwaway HOME: how it edits config.ini (the
# file the layer hot-reloads) and how status reads the layer's log. Usage: ctl_test.sh <path to ctl>
set -u

ctl_script="${1:?usage: ctl_test.sh <path to steamframe-passthrough-ctl>}"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
fails=0

home="$root/home"
config="$home/.config/steamframe-passthrough/config.ini"
log="$home/.cache/steamframe-passthrough/layer.log"
manifest="$home/.local/share/openxr/1/api_layers/implicit.d/steamframe_passthrough.json"

# Run the script against the throwaway HOME, with none of the variables that would redirect it.
ctl() { env -u SF_CONFIG -u SF_LOG -u XDG_CACHE_HOME -u XDG_DATA_HOME HOME="$home" bash "$ctl_script" "$@" --no-wait; }

check() { # description, command...
    local what="$1"
    shift
    if "$@"; then
        echo "ok   $what"
    else
        echo "FAIL $what"
        fails=$((fails + 1))
    fi
}
contains() { grep -qF -- "$2" <<<"$1"; }
count_enabled() { grep -cE '^[[:space:]]*enabled[[:space:]]*=' "$config"; }

reset() {
    rm -rf "$home"
    mkdir -p "$home"
}

# --- nothing installed yet -------------------------------------------------------------------
reset
out="$(ctl status)"
status_rc=$?
check "status with nothing installed exits 0" test "$status_rc" = 0
check "status reports the filter on by default" contains "$out" "filter: ON"
check "status flags the missing library" contains "$out" "MISSING"
check "status explains an empty log" contains "$out" "has not been loaded by any OpenXR app"

# --- off / on / toggle on a missing config ---------------------------------------------------
reset
ctl off >/dev/null
check "off creates config.ini with enabled = 0" grep -qx 'enabled = 0' "$config"
check "off creates the [passthrough] header" grep -qx '\[passthrough\]' "$config"
check "status now says OFF" contains "$(ctl status)" "filter: OFF"
check "status shows 'default' for settings the file doesn't set" contains "$(ctl status)" "temporal_alpha=default"
ctl on >/dev/null
check "on rewrites it to enabled = 1" grep -qx 'enabled = 1' "$config"
check "on leaves exactly one enabled line" test "$(count_enabled)" = 1
ctl toggle >/dev/null
check "toggle from on gives enabled = 0" grep -qx 'enabled = 0' "$config"
ctl toggle >/dev/null
check "toggle from off gives enabled = 1" grep -qx 'enabled = 1' "$config"

# --- an existing config with no enabled key: add one after the header, change nothing else ---
reset
mkdir -p "$(dirname "$config")"
cat >"$config" <<'EOF'
; Passthrough denoise settings
[passthrough]
; weight of the current frame on static pixels (lower = smoother)
temporal_alpha = 0.22
motion_threshold = 0.5
EOF
ctl off >/dev/null
expected="$(printf '%s\n' '; Passthrough denoise settings' '[passthrough]' 'enabled = 0' \
    '; weight of the current frame on static pixels (lower = smoother)' 'temporal_alpha = 0.22' 'motion_threshold = 0.5')"
check "a new enabled line goes right after the header, the rest untouched" test "$(cat "$config")" = "$expected"

# --- an existing enabled line is replaced where it stands ------------------------------------
reset
mkdir -p "$(dirname "$config")"
cat >"$config" <<'EOF'
[passthrough]
temporal_alpha = 0.3
; keep this comment
enabled = 1
noise_floor = 0.04
EOF
ctl off >/dev/null
expected="$(printf '%s\n' '[passthrough]' 'temporal_alpha = 0.3' '; keep this comment' 'enabled = 0' 'noise_floor = 0.04')"
check "an existing enabled line is edited in place" test "$(cat "$config")" = "$expected"

# --- duplicates collapse, commented-out lines are left alone ---------------------------------
reset
mkdir -p "$(dirname "$config")"
printf '%s\n' '[passthrough]' '; enabled = 5' 'enabled = 1' 'enabled = 1' >"$config"
ctl off >/dev/null
check "duplicate enabled lines collapse to one" test "$(count_enabled)" = 1
check "a commented-out enabled line is not touched" grep -qx '; enabled = 5' "$config"

# --- a value the layer can't parse counts as its default (on), like the layer ----------------
reset
mkdir -p "$(dirname "$config")"
printf '%s\n' '[passthrough]' 'enabled = abc' >"$config"
check "an unparseable value reads as ON" contains "$(ctl status)" "filter: ON"
ctl toggle >/dev/null
check "toggling from an unparseable value turns it off" grep -qx 'enabled = 0' "$config"

# --- SF_CONFIG moves the file, like the layer -------------------------------------------------
reset
env -u SF_LOG HOME="$home" SF_CONFIG="$root/elsewhere.ini" bash "$ctl_script" off --no-wait >/dev/null
check "SF_CONFIG redirects the edit" grep -qx 'enabled = 0' "$root/elsewhere.ini"
check "...and leaves the default path alone" test ! -e "$config"

# --- status interprets the layer log -----------------------------------------------------------
write_log() {
    reset
    mkdir -p "$(dirname "$log")"
    printf '%s\n' "$@" >"$log"
}
write_log '2026-11-04 10:00:00 [game:1] layer loaded into this OpenXR app' \
    '2026-11-04 10:00:01 [game:1] swapchain 1920x1080 format=37(R8G8B8A8_UNORM) arraySize=1 usage=0x1: skipped'
check "log with only non-R8 swapchains says so" contains "$(ctl status)" "none of the app's swapchains is R8_UNORM"
write_log '2026-11-04 10:00:00 [game:1] layer loaded into this OpenXR app' \
    '2026-11-04 10:00:02 [game:1] swapchain 640x480 format=9(R8_UNORM) arraySize=2 usage=0x1: processing'
check "a processing swapchain with no frames yet is reported" contains "$(ctl status)" "no frame has been processed yet"
write_log '2026-11-04 10:00:03 [game:1] swapchain 0x1: filtering frames'
check "filtering frames is reported as working" contains "$(ctl status)" "Result: working"
write_log '2026-11-04 10:00:03 [game:1] swapchain 0x1: bypassed (enabled = 0): frames pass through untouched'
check "bypass is reported as loaded-but-off" contains "$(ctl status)" "filter is switched OFF"
write_log '2026-11-04 10:00:00 [game:1] session setup failed, layer inactive for this session: device lacks x'
check "a GPU setup failure is reported" contains "$(ctl status)" "the GPU setup failed"

# --- status flags a manifest that would keep the layer from loading --------------------------
reset
mkdir -p "$(dirname "$manifest")"
printf '%s\n' '{ "api_layer": { "enable_environment": "ENABLE_X" } }' >"$manifest"
check "a manifest with enable_environment is flagged" contains "$(ctl status)" "still has \"enable_environment\""

# --- disable / enable: the reversible "stop loading the layer at all" ---------------------------
reset
mkdir -p "$(dirname "$manifest")"
printf '%s\n' '{ "api_layer": { "name": "x" } }' >"$manifest"
ctl disable >/dev/null
check "disable moves the manifest out of the loader's sight" test ! -e "$manifest" -a -e "$manifest.disabled"
out="$(ctl status)"
check "status flags a disabled layer" contains "$out" "layer DISABLED"
check "status gives a disabled verdict, not a log verdict" contains "$out" "Result: the layer is DISABLED"
ctl disable >/dev/null 2>&1
check "disable twice is harmless" test "$?" = 0
ctl enable >/dev/null
check "enable restores the manifest byte for byte" test "$(cat "$manifest")" = '{ "api_layer": { "name": "x" } }'
check "enable leaves no .disabled copy behind" test ! -e "$manifest.disabled"
ctl enable >/dev/null 2>&1
check "enable twice is harmless" test "$?" = 0
reset
ctl disable >/dev/null 2>&1
check "disable with nothing installed fails (exit 1)" test "$?" = 1
ctl enable >/dev/null 2>&1
check "enable with nothing installed fails (exit 1)" test "$?" = 1

# --- uninstall: offline removal of exactly what the installer put in place ---------------------
install_fake() { # lays out what get.sh installs, using the real script as the installed copy
    reset
    mkdir -p "$home/.local/lib/openxr" "$home/.local/bin" "$home/.local/share/applications" \
        "$(dirname "$manifest")" "$(dirname "$config")" "$(dirname "$log")"
    : >"$home/.local/lib/openxr/libsteamframe_layer.so"
    printf '{}\n' >"$manifest"
    cp "$ctl_script" "$home/.local/bin/steamframe-passthrough-ctl"
    : >"$home/.local/share/applications/steamframe-passthrough-toggle.desktop"
    : >"$home/.local/share/applications/steamframe-passthrough-status.desktop"
    : >"$home/.local/share/applications/someone-elses-app.desktop"
    printf '[passthrough]\nenabled = 1\n' >"$config"
    printf 'a log line\n' >"$log"
}
installed_ctl() { env -u SF_CONFIG -u SF_LOG -u XDG_CACHE_HOME -u XDG_DATA_HOME HOME="$home" \
    bash "$home/.local/bin/steamframe-passthrough-ctl" "$@"; }

install_fake
installed_ctl uninstall >/dev/null
check "uninstall removes the library" test ! -e "$home/.local/lib/openxr/libsteamframe_layer.so"
check "uninstall removes the manifest" test ! -e "$manifest"
check "uninstall removes both launchers" test ! -e "$home/.local/share/applications/steamframe-passthrough-toggle.desktop" \
    -a ! -e "$home/.local/share/applications/steamframe-passthrough-status.desktop"
check "uninstall removes the installed script itself" test ! -e "$home/.local/bin/steamframe-passthrough-ctl"
check "uninstall keeps the user's settings" grep -qx 'enabled = 1' "$config"
check "uninstall keeps the log" test -s "$log"
check "uninstall leaves other apps' launchers alone" test -e "$home/.local/share/applications/someone-elses-app.desktop"

install_fake
installed_ctl disable >/dev/null
installed_ctl uninstall >/dev/null
check "uninstall also removes a disabled manifest" test ! -e "$manifest.disabled" -a ! -e "$manifest"

install_fake
ctl uninstall >/dev/null   # the script under test is a different file than the installed copy
check "uninstall run from another copy removes the installed one" test ! -e "$home/.local/bin/steamframe-passthrough-ctl"
check "...but never deletes the copy it is running from" test -f "$ctl_script"
ctl uninstall >/dev/null 2>&1
check "uninstall twice is harmless" test "$?" = 0

# --- argument handling --------------------------------------------------------------------------
check "an unknown argument exits 2" bash -c "env HOME='$home' bash '$ctl_script' bogus >/dev/null 2>&1; test \$? = 2"
check "no command exits 2" bash -c "env HOME='$home' bash '$ctl_script' >/dev/null 2>&1; test \$? = 2"
check "--help exits 0" bash -c "env HOME='$home' bash '$ctl_script' --help >/dev/null 2>&1"

if [ "$fails" -eq 0 ]; then
    echo "OK"
else
    echo "$fails check(s) FAILED"
    exit 1
fi
