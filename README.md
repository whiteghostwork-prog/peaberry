# Peaberry

Peaberry is an open-source **C** library for **physically based rendering (PBR)** on **Vulkan**.

## About

Peaberry is the **open-source codebase for my research on graphics programming**. The goal is a small, inspectable renderer that agents (and humans) can extend, test, and measure—Vulkan usage, PBR, glTF pipelines, shaders, and benchmarks in one place.

It doubles as a **research and learning platform**: readable C for studying real-time graphics, running experiments, and comparing results—not a production game engine.

The project keeps a narrow scope on purpose: a library core with examples, automated tests, and GPU timing tools so work can be reproduced and evaluated.

Application summary: [OPEN_SOURCE_PLAN.md](OPEN_SOURCE_PLAN.md)

**License:** [Apache-2.0](LICENSE)

## Features

- Vulkan RHI (buffers, textures, meshes, shaders)
- Forward PBR — Cook-Torrance, normal maps, occlusion, emissive, ACES tone mapping
- Image-based lighting — BRDF LUT, irradiance, prefiltered environment
- glTF 2.0 loading — materials, scenes, transparency, double-sided materials, texture transforms
- Interactive examples and a glTF viewer
- GPU benchmarks and frame metrics (CPU/GPU timing, FPS)
- Automated tests (headless Vulkan + render smoke)

## Plans

Near term:

- Richer glTF content support (tangents, animation, skinning)
- Scene polish — shadows, MSAA, basic culling

Longer term (optional):

- Ray-traced effects behind a feature flag

More detail: [docs/roadmap.md](docs/roadmap.md)

## Quick start

**Requirements:** Linux, **Vulkan-capable GPU**, **Wayland** for interactive examples ([dependencies](docs/dependencies.md)).

```bash
sudo apt install build-essential cmake git pkg-config glslang-tools \
    libvulkan-dev vulkan-tools vulkan-validationlayers \
    libwayland-dev wayland-protocols libxkbcommon-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

```bash
./build/examples/peaberry_gltf assets/models/test_cube.gltf
```

**Controls:** left-drag orbit · scroll or Q/E zoom · Esc quit

## Examples

| Binary | Description |
|--------|-------------|
| `peaberry_hello_vk` | Clear-color smoke test |
| `peaberry_triangle` | Rotating triangle |
| `peaberry_quad` | Textured quad |
| `peaberry_sphere` | PBR sphere + IBL |
| `peaberry_gltf` | glTF viewer |
| `peaberry_bench` | GPU benchmark runner |

## Layout

```
include/peaberry/   Public headers
src/                core, vk, rhi, pbr, load
tests/              Unit and GPU tests
examples/           Sample apps (Wayland WSI in examples/common/)
shaders/            GLSL sources
assets/             Textures and test models
docs/               Roadmap and notes
```

## Contributing

1. Keep changes small and match existing C style.
2. Windowing stays in `examples/` — the library has no GLFW dependency.
3. Verify: `cmake --build build --target peaberry_tests && ./build/tests/peaberry_tests`
4. Shaders live in `shaders/`; SPIR-V is built automatically.

CI runs build + tests on every push (`.github/workflows/ci.yml`).

## License

Copyright 2026 The Peaberry Authors · [Apache-2.0](LICENSE)
