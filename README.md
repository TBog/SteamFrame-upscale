MVP

## Passthrough denoise
- `include/denoise.hpp`: reference pipeline (motion-masked temporal accumulation + contrast-adaptive sharpening, per-eye ping-pong history) and `config.ini` loader.
- `shaders/denoise_cas.comp`: GLSL compute port (two passes with a barrier between them).
- `layer/*.json`: OpenXR API layer manifest.
- `tests/test_pipeline.cpp`: config, denoise, ghosting, per-eye and teardown tests (`cmake -B build && cmake --build build && ctest --test-dir build`).

- `src/layer.cpp`: the OpenXR API layer (`xrNegotiateLoaderApiLayerInterface`, `xrGetInstanceProcAddr` hooking, session/swapchain lifecycle). It processes `VK_FORMAT_R8_UNORM` Vulkan swapchains in place on `xrReleaseSwapchainImage` (adds `UNORDERED_ACCESS` usage at creation), with per-eye R16F ping-pong history, Vulkan barriers, and GPU timestamps.

Build (Linux): `apt install libvulkan-dev libopenxr-dev glslang-tools`, then `cmake -B build && cmake --build build`. Use with `XR_API_LAYER_PATH=<build dir> XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise`.
Runtime knobs: `SF_CONFIG=<path to config.ini>` (reloaded live), `SF_PROFILE=1` prints GPU ms per 300 frames and flags >2 ms.

Status: the layer compiles and exports the negotiation entry point, but has not been run against a real runtime/GPU, so the shader, barriers and the 2 ms budget are unverified. It requires the `shaderStorageImageExtendedFormats` device feature and submits on the app's queue (the app must not submit on it concurrently from another thread during `xrReleaseSwapchainImage`).

Install (user-local, survives SteamOS updates): `curl -fsSL https://tbog.github.io/SteamFrame-upscale/get.sh | bash`; remove with `... | bash -s -- --uninstall`. From a local build, `cmake --install build` does the same. Set `SF_PROFILE=1` for timing logs.

## Live bypass toggle and GUI
- `enabled = 1|0` in `config.ini` (reloaded live) switches between the filter and raw passthrough without restarting the OpenXR session; when off, `xrReleaseSwapchainImage` dispatches no compute work.
- The layer also listens on a UNIX socket (`/tmp/steamframe_passthrough.sock`, override with `SF_SOCKET`). Send one line per connection: `TOGGLE`, `ENABLE`, `DISABLE`, `SET_ALPHA <0..1>`, `SET_MOTION <0..0.2>`, `SET_SHARPNESS <0..1>`, `STATUS`. Example: `echo TOGGLE | socat - UNIX-CONNECT:/tmp/steamframe_passthrough.sock`.
- `steamframe-gui` (Dear ImGui + GLFW): master toggle, live sliders, presets (Default Clean, High Motion, Maximum Denoise, Raw Bypass), layer status (Active / Inactive / Bypassed) and GPU frame time (with `SF_PROFILE=1`). It saves `~/.config/steamframe-passthrough/config.ini` and notifies the running layer.
- Build with `cmake -B build -DBUILD_GUI=ON` (needs OpenGL/X11 or Wayland dev packages; GLFW and Dear ImGui are fetched if not installed). `cmake --install build` and `get.sh` put the binary in `~/.local/bin/` and `steamframe-gui.desktop` in `~/.local/share/applications/`; `get.sh` takes the binary from `build/`, `SF_GUI_PATH`, or the release asset `steamframe-gui-<arch>`.
