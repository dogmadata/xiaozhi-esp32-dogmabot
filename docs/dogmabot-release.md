# DogmaBot firmware release & maintenance

How this fork produces firmware binaries, how to publish a new release,
and how to absorb a new upstream version of `78/xiaozhi-esp32`.

Reading order: [Overview](#overview) → [Project layout](#project-layout) →
[Routine release](#routine-release) → procedures further down as needed.

---

## Overview

This fork (`dogmadata/xiaozhi-esp32-dogmabot`) ships **customized
firmware** for devices that talk to a **DogmaBotServer** deployment.
Devices that want stock behavior should run upstream firmware
(`78/xiaozhi-esp32`) — there's no reason to use this fork unless you
also run our server.

A "release" here means: a **GitHub Release** on this repo, with one
**zip asset per board** containing a `merged-binary.bin`. The
DogmaBotServer admin pulls these zips via its **Sync from upstream**
UI, slices the app partition out (`MergedBinaryExtractor` parses the
embedded partition table), and stores them under
`{firmware_root}/dogmadata/xiaozhi-esp32-dogmabot/{board}_{ver}.bin`.

Boards currently in the release set (matrix in
`.github/workflows/release.yml`):

- `sp-esp32-s3-1.28-box` — Spotpear ESP32-S3 1.28" round display.
- `waveshare/esp32-s3-rlcd-4.2` — Waveshare ESP32-S3 4.2" round LCD.

`PROJECT_VER` in `CMakeLists.txt` stays pinned to **`2.2.6`** (the
upstream baseline we're currently tracking). We re-use the **same tag
`v2.2.6`** across iterations rather than bumping — see
[Re-releasing under the same tag](#re-releasing-under-the-same-tag).

---

## Project layout

This repo has **two long-lived branches**:

| Branch | Tracks | Purpose |
|---|---|---|
| **`main`** | `upstream/main` (`78/xiaozhi-esp32:main`) | Vanilla upstream code, never diverges. Used as a base for merging upstream changes into `dogmabot`. |
| **`dogmabot`** | — | The trunk of our fork. All customizations (pt-PT default, release CI matrix, board-specific UI like the round 1.28" layout, OTA URL, etc.) live here. **CI for releases runs from this branch's tags.** |

`origin` is the GitHub fork; `upstream` is the original Espressif repo:

```bash
# (informational — already configured in working tree)
git remote -v
# origin    git@github.com:dogmadata/xiaozhi-esp32-dogmabot.git
# upstream  git@github.com:78/xiaozhi-esp32.git
```

The `release.yml` workflow triggers on push of any tag matching `v*`,
regardless of which branch the tag points at. In practice, **only tag
commits on `dogmabot`** — `main` mirrors upstream and isn't where our
customizations live.

---

## CI workflow

File: [`.github/workflows/release.yml`](../.github/workflows/release.yml)

What it does on `git push origin v*`:

1. Spawns a matrix job per board (currently the two listed above) inside
   the `espressif/idf:v5.5.2` container.
2. Runs `python scripts/release.py <board> --name <variant>` which:
   - Calls `idf.py build` with the right `sdkconfig_append` flags from
     the board's `config.json`.
   - Calls `merge-bin` to produce `build/merged-binary.bin`.
   - Zips it into `releases/v{PROJECT_VER}_{variant}.zip`.
3. Uploads the zip as a workflow artifact for traceability.
4. Attaches the zip to the GitHub Release for the pushed tag (creates
   the release on first asset, updates on subsequent ones), using
   `softprops/action-gh-release@v2`.

The zip is **the contract with DogmaBotServer**: the server's
`UpstreamSyncService.ExtractAppBytes`
(`backend/src/DogmaBot.Admin.Api/Firmware/UpstreamSyncService.cs`)
expects a `.zip` containing a `merged-binary.bin` at any depth.

---

## Routine release

When you've made changes on `dogmabot` and want to publish a new
firmware (no new boards, no upstream merge).

```bash
# 1. Make sure the branch is in good shape and pushed.
git switch dogmabot
git status
# (commit / push any pending work)

# 2. Re-tag v2.2.6 at HEAD. See "Re-releasing under the same tag"
#    below if v2.2.6 already exists on the remote.
git tag v2.2.6
git push origin v2.2.6
```

CI runs and the Release at
`https://github.com/dogmadata/xiaozhi-esp32-dogmabot/releases/tag/v2.2.6`
will have the two asset zips when it finishes (~5–10 min).

**On the server side:**

1. Open the admin SPA → **Firmware → Sync from upstream**.
2. Folder/repo `dogmadata/xiaozhi-esp32-dogmabot` should already be in
   the dropdown (declared in `system_settings.firmware_upstreams_json`).
   If not, add it under **Firmware → Upstreams** first.
3. Pick the new release tag, then for each board map the asset to a
   target board name.
4. After sync, devices polling OTA receive the new
   `firmware.url` and download on the next cycle. To force a connected
   device to update immediately, use the admin "Force OTA" action (which
   sets `firmware.force: 1` in the next OTA response).

---

## Re-releasing under the same tag

Since `PROJECT_VER` is pinned, we deliberately reuse the same `v2.2.6`
tag for each iteration. To replace an existing release:

```bash
# 1. Delete the local + remote tag.
git tag -d v2.2.6
git push origin :refs/tags/v2.2.6

# 2. Delete the GitHub Release (otherwise softprops will try to attach
#    assets to an "old" release that points at the deleted tag).
#    Via the GitHub UI: Releases → v2.2.6 → ... → Delete release.
#    Or via gh CLI:
gh release delete v2.2.6 -y

# 3. Re-tag at the new HEAD and push.
git tag v2.2.6
git push origin v2.2.6
```

CI runs and creates a fresh Release.

**On the server side** the previously-synced files
`{board}_2.2.6.bin` need to be deleted before re-syncing because
`UpstreamSyncService` calls `_catalog.SaveAsync(... overwrite:false ...)`
(see `UpstreamSyncService.cs:165`). Either delete them through the
admin UI or — if this becomes painful — expose `overwrite=true` on the
sync endpoint (out of scope here but worth knowing).

---

## Adding a new board

Use this when a new ESP32 board needs DogmaBot firmware. The board
itself (pin map, codec, display driver, custom Display subclass) is
the usual upstream "add a new board" procedure
(see [`docs/custom-board.md`](custom-board.md) for the full template);
this section only covers **what's specific to our release pipeline**.

Concrete checklist:

1. **Board sources under `main/boards/<vendor>/<board>/`** (or
   `main/boards/<board>/` without a vendor dir, like
   `sp-esp32-s3-1.28-box`). At minimum:
   - `config.h` — pin definitions.
   - `config.json` — release manifest with `target`, `manufacturer`,
     `builds[]` (the `name` field becomes the asset filename suffix).
   - `<board>.cc` — the board class registered via `DECLARE_BOARD(...)`.
   - Custom display, codec, power manager files as needed.
2. **CMake plumbing** in `main/CMakeLists.txt`: add an
   `elseif(CONFIG_BOARD_TYPE_...)` branch with `BOARD_TYPE`,
   `BUILTIN_TEXT_FONT`, `BUILTIN_ICON_FONT`,
   `DEFAULT_EMOJI_COLLECTION`, etc.
3. **Kconfig** entry in `main/Kconfig.projbuild` so menuconfig can
   select the board type.
4. **Release matrix** in `.github/workflows/release.yml` — add the
   board to the matrix `include`:

   ```yaml
   matrix:
     include:
       - { board: sp-esp32-s3-1.28-box,         name: sp-esp32-s3-1.28-box,         full_name: sp-esp32-s3-1.28-box }
       - { board: waveshare/esp32-s3-rlcd-4.2,  name: esp32-s3-rlcd-4.2,            full_name: waveshare-esp32-s3-rlcd-4.2 }
       # ↓ new entry
       - { board: <board-path>,                 name: <name from config.json>,      full_name: <vendor-name or board-name> }
   ```

   The `full_name` is what artifacts/zip filenames use; pick something
   filesystem-safe (no slashes).
5. **Local smoke test** inside Docker before tagging:

   ```bash
   docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.2 \
     bash -c 'source $IDF_PATH/export.sh && \
              python scripts/release.py <board-path> --name <name>'
   # Should produce: releases/v2.2.6_<name>.zip
   ```
6. **Commit + push to `dogmabot`**, then re-release via the
   [routine release](#routine-release) flow.
7. **On the server**, map the new board's asset in the Sync dialog.

> **Heads-up:** if the new board has assets specific to it (custom
> animations, a custom font, etc.), see "Custom assets per board" in
> [`docs/custom-board.md`](custom-board.md) — the
> `DEFAULT_ASSETS_EXTRA_FILES` mechanism in `main/CMakeLists.txt`
> points to a directory whose contents end up inside
> `generated_assets.bin` (the 8 MB SPIFFS partition at `0x800000`).

---

## Merging a new upstream version

Upstream (`78/xiaozhi-esp32:main`) periodically bumps `PROJECT_VER`,
adds boards, fixes drivers, etc. When we want those changes in our
fork:

```bash
# 1. Pull the latest from upstream onto our mirror branch.
git fetch upstream
git switch main
git merge --ff-only upstream/main      # main must stay a pure mirror
git push origin main

# 2. Merge main into dogmabot.
git switch dogmabot
git merge main
# Resolve conflicts (see "Likely conflict zones" below).

# 3. Verify build for every board in the release matrix.
for board in sp-esp32-s3-1.28-box waveshare/esp32-s3-rlcd-4.2 ; do
  echo "== building $board"
  docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.2 \
    bash -c "source \$IDF_PATH/export.sh && \
             python scripts/release.py $board"
done

# 4. Commit + push the merge.
git push origin dogmabot

# 5. Re-release. If upstream bumped PROJECT_VER to e.g. 2.2.7, the new
#    tag is v2.2.7 — see "Bumping PROJECT_VER" below. Otherwise reuse
#    v2.2.6 per the "Re-releasing under the same tag" flow.
```

### Likely conflict zones

| File | Why it conflicts | Resolution |
|---|---|---|
| `CMakeLists.txt` line `set(PROJECT_VER "...")` | Upstream bumps the version. | Take upstream's value if you want to track it; otherwise keep our pin. |
| `main/Kconfig.projbuild` `OTA_URL` default | Upstream uses tenclass.net. | Keep tenclass.net as the global default (devices set the URL via NVS), or change to a placeholder DogmaBot URL if you want fork users to start there. Document the choice in the commit. |
| `main/boards/*/config.json` (touched boards) | Upstream tweaks board config. | Three-way merge usually fine. Re-run `scripts/release.py --list-boards --json` after to confirm the variant graph is intact. |
| `.github/workflows/build.yml` | Upstream changes their matrix logic. | We keep `build.yml` aligned with upstream; our release machinery lives in the separate `release.yml`. Take upstream's version of `build.yml` unless we have a specific deviation. |
| `partitions/v2/*.csv` | Upstream resizes partitions. | Read carefully — a layout change can move the NVS / OTA offsets and silently brick fielded devices. If offsets shift, devices need a full reflash (`flash.ps1` without `-AppOnly`) on next deploy, and an `OTA → full flash` migration plan. |

### Bumping `PROJECT_VER`

If upstream's new version is e.g. `2.2.7` and we want to follow:

```bash
# 1. CMakeLists.txt already has the new value from the merge.
grep PROJECT_VER CMakeLists.txt
# => set(PROJECT_VER "2.2.7")

# 2. Tag with the matching v*.*.*
git tag v2.2.7
git push origin v2.2.7
```

CI publishes a new release at the new tag. Old `v2.2.6` release stays
in the GitHub Releases list for rollback purposes (DogmaBotServer keeps
the older `.bin` until an admin deletes it).

---

## Reverting a bad release

If a release boots OK but has a bug visible only after deploy:

1. Revert the offending commit on `dogmabot` (or branch off and fix
   forward).
2. Re-release per [Re-releasing under the same tag](#re-releasing-under-the-same-tag).
3. On the server, push the new `.bin` (overwrites the old one with
   `overwrite=true` flow if exposed, otherwise delete-and-resync).
4. To force already-deployed devices to downgrade/re-update, the server
   needs to emit `firmware.force: 1` for them on the next OTA cycle —
   either via the admin "Force OTA" action or by changing the version
   string the server sees (it's keyed off the filename).

If a release **boot-loops** the device:

- Connected devices that have the bad version installed need a serial
  reflash with the previous good version (`flash.ps1` with `-Bin
  <path-to-previous-good.zip>`).
- Fix the regression on `dogmabot`, retest in Docker, then re-release.
- Update the team — fielded devices that auto-OTAed to the bad version
  are not recoverable through OTA alone.

---

## Quick reference (cheat sheet)

| Action | Command |
|---|---|
| Sync `main` with upstream | `git fetch upstream && git switch main && git merge --ff-only upstream/main && git push origin main` |
| Merge upstream into our fork | `git switch dogmabot && git merge main` |
| Local smoke build (one board) | `docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.2 bash -c 'source $IDF_PATH/export.sh && python scripts/release.py <board>'` |
| Re-release `v2.2.6` | `git tag -d v2.2.6 && git push origin :refs/tags/v2.2.6 && gh release delete v2.2.6 -y && git tag v2.2.6 && git push origin v2.2.6` |
| Fresh release at new version | `git tag v2.2.7 && git push origin v2.2.7` |
| List boards the release matrix builds | `python scripts/release.py --list-boards --json` |
| Tail CI for the latest run | open `https://github.com/dogmadata/xiaozhi-esp32-dogmabot/actions` |

---

## Pointers

- `.github/workflows/release.yml` — the CI workflow.
- `scripts/release.py` — produces the per-board zip.
- `scripts/dogmabot/README.md` — flash helpers + brief publishing recipe.
- `main/CMakeLists.txt` — per-board cmake branches.
- `main/Kconfig.projbuild` — board type list + OTA URL default.
- DogmaBot server side: `backend/src/DogmaBot.Admin.Api/Firmware/UpstreamSyncService.cs`.
- DogmaBot protocol spec: `DogmaBot/spec/01-ota-http.md` in the server repo.
