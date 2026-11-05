MVP

## Install

```bash
curl -fsSL https://tbog.github.io/SteamFrame-upscale/get.sh | bash
```

User-local (`~/.local`, `~/.config`), survives SteamOS updates. Uninstall: add `-s -- --uninstall`
to the command above. Requires `libvulkan.so.1` to be present; the OpenXR loader is provided by the
runtime and need not be installed in a system library directory.
Fetches a prebuilt `x86_64`/`aarch64` binary from the latest GitHub release (built by
`.github/workflows/release.yml`); if you need a build from a specific commit instead, build from
source -- see "Build from source" below -- then run `cmake --install build`, which does the same
user-local install as `get.sh`.

### After installing

The layer is not a program you start: the OpenXR loader loads it into every OpenXR app that starts
after the install, and it filters any `R8_UNORM` Vulkan swapchain it finds (the monochrome
passthrough stream). Nothing to enable, but **apps that were already running need a restart**.

The installer adds two entries to the SteamVR dashboard's **+** list (like the other Frame apps; they
open a small Konsole window):

- **Passthrough Denoise: Status** -- is it installed, did any app load it, what swapchains did it see,
  and is it filtering. This is the first thing to check if it seems to do nothing: an empty log means
  no OpenXR app has loaded it yet; "none of the app's swapchains is R8_UNORM" means it loads but the
  passthrough isn't a swapchain it can touch.
- **Passthrough Denoise: On/Off** -- switch the filter off for raw passthrough, and back on.

The same from a terminal: `~/.local/bin/steamframe-passthrough-ctl status|on|off|toggle`. The layer
writes what it does to `~/.cache/steamframe-passthrough/layer.log`. Tunables live in
`~/.config/steamframe-passthrough/config.ini` (live-reloaded; see `SETUP.md`).

### If something goes wrong (even if you can't see any UI)

The layer is loaded into every VR app, so a bug in it can mean a garbled or black view, or an app
that won't start, with no way to reach the `+` list. Each step below works without the headset UI.
Try them in order:

1. **Raw passthrough:** `~/.local/bin/steamframe-passthrough-ctl off` (or the *On/Off* entry in the
   `+` list). This only skips the GPU work; the layer stays loaded, so it won't help if loading or
   setting up the layer is what breaks.
2. **Stop apps loading it at all (reversible):** `steamframe-passthrough-ctl disable`. Apps started
   afterwards don't load the layer; restart the affected app. `enable` undoes it.
3. **Remove it** (works offline, keeps your settings): `steamframe-passthrough-ctl uninstall`.
   If that script is missing or broken, and the headset is online:
   `curl -fsSL https://tbog.github.io/SteamFrame-upscale/get.sh | bash -s -- --uninstall`.
4. **By hand, with none of our tools:**
   `rm -f ~/.local/share/openxr/1/api_layers/implicit.d/steamframe_passthrough.json`
5. **One app only:** give that app the Steam launch option
   `DISABLE_XR_APILAYER_STEAMFRAME_passthrough_denoise=1 %command%`.

**From a PC** (when the headset view is unusable): run any of the commands above through SSH, e.g.

```sh
ssh steamos@<headset-ip> '~/.local/bin/steamframe-passthrough-ctl disable'
ssh steamos@<headset-ip> '~/.local/bin/steamframe-passthrough-ctl uninstall'
ssh steamos@<headset-ip> 'rm -f ~/.local/share/openxr/1/api_layers/implicit.d/steamframe_passthrough.json'
```

Windows 10/11 have an `ssh` client in PowerShell. If an app is still misbehaving, restart it, or
restart SteamVR with `ssh steamos@<headset-ip> 'systemctl --user restart steamvr.service'`.

**Set SSH up now, not after something breaks.** On the headset: Settings > System > Developer Mode,
then set a password under Developer (the other Frame apps document the same steps and the
`steamos@<headset-ip>` login; the headset's IP address is in your router's list of connected
devices). Developer Mode also makes Konsole appear in the `+` list. Choose a strong password: with
SSH on, anyone on your network who knows it can log in to the headset. I have not been able to try
these steps on a real Frame, so check that `ssh` works while everything still does.

## Passthrough denoise
- `include/denoise.hpp`: reference pipeline (motion-masked temporal accumulation + contrast-adaptive sharpening, per-eye ping-pong history) and `config.ini` loader.
- `shaders/denoise_cas.comp`: GLSL compute port (two passes with a barrier between them).
- `layer/*.json`: OpenXR API layer manifest.
- `tests/test_pipeline.cpp`: config, denoise, ghosting, per-eye and teardown tests (`cmake -B build && cmake --build build && ctest --test-dir build`).
- `tests/integration/`: end-to-end smoke test for the actual layer (loader negotiation, real Vulkan compute shader) against a stub OpenXR runtime, no HMD needed; also run via `ctest`.

- `src/layer.cpp`: the OpenXR API layer (`xrNegotiateLoaderApiLayerInterface`, `xrGetInstanceProcAddr` hooking, session/swapchain lifecycle). It processes `VK_FORMAT_R8_UNORM` Vulkan swapchains in place on `xrReleaseSwapchainImage` (adds `UNORDERED_ACCESS` usage at creation), with per-eye R16F ping-pong history, Vulkan barriers, and GPU timestamps.

## Build from source (Linux)

```bash
apt install libvulkan-dev libopenxr-dev glslang-tools
cmake -B build && cmake --build build
```

Use with `XR_API_LAYER_PATH=<build dir> XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise`.
Runtime knobs: `SF_CONFIG=<path to config.ini>` (reloaded live), `SF_PROFILE=1` prints GPU ms per 300 frames and flags >2 ms.

Status: the layer compiles, negotiates correctly with a real OpenXR loader, and its compute shader has been verified end-to-end (see `tests/integration/`) against a stub runtime on a software Vulkan device -- but it has not yet been run against a real OpenXR runtime/HMD, so the 2 ms budget is still unverified. It requires the `shaderStorageImageExtendedFormats` device feature and submits on the app's queue (the app must not submit on it concurrently from another thread during `xrReleaseSwapchainImage`).

## GUI

`decky-plugin/`: a [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) plugin exposing the four `config.ini` tunables as Quick Access Menu sliders -- no IPC, it just edits the file the layer already hot-reloads. See that directory's README, including the caveat that Decky's own Steam Frame/VR support is itself still incomplete upstream.
