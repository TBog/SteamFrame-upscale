#!/usr/bin/env bash
# Installer for the SteamFrame passthrough OpenXR API layer (user-local, SteamOS-safe).
# Usage: curl -fsSL https://tbog.github.io/SteamFrame-upscale/get.sh | bash
#        curl -fsSL .../get.sh | bash -s -- --uninstall
# Everything lives under $HOME, so SteamOS root-image updates do not wipe it.
set -euo pipefail

REPO="${SF_REPO:-TBog/SteamFrame-upscale}"
REF="${SF_REF:-main}"
BASE_URL="${SF_BASE_URL:-https://github.com/$REPO/releases/latest/download}"
RAW_URL="${SF_RAW_URL:-https://raw.githubusercontent.com/$REPO/$REF}"

LIB_DIR="$HOME/.local/lib/openxr"
MANIFEST_DIR="$HOME/.local/share/openxr/1/api_layers/implicit.d"
CONFIG_DIR="$HOME/.config/steamframe-passthrough"
LIB_NAME="libsteamframe_layer.so"
MANIFEST_NAME="steamframe_passthrough.json"

# Launcher entries for the SteamVR dashboard's "+" list (same mechanism as the other Frame apps).
CTL_DIR="$HOME/.local/bin"
CTL_NAME="steamframe-passthrough-ctl"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DESKTOP_FILES="steamframe-passthrough-toggle.desktop steamframe-passthrough-status.desktop"

if [ -t 1 ]; then
  R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[34m'; N=$'\033[0m'
else
  R=; G=; Y=; B=; N=
fi
info() { printf '%s==>%s %s\n' "$B" "$N" "$*"; }
ok()   { printf '%s[ok]%s %s\n' "$G" "$N" "$*"; }
warn() { printf '%s[warn]%s %s\n' "$Y" "$N" "$*" >&2; }
die()  { printf '%s[error]%s %s\n' "$R" "$N" "$*" >&2; exit 1; }

case "${1:-}" in
  --uninstall|--remove)
    info "Removing SteamFrame passthrough layer"
    rm -f "$MANIFEST_DIR/$MANIFEST_NAME" "$LIB_DIR/$LIB_NAME" "$CTL_DIR/$CTL_NAME"
    for f in $DESKTOP_FILES; do rm -f "$APPS_DIR/$f"; done
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" 2>/dev/null || true
    ok "Removed manifest, library and launchers"
    if [ -d "$CONFIG_DIR" ]; then
      warn "Config kept at $CONFIG_DIR (delete it manually if unwanted)"
    fi
    exit 0 ;;
  ""|--install|--update) ;;
  -h|--help) echo "Usage: get.sh [--install|--update|--uninstall|--remove]"; exit 0 ;;
  *) die "Unknown option: $1" ;;
esac

[ -n "${HOME:-}" ] || die "HOME is not set"

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|aarch64) ok "Architecture: $ARCH" ;;
  *) die "Unsupported architecture: $ARCH (need x86_64 or aarch64)" ;;
esac

have_lib() {
  if command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -q "$1"; then return 0; fi
  local d
  for d in /usr/lib /usr/lib64 /usr/lib/"$ARCH"-linux-gnu /lib /lib64 /usr/local/lib; do
    [ -e "$d/$1" ] && return 0
  done
  return 1
}
missing=0
for l in libvulkan.so.1; do
  if have_lib "$l"; then ok "Found $l"; else warn "Missing $l"; missing=1; fi
done
[ "$missing" -eq 0 ] || die "Install the missing libraries and re-run"

fetch() { # url dest
  if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
  elif command -v wget >/dev/null 2>&1; then wget -qO "$2" "$1"
  else die "curl or wget is required"; fi
}

mkdir -p "$LIB_DIR" "$MANIFEST_DIR" "$CONFIG_DIR"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

SRC_DIR=""
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
  SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

# Copy a repository file (path relative to the repo root) from this checkout if we're running from
# one, otherwise download it from the repo. Returns non-zero instead of exiting on failure.
fetch_asset() { # repo-relative-path dest
  if [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/$1" ]; then cp "$SRC_DIR/$1" "$2"
  else fetch "$RAW_URL/$1" "$2"; fi
}

info "Installing library to $LIB_DIR"
if [ -n "${SF_LIB_PATH:-}" ]; then
  cp "$SF_LIB_PATH" "$tmp/$LIB_NAME"
elif [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/build/$LIB_NAME" ]; then
  cp "$SRC_DIR/build/$LIB_NAME" "$tmp/$LIB_NAME"
else
  fetch "$BASE_URL/libsteamframe_layer-$ARCH.so" "$tmp/$LIB_NAME" \
    || die "Download failed; build locally (cmake) or set SF_LIB_PATH"
fi
install -m 755 "$tmp/$LIB_NAME" "$LIB_DIR/$LIB_NAME"

info "Writing manifest to $MANIFEST_DIR/$MANIFEST_NAME"
cat > "$MANIFEST_DIR/$MANIFEST_NAME" <<JSON
{
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_STEAMFRAME_passthrough_denoise",
    "library_path": "$LIB_DIR/$LIB_NAME",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "Temporal denoise + CAS for monochrome passthrough",
    "functions": {
      "xrNegotiateLoaderApiLayerInterface": "xrNegotiateLoaderApiLayerInterface"
    },
    "disable_environment": "DISABLE_XR_APILAYER_STEAMFRAME_passthrough_denoise"
  }
}
JSON
# No "enable_environment" here on purpose: the OpenXR loader skips an implicit layer that declares one
# unless that variable is set in the app's environment, which nobody can do for apps launched from the
# headset's dashboard. Set DISABLE_XR_APILAYER_STEAMFRAME_passthrough_denoise=1 to opt an app out, or
# put "enabled = 0" in config.ini (or use the launcher) to leave frames untouched everywhere.

if [ -e "$CONFIG_DIR/config.ini" ]; then
  ok "Keeping existing $CONFIG_DIR/config.ini"
else
  if [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/config.ini" ]; then
    cp "$SRC_DIR/config.ini" "$CONFIG_DIR/config.ini"
  else
    fetch "$RAW_URL/config.ini" "$CONFIG_DIR/config.ini" || die "Could not fetch default config.ini"
  fi
  ok "Created $CONFIG_DIR/config.ini"
fi

install_launchers() {
  local f
  mkdir -p "$CTL_DIR" "$APPS_DIR" || return 1
  fetch_asset "scripts/$CTL_NAME" "$tmp/$CTL_NAME" || return 1
  install -m 755 "$tmp/$CTL_NAME" "$CTL_DIR/$CTL_NAME" || return 1
  for f in $DESKTOP_FILES; do
    fetch_asset "contrib/$f" "$tmp/$f" || return 1
    # The launcher must name the script by absolute path: Steam may start it without ~/.local/bin in PATH.
    sed "s|@CTL@|$CTL_DIR/$CTL_NAME|g" "$tmp/$f" > "$APPS_DIR/$f" || return 1
    chmod 644 "$APPS_DIR/$f"
  done
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" 2>/dev/null || true
}
if install_launchers; then
  ok "Launchers installed: $APPS_DIR/steamframe-passthrough-*.desktop"
  command -v konsole >/dev/null 2>&1 \
    || warn "konsole not found: the launchers open their report in Konsole; run $CTL_DIR/$CTL_NAME status from any terminal instead"
else
  warn "Could not install the launcher entries (the layer itself is installed and works without them)"
fi

ok "Layer installed and registered: $MANIFEST_DIR/$MANIFEST_NAME"
info "It loads by itself into OpenXR apps started from now on; there is nothing to switch on."
info "Check it from the headset: SteamVR dashboard ${G}+${N} > ${G}Passthrough Denoise: Status${N} (turn it off/on: ${G}Passthrough Denoise: On/Off${N})"
info "Profiling: launch your OpenXR app with ${G}SF_PROFILE=1${N} to log GPU timings"
info "Update: re-run this script. Remove: bash get.sh --uninstall"
