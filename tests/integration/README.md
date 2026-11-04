# Layer integration smoke test

Exercises `src/layer.cpp` end-to-end (real loader negotiation, real Vulkan compute shader) without
a real HMD/OpenXR runtime, which previously made the layer untestable on a machine without
SteamVR/Monado (see `SETUP.md`). Needs no network access and no root.

- `stub_runtime.cpp` -- a minimal fake OpenXR *runtime* (not a layer): implements just enough of
  the loader<->runtime interface to get the loader past runtime discovery, plus the handful of
  core functions the layer hooks. It allocates real `VkImage`s for swapchains on the session's
  Vulkan device but never presents anything anywhere.
- `smoke_test.cpp` -- a small Vulkan+OpenXR app: creates a real (possibly software) Vulkan device,
  creates an `XrInstance` with the layer enabled, creates a session/swapchain, and feeds 40 frames
  of synthetic sensor noise through `xrAcquireSwapchainImage`/`xrReleaseSwapchainImage`. It then
  checks that the per-pixel variance of the last 10 frames actually *dropped* versus the input --
  i.e. that the layer's temporal-denoise compute shader really ran on the GPU, not just that
  nothing crashed. A second phase writes `enabled = 0` to `config.ini`, drives more frames past the
  layer's hot-reload poll boundary, and checks variance *stops* dropping -- i.e. that the bypass
  fast path really is skipping the compute dispatch, not just that the config key parses.
- `loader_runtime_interfaces.h` -- vendored copy of the loader<->runtime negotiation structs
  (mirrors `include/loader_interfaces.h`, which covers the loader<->layer side used by the layer
  itself).

## Running

Built and registered automatically by CMake when Vulkan, OpenXR headers, glslang, *and* the real
`openxr_loader` library are all found (same deps as `steamframe_layer`, plus the loader lib to link
the test app against):

```bash
cmake -B build && cmake --build build -j
ctest --test-dir build -R layer_smoke --output-on-failure   # runs both tests below
```

The same `layer_smoke_test` binary runs twice, differing only in how the layer is made visible:

- `layer_smoke` -- explicit, as a developer uses it: `XR_API_LAYER_PATH` + `XR_ENABLE_API_LAYERS`.
- `layer_smoke_implicit` -- installed, as `get.sh` / `cmake --install` leave it: the same implicit
  manifest (CMake's `SF_IMPLICIT_MANIFEST`, pointing at the fresh build) under `XDG_DATA_HOME`, and
  **no** `XR_API_LAYER_PATH`, `XR_ENABLE_API_LAYERS` or `ENABLE_*` variable. This is the regression
  test for the installed layer being silently skipped by the loader (it fails if `enable_environment`
  is put back in the manifest).

If no Vulkan device is present, or it lacks `shaderStorageImageExtendedFormats` (the same
requirement `src/layer.cpp` itself checks before enabling), the test is **skipped** (exit code 77,
reported by CTest as "Not Run") rather than failed -- that is an environment limitation, not a
layer bug. On a machine with no real GPU, install a software Vulkan driver to get a real pass
(Arch: `pacman -S vulkan-swrast`; Debian/Ubuntu: `apt install mesa-vulkan-drivers`).

To run manually outside `ctest` (e.g. to add `XR_LOADER_DEBUG=all`):

```bash
XR_RUNTIME_JSON=$PWD/build/integration/active_runtime.json \
XR_API_LAYER_PATH=$PWD/build \
XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise \
SF_CONFIG=$PWD/build/integration/smoke_config.ini \
SF_LOG=$PWD/build/integration/layer.log \
build/layer_smoke_test
```

`SF_CONFIG` and `SF_LOG` must point at writable paths; the test deletes both at startup (a leftover
`enabled = 0` from a previous run would otherwise start the next one bypassed) and creates them as it
goes. At the end it reads `SF_LOG` and requires the layer's diagnostic log to record the whole run:
loaded, session ready, an `R8_UNORM` swapchain `processing`, `images ready`, `filtering frames`,
`config reloaded:` and `bypassed (enabled = 0)`. `scripts/steamframe-passthrough-ctl status` keys on
the same phrases, so a message change in `src/layer.cpp` shows up here first. `ctest` points both at
files under `build/integration/`.

`smoke_test.cpp` deliberately does **not** also set `enabledApiLayerNames` in its
`XrInstanceCreateInfo` -- the layer is activated purely through `XR_ENABLE_API_LAYERS`, matching
how a real end user activates it on a VR game they don't control the source of (see `SETUP.md`).
Requesting the same explicit layer through both `XR_ENABLE_API_LAYERS` *and*
`enabledApiLayerNames` at once is not just redundant: it outright broke layer loading (`XrResult
-36`, `XR_ERROR_API_LAYER_NOT_PRESENT`) on Ubuntu 24.04's `libopenxr-loader1` 1.0.20, while working
fine on a newer loader (Arch's openxr 1.1.60). Confirmed via instrumenting the layer's negotiation
entry point: it was called and succeeded either way, but with both activation paths set the loader
discarded it immediately afterward and never called `xrCreateApiLayerInstance` at all. If you ever
need to request this layer programmatically from a test/dev app again, pick exactly one mechanism.

## What this does and does not prove

Confirms: the layer's manifest/negotiation is discoverable and correct, `xrGetInstanceProcAddr`
hooking works, session/swapchain hook sequencing matches a real app's call order, the Vulkan
barriers in `record()` don't crash or corrupt the image, and the compute shader
(`shaders/denoise_cas.comp`) produces the expected denoising effect on real GPU hardware (or a
software rasterizer).

Does not confirm: the 2ms GPU budget (`SF_PROFILE=1` reports timings but 40 frames on a software
rasterizer is not representative of real headset hardware), behavior with a real compositor's
actual swapchain recycling/synchronization, or anything about `arraySize == 2` (stereo) swapchains
-- this test only exercises `arraySize == 1`.
