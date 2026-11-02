# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An OpenXR API layer (`XR_APILAYER_STEAMFRAME_passthrough_denoise`) that applies temporal denoise +
contrast-adaptive sharpening (CAS) to monochrome VR passthrough camera feeds, in place, on the GPU,
right before `xrReleaseSwapchainImage`. Target budget: <2ms GPU time per frame. **Status: verified
end-to-end against a stub OpenXR runtime + software Vulkan device** (`tests/integration/`; loader
negotiation, hook sequencing, and the real compute shader all confirmed working), **but never run
against a real OpenXR runtime/HMD** — the 2ms timing budget is still unverified. Treat any change
to `src/layer.cpp` or `shaders/denoise_cas.comp` as needing a `layer_smoke` rerun (see below), and
still unverified-on-real-hardware regardless.

## Build & test commands

```bash
# Linux only (the OpenXR layer target needs Vulkan/OpenXR dev headers + glslang)
sudo apt install libvulkan-dev libopenxr-dev glslang-tools   # Debian/Ubuntu
# or: sudo pacman -S --needed base-devel cmake vulkan-headers vulkan-icd-loader openxr glslang  # Arch

cmake -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # CPU pipeline tests + the layer_smoke integration test
build/test_pipeline                              # run the CPU test binary directly (more verbose output)
```

If Vulkan/OpenXR/glslang aren't found, CMake skips the `steamframe_layer` target (and, transitively,
`layer_smoke`) but still builds `test_pipeline` — the CPU reference pipeline (`include/denoise.hpp`)
has no graphics-API dependency, so that test runs anywhere (including this Windows dev machine, via
WSL for the full build). There is no single-test filter for `test_pipeline.cpp`; it's one `main()`
with sequential `CHECK` blocks per scenario (config parsing, temporal denoise variance, edge
sharpening, ghosting, per-eye independence, teardown cycles, CPU timing) — comment out blocks
locally if iterating on one. `ctest -R layer_smoke` isolates the integration test; it's skipped
(not failed) with CTest exit code 77 on a machine with no usable Vulkan device — see
`tests/integration/README.md`.

Runtime env vars for the built layer: `XR_API_LAYER_PATH=<build dir>`,
`XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise`, `SF_CONFIG=<path>` (config.ini,
hot-reloaded on mtime change), `SF_PROFILE=1` (prints GPU ms/frame to stderr every 300 frames).
See `SETUP.md` for the full install/validation/benchmarking workflow (layer manifest discovery,
`hello_xr` smoke test, Khronos/Vulkan validation layers, live config tuning, troubleshooting).

## Architecture: two parallel implementations, kept in lockstep

The denoising algorithm (motion-masked temporal blend → contrast-adaptive sharpen) exists in two
places that **must stay logically identical**:

1. **`include/denoise.hpp`** (`sf::Pipeline`) — CPU reference implementation, header-only,
   no graphics API dependency. This is what `tests/test_pipeline.cpp` exercises via `ctest`.
2. **`shaders/denoise_cas.comp`** — GLSL compute shader port of the same math, run as two dispatches
   (pass 0 = temporal blend writing to the pong buffer, pass 1 = CAS reading that buffer with a
   barrier in between) per eye/layer.

When changing the algorithm (temporal blend weights, motion masking, CAS sharpening curve), mirror
the change in **both** files — there is no shared source of truth between CPU and GPU paths; the
comment at the top of each file says as much. `include/loader_interfaces.h` is unrelated to this —
it's a vendored copy of OpenXR loader/layer negotiation structs not shipped in public openxr-dev
packages.

`sf::Config` (also in `denoise.hpp`) parses `config.ini`'s four tunables (`temporal_alpha`,
`motion_threshold`, `sharpness_weight`, `noise_floor`) and is shared by both the CPU tests and the
layer (`src/layer.cpp` loads/hot-reloads it and passes it to the shader via push constants —
`PushConstants` in `layer.cpp` must match the `P` uniform block layout in the `.comp` file).

### `src/layer.cpp`: the OpenXR layer itself

Single-file layer implementing the loader negotiation + a handful of OpenXR function hooks
(`hGetInstanceProcAddr` dispatches to `h<Name>` hooks: `CreateSession`, `DestroySession`,
`CreateSwapchain`, `DestroySwapchain`, `EnumerateSwapchainImages`, `AcquireSwapchainImage`,
`ReleaseSwapchainImage`, `DestroyInstance`). Key structures:

- `SessionCtx` — one per `XrSession`; owns the Vulkan device/queue/compute pipeline. Built from the
  `XrGraphicsBindingVulkanKHR` passed to `xrCreateSession`; requires `shaderStorageImageExtendedFormats`
  and storage-image support for both `R8_UNORM` (swapchain) and `R16_SFLOAT` (history) formats — if
  either check fails, the layer disables itself and passes frames through unmodified.
- `SwapchainCtx` — one per `XrSwapchain`; only swapchains matching `VK_FORMAT_R8_UNORM`,
  `arraySize <= kMaxLayers (2)`, single sample/face/mip, with the color-attachment usage bit are
  opted into processing (`want` in `hCreateSwapchain`), and `UNORDERED_ACCESS` usage is injected at
  creation so the images can be bound as storage images later.
- `LayerRes` — per array layer ("eye"): a ping-pong pair of `R16_SFLOAT` history images.
- `ImageRes` — per swapchain image: command buffer, fence, timestamp query pool, and a
  `[layer][parity][pass]` descriptor-set grid (built lazily in `hEnumerateSwapchainImages` via
  `buildSwapchainRes`, since that's the first point the actual `VkImage` handles are known).

The actual processing happens in `hReleaseSwapchainImage`: look up the acquired-image index via a
ring buffer (`SwapchainCtx::ring`, filled in `hAcquireSwapchainImage` — acquire and release order
can differ), wait on that image's fence from the *previous* submission, collect its GPU timestamps,
re-record (`record()`) and resubmit the compute pass, then call through to the real
`xrReleaseSwapchainImage`. `record()` barriers the swapchain image from
`COLOR_ATTACHMENT_OPTIMAL`/`UNDEFINED` into `GENERAL` for compute, dispatches pass 0 then pass 1 with
a memory barrier between them, then barriers back to `COLOR_ATTACHMENT_OPTIMAL`. All Vulkan state
and the `g_sessions`/`g_swapchains` maps are protected by one global `g_mutex`; the layer assumes the
app does not submit on the same `VkQueue` concurrently during `xrReleaseSwapchainImage`.

### `tests/integration/`: proving the layer actually loads and runs

Nothing above this could previously be verified without a real OpenXR runtime (SteamVR/Monado) —
the OpenXR loader resolves the active *runtime* before it ever looks at layers, so with no runtime
installed, `xrCreateInstance` fails at `RuntimeInterface::LoadRuntimes` and the layer is never even
discovered, regardless of layer-specific setup. `tests/integration/` solves this with a from-scratch
fake OpenXR *runtime* (`stub_runtime.cpp`, a separate concept from an API *layer* — it sits at the
bottom of the chain and must itself allocate real `VkImage`s for swapchains) plus a Vulkan+OpenXR
test app (`smoke_test.cpp`) that drives a real session/swapchain through the real layer and checks
that per-pixel noise variance actually drops after `xrReleaseSwapchainImage` — i.e. that the
compute shader really ran, not just that nothing crashed. `XR_RUNTIME_JSON` (set by the `layer_smoke`
ctest entry) points the loader straight at the stub runtime's manifest, bypassing the normal
`active_runtime.json` discovery entirely. See that directory's README for what this does and does
not prove (notably: timing is not representative, and only `arraySize == 1` is exercised).

## Packaging & distribution

- `layer/XR_APILAYER_SteamFrame_passthrough_denoise.json` — explicit-layer manifest template with a
  relative `library_path`, copied next to the built `.so` in the build dir by CMake
  (`configure_file`). A relative path here only resolves correctly when the manifest and `.so` are
  in the same directory — see `SETUP.md`'s troubleshooting section before symlinking this elsewhere.
- `cmake --install build` (or `get.sh`, which mirrors the same layout) installs the layer
  **user-locally** under `$HOME` — `~/.local/lib/openxr`, `~/.local/share/openxr/1/api_layers/implicit.d`,
  `~/.config/steamframe-passthrough/config.ini` — specifically so it survives SteamOS read-only root
  image updates. `get.sh` and `docs/get.sh` are kept byte-identical (the latter is what
  `.github/workflows/deploy-pages.yml` publishes to GitHub Pages for the documented
  `curl ... | bash` one-liner) — update both together.
- The installed manifest (written by both `CMakeLists.txt`'s install step and `get.sh`) is an
  *implicit* layer with `enable_environment`/`disable_environment` guards, unlike the dev-build
  manifest in `layer/` which is explicit-only — intentional difference between "installed for daily
  use, opt-in via env var" and "loaded only when testing from the build dir."
