#!/usr/bin/env bash
# Interactive firmware build via docker (espressif/idf:v5.5.2).
# Run from WSL/Linux. Wraps scripts/release.py.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.2}"

# Boards prioritized for DogmaBot — these come first in the picker.
PRIORITY_BOARDS=(
    "sp-esp32-s3-1.28-box"
    "waveshare/esp32-s3-rlcd-4.2"
)

cd "$REPO_ROOT"

discover_boards() {
    # All directories under main/boards/ that ship a config.json, in
    # either 'board' or 'vendor/board' shape.
    find main/boards -maxdepth 3 -name config.json -printf '%P\n' \
        | sed 's|/config.json$||' \
        | sort -u
}

mapfile -t ALL_BOARDS < <(discover_boards)
if [[ ${#ALL_BOARDS[@]} -eq 0 ]]; then
    echo "No boards found under main/boards/*/config.json" >&2
    exit 1
fi

# Reorder: priority first (only if they exist), then everything else.
ORDERED=()
for p in "${PRIORITY_BOARDS[@]}"; do
    for b in "${ALL_BOARDS[@]}"; do
        [[ "$b" == "$p" ]] && ORDERED+=("$b")
    done
done
for b in "${ALL_BOARDS[@]}"; do
    skip=0
    for p in "${PRIORITY_BOARDS[@]}"; do [[ "$b" == "$p" ]] && skip=1; done
    [[ $skip -eq 0 ]] && ORDERED+=("$b")
done

echo "DogmaBot firmware build"
echo "  repo:  $REPO_ROOT"
echo "  image: $IDF_IMAGE"
echo
echo "Boards (1-${#PRIORITY_BOARDS[@]} = DogmaBot defaults):"
for i in "${!ORDERED[@]}"; do
    num=$((i + 1))
    marker=""
    [[ $i -lt ${#PRIORITY_BOARDS[@]} ]] && marker=" *"
    printf "  %3d) %s%s\n" "$num" "${ORDERED[$i]}" "$marker"
done
echo

default_idx=1
read -r -p "Select board [#, name, or Enter for ${ORDERED[0]}]: " choice
choice="${choice:-$default_idx}"

BOARD=""
if [[ "$choice" =~ ^[0-9]+$ ]]; then
    idx=$((choice - 1))
    if [[ $idx -ge 0 && $idx -lt ${#ORDERED[@]} ]]; then
        BOARD="${ORDERED[$idx]}"
    fi
else
    for b in "${ORDERED[@]}"; do
        [[ "$b" == "$choice" ]] && BOARD="$b"
    done
fi

if [[ -z "$BOARD" ]]; then
    echo "Invalid selection: $choice" >&2
    exit 1
fi

# Sanitized zip name pattern that release.py uses for vendor/board
# entries (it replaces '/' with '-').
ZIP_BOARD="${BOARD//\//-}"
ZIP_GLOB="releases/v*_${ZIP_BOARD}.zip"

echo
echo "Selected: $BOARD"

CLEAN=0
existing_zips=( $(ls $ZIP_GLOB 2>/dev/null || true) )
if [[ ${#existing_zips[@]} -gt 0 || -d build ]]; then
    echo "Existing artifacts:"
    [[ -d build ]] && echo "  build/ (incremental rebuild possible)"
    for z in "${existing_zips[@]}"; do echo "  $z"; done
    read -r -p "Clean build/ and remove existing zip(s) for this board? [y/N]: " ans
    [[ "${ans,,}" == "y" || "${ans,,}" == "yes" ]] && CLEAN=1
fi

read -r -p "Run docker build now? [Y/n]: " go
go="${go:-y}"
if [[ "${go,,}" != "y" && "${go,,}" != "yes" ]]; then
    echo "Aborted."
    exit 0
fi

clean_cmd=""
if [[ $CLEAN -eq 1 ]]; then
    clean_cmd="rm -rf build $ZIP_GLOB && "
fi

echo
echo ">>> docker run $IDF_IMAGE python scripts/release.py $BOARD"
echo

# -t for clean output progress; bind-mount repo as /project.
docker run --rm \
    -v "$REPO_ROOT":/project \
    -w /project \
    "$IDF_IMAGE" \
    bash -lc "${clean_cmd}python scripts/release.py '$BOARD'"

echo
echo "Artifacts:"
ls -lh $ZIP_GLOB 2>/dev/null || true
[[ -f build/xiaozhi.bin ]]      && ls -lh build/xiaozhi.bin
[[ -f build/merged-binary.bin ]] && ls -lh build/merged-binary.bin
echo
echo "Flash:"
echo "  PowerShell (Windows):  cd scripts\\dogmabot ; .\\flash.ps1 -AppOnly   # fast, preserves Wi-Fi"
echo "  PowerShell (full):     .\\flash.ps1                                  # full merged-binary"
