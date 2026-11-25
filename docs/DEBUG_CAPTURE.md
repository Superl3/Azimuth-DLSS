# Azimuth SR + FG debug capture

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

## Automatic metadata test

From the extracted debug artifact, with Azimuth closed:

```powershell
.\Start-DebugCapture.ps1 -GameDir 'D:\Azimuth' -Label FH2-auto -Output 3840x1600 `
    -RenderSetting native -InstallProxy -AutoResolve 2000 -ExitAfterCapture
```

Enter the desired race or free-roam scene before the requested threshold.
The first poll at or after the specified resolve count starts one capture
using the same bounded and prioritized metadata path as F8. A manual F8
capture started earlier consumes that run's automatic window. The count
includes resolved scenes before gameplay, so choose a threshold from an
earlier log or increase it for slower loading.

`-ExitAfterCapture` requests a normal window close after the capture ends
or after `-WaitSeconds` (default 180) and never force-kills the game. Omit
it to exit manually. A missing, interrupted, or timed-out automatic
window fails the command after packaging available evidence. The ZIP
records the requested threshold and observed automatic result. The
launcher restores the original proxy after the process exits.

For a custom launcher, set `AZIMUTH_SR_MODE=debug` and
`AZIMUTH_DEBUG_AUTO_RESOLVE` to an integer from 1 through 1000000.
A value of 0 leaves the F8-only behavior. The automatic window collects
metadata. Use a separate native RenderDoc or PIX session if instrumenting
both proxies at once fails at startup.

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

## Targeted shader disassembly after successful probe

Use `Start-DebugCapture.cmd -ShaderDisasm` with the 1080p setting, press F8 once in gameplay, then exit. This explicitly opts in to local disassembly of the three known postprocess PS/VS pairs and recognized scene-resolve/MV roles at pipeline creation. The ZIP includes at most 16 assembly text files (1 MiB each); review and share privately. Do not commit game shader dumps publicly. No raw shader binaries or game assets are bundled. The sidecar setting `ShaderDisasm=1` enables local export on manual launches; use `-ShaderDisasm -CollectOnly` to package those files. D3DDisassemble may fail on unsupported bytecode. Actual GPU textures, constant values and execution/presentation remain unverified. Probe now logs up to 12 explicit NGX API returns; legacy resolve `evaluated=0` only means no route output pointer.

The third capture identified a 16:9 final variant and a probable pre-overlay boundary. The analyzer now reports `fh2_postfx` same-list/same-resolve candidate chains and intervening same-target blends, without classifying HUD or enabling routing. It also recognizes the 16:9 final PS for optional disassembly. See [FH2 capture findings](FH2_CAPTURE_FINDINGS.md) for the postprocess coordinate-domain requirements. The next useful evidence is an actual GPU capture at the intended output aspect ratio, not more broad metadata-only runs. Descriptor occupancy now appears in `DEBUG DESCRIPTOR_STATS`.

The `hdr_scene_copy` report distinguishes final HDR-PQ and SDR-curve shader candidates, measured downscale/upscale direction, and observed pre-overlay copy to HDR t2 (same list/resolve and before recorded blends). It never asserts actual UI pixels or enables FG. Existing raw logs can be re-analyzed with the updated Python scripts; no new game run or DLL installation is needed for that analysis.


## Unified SR preview (experimental)

Use the full azimuth-sr-experimental-windows-x64 artifact, not the
standalone debug artifact. Keep your NVIDIA nvngx_dlss.dll in the game
folder. The launcher can temporarily install and restore the matching
proxy and NGX bridge. Set the game to the measured 2592x1080 render
size and a 3840x1600 requested output. Do not assume the setting label
matches actual dimensions.

    Start-DebugCapture.cmd -Preview -PreviewPostFx -ShaderDisasm -Output 3840x1600

Drive, press F8 once, and exit after PREVIEW COMPLETE. Optional
-AutoResolve 2600 -ExitAfterCapture waits for both metadata and preview
completion; the trigger counts recognized resolves, not gameplay time.
Preview is the existing offscreen probe plus debug metadata. No jitter
or visible SR replacement. Two first resolve intervals of the first
capture window are selected, with up to four core textures (scene
R11G11B10, depth, reconstructed R16G16 motion, NGX output) and three
optional same-list postprocess targets (preoverlay, composite, final).
The latter are skipped unless exact PS/VS, t0, live RTV identity,
viewport, single-subresource state and same recording are observed.
They are native-path color, NOT postprocessed SR, and preoverlay is
not verified HUD-less. Original game output remains unchanged by design.

Copies are recorded on the original producer list; only actual
observed submission followed by completion of a private queue fence
allows mapping. No guessed queue or frame-count retirement, no
blocking CPU fence wait. At most 2 groups, 14 textures, 256 MiB allocated;
references retained until process exit. Reset before submission,
repeat submission, failure, missing queue hook, unsupported format or
timeout produce partial evidence rather than guessed pixels. Use a
separate run through RenderDoc/PIX for actual constants, active
swapchain and draw semantics; if RenderDoc is loaded private readback
is disabled to avoid hooking conflicts. Preview is not a benchmark.

Raw files are tightly packed little-endian 32-bit pixels with size
and DXGI resource format in the filename. They contain game imagery;
review ZIP locally before sharing privately. No capture content is
automatically uploaded. Metadata, logs and optional shader assembly
are included even if GPU texture capture partially fails.


### Temporal test after first successful actual preview

The 720p SDR preview saved all eight core raw textures in resolves
2601 and 2602, both fences completed. However the scene was nearly
stationary (identical MV bytes, maximum motion ~0.0015 render pixels).
For the next test use the same low resolution, while *actually driving*
or rotating the chase camera. Prefer manual F8 in motion rather than
a resolve-number trigger while parked:

    Start-DebugCapture.cmd -Preview -PreviewPostFx -PreviewJitter -ShaderDisasm -Output 1920x1080

-PreviewJitter is explicitly opt-in. It uses the existing DLAA viewport
jitter mechanism only after a successful probe has armed the exact render
size. The original displayed scene may jitter, but SR output is still
offscreen. Jitter used and actual offsets are logged per captured resolve.
This is a comparison experiment, not a validated projection/motion-vector
contract. With no flag the baseline remains unjittered.

All three native postprocess snapshots were missing in the first preview.
New PREVIEW POSTFX_GATE records report missing bits: 0x01 global metadata,
0x02 draw/topology/query, 0x04 root/PSO, 0x08 target/view, 0x10 exact
recording state, 0x20 viewport/scissor, 0x40 t0 binding. A later stage
may save even if the earlier one failed to save, when its identity chain
and its own state are verified. These are original-game snapshots, not
SR-replayed postprocess results. No state gate was relaxed.

### Separate recording batches (v0.4 foundation)

The moving Jitter ON capture showed the same command-list pointer being submitted for the core SR work, then reused before native postprocess draws. A logical resolve group is now separate from up to four independent recording batches. Each batch holds its own fence and resource references. Mapping requires a successful reset of that recording and fence completion; a never-reset list may time out rather than return ambiguous pixels. No assumption of identical queues or Present frame identity is made. Native postprocess readback still requires the existing explicit render-target barrier/view checks. PREVIEW BATCH, SUBMITTED, SAVED and GROUP_RESULT give batch/recording and per-stage masks. Capture size remains 14 textures and 256 MiB maximum; no screen modification. Repeated submissions are rejected before mapping. The previous version's postfx gate never ran after core submission; now later recordings can independently reach it.

### Native high-resolution postprocess replay baseline

    Start-DebugCapture.cmd -Preview -PreviewPostFx -PreviewReplay -PreviewJitter -ShaderDisasm -Output 1920x1080

Only 1280x720 actual render / 1920x1080 output is supported by this first
replay experiment. The original FH2 postprocess shader reads low-resolution
auxiliaries using two integer conversions from SV_POSITION. The runtime
creates a variant of the exact observed PS/VS pair, verifies the original
DXBC retail checksum, checks that exactly two matching instructions are
present, and changes only these position conversions to use 2/3 coordinates.
The rest of the original shader, root signature, bindings, normalized UVs,
constant buffers, samplers, native HDR input and original color transform
are reused. The extra draw targets its own 1920x1080 texture and restores
the game's PSO, RTV, viewport and scissor. All new resources are bounded
and retained; the raw replay texture uses its recording batch fence.

This is the native-input high-resolution postprocess baseline, NOT yet
a replay with the SR output bound at t0 and NOT visible SR. It deliberately
does not swap descriptor heaps or overwrite game descriptors. The next
step is to reproduce the exact native root bindings in private descriptors
and route the SR output to t0 with proven producer/consumer lifetime.
Do not silently substitute a simplified tone mapper or low-res image
sharpening for this step. Captures may include f*-replay-1920x1080-fmt24.raw.
Check POSTFX REPLAY PSO ready, RECORDED, SKIP and PREVIEW GROUP_RESULT.
No game bytecode is stored in source control. Token/checksum references:
https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/D3D12TokenizedProgramFormat.hpp
and https://github.com/microsoft/hlsl-specs/blob/main/proposals/infra/INF-0004-validator-hashing.md .
Tests cover a synthetic container and a real Windows D3DCompile/D3DDisassemble
fixture. Real FH2 variant creation and replay pixels still require runtime
verification. Up to 16 textures, still bounded by 256 MiB of readbacks.


### SR input binding experiment

    Start-DebugCapture.cmd -Preview -PreviewPostFx -PreviewReplay -PreviewSRReplay -PreviewJitter -ShaderDisasm -Output 1920x1080

The additional opt-in mode keeps CPU-only copies of created SRV/CBV/UAV
descriptors and propagates them through descriptor copies. The exact
postprocess PS/VS pair's declared shader registers are resolved from the
original root signature. A private shader-visible heap is populated
from the CPU mirror; the pixel shader's scene t0 is replaced by an
immutable GPU-local SR snapshot, while the vertex shader's different
t0 and every other required binding stay native. Tier 3 and distinct
PS/VS t0 bindings are required. Unknown or ambiguous roots, missing
descriptor shadows, and an unfinished producer fence skip the replay.
The game descriptors are never modified. Original heaps and graphics
and compute table bindings are restored. Direct root args are not
changed. At most two SR snapshots and four replay surfaces are retained.
No GPU queue waits are inserted; producer must have completed (or be
earlier in the same physical recording). This is an experimental
offscreen result, not visible SR or proven postprocess quality. It may
save f*-srpostfx-1920x1080-fmt24.raw along with native replay and
original stages. See POSTFX SR_LAYOUT, SR_BINDINGS and SR_REPLAY SKIP.
Up to 18 textures under the existing 256 MiB readback cap. No FG,
UI recomposition, actual camera constant values, or present takeover.

The SR replay additionally rejects every alias of PS t0, requires all original graphics/compute descriptor table bindings to be accounted for before switching heaps, and checks that a separate SR producer recording has been reset and its fence completed. A producer on the same still-recording list may be used in order. These strict gates can legitimately skip the experiment; the recorded reason is evidence to improve the next implementation, not permission to bypass GPU synchronization. The native replay remains independent of descriptor mirroring.


### SR replay root-layout diagnostics

The former combined reason=root_layout has been split into three ordered
replay gates (all include the actual Resolve and graphics root pointer):
root_layout_unregistered, root_layout_validation_failed, trace_incomplete.
No acceptance predicate, heap limit, descriptor restoration requirement
or GPU synchronization condition was relaxed. SR_LAYOUT_GATE reports
registration/count/trace state. SR_LAYOUT_REJECT identifies the exact
failed validation check, first failing requirement (stage/type/register,
mode/root/offset), missing requirement mask and the two t0 bindings.
Registration failures are emitted at root creation with resolve=0
(not an actual Resolve), phase=registration and the root pointer; correlate
with the pointer in the later per-Resolve skip. Deserializer failures,
unsupported/invalid layouts and a full registry are logged too. The
replay path still registers only validated layouts. If several conditions
are false, the first failure follows the original short-circuit ordering.
The debug summary already includes POSTFX SR_ lines.


### Multi-plan SR replay preflight (one gameplay test)

The 812079b capture already demonstrated native replay and all six native
postprocess snapshots, with two complete core groups. Its actual shader
assembly declares the same 20 PS/VS bindings as the replay policy. No
need to run the diagnosis-only build before trying fallback plans.

The same -PreviewSRReplay invocation now evaluates A_compact (original
32K packed policy), B_physical (rebase each required table to its original
physical index in a new same-capacity resource heap), and C_extended
(disjoint packing with a bounded one-million-descriptor maximum).
The root's parsed requirements and bounded full schema are stored as
private data on the live root-signature COM object, including rejected
layouts, independent of the fixed approved-layout cache. The object
data is revalidated at replay, never treated as approval. The physical
plan preserves original heap size, verifies all required slots, rejects
physical aliases to PS t0 across tables, and does not use an unknown
range/registration as permission to replay. Extended packing can
separate physical aliases between different tables. All paths reject
logical aliases, missing required registers, incomplete global tracking,
unproven state restoration and producer ordering. For B/C the original
PS t0 CPU shadow is unnecessary only when its exact native scene
identity, simple view and format are verified: the replacement descriptor
is created anew in the private heap. Other required descriptors still
need exact shadow copies. Neither game descriptor writes nor GPU waits
nor independent root-signature switching are introduced.

SR_PREFLIGHT includes independent producer, original draw, heap, global
trace and graphics/compute restoration checks. SR_SCHEMA_ROW describes
original parameter/range/register/space/offset information, with bounded
truncation status. SR_REQ includes every required binding and its
availability, including native root arguments preserved but not read
back. SR_PLAN records each plan's independent layout and descriptor
checks even if no replay may be recorded. The first feasible plan is
allocated; allocation failure advances to the next plan before recording
any draw. SR_BINDINGS path=1/2/3 identifies the selected implementation.
If none is feasible, native rendering and native replay remain intact.
This is still offscreen SR postprocess, not visible output or FG. An
independent root signature/PSO is not implemented; it requires complete
direct root-argument and state restoration tracking. The existing
launcher and ZIP format are unchanged.
