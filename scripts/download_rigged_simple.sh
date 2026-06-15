#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/assets/models"
URL="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF-Binary/RiggedSimple.glb"

mkdir -p "${OUT}"
curl -fsSL "${URL}" -o "${OUT}/RiggedSimple.glb"
echo "Downloaded ${OUT}/RiggedSimple.glb"
