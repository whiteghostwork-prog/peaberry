#!/usr/bin/env python3
"""Generate assets/models/test_cube.gltf and test_cube.bin for loader tests."""

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "models"
OUT_DIR.mkdir(parents=True, exist_ok=True)

s = 0.5

# Closed unit cube: 24 vertices (4 per face), CCW from outside.
face_defs = [
    ((0, 0, 1), [(-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]),
    ((0, 0, -1), [(s, -s, -s), (-s, -s, -s), (-s, s, -s), (s, s, -s)]),
    ((1, 0, 0), [(s, -s, s), (s, -s, -s), (s, s, -s), (s, s, s)]),
    ((-1, 0, 0), [(-s, -s, -s), (-s, -s, s), (-s, s, s), (-s, s, -s)]),
    ((0, 1, 0), [(-s, s, s), (s, s, s), (s, s, -s), (-s, s, -s)]),
    ((0, -1, 0), [(-s, -s, -s), (s, -s, -s), (s, -s, s), (-s, -s, s)]),
]

positions = []
normals = []
uvs = []
indices = []

for normal, corners in face_defs:
    base = len(positions) // 3
    face_uvs = [0, 0, 1, 0, 1, 1, 0, 1]
    for i, corner in enumerate(corners):
        positions.extend(corner)
        normals.extend(normal)
        uvs.extend(face_uvs[i * 2 : i * 2 + 2])
    indices.extend([base, base + 1, base + 2, base + 2, base + 3, base])

pos_bytes = struct.pack(f"{len(positions)}f", *positions)
norm_bytes = struct.pack(f"{len(normals)}f", *normals)
uv_bytes = struct.pack(f"{len(uvs)}f", *uvs)
idx_bytes = struct.pack(f"{len(indices)}H", *indices)
bin_blob = pos_bytes + norm_bytes + uv_bytes + idx_bytes

bin_path = OUT_DIR / "test_cube.bin"
bin_path.write_bytes(bin_blob)

pos_off = 0
norm_off = len(pos_bytes)
uv_off = norm_off + len(norm_bytes)
idx_off = uv_off + len(uv_bytes)
vertex_count = len(positions) // 3

gltf = {
    "asset": {"version": "2.0", "generator": "peaberry gen_test_cube.py"},
    "buffers": [{"byteLength": len(bin_blob), "uri": "test_cube.bin"}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": pos_off, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": norm_off, "byteLength": len(norm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": uv_off, "byteLength": len(uv_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": idx_off, "byteLength": len(idx_bytes), "target": 34963},
    ],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": vertex_count,
            "type": "VEC3",
            "max": [s, s, s],
            "min": [-s, -s, -s],
        },
        {"bufferView": 1, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": vertex_count, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ],
    "materials": [
        {
            "name": "default",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.8, 0.2, 0.2, 1.0],
                "metallicFactor": 0.1,
                "roughnessFactor": 0.6,
            },
        }
    ],
    "meshes": [
        {
            "primitives": [
                {
                    "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                    "indices": 3,
                    "material": 0,
                }
            ]
        }
    ],
    "nodes": [{"mesh": 0}],
    "scenes": [{"nodes": [0]}],
    "scene": 0,
}

gltf_path = OUT_DIR / "test_cube.gltf"
gltf_path.write_text(json.dumps(gltf, indent=2))
print(f"Wrote {gltf_path} and {bin_path} ({vertex_count} verts, {len(indices)} indices)")
