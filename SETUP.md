# Setup & Testing Guide

Guide for testing the SteamFrame passthrough denoise OpenXR API layer
(`libsteamframe_layer.so`) on an Arch/CachyOS workstation with SteamVR or Monado.

- Layer name (from the manifest): `XR_APILAYER_STEAMFRAME_passthrough_denoise`
- Manifest: `XR_APILAYER_SteamFrame_passthrough_denoise.json` (copied next to the library in the build dir)

> Status: the layer has not yet been verified on a real runtime/GPU. Treat the 2 ms budget as unconfirmed until measured.

## 0. Build

```bash
sudo pacman -S --needed base-devel cmake vulkan-headers vulkan-icd-loader openxr glslang
cd /path/to/SteamFrame-upscale
cmake -B build && cmake --build build -j
ctest --test-dir build          # CPU reference pipeline tests
ls build/libsteamframe_layer.so build/XR_APILAYER_SteamFrame_passthrough_denoise.json
```

## 1. System & Layer Registration

### How the loader finds layers on Linux

The OpenXR loader scans these manifest directories (`$XDG_*` fall back to the defaults shown):

| Kind | Location |
|------|----------|
| Explicit layers | `~/.local/share/openxr/1/api_layers/explicit.d/`, `/etc/xdg/openxr/1/api_layers/explicit.d/`, `/etc/openxr/1/api_layers/explicit.d/`, `/usr/local/share/openxr/1/api_layers/explicit.d/`, `/usr/share/openxr/1/api_layers/explicit.d/` |
| Implicit layers | same, with `implicit.d/` |
| Override | `XR_API_LAYER_PATH` (colon-separated directories; searched in addition to the above) |

Our manifest does not declare `enable_environment`, so it is an **explicit** layer: it is
loaded only when requested via `XR_ENABLE_API_LAYERS` (or by the application). Put it in
`explicit.d/`. An implicit layer (`implicit.d/`) is loaded for every app, but must also
carry `disable_environment` in the manifest; add that only if you want always-on behaviour.

### Transient test (no install)

```bash
export XR_API_LAYER_PATH=$PWD/build
export XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise
export SF_CONFIG=$PWD/config.ini
```

Note: the variable is `XR_ENABLE_API_LAYERS` (not `XR_ENABLE_LAYERS`), and several layers
are separated by `:`.

### Persistent install

```bash
BUILD=$PWD/build
DEST=~/.local/share/openxr/1/api_layers/explicit.d
mkdir -p "$DEST"
# Rewrite library_path to an absolute path (see Troubleshooting) instead of symlinking the raw manifest
sed "s#\"./libsteamframe_layer.so\"#\"$BUILD/libsteamframe_layer.so\"#" \
  "$BUILD/XR_APILAYER_SteamFrame_passthrough_denoise.json" \
  > "$DEST/XR_APILAYER_SteamFrame_passthrough_denoise.json"
# Optional: persistent config
mkdir -p ~/.config/steamframe && cp config.ini ~/.config/steamframe/config.ini
```

Then enable it per launch with `XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise`
(for Steam: set it in the game's launch options, `XR_ENABLE_API_LAYERS=... %command%`).
Uninstall: `rm ~/.local/share/openxr/1/api_layers/explicit.d/XR_APILAYER_SteamFrame_passthrough_denoise.json`.

Make sure an active runtime is selected (Monado: `export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json`;
SteamVR: Settings > OpenXR > "Set SteamVR as OpenXR runtime").

## 2. Validation & Verification Suite

### Confirm the layer is discovered and loaded

```bash
# hello_xr is built from Khronos OpenXR-SDK-Source (or an AUR package)
XR_LOADER_DEBUG=all \
XR_API_LAYER_PATH=$PWD/build \
XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise \
hello_xr -g Vulkan2 -ff Hmd -vc Stereo 2>&1 | grep -iE "steamframe|layer|error"
```

Expected: loader debug lines mentioning `XR_APILAYER_STEAMFRAME_passthrough_denoise`
and the library path, and no `Failed to find ... library` / `Failed to negotiate` errors.
`openxr-explorer` (GUI, from Khronos OpenXR-Explorer) also lists loaded API layers under
the instance/"API Layers" tab. `hello_xr` must use the Vulkan graphics plugin (`-g Vulkan2`
or `Vulkan`) because the layer only processes Vulkan swapchains.

Additionally check the process maps while the app runs:

```bash
grep -l libsteamframe_layer /proc/$(pgrep -n hello_xr)/maps
```

### Khronos validation layer

Stack validation after ours to look for synchronization hazards around `xrReleaseSwapchainImage`:

```bash
export XR_API_LAYER_PATH=$PWD/build:/usr/share/openxr/1/api_layers/explicit.d
export XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise:XR_APILAYER_LUNARG_core_validation
hello_xr -g Vulkan2 2>&1 | tee /tmp/validation.log
```

`XR_APILAYER_LUNARG_core_validation` is the layer name in current OpenXR SDKs
(older SDKs used `XR_APILAYER_LUNARG_standard_validation`; use whichever your install ships).
OpenXR validation checks API usage only. For Vulkan swapchain image layout/barrier hazards
also enable Vulkan validation:

```bash
sudo pacman -S --needed vulkan-validation-layers
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
hello_xr -g Vulkan2 2>&1 | grep -iE "SYNC-HAZARD|VUID|steamframe"
```

A clean run has no `SYNC-HAZARD-*` or `VUID-*` messages referencing the swapchain images.

## 3. Runtime Benchmarking & Calibration

### Profiling

```bash
SF_PROFILE=1 \
SF_CONFIG=$PWD/config.ini \
XR_API_LAYER_PATH=$PWD/build \
XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise \
<your OpenXR app>  2>&1 | grep STEAMFRAME
```

The layer prints to **stderr** once per 300 frames:

```
[XR_APILAYER_STEAMFRAME_passthrough_denoise] GPU 0.812 ms avg, 1.204 ms max over 300 frames (2 layer(s)) ok
```

- `avg` / `max` are GPU timestamp times (ms) of the denoise passes over the window.
- The trailing `ok` means the max stayed within the 2 ms budget; `OVER 2ms BUDGET` means at least one frame exceeded it.
- Run for several windows under load (rapid head motion, busy scene). Consistent `OVER` lines mean the layer is too slow for your GPU.
- With SteamVR, stderr goes to the vrserver/app log: `~/.local/share/Steam/logs/vrserver.txt` or the app's log in `~/.local/share/Steam/logs/`; for Steam games use `%command% 2>/tmp/sf.log` in launch options.

### Live tuning `config.ini`

The file named by `SF_CONFIG` is re-read when its modification time changes; edit while the app runs.

```bash
$EDITOR "$SF_CONFIG"     # save, effect applies within a frame or two
```

| Key | Meaning | Tuning |
|-----|---------|--------|
| `temporal_alpha` (0.15) | Weight of current frame on static pixels | Lower = smoother but more lag; raise (0.25-0.4) if trailing/ghosting appears |
| `motion_threshold` (0.08) | Luma delta above which history is ignored | Ghosting on fast motion: lower it (0.04-0.06). Noise flickering on static areas being treated as motion: raise it |
| `sharpness_weight` (0.8) | Max CAS sharpening | Sensor grain looks crunchy/amplified: lower (0.3-0.5). Image too soft: raise |
| `noise_floor` (0.02) | Contrast below this is not sharpened | Raise (0.03-0.05) so remaining ISO grain is not sharpened |

Suggested workflow:
1. Start with defaults in a dim room (worst-case grain) and static head: raise `sharpness_weight`/`noise_floor` until grain is not enhanced.
2. Shake your head / move a hand quickly: if trails appear, lower `motion_threshold`, then raise `temporal_alpha`.
3. Re-check static noise; iterate. Confirm `SF_PROFILE` timings stay under 2 ms.

## 4. Troubleshooting

### `Vulkan setup unsupported; passthrough denoise disabled` (missing `shaderStorageImageExtendedFormats`)

The layer needs the `shaderStorageImageExtendedFormats` device feature for R8/R16F storage images and the app's
`VkDevice` must have enabled it. Check support:

```bash
vulkaninfo 2>/dev/null | grep -i shaderStorageImageExtendedFormats
```

If `false`, update the GPU driver (`mesa`/`vulkan-radeon`/`nvidia-utils`). If supported but the app does not
enable it, the layer disables itself and passes frames through unmodified (the message above appears on stderr).

### Swapchain not processed / `swapchain passes through`

Only `VK_FORMAT_R8_UNORM` Vulkan swapchains are processed (the monochrome passthrough
stream). Swapchains with other formats (RGBA8, SRGB, etc.) are left untouched by design. If the
passthrough swapchain uses another format, nothing is denoised: check with
`XR_LOADER_DEBUG=all` / the app's swapchain format list, and confirm the app picks `R8_UNORM`
from `xrEnumerateSwapchainFormats`. A `swapchain resource allocation failed` message means the
format lacks `STORAGE_IMAGE` support on your GPU.

### Loader cannot find `libsteamframe_layer.so`

Symptoms: `Failed to find library ...`, `Failed to load layer`, or the layer silently missing.
The manifest uses `"library_path": "./libsteamframe_layer.so"`. A relative path with a `/`
is resolved relative to the manifest's directory, so it only works when the `.so` sits next
to the manifest (true for the build dir with `XR_API_LAYER_PATH=build`). A symlinked or
copied manifest in `~/.local/share/openxr/...` will not find it. Fix by using an absolute path:

```bash
sed -i "s#\"./libsteamframe_layer.so\"#\"$PWD/build/libsteamframe_layer.so\"#" \
  ~/.local/share/openxr/1/api_layers/explicit.d/XR_APILAYER_SteamFrame_passthrough_denoise.json
```

Other checks: `ldd build/libsteamframe_layer.so` (no missing libs), the layer name in
`XR_ENABLE_API_LAYERS` matches the manifest `name` exactly (case-sensitive), and
`XR_LOADER_DEBUG=all` shows the manifest being parsed.
