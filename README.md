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
