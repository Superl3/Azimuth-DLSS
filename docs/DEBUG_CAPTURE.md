# Azimuth SR + FG debug capture (F8)

**Debugging only, not working DLSS SR or FG.** The debug proxy selects the
existing trace-only path: no NGX DLL or DLSS runtime required, no injected
motion-vector pass, no generated frames, no Present hook. It still installs
experimental D3D12 metadata hooks. Keep your original proxy backed up.

## Quick start: metadata (no SDK, Python optional)

1. Open this branch's GitHub Actions run and download
   **azimuth-debug-windows-x64** only after that artifact was uploaded.
   Extract it into an ordinary writable folder.
2. Close Azimuth. Run Start-DebugCapture.cmd from the extracted artifact.
   Enter the folder containing azimuth.exe. Accept temporary installation
   of the matching debug dxgi.dll if prompted; the script backs up and
   restores the previous proxy after exit if its identity is unchanged.
   It does not change the game's AA, render settings, saves or NGX DLL.
   PowerShell execution policy may prevent script launch; review the script
   and use your normal approved local script execution method.
3. Enter an actual race/free-roam scene, drive and press **F8** once.
   Default capture spans eight recognized scene-resolve intervals or
   eight seconds, whichever ends first. F8 again ends an active capture;
   after it ends, F8 starts another with fresh budgets. A resolve sequence
   is not a Present frame ID. No F11 toggling in debug mode.
4. Exit normally. The launcher writes a ZIP with azimuth_dlaa.log,
   capture.json and optional summary.json if Python is available.
   It retains previous logs and backups. No files are uploaded.
5. Repeat with native render settings and with the lowest available
   internal rendering setting, keeping the output/window and scene fixed
   if possible. Repeat HUD-on/HUD-off if supported. If the launcher
   only changes the whole output size, record that rather than assuming
   independent render scaling.

Alternatively, install the debug dxgi.dll yourself and launch with
AZIMUTH_SR_MODE=debug, or copy the explicit azimuth_debug.ini sidecar
next to azimuth.exe. An environment mode overrides the sidecar.
Remove/disable the sidecar for normal DLAA operation. With Python:

    py -3 tools/capture_sr.py --game-dir "D:\Azimuth" --label FH2-native --render-setting native --output 3840x1600 --mode debug

## Actual GPU data: RenderDoc (or PIX)

The metadata ZIP cannot prove texture contents, camera matrices, actual
GPU execution order, or which descriptor a shader really samples.

1. Install RenderDoc from https://renderdoc.org/ . Close Azimuth.
   Back up dxgi.dll and install the debug proxy. Copy azimuth_debug.ini
   beside azimuth.exe (Enabled=1, RenderDoc=1, QueueTrace=0). Launch
   azimuth.exe via RenderDoc's Launch Application with the correct working
   directory; verify its capture overlay is active. Do not use competing
   capture tools/overlays simultaneously.
2. Drive in a stable scene and press F8. The mod only queries
   RENDERDOC_GetAPI on an already-loaded renderdoc.dll. It requests the next
   two presented GPU frames of RenderDoc's active window through the
   official TriggerMultiFrameCapture API. It never loads/injects RenderDoc,
   changes the active swapchain, or claims the request succeeded until a
   capture count is observed. Save actual captures in RenderDoc UI.
3. Save the .rdc files locally. If the API is unavailable or no capture
   appears, use RenderDoc's own capture hotkey or PIX instead. If the debug
   proxy conflicts with the capture tool, restore the original DLL and
   capture native rendering separately. Never force an incompatible hook.
4. After closing the game, collect the debug log without relaunch:

       powershell -NoProfile -File Start-DebugCapture.ps1 -GameDir "D:\Azimuth" -CollectOnly

   Keep the raw .rdc/.wpix captures separate from the ZIP. They may contain
   copyrighted game assets and other captured data; review/share privately,
   do not commit them to GitHub. If full captures are impractical, export
   selected resource previews, pipeline-state bindings and shader
   disassembly/constant values from the capture tool.
5. PIX instructions: https://devblogs.microsoft.com/pix/taking-a-capture/
   Use its GPU capture, with the debug metadata captured separately if
   required. RenderDoc integration uses an unmodified MIT application API
   header; no vendor capture executable or SDK runtime is bundled.

## Data captured and how to use it

| Evidence | SR | FG |
| --- | --- | --- |
| Geometry draw and resolve allocation/viewport | Identify real internal scaling | Verify depth/MV dimensions |
| Known resolve/MV roles, hashes, bound SRVs | Select input resources | Verify temporal input provenance |
| Output-size and fullscreen intermediate draw list | Locate upscale/tonemap consumers | Find pre-HUD color boundary |
| Texture copy/resolve edges | Find resource chain | Track final composition |
| Compute dispatch/group counts and available CS hash | Find possible compute upscale path | Find compute/postprocess passes |
| Queue creation and optional submission/fence API metadata | Validate ordering candidates | Determine present queue/lifetimes |
| RenderDoc/PIX texture contents, shader bindings and constants | Verify depth, jitter, exposure, camera | Verify camera/reprojection, HUD-less/UI, exact frames |

F8 captures metadata of recognized old-style D3D12 interfaces only. Root
signature layouts, PSOs and descriptors can be incomplete; graphics SRVs
are bound candidates, not shader sampling evidence. Compute SRV/UAV
binding and all modern API paths are not fully tracked. QueueTrace=1 is
optional in the sidecar (standalone metadata run only, off by default);
it records existing ExecuteCommandLists/Signal/Wait calls without inserting
fences. CPU submission call order does not prove GPU execution or
completion. Swapchain snapshots are creation-time only, not refreshed on
resize. Restart capture after changing output mode. Actual resource data,
CB contents, queue execution and camera values must come from GPU capture;
do not fake identity matrices, near/far planes or motion vectors.

For each native and reduced-resolution run, collect a short straight-drive
window plus a steering/camera-motion window. If convenient, also capture
HUD-off and pause/photo mode as distinct labels; avoid menus as the first
test. The JSON groups windows and warns about missing resolves, missing
MV passes, truncated budgets and incomplete captures. No analyzer enables
routing or FG.

## Relevant non-native approaches

- [FH2-FSR4](https://github.com/aranhoso/FH2-FSR4): replays the native motion
  blur reprojection at full resolution and captures pre-tonemap HDR;
  independently animated wheels/foliage and approximate camera values
  are known limitations. We retain exact native pass identities but need
  real GPU evidence before using these vectors for FG.
- [Luma Quantum Break](https://github.com/Filoppi/Luma-Framework/blob/main/Source/Games/Quantum%20Break/UpscalingNotes.md):
  validates live native camera/jitter, same-frame textures, bound shader
  and state restoration. Our debug captures must establish the equivalent
  facts; resolve counters alone are insufficient.
- [Luma FFXV resource tracking](https://github.com/Filoppi/Luma-Framework/blob/main/Source/Games/Final%20Fantasy%20XV/includes/upscale_tracking.hpp):
  output-sized texture/view mirrors and per-frame links. The intermediate
  draw and copy chain is needed to decide where similar mirroring belongs.
- [OptiFG](https://github.com/optiscaler/OptiScaler/wiki/OptiFG): frame
  generation from existing upscaler inputs with HUD handling. FH2 has no
  native SR input contract, so we must create a validated frame contract.
- [NVIDIA Streamline DLSS-G](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md):
  exact frame token, depth, dense motion, HUD-less final color, camera
  constants, resource lifetime and swapchain pacing are required to
  implement an appropriate FG backend.

No actual FH2 GPU capture, generated frame, or performance result is
included until the user runs these steps and the evidence is inspected.

## Follow-up after first actual FH2 captures

See [FH2 capture findings](FH2_CAPTURE_FINDINGS.md). The next debug build prioritizes three observed fullscreen PS/VS pairs with a separate quota, omits most unrelated depth-using geometry, keeps partial role SRVs marked unverified, and observes up to 32 direct queues. Multiple direct queues still disallow the visible SR route. The native texture/scene identity recorded for debugging is not a frame token. Repeat one low-resolution capture, then use RenderDoc or PIX to inspect the three passes and camera/MV resources. The offscreen `probe` mode separately permits a covering scissor; it does not change displayed output or establish GPU synchronization.
