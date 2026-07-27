# Peaberry

Peaberry is an open-source **C** library for **physically based rendering (PBR)** on **Vulkan**.

## About

Peaberry is the **open-source codebase for my research on graphics programming**. The goal is a small, inspectable renderer that agents (and humans) can extend, test, and measure—Vulkan usage, PBR, glTF pipelines, shaders, and benchmarks in one place.

It doubles as a **research and learning platform**: readable C for studying real-time graphics, running experiments, and comparing results—not a production game engine.

The project keeps a narrow scope on purpose: a **library core** with headless tests and GPU timing APIs. Interactive examples, benchmarks, and assets live in the sibling [**espresso**](https://github.com/whiteghostwork-prog/espresso) repo (Phase 10.7 split).

Application summary: [OPEN_SOURCE_PLAN.md](OPEN_SOURCE_PLAN.md)

**License:** [Apache-2.0](LICENSE)

## Features

- Vulkan RHI (buffers, textures, meshes, shaders)
- Forward PBR — Cook-Torrance, normal maps, occlusion, emissive, directional shadows, ACES tone mapping
- Image-based lighting — BRDF LUT, irradiance, prefiltered environment
- glTF 2.0 loading — materials, scenes, transparency, double-sided materials, texture transforms, animation, skinning
- Frustum culling, MSAA hooks in WSI consumers
- Automated tests (headless Vulkan + render smoke)

## Plans

Near term: bench polish (ground plane for cast shadows), docs sync, tagged release.

**Forward PBR (VS/FS) stays the default.** Additive Phase 18: spectral path tracing on `VK_KHR_ray_tracing_pipeline` (RGB path tracer first, then Fourier-sRGB + hero-wavelength). Games typically hybridize raster primary + RGB RT lighting; spectral plugs into that lighting path. Vevio targets both workloads as a full GPU.

Also planned: stress-scene baselines, mikktspace tangents, RHI hardening (Phase 19).

More detail: [OPEN_SOURCE_PLAN.md](OPEN_SOURCE_PLAN.md) and [docs/benchmarks.md](docs/benchmarks.md)

## Quick start

**Requirements:** Linux, **Vulkan-capable GPU** ([dependencies](docs/dependencies.md)).

```bash
# Library + tests (assets come from sibling espresso checkout)
git clone https://github.com/whiteghostwork-prog/espresso ../espresso

sudo apt install build-essential cmake git pkg-config glslang-tools \
    libvulkan-dev vulkan-tools vulkan-validationlayers

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

For the glTF viewer and `peaberry_bench`, build [**espresso**](https://github.com/whiteghostwork-prog/espresso) instead.

## Layout

```
include/peaberry/   Public headers
src/                core, vk, rhi, pbr, load
tests/              Unit and GPU tests (fixtures in espresso/assets)
shaders/            GLSL sources
docs/               Roadmap and notes
```

## Contributing

1. Keep changes small and match existing C style.
2. The library has no windowing dependency — apps live in **espresso**.
3. Verify: `cmake --build build --target peaberry_tests && ./build/tests/peaberry_tests`
4. Shaders live in `shaders/`; SPIR-V is built automatically.

CI runs build + tests on every push (`.github/workflows/ci.yml`), checking out espresso for test assets.

## License

Copyright 2026 The Peaberry Authors · [Apache-2.0](LICENSE)
