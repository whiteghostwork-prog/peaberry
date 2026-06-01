#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/assets/models"
URL="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb"

mkdir -p "${OUT}"
curl -fsSL "${URL}" -o "${OUT}/DamagedHelmet.glb"
echo "Downloaded ${OUT}/DamagedHelmet.glb"
