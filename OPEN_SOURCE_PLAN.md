# Peaberry — development plan

## Summary

**Peaberry** is an Apache-2.0 C library for physically based rendering on Vulkan. It is the public codebase for my **research for graphics programming**: how coding agents can safely extend a real renderer—shaders, loaders.

The repository is intentionally small and structured so both humans and agents can navigate it, make focused changes, and confirm results without a commercial engine or closed toolchain.

**Espresso** (sibling repo) holds examples, benchmarks, and assets built on top of Peaberry.

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
| Ray tracing (Phase 12) | In progress — hybrid rayQuery reflections behind `PEABERRY_ENABLE_RAYTRACING` |

## Immediate next steps

1. **Phase 12 follow-ups** — RT material hit shading, optional RT shadows, `peaberry_pathtrace` reference in espresso
2. **Docs sync** — README, benchmarks.md, dependencies aligned with current scenarios (`gltf_instanced`, `gltf_shadows`, `gltf_stress`, `rt_hybrid`, `--window`)
3. **Tagged release** — pin espresso CI to semver peaberry

## Medium-term plans

### Bench & profiling polish

- **Stress baselines** — capture local JSON baselines on `gltf_stress` / `gltf_stress_shadows` (never CI; micro scenes stay in CI smoke)
- **Scale-up scenes** — regenerate `stress_grid.gltf` at 16×16 (256 draws) or larger via `gen_stress_scene.py` for local culling/sort/shadow profiling
- **Heavy content (optional)** — downloaded Sponza or similar for local baselines only (gitignored)

### Packaging & documentation

- **Docs sync** — README, `docs/benchmarks.md`, `docs/dependencies.md` aligned with `gltf_instanced`, `gltf_shadows`, `gltf_stress`, `--window`, `--detailed`, `--fps`
- **Tagged release** — semver peaberry tag; pin espresso CI to a released version instead of floating `main`
- **`find_package(Peaberry)`** — CMake config polish for downstream consumers

### Ray tracing (Phase 12, optional)

Hybrid desktop RT behind `PEABERRY_ENABLE_RAYTRACING=OFF` by default:

- **Done (12.1)** — BLAS/TLAS from glTF meshes, `rayQuery` specular reflections (`pbr_forward_rt.frag`), `pb_pbr_forward_pass_set_raytracing_enabled`, `rt_hybrid` bench scenario (local only)
- RT shadows as optional compare to Phase 11 shadow maps
- Material-aware RT hit shading (instance custom index → albedo SSBO)
- `peaberry_pathtrace` quality reference in espresso only
- Skinning + RT deferred; forward path unchanged when RT off

### Loader & content

- **Tangents via mikktspace** (Phase 9.6, deferred) — proper normal mapping without artist-supplied tangents

## Why open source

Publishing Peaberry supports reproducible research, community review, and transparent use of AI coding tools on a **non-trivial but bounded** graphics stack. It is a learning and experimentation platform—not a shipped game engine.

## License

Apache License 2.0. See [LICENSE](LICENSE).

---

*Repository: Peaberry · Language: C11 · API: Vulkan · License: Apache-2.0*
