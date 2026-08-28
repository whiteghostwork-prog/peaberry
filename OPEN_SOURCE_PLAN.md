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

**Rule of thumb:** if it adds a feature to the renderer, it goes in **peaberry** behind a public header and a headless test. If it shows the feature off, ships a scene, or measures it, it goes in **espresso**. Optional or expensive features stay behind a CMake flag (`PEABERRY_ENABLE_*`) so the default build stays small — see the planned `PEABERRY_ENABLE_SPECTRAL` pattern in Phase 18.

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
| Ray tracing (Phase 12) | Removed — single-bounce `rayQuery` reflections were a dead-end bet. Superseded by Phase 18, which is now a **ray-tracing-pipeline spectral path tracer** |
| Spectral rendering | **Gap (additive track)** — RGB-only forward path; no wavelength-resolved light transport (blocks dispersion, iridescence, fluorence, true color fidelity). Phase 18 |
| Path tracing | **Gap** — no acceleration structures, no ray shaders, no stochastic sampling, no accumulation buffer. Phase 18 |
| Lighting — multi-light | Done (Phase 13.1/13.2) — 1 directional + up to 8 punctual, point and spot. `KHR_lights_punctual` loaded from glTF scenes (13.3, done) — scene lights flow into the forward pass automatically and animate with their nodes. **Gap** — no per-light enable/shadow toggles (13.4) |
| Shadows | Directional 1024² PCF + cube/omni maps for point lights (14.2, done). **Gap** — no cascades (CSM), no PCSS |
| Post-processing | HDR offscreen + ACES tonemap (15.1, done); auto-exposure via luminance histogram + temporal adaptation (15.2, done); bloom (15.3, done). **Gap** — no SSAO/TAA |
| glTF material extensions | `KHR_materials_unlit` + `_emissive_strength` done (16.1). **Gap** — `_clearcoat/_sheen/_transmission/_volume/_ior/_specular/_iridescence/_anisotropy` unsupported |
| sRGB output | Done (15.1) — sRGB render target / proper OETF, replacing the old in-shader `pow(1/2.2)` |
| Sampling quality | **Gap** — no Sobol, no blue noise, no per-pixel RNG. `brdf_lut.frag:33` still uses a `fract(sin(...))` hash where `prefilter.frag` correctly uses Hammersley |
| Memory allocation | **Gap** — `src/rhi/alloc.c` is a no-op stub; one `vkAllocateMemory` per resource, which will hit `maxMemoryAllocationCount` on real scenes |
| Synchronization | **Gap** — zero `vkCreateSemaphore` call sites; all cross-queue and swapchain sync is pushed onto the application |
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

### Ray tracing (Phase 12) — removed, and partly re-litigated

Phase 12 shipped a single-bounce `rayQuery` reflection behind `PEABERRY_ENABLE_RAYTRACING`. It was a dead-end bet: one-bounce reflections are a 2018-era technique that path tracing has since made obsolete, and the hit shader returned a constant color (never made material-aware). Removing it was right.

**The CI half of that rationale has expired.** The original note said it "couldn't be exercised in CI (Lavapipe has no ray query)". Lavapipe has implemented `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations`, `VK_KHR_ray_query` and `VK_KHR_ray_tracing_pipeline` since Mesa 24.1 (2024), with software traversal. Ray tracing is therefore fully CI-testable on the existing Lavapipe baseline, and Phase 18 no longer needs to avoid it.

**Spectral path tracing (Phase 18)** is an **additive** opt-in track on the ray-tracing pipeline (not a compute-only convention). Forward raster PBR (VS/FS) remains the default library path and a first-class vevio workload.

## Roadmap — PBR completeness phases

New tracks addressing the gaps that currently keep peaberry from reading as a complete PBR library. Each phase lists goal, sub-phases, **ownership** (peaberry vs espresso), and whether it is **core** or **optional/feature-flagged**. Major numbers 6 and 8 are unused in history, so new tracks start at 13.

### Phase 13 — Multi-light system  *(core, highest-impact gap)*

| Sub | Goal | Owner |
|-----|------|-------|
| 13.1 | Light-list UBO + shader light loop (1 directional + N point) — *done* | peaberry |
| 13.2 | Spot lights (cone + penumbra) — *done* | peaberry |
| 13.3 | `KHR_lights_punctual` loader support — *done* (scene lights flow into the pass automatically; getters expose world-space position/direction per frame) | peaberry |
| 13.4 | Per-light enable/shadow toggles (public light-setter API landed with 13.1) | peaberry |
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

### Phase 18 — Spectral path tracing  *(additive; opt-in behind `PEABERRY_ENABLE_SPECTRAL`)*

Spectral rendering is the ground-truth lighting track that justifies calling peaberry "physically based" under hard illuminants and materials. RGB path tracing converges to a *less wrong* image than RGB raster; **spectral** path tracing converges to wavelength-resolved radiance (dispersion, thin-film iridescence, fluorence, non-daylight illuminants).

**How this combines with raster (game-style hybrid):** shipping games almost always keep a **raster primary** (G-buffer or forward VS/FS) and add **RGB** ray/path tracing for shadows, reflections, or GI, then denoise and composite. They do not replace VS/FS with a pure path tracer for the whole frame, and they almost never use spectral transport in real time. Peaberry follows that pattern: default build stays forward PBR; Phase 18 adds an RT path tracer (RGB first, then spectral) that can run standalone or later composite over a raster primary. The RT unit and spectral evaluation are complementary layers (visibility vs radiometry), not alternatives.

**Design basis.** The spectral technique follows Christoph Peters' spectral rendering series ([overview](https://momentsingraphics.de/SpectralRenderingOverview.html), [part 1 — spectra](https://momentsingraphics.de/SpectralRendering1Spectra.html), [part 2 — real-time rendering](https://momentsingraphics.de/SpectralRendering2Rendering.html)). His measured overhead on real content (Bistro) is **2–7%** over RGB path tracing — spectral rendering is no longer expensive. Three ingredients:

1. **Fourier sRGB** (Peters' RGB→spectrum upsampling): each sRGB texel becomes a 3-float Fourier-sRGB triple, **same storage as RGB** and still BC-compressible. Reflectance at wavelength λ is evaluated as `a(λ) = (1/π)·arctan(L₀ + 2L₁·cos(φ) + 2L₂·cos(2φ)) + ½` — a few trig ops.
2. **Hero-wavelength sampling**: each path samples one random wavelength (stratified via a per-illuminant 1D inverse-CDF LUT, ~8 KB per light). At the end of the path, convert to XYZ using CIE color matching functions, then to the output RGB space.
3. **Spectral BRDF**: the GGX microfacet model is unchanged — it just produces a 2-vector (base-color weight, white weight) instead of an RGB triple, and the spectral renderer multiplies by the sampled reflectance.

**Transport is a real path tracer on `VK_KHR_ray_tracing_pipeline`** — `rgen`/`rchit`/`rahit`/`rmiss` shaders, a shader binding table, and `VK_KHR_acceleration_structure` for BLAS/TLAS. The earlier compute-only formulation existed solely to dodge a CI limitation that no longer exists (see the Phase 12 note above); Lavapipe has supported the RT pipeline since Mesa 24.1, so this remains fully CI-testable.

**Vevio** is a full GPU (raster + compute + RT + spectral). Both forward PBR and Phase 18 are vevio goal lines — see `~/vevio/docs/vevio-vulkan-roadmap.md`.

| Sub | Goal | Owner |
|-----|------|-------|
| 18.1 | **RT scaffolding**: enable `VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_EXT_descriptor_indexing` and the device features they need. Note this replaces the current "no device features requested" posture in `src/vk/context.c` | peaberry |
| 18.2 | **Acceleration structures**: BLAS per glTF mesh, TLAS over the draw list, rebuilt on animation; reuse the existing flat `draws[]` array and its world matrices | peaberry |
| 18.3 | **Minimal RGB path tracer**: `rgen` camera rays, `rmiss` environment lookup, `rchit` with the existing Cook-Torrance GGX BSDF; NEE against the existing `pb_light` list, MIS between BSDF and light sampling, Russian roulette; accumulation buffer that resets on camera move. Ships a ground-truth reference to diff the raster path against — valuable on its own | peaberry |
| 18.4 | **Fourier-sRGB pipeline**: build the 256³ sRGB→Fourier-sRGB 3D LUT (one-time), preprocess textures at load to the 3-float triple (same footprint, BC-friendly), implement the wavelength-warp + arctan reflectance evaluator in the shader | peaberry |
| 18.5 | **Hero-wavelength sampling**: per-illuminant 1D inverse-CDF LUT generation; stratified single-wavelength selection per path | peaberry |
| 18.6 | **Spectral BRDF**: adapt GGX to emit (base-color, white) 2-vector weights; multiply by the sampled reflectance spectrum per wavelength | peaberry |
| 18.7 | **Spectral→XYZ→RGB output**: CIE color matching functions sampled at the hero wavelength; configurable output color space (sRGB / Rec.2020 / ACES) | peaberry |
| 18.8 | **Native spectral effects** — dispersion (wavelength-dependent index of refraction) and thin-film iridescence, the payoff that RGB physically cannot express | peaberry |
| 18.9 | **Sampling quality**: Sobol or blue-noise sequences replacing the `fract(sin(...))` hash still used at `shaders/brdf_lut.frag:33`; per-pixel decorrelated RNG | peaberry |
| 18.10 | *(optional)* Denoiser: SVGF-style spatio-temporal filter so images are usable before full convergence | peaberry |
| —   | `peaberry_spectral` reference app: side-by-side RGB raster vs spectral ground truth, with a dispersion/iridescence showcase scene | *espresso only* |

### Phase 19 — RHI hardening  *(core; prerequisite for 18 at scale)*

Two long-standing shortcuts become real problems once every mesh needs a BLAS and every frame traces rays.

| Sub | Goal | Owner |
|-----|------|-------|
| 19.1 | **Memory sub-allocator** — `src/rhi/alloc.c` is currently a no-op stub and every buffer/image calls `vkAllocateMemory` directly. Suballocate from pooled blocks; add a `DEVICE_LOCAL \| HOST_VISIBLE` (ReBAR) class | peaberry |
| 19.2 | **Staging upload path** — `src/rhi/buffer.c:180` errors out with "staging upload not implemented", so device-local buffers cannot be filled. Acceleration-structure scratch and vertex buffers need this | peaberry |
| 19.3 | **Semaphores** — there are currently zero `vkCreateSemaphore` call sites; all cross-queue and swapchain synchronization is the application's problem. Add timeline semaphores to the RHI | peaberry |

## Scope guard

The README states peaberry is *"small, inspectable, not a game engine."* To keep the default build minimal, the following are deliberately **optional / feature-flagged** and must not creep into the core build:

- Spectral path tracing (Phase 18) — behind `PEABERRY_ENABLE_SPECTRAL`. Multi-bounce GI is in scope inside this flag. Default build stays forward raster PBR.
- KTX2 / BasisU, Draco compression (Phase 17.3–17.4) — behind their own flags
- Screen-space effects that approach engine territory (SSR, SSGI) — explicit opt-in only

Core phases (13–16) and 19 each ship a bounded, headless-testable module and a public header. Phase 18 is an opt-in ground-truth / research track, not a deletion of the raster renderer.

### Loader & content

- **Mikktspace tangents** (Phase 9.6, done) — the glTF-standard algorithm, replacing the Lengyel UV-gradient generator
- **Morph targets, KTX2/BasisU, Draco, data-URI images** — now tracked under Phase 17

## Why open source

Publishing Peaberry supports reproducible research, community review, and transparent use of AI coding tools on a **non-trivial but bounded** graphics stack. It is a learning and experimentation platform—not a shipped game engine.

## License

Apache License 2.0. See [LICENSE](LICENSE).

---

*Repository: Peaberry · Language: C11 · API: Vulkan · License: Apache-2.0*
