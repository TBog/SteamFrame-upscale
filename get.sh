#!/usr/bin/env bash
# Installer for the SteamFrame passthrough OpenXR API layer (user-local, SteamOS-safe).
# Usage: curl -fsSL https://tbog.github.io/SteamFrame-upscale/get.sh | bash
#        curl -fsSL .../get.sh | bash -s -- --uninstall
# Everything lives under $HOME, so SteamOS root-image updates do not wipe it.
set -euo pipefail

REPO="${SF_REPO:-TBog/SteamFrame-upscale}"
REF="${SF_REF:-main}"
BASE_URL="${SF_BASE_URL:-https://github.com/$REPO/releases/latest/download}"
RAW_URL="https://raw.githubusercontent.com/$REPO/$REF"

LIB_DIR="$HOME/.local/lib/openxr"
MANIFEST_DIR="$HOME/.local/share/openxr/1/api_layers/implicit.d"
CONFIG_DIR="$HOME/.config/steamframe-passthrough"
LIB_NAME="libsteamframe_layer.so"
MANIFEST_NAME="steamframe_passthrough.json"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
GUI_NAME="steamframe-gui"
DESKTOP_NAME="steamframe-gui.desktop"

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
    rm -f "$MANIFEST_DIR/$MANIFEST_NAME" "$LIB_DIR/$LIB_NAME"
    rm -f "$BIN_DIR/$GUI_NAME" "$APP_DIR/$DESKTOP_NAME"
    ok "Removed manifest, library and GUI"
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
for l in libvulkan.so.1 libopenxr_loader.so.1; do
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
    "disable_environment": "DISABLE_XR_APILAYER_STEAMFRAME_passthrough_denoise",
    "enable_environment": "ENABLE_XR_APILAYER_STEAMFRAME_passthrough_denoise"
  }
}
JSON

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

# Optional GUI (steamframe-gui); failure to obtain it is not fatal.
gui_src=""
if [ -n "${SF_GUI_PATH:-}" ]; then gui_src="$SF_GUI_PATH"
elif [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/build/$GUI_NAME" ]; then gui_src="$SRC_DIR/build/$GUI_NAME"
elif fetch "$BASE_URL/$GUI_NAME-$ARCH" "$tmp/$GUI_NAME" 2>/dev/null; then gui_src="$tmp/$GUI_NAME"
fi
if [ -n "$gui_src" ]; then
  mkdir -p "$BIN_DIR" "$APP_DIR"
  install -m 755 "$gui_src" "$BIN_DIR/$GUI_NAME"
  if [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/packaging/$DESKTOP_NAME" ]; then
    cp "$SRC_DIR/packaging/$DESKTOP_NAME" "$APP_DIR/$DESKTOP_NAME"
  else
    fetch "$RAW_URL/packaging/$DESKTOP_NAME" "$APP_DIR/$DESKTOP_NAME" || warn "Could not fetch $DESKTOP_NAME"
  fi
  sed -i "s|^Exec=.*|Exec=$BIN_DIR/$GUI_NAME|" "$APP_DIR/$DESKTOP_NAME" 2>/dev/null || true
  ok "GUI installed: $BIN_DIR/$GUI_NAME"
else
  warn "GUI binary not available; skipping (build with -DBUILD_GUI=ON or set SF_GUI_PATH)"
fi

ok "Layer installed and registered: $MANIFEST_DIR/$MANIFEST_NAME"
info "Profiling: launch your OpenXR app with ${G}SF_PROFILE=1${N} to log GPU timings"
info "Update: re-run this script. Remove: bash get.sh --uninstall"
