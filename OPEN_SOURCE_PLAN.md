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
| Stress benchmark scenes | Planned (Phase 7.8) |
| Bench preview window (11.8) | Done |
| Ray tracing | Planned (Phase 12, optional) |

## Immediate next steps

1. **Phase 7.8** — `stress_grid.gltf` + `gltf_stress` bench (multi-draw scene for culling/shadows)
2. **Bench polish** — `test_cube_ground.gltf` or floor mesh so cast shadows are visible in `--window` / viewer
3. **Docs sync** — README, benchmarks.md, dependencies aligned with current scenarios (`gltf_instanced`, `gltf_shadows`, `--window`)
4. **Tagged release** — pin espresso CI to semver peaberry

## Medium-term plans

### Bench preview window (Phase 11.8) — done

`peaberry_bench --window` in espresso presents each sample frame via WSI (orbit camera, glTF animation when present), then prints the usual timing table / JSON to stdout when the run finishes. Headless remains the default for CI.

Shadow map light projection was fixed for LH view space (peaberry); glTF viewer `S` toggles an optional neutral shadow overlay.

### Stress benchmarks (Phase 7.8)

Current bench smoke uses `test_cube` (1 draw) — too small to measure culling, sorting, or shadow cost at scale. Plan:

- `scripts/gen_stress_scene.py` in espresso — N×M grid of cubes, multiple materials
- Checked-in `assets/scenes/stress_grid.gltf` (~64–256 draws)
- New scenarios: `gltf_stress`, `gltf_stress_shadows`, camera preset to validate culling
- Optional downloaded Sponza for local baselines only (gitignored)
- CI stays on micro scenes; stress runs locally

### Ray tracing (Phase 12, optional)

Hybrid desktop RT behind `PEABERRY_ENABLE_RAYTRACING=OFF` by default:

- BLAS/TLAS from existing meshes + scene graph transforms
- Single-bounce specular reflections via `rayQuery` (dedicated RT shader variant)
- RT shadows as optional compare to Phase 11 shadow maps
- `peaberry_pathtrace` quality reference in espresso only
- `rt_hybrid` bench on `stress_grid` — local only, never CI
- Skinning + RT deferred; forward path unchanged when RT off

See `docs/roadmap.md` Phase 12 for full step list, extensions, and out-of-scope items.

### Other

- Tangents via mikktspace (Phase 9.6, deferred)
- `find_package(Peaberry)` polish and tagged releases for espresso pinning

## Why open source

Publishing Peaberry supports reproducible research, community review, and transparent use of AI coding tools on a **non-trivial but bounded** graphics stack. It is a learning and experimentation platform—not a shipped game engine.

## License

Apache License 2.0. See [LICENSE](LICENSE).

---

*Repository: Peaberry · Language: C11 · API: Vulkan · License: Apache-2.0*
