MVP

## Passthrough denoise
- `include/denoise.hpp`: reference pipeline (motion-masked temporal accumulation + contrast-adaptive sharpening, per-eye ping-pong history) and `config.ini` loader.
- `shaders/denoise_cas.comp`: GLSL compute port (two passes with a barrier between them).
- `layer/*.json`: OpenXR API layer manifest.
- `tests/test_pipeline.cpp`: config, denoise, ghosting, per-eye and teardown tests (`cmake -B build && cmake --build build && ctest --test-dir build`).
- `tests/integration/`: end-to-end smoke test for the actual layer (loader negotiation, real Vulkan compute shader) against a stub OpenXR runtime, no HMD needed; also run via `ctest`.

- `src/layer.cpp`: the OpenXR API layer (`xrNegotiateLoaderApiLayerInterface`, `xrGetInstanceProcAddr` hooking, session/swapchain lifecycle). It processes `VK_FORMAT_R8_UNORM` Vulkan swapchains in place on `xrReleaseSwapchainImage` (adds `UNORDERED_ACCESS` usage at creation), with per-eye R16F ping-pong history, Vulkan barriers, and GPU timestamps.

Build (Linux): `apt install libvulkan-dev libopenxr-dev glslang-tools`, then `cmake -B build && cmake --build build`. Use with `XR_API_LAYER_PATH=<build dir> XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise`.
Runtime knobs: `SF_CONFIG=<path to config.ini>` (reloaded live), `SF_PROFILE=1` prints GPU ms per 300 frames and flags >2 ms.

Status: the layer compiles, negotiates correctly with a real OpenXR loader, and its compute shader has been verified end-to-end (see `tests/integration/`) against a stub runtime on a software Vulkan device -- but it has not yet been run against a real OpenXR runtime/HMD, so the 2 ms budget is still unverified. It requires the `shaderStorageImageExtendedFormats` device feature and submits on the app's queue (the app must not submit on it concurrently from another thread during `xrReleaseSwapchainImage`).

Install (user-local, survives SteamOS updates): `curl -fsSL https://tbog.github.io/SteamFrame-upscale/get.sh | bash`; remove with `... | bash -s -- --uninstall`. From a local build, `cmake --install build` does the same. Set `SF_PROFILE=1` for timing logs.

## GUI

`decky-plugin/`: a [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) plugin exposing the four `config.ini` tunables as Quick Access Menu sliders -- no IPC, it just edits the file the layer already hot-reloads. See that directory's README, including the caveat that Decky's own Steam Frame/VR support is itself still incomplete upstream.
