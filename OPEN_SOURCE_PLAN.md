# Peaberry — development plan

## Summary

**Peaberry** is an Apache-2.0 C library for physically based rendering on Vulkan. It is the public codebase for my **research for graphics programming**: how coding agents can safely extend a real renderer—shaders, loaders.

The repository is intentionally small and structured so both humans and agents can navigate it, make focused changes, and confirm results without a commercial engine or closed toolchain.

**Espresso** (sibling repo) holds examples, benchmarks, and assets built on top of Peaberry.

## Project layout & ownership

Peaberry is the library; espresso is everything that shows it off or measures it. This split (established in Phase 10.7) is binding for all new work:

| Layer | Lives in | Examples |
|-------|----------|----------|
| RHI, PBR passes, glTF loader, shaders, headless tests | **peaberry** | `src/`, `shaders/`, `tests/`, `include/peaberry/` |
| Interactive viewer, demo apps | **espresso** (`~/espresso/examples/`) | `gltf/`, `sphere/`, `quad/`, `triangle/`, `hello_vk/` |
| Benchmark runner + scenarios | **espresso** (`~/espresso/benchmarks/`) | `peaberry_bench`, `gltf_shadows`, `gltf_stress` |
| Test/bench assets & asset generators | **espresso** (`~/espresso/assets/`, `~/espresso/scripts/`) | `models/`, `scenes/`, `gen_stress_scene.py` |

**Rule of thumb:** if it adds a feature to the renderer, it goes in **peaberry** behind a public header and a headless test. If it shows the feature off, ships a scene, or measures it, it goes in **espresso**. Optional or expensive features stay behind a CMake flag (`PEABERRY_ENABLE_*`) so the default build stays small — see the planned `PEABERRY_ENABLE_PATHTRACE` pattern in Phase 18.

## Research purpose

Graphics programming is hard to automate because feedback is slow, stateful, and easy to break silently (GPU crashes, wrong pixels, subtle sorting bugs). Peaberry is a **controlled research testbed** to study:

- **Agent-friendly architecture** — clear layers (`rhi/`, `pbr/`, `load/`), public headers, examples kept separate from the library
- **Verifiable tasks** — automated tests including headless Vulkan render smoke; agents can run `./build/tests/peaberry_tests` after edits
- **Measurable performance** — frame metrics APIs consumed by `peaberry_bench` in espresso (Vulkan timestamps, CPU/GPU time, FPS)
- **Real content paths** — glTF 2.0 loading, PBR forward pass, transparency, scene hierarchy, animation, skinning

The research question is not “can an agent write any code,” but **can agents reliably contribute to a graphics codebase when the project gives them tests, benchmarks, and bounded modules to work in.**

## What exists today

| Area | Status |
|------|--------|
| Vulkan RHI + forward PBR + IBL | Done |
| glTF loader (materials, alpha, sort, hierarchy, UV transform) | Done |
| Animation + skinning | Done |
| Directional shadows, MSAA, frustum culling | Done |
| Repo split (`peaberry` / `espresso`) | Done |
| Asset dedup (fixtures only in espresso) | Done |
| Per-frame allocators + instancing (11.5–11.6) | Done |
| Bench `gltf_shadows` scenario (11.7) | Done |
| Stress benchmark scenes | Done (Phase 7.8) |
| Bench preview window (11.8) | Done |
| Ground plane shadow demo asset | Done |
| Mikktspace tangents (Phase 9.6) | Done — generator now uses the glTF-standard mikktspace algorithm |
| Ray tracing (Phase 12) | Removed — single-bounce `rayQuery` reflections were a dead-end bet; superseded by the Phase 18 path-tracing track |
| Lighting — single hardcoded directional | **Gap** — no point/spot, no multi-light, no public setter, no `KHR_lights_punctual` |
| Shadows — single 1024² PCF map | **Gap** — no cascades (CSM), no cube/omni maps (blocks point lights), no soft-shadow option |
| Post-processing | HDR offscreen + ACES tonemap post pass (15.1, done); auto-exposure via luminance histogram + temporal adaptation (15.2, done); **gap** — no bloom/SSAO/TAA |
| glTF material extensions | **Gap** — none loaded; `KHR_materials_unlit/_clearcoat/_sheen/_transmission/_volume/_ior/_specular/_emissive_strength/_iridescence/_anisotropy` all unsupported |
| sRGB output | **Gap** — approximate `pow(1/2.2)` in-shader instead of sRGB render-target / proper OETF |
| glTF completeness | **Gap** — no morph targets, no KTX2/BasisU, embedded data-URI images skipped |

## Immediate next steps

### Close open loops (small, fast)

1. **Docs sync** — README, benchmarks.md, dependencies aligned with current scenarios (`gltf_instanced`, `gltf_shadows`, `gltf_stress`, `--window`). Also resolves dangling `docs/benchmarks.md` / `docs/dependencies.md` links (the `docs/` directory is gitignored and absent on disk).
2. **Tagged release** — pin espresso CI to semver peaberry.

## Medium-term plans

### Bench & profiling polish

- **Stress baselines** — capture local JSON baselines on `gltf_stress` / `gltf_stress_shadows` (never CI; micro scenes stay in CI smoke)
- **Scale-up scenes** — regenerate `stress_grid.gltf` at 16×16 (256 draws) or larger via `gen_stress_scene.py` for local culling/sort/shadow profiling
- **Heavy content (optional)** — downloaded Sponza or similar for local baselines only (gitignored)

### Packaging & documentation

- **Docs sync** — README, `docs/benchmarks.md`, `docs/dependencies.md` aligned with `gltf_instanced`, `gltf_shadows`, `gltf_stress`, `--window`, `--detailed`, `--fps`
- **Tagged release** — semver peaberry tag; pin espresso CI to a released version instead of floating `main`
- **`find_package(Peaberry)`** — CMake config polish for downstream consumers

### Ray tracing (Phase 12) — removed

Phase 12 shipped a single-bounce `rayQuery` reflection behind `PEABERRY_ENABLE_RAYTRACING`. It was a dead-end bet: one-bounce reflections are a 2018-era technique that path tracing has since made obsolete, the hit shader returned a constant color (never made material-aware), and it couldn't be exercised in CI (Lavapipe has no ray query). The code has been removed wholesale. The forward path is unchanged. Real-time path tracing (Phase 18) is the new forward direction for a ground-truth reference.

## Roadmap — PBR completeness phases

New tracks addressing the gaps that currently keep peaberry from reading as a complete PBR library. Each phase lists goal, sub-phases, **ownership** (peaberry vs espresso), and whether it is **core** or **optional/feature-flagged**. Major numbers 6 and 8 are unused in history, so new tracks start at 13.

### Phase 13 — Multi-light system  *(core, highest-impact gap)*

| Sub | Goal | Owner |
|-----|------|-------|
| 13.1 | Light-list UBO + shader light loop (1 directional + N point) | peaberry |
| 13.2 | Spot lights (cone + penumbra) | peaberry |
| 13.3 | `KHR_lights_punctual` loader support | peaberry |
| 13.4 | Public light-setter API + per-light enable/shadow toggles | peaberry |
| —   | Multi-light demo scene | espresso |

### Phase 14 — Shadow quality  *(core)*

| Sub | Goal | Owner |
|-----|------|-------|
| 14.1 | Cascaded shadow maps (directional; camera-frustum splits + per-cascade PCF) | peaberry |
| 14.2 | Cube/omni shadow maps for point lights (depends on 13) | peaberry |
| 14.3 | *(optional)* PCSS / higher-tap filtering | peaberry |
| —   | `csm` / `point_shadow` bench scenarios | espresso |

### Phase 15 — Post-processing foundation  *(core; folds in the sRGB correctness fix)*

| Sub | Goal | Owner |
|-----|------|-------|
| 15.1 | HDR offscreen target (`R16G16B16A16_SFLOAT`) + fullscreen-triangle post pass; move tonemap out of the forward frag shader; replace `pow(1/2.2)` with sRGB render-target / proper OETF | peaberry |
| 15.2 | Auto-exposure (log-luminance histogram + temporal eye-adaptation; peaberry's first compute shader) — *done* | peaberry |
| 15.3 | Bloom | peaberry |
| 15.4 | SSAO (GTAO-style) | peaberry |
| 15.5 | TAA (requires per-pixel motion vectors) | peaberry |
| —   | Before/after tone-map + post demo | espresso |

### Phase 16 — glTF material extensions  *(core, sequenced by value/cost)*

| Sub | Goal | Owner |
|-----|------|-------|
| 16.1 | `KHR_materials_unlit` + `KHR_materials_emissive_strength` (trivial; huge asset coverage) — *done* | peaberry |
| 16.2 | `KHR_materials_ior` + `KHR_materials_specular` | peaberry |
| 16.3 | `KHR_materials_sheen` | peaberry |
| 16.4 | `KHR_materials_clearcoat` (second GGX lobe) | peaberry |
| 16.5 | `KHR_materials_transmission` + `KHR_materials_volume` (refraction + transparent render-order work) | peaberry |
| 16.6 | `KHR_materials_iridescence` + `KHR_materials_anisotropy` (advanced) | peaberry |
| —   | Extension sample assets (e.g. clearcoat helmet) | espresso |

### Phase 17 — glTF completion  *(mixed core/optional)*

| Sub | Goal | Owner |
|-----|------|-------|
| 17.1 | Morph targets (animation `weights`/`targets` channels) | peaberry + animated test asset. *espresso* |
| 17.2 | Embedded data-URI image decoding (currently silently skipped) | peaberry |
| 17.3 | *(optional, behind a flag)* KTX2 / Basis-Universal (`KHR_texture_basisu`) | peaberry |
| 17.4 | *(optional, behind a flag)* `KHR_draco_mesh_compression` | peaberry |

### Phase 18 — Path tracing reference  *(core-for-a-PBR-library; opt-in behind `PEABERRY_ENABLE_PATHTRACE`)*

A path tracer is the **ground truth** that justifies calling peaberry "physically based" — it validates the raster PBR approximations and gives GI / transmission / caustics / soft shadows for free. Compute-based (no `ray_query` / no RT pipeline), so it runs on **any compute-capable GPU** and is CI-testable on Lavapipe (unlike the deleted Phase 12). It supersedes the abandoned Phase 12 hybrid-RT experiment.

| Sub | Goal | Owner |
|-----|------|-------|
| 18.1 | Compute-pass scaffolding: Vulkan compute pipeline + dispatch + storage-image output; camera-ray generation; reuse the glTF scene's vertex/index/material buffers | peaberry |
| 18.2 | Direct lighting: camera rays → closest hit → explicit light sampling + shadow ray (against the existing directional) | peaberry |
| 18.3 | BSDF sampling (GGX microfacet) + multiple importance Sampling (MIS) between BRDF and light sampling | peaberry |
| 18.4 | IBL importance sampling: sample the prefiltered env / irradiance maps for the indirect path | peaberry |
| 18.5 | Russian roulette + path length bounding (unbiased termination of long paths) | peaberry |
| 18.6 | Accumulation buffer: temporally accumulate samples while the camera is static, reset on move | peaberry |
| 18.7 | *(optional)* Denoiser: SVGF-style spatio-temporal filter so convergent images are usable at realtime | peaberry |
| 18.8 | `peaberry_pathtrace` reference app: side-by-side raster PBR vs path-traced ground truth | *espresso only* |

## Scope guard

The README states peaberry is *"small, inspectable, not a game engine."* To keep the default build minimal, the following are deliberately **optional / feature-flagged** and must not creep into the core build:

- Path tracing (Phase 18) — behind `PEABERRY_ENABLE_PATHTRACE`
- KTX2 / BasisU, Draco compression (Phase 17.3–17.4) — behind their own flags
- Heavier post/effects that approach engine territory (SSR, full RT-GI, multi-bounce GI) — explicit opt-in only

Core phases (13–16) each ship a bounded, headless-testable module and a public header, in the spirit of the existing PBR/IBL/glTF work.

### Loader & content

- **Mikktspace tangents** (Phase 9.6, done) — the glTF-standard algorithm, replacing the Lengyel UV-gradient generator
- **Morph targets, KTX2/BasisU, Draco, data-URI images** — now tracked under Phase 17

## Why open source

Publishing Peaberry supports reproducible research, community review, and transparent use of AI coding tools on a **non-trivial but bounded** graphics stack. It is a learning and experimentation platform—not a shipped game engine.

## License

Apache License 2.0. See [LICENSE](LICENSE).

---

*Repository: Peaberry · Language: C11 · API: Vulkan · License: Apache-2.0*
