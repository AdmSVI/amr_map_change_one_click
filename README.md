# AMR Map Manager

AMR Map Manager is a Qt 6 desktop application for maintaining a manually configured fleet of SEER-compatible AMRs, downloading and previewing 2D maps, and safely uploading and switching maps across multiple robots.

The application is currently at version **0.3.0** and is designed for both Ubuntu and Windows.


## Features

### Robot management

- Add robots manually using a display name and IPv4 address.
- Remove selected robots.
- Persist the robot list and language preference with `QSettings`.
- Refresh the current map and connection state for one or more robots.
- Reject duplicate or invalid IPv4 addresses.

### Map download

- Download the current map from exactly one selected robot.
- Resolve the current map's actual stored filename by matching `current_map_md5`
  against the robot's stored-map list before downloading.
- Fall back to checking stored files individually when a stale list entry makes
  the robot reject a batch MD5 query.
- Save the original response bytes as `<map-name>.smap` in the system Downloads directory.
- Add a timestamp automatically when a file with the same name already exists.
- Calculate and display the local MD5 after download.
- Automatically open the downloaded map in the map preview.

### Batch upload and map switching

- Validate that a selected 2D `.smap` file contains a JSON object before upload.
- Calculate the local map MD5.
- Process up to three robots concurrently.
- Query each robot's navigation state before making changes.
- Skip robots in `WAITING`, `RUNNING`, or `SUSPENDED` state.
- Upload a map without switching it.
- Read each robot's actual stored-map list after upload, query those maps, and
  locate the uploaded map by matching its remote MD5 to the local file.
- Use the robot's actual stored filename for subsequent map switching.
- Optionally switch to the uploaded map after MD5 verification.
- Query `current_map` and `current_map_md5` after switching.
- Continue processing other robots when one robot fails.
- Show per-robot progress, results, and an operation log.

The application does **not** cancel active robot tasks automatically.

### SEER map preview

The viewer follows the map structure documented by SEER's `message_map.proto` reference and reads the Protobuf JSON representation directly with Qt JSON APIs. Both lower-camel-case and snake-case field names are accepted.

Displayed layers include:

- laser/occupancy scan points (`normalPosList`);
- normal map lines;
- landmarks, location marks, action points, park points, and charge points;
- station headings;
- Bezier navigation paths;
- advanced and forbidden lines;
- advanced areas.

Viewer controls:

- **Open map** loads a local `.smap` or JSON file and opens the separate full-screen map window.
- **Map window** reopens the map without reloading it. **Back to main window** hides it;
  Esc leaves full-screen mode.
- The mouse wheel zooms in and out.
- Dragging with the mouse pans the map.
- **Fit map** in the map window restores the full-map view.
- A summary shows the map name, type, version, resolution, scan-point count, station count, path count, and area count.
- Zoom is capped at four times the native tile density (and an absolute 800 px/m)
  to prevent oversized pixmap transforms that can stall the UI.

Large scan maps use a multi-resolution 256 x 256 tile pyramid. Only tiles intersecting
the viewport are generated, and scan points are spatially indexed so a tile never
scans the complete map. The least-recently-used cache is capped at 256 tiles
(approximately 64 MB), while stations, routes, live robot pose, and confidence remain
independent vector overlays.

### Live localization confidence

- Select exactly one robot and open or download its current map.
- Use **Live confidence** to query the robot position twice per second.
- Verify that the robot's loaded map matches the preview before displaying its pose.
- Draw a fixed-size robot marker with heading and numeric confidence on the map.
- Show the current coordinates, localization method, confidence, and update time.
- Color the marker green at 0.8 or above, amber from 0.6 to 0.8, and red below 0.6.
- Keep at most one location request in flight and hide the marker after three consecutive failures.

### Single-robot trajectory sampling

- Record every successful live-position response without spatial or time downsampling.
- Draw the travelled path over the map: green at 80% confidence or above, amber from
  60% to 80%, red below 60%, and gray when confidence is unavailable.
- Keep a sampling table with timestamp, coordinates, heading, confidence, localization
  method, travelled distance, calculated speed, confidence change, and anomaly status.
- Flag confidence below 60%, a confidence drop of 20 percentage points, a position
  jump of at least 0.5 m above 3 m/s, coordinates outside the map, and missing confidence.
- Break the displayed path at position jumps so invalid coordinates do not create a
  misleading line across the map.
- Retain the complete in-memory session while limiting the visible table to the latest
  2,000 rows, and export the complete session to UTF-8 CSV for validation and analysis.

### Localization confidence heatmap

- Open the matching `.smap` map, then import one or more localization CSV exports
  from the same robot and map. CSV reading and visit aggregation run in the background.
- Ignore stationary repeats for scoring. Only valid samples with at least 0.05 m
  of recorded movement contribute to 0.5 m source cells; samples above 3 m/s
  are rejected as position jumps.
- Treat a return to the same cell after 2 m of travel, a 60-second gap, or a new
  CSV file as another visit. A visit contributes one 20th-percentile confidence
  score, regardless of how many responses were recorded there.
- Use the median of visit scores as the cell score. Cells with only one visit are
  shown faintly; areas without sampled movement remain transparent.
- Smooth the display into 0.25 m cells with a 1.0 m maximum radius. Map scan
  points stop smoothing through occupied locations. Toggle between track and
  heatmap views in the full-screen map window. Saturated red (<60%), amber
  (60–80%), and green (>=80%) bands emphasize the difference; repeated visits
  are more opaque than a single visit.
- Map identity is checked by name because the current CSV format does not
  include a map checksum. Confirm that the previewed map is the same revision
  used when the data was collected.

### Languages

- Simplified Chinese
- English

The language can be changed at runtime from the toolbar. The selection is remembered for the next launch. English is the primary documentation language.

## Implemented robot APIs

| Operation | Request | Response | Default service port |
|---|---:|---:|---:|
| Query position and localization confidence | `1004` | `11004` | `19204` |
| Query navigation status | `1020` | `11020` | `19204` |
| Query current/stored map information | `1300` | `11300` | `19204` |
| Query map MD5 | `1302` | `11302` | `19204` |
| Switch loaded map | `2022` | `12022` | `19205` |
| Upload map | `4010` | `14010` | `19207` |
| Download map | `4011` | `14011` | `19207` |

The protocol implementation uses the 16-byte, big-endian header shown in the supplied API examples:

```text
5A 01 | sequence | payload length | command | original request | reserved
```

TCP responses are buffered until the complete payload length declared by the header has arrived. This is required for large map files that span multiple TCP reads.

## Fleet rollout agent

The desktop app and the new headless `amr-map-agent` share one `MapDeployer`.
The agent claims map rollout jobs from Fleet, verifies the downloaded bytes,
deploys to idle robots, and durably reports the results. Start with one robot.
See [docs/AGENT.md](docs/AGENT.md) for configuration, Task Scheduler setup,
recovery, API compatibility, automated checks and pilot acceptance.

## Build on Ubuntu

Ubuntu 22.04 or later:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/amr-map-manager
```

## Build on Windows

Install Qt 6 with the MSVC component, Visual Studio C++ Build Tools, CMake, and Ninja. From a Qt-enabled terminal:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\amr-map-manager.exe
```

## Create release packages

### Ubuntu DEB

Install the packaging dependencies, then run the provided script on Ubuntu 22.04. The package architecture is selected automatically from the build machine (`amd64` or `arm64`):

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev dpkg-dev \
  libgl1-mesa-dev libglx-dev libopengl-dev
bash scripts/package-ubuntu.sh
```

Outputs, depending on the build machine:

```text
dist/ubuntu/amr-map-manager_0.3.0_amd64.deb
dist/ubuntu/amr-map-manager_0.3.0_arm64.deb
```

Install and remove it with:

```bash
sudo apt install ./dist/ubuntu/amr-map-manager_0.3.0_amd64.deb
# On a 64-bit Raspberry Pi or other ARM64 Ubuntu/Debian system:
sudo apt install ./dist/ubuntu/amr-map-manager_0.3.0_arm64.deb
sudo apt remove amr-map-manager
```

### Windows installer

Run the packaging script from a regular PowerShell terminal after installing Visual Studio 2022 Build Tools, Qt 6 MSVC x64, CMake, and Inno Setup 6. Pass the Microsoft Visual C++ x64 Redistributable downloaded from Microsoft:

```powershell
.\scripts\package-windows.ps1 `
  -QtRoot "C:\Qt\6.8.3\msvc2022_64" `
  -VCRedistPath "C:\Installers\VC_redist.x64.exe" `
  -Version "0.3.0"
```

When Qt is installed under `C:\Qt` and the Visual C++ Redistributable is
available in the Visual Studio installation, both paths are detected
automatically:

```powershell
.\scripts\package-windows.ps1 -Version "0.3.0"
```

Outputs:

```text
dist/windows/AMRMapManager-0.3.0-win64-setup.exe
dist/windows/AMRMapManager-0.3.0-win64-portable.zip
```

The installer deploys the required Microsoft Visual C++ Runtime. The portable
archive requires that runtime to already be installed on the target computer.

### GitHub Actions

The `Build release packages` workflow builds Windows x64, Ubuntu amd64, and
Ubuntu arm64 packages. A manual run keeps the packages as workflow artifacts.
When a `v*` version tag is pushed, the workflow also creates or updates the
matching GitHub Release, generates release notes, and uploads all installers,
portable archives, DEB packages, and platform-specific SHA-256 files:

```bash
git tag v0.3.0
git push origin v0.3.0
```

## Basic workflow

1. Add the robot name and IPv4 address.
2. Select the robot and click **Refresh status**.
3. Verify that the current map and MD5 are displayed.
4. Use **Download current map** to save and preview the robot's current map.
5. Select a local map and use **Upload only** for the first test.
6. Confirm that remote MD5 verification succeeds.
7. Use **Upload, verify and switch** only after validating the workflow on one test robot.

## Project structure

```text
src/
  core/       Robot configuration model
  map/        SEER SMAP parser and graphics viewer
  network/    Asynchronous TCP request handling
  protocol/   RBK/SEER packet encoder and header decoder
  MainWindow  Robot table, workflows, logging, and language switching
```

## Safety and current limitations

- Verify the configured service ports and protocol header against one test robot before production use.
- Confirm that robots are stationary and in a safe operating state before uploading or switching maps.
- A successful `ret_code` is not treated as final success; upload workflows also verify MD5 and, when switching, the current map.
- Busy robots are skipped. Automatic task cancellation is intentionally disabled.
- Only 2D JSON `.smap` upload is currently exposed in the UI.
- 3D ZIP maps are not yet supported.
- The map viewer is read-only and does not edit or export map geometry.
- The application has not yet completed production validation across all robot firmware versions.

---

## 中文简介

AMR 地图批量管理器是一款基于 Qt 6 的桌面软件，目前支持：

- 手动添加和保存 AMR；
- 刷新机器人当前地图及 MD5；
- 直接下载当前地图到系统下载目录；
- 打开并预览 SEER 2D `.smap` 地图；
- 显示扫描点、站点、方向、贝塞尔路径、禁行线和区域；
- 批量上传地图并校验远端 MD5；
- 在校验成功后选择性切换地图；
- 切图前检查导航状态并跳过繁忙机器人；
- 中文与 English 运行时切换。

首次使用时请先在单台测试机器人上验证端口、协议和地图切换流程，再用于生产机器人。
