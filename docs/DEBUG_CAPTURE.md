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

When semantic validation of the full-layout parser fails, the physical plan additionally attempts independent schema projection of all 20 actual shader requirements. It is permitted only with a complete recorded original schema (no truncation), supported parameter count and no direct indexing flags; each required register must resolve uniquely. The projection cannot be used for packed layouts, which require full spans. SR_SCHEMA_PROJECT and layoutSource=schema_projection identify this alternative. Missing, ambiguous or physically aliased requirements still reject execution. This does not create a different root signature.


### 64-DWORD root layout and observed-state restoration

The 1fb4437 moving capture of resolves 3668/3669 stored 16/18 RAW
textures. Both original replay stages and the immutable SR producer
completed. The root signature reports 40 parameters but the old parser
iterated only the first 32, incorrectly showing 37 schema rows as
untruncated. PS b1 was absent from that partial schema, not proven to
be a missing descriptor shadow. Existing PS t0 and VS t0 were root
17 and 1; both resource table shadows, both samplers, and other
required bindings passed their old checks. Graphics bound mask 0x60002
and declared mask 0x177777; compute bound 0xe and declared 0xf.
No unknown high root index is hardcoded.

All replay and trace tables now support the D3D12 64-DWORD root budget
with 64-bit masks. Full schema enumeration and truncation are explicit.
The existing pure-route complete flag retains its conservative
32-parameter restriction; replay uses the full table-layout metadata.
The SR replay keeps the original root signature, so a direct CBV is
not a descriptor table. Graphics root CBV writes and constant writes
are observed; b13 uses cb13[0].x in the exact PS/VS. Both must be
observed before replay. No constant value or GPU VA is synthesized.

The restore gate now checks that the recording was observed from
successful Reset/Create, that every actually bound graphics/compute
table is declared and its handle is inside the correct current
shader-visible heap, and that no untracked binding invalidation occurred.
A declared but never-bound table is undefined; it is not necessary
or valid to invent a binding for unused shader stages. Restore all
actually observed tables, not merely the replay-required subset.
Sampler heap metadata is tracked, and the original sampler heap is
kept. ClearState and bundles mark the epoch uncertain; indirect calls
invalidate direct argument evidence. NGX may alter direct arguments;
fresh native binding observations are required after injected work.
POSTFX SR_RESTORE and SR_ROOT_ARG expose these gates and all 64 bits.
All other replay checks (original shader pair, exact required views,
producer fence, snapshot, no global trace overflow) remain.

Private GPU descriptor heap creation now calls the original unhooked
CreateDescriptorHeap while holding the metadata lock: the hooked
entry would acquire the same lock and deadlock once a plan passed.
Host regression tests use the observed low root positions and synthetic
high indices, including index 63, missing epochs, invalid heap handles,
and the actual partial binding masks. This is not proof of runtime
success or a visible SR path. The same launcher command applies.


### Visible SR SDR alpha (bounded trial, not confirmed Present)

    Start-DebugCapture.cmd -Preview -PreviewPostFx -PreviewReplay -PreviewSRReplay -PreviewJitter -VisibleSR -ShaderDisasm -Output 1920x1080

After F8, at most ten seconds of gated output trials; do not use
-ExitAfterCapture. Render 1280x720, output 1920x1080, SDR, Quality.
The a2a200a capture proves native/SR postprocess pixels and the 34 native
blend draws. Many HUD draws bind depth/stencil, and intermediate passes
read the scene. Blindly enlarging their RTV/viewport is not safe.
The first alpha instead retains the already blended native pixels for
all four low-resolution taps with any preoverlay/composite difference;
outside those regions it loads the original full-resolution srpostfx
pixel. This preserves native HUD position and composite colors, but
retains the low-res background underneath the HUD and is NOT exact
alpha extraction or a native 1080p HUD replay.

The original 1080p SDR final shader (a93721c4 + 79d0ca68) is run again
on the private merged texture using its actual native t1 curve and s0
sampler. This applies the original sRGB decode/BT.709-like transfer/LUT
once to the replacement pixels. All required bindings and original
graphics/compute table restoration must pass before modifying the live
final RTV. Native rendering runs first and is untouched on a skipped
path. No HDR, no FG, no Present hook, no game descriptor writes.
The original final target is a candidate, not proven to be the actual
swapchain buffer (creation snapshots did not match in 671/672).

Only first two resolve groups have raw readbacks, up to 22 files under
256 MiB. visiblehud is the pre-SDR merged image; visiblefinal is the
replacement recorded into the game's original final RTV, not proof
of presentation. The legacy final RAW is captured before replacement.
Four rolling live slots own all extra surfaces and heaps. Recycle only
after every associated recording was successfully reset and its
private fence completed; failed/repeated submissions quarantine the
slot until exit. No queue waits. Once the trial window ends, native
output continues. Current offscreen quality and native HUD pixels
remain the fallback. The Windows shader compiler fixture compiles
the new merge PS; real PSO and final target routing need GPU testing.


The trial also logs VISIBLE HUD_PSO (full blend and depth/stencil state),
HUD_DRAW and bounded HUD_BIND rows, with locally disassembled observed
HUD and auxiliary shaders (max 40 files). This evidence is for a future
exact high-resolution HUD replay, not authorization to replay arbitrary
UI draws. Require the final native draw to be immediately adjacent to
the captured copy in the same physical recording; intervening tracked
native draws, copies, dispatches and writes invalidate the merge.
A live group is not recyclable merely because its producer fence
completed: it must have seen the final pass or a later resolve, and
all recordings must be retired and fence-complete.

VISIBLE FINAL_SUBMITTED and FINAL_GPU_COMPLETE distinguish recording,
actual queue submission and fence completion. VISIBLE EDGE records
candidate downstream copies and bound-only draws. FINAL_CANDIDATE
records the native SDR/HDR final target even if Visible SR fails, so
the next run can still establish downstream lineage.

### Startup regression fix (2ea6b1f -> reference-free observation)

The 2026-11-25 startup capture showed a 1920x1080 launcher chain
retained by the experimental observer, followed by CreateSwapChainForHwnd
E_ACCESSDENIED (0x80070005) and Azimuth exception 0xE0424833 at +0x4c619.
No F8 was reached. Same HWND was not logged in that build, so the exact
cause is not proven. DXGI permits only one flip-model swapchain per HWND.
The observer now retains **zero** swapchain interfaces at all times,
never polls stale pointers, and does not hook Present/Present1.
It reuses the existing FG creation snapshot (temporary GetBuffer and
queue references released synchronously). SWAPCHAIN_REQUEST and
SWAPCHAIN_RESULT record both successful and failed calls, HWND, queue,
dimensions, format, flags, QPC, and HRESULT. CHAIN/BUFFER log value-only
identity with retainedForTrial=0; a successful replacement on the same
HWND invalidates the prior observation. A new create attempt during the
trial disables further visible replacement. Do not resize/recreate
chains during this experimental capture (creation snapshots may become
stale on resize; completed RAW readbacks release their source and buffer
references after successful reset and fence completion, while ambiguous
or failed GPU batches remain quarantined until exit).

PRESENT_BARRIER and PRESENT_BARRIER_SUBMITTED record transitions to the
aliased PRESENT/COMMON state, their command-list recording, matching
creation-snapshot buffer identity and submission queue. These are NOT
actual Present calls or scan-out proof. Match with external PresentMon
ETW (QPC and swapchain address) and actual game video; no added graphics
hook is required. With an existing official PresentMon console binary:

    PresentMon.exe --process_name azimuth.exe --qpc_time --output_file C:\Temp\fh2-present.csv --no_console_stats --terminate_on_proc_exit

Start it before the game and stop it after exit if needed. Optionally
pass -PresentMonCsv C:\Temp\fh2-present.csv to the launcher to package
an existing CSV at collection time. No download, elevation, or automatic
start of an external executable. ETW can fail due to permissions or
missing events; an absent CSV never blocks the render trial.

The failed startup log also recorded HDR on. Explicitly disable HDR in
the Azimuth FH2 configuration before testing; do not infer SDR from
the 10-bit swapchain format. An observed HDR final shader produces
reason=hdr_unsupported and keeps native output. Existing offscreen
SR evidence, HUD diagnostics and first two groups (22 RAW maximum)
remain unchanged. QPC frequency is logged for external correlation.

Before F8 only the factory creation log and PSO diagnostics are active for the visible observer; draw/copy/barrier/submit lineage starts at the trial marker. summary.json includes visible_trial with request/result pairs, observed prior success on the same HWND, per-resolve stages and skip reasons. It never marks Present as verified from an in-process log alone. The optional CSV packaging is covered by a launcher fixture; value-only identity replacement and absence of DXGI interface calls are covered by CI regression tests.


### fdc89e9 follow-up: one-shot readiness and work-tail diagnostics

The SDR sample 2762 recorded composite work 184 and final work 186 in
list 000001CDF1495520 recording 44756, then submitted on
000001CDE7C64870. Work 185 is not identified by the old log; do not
assume it is harmless. The adjacency policy is unchanged. The
VISIBLE WORK_WATCH/WORK/WORK_BIND/WORK_BARRIER/WORK_SUBMIT records
the previously counted methods and their identities, arguments,
bound-only graphics resources, native barriers and actual queue order.
The watch is bounded to 64 records per recording and reports truncation;
unsupported API or shader accesses are not proven absent. Composite
failure reports expected/actual recording identities. Creation-time
game chain in the sample is 1280x720, not the 1920x1080 final RTV.
Lineage draw diagnostics now inspect any texture size, and report
creation-snapshot backbuffer writers; no bound-only edge authorizes
routing or proves Present. No swapchain ref is retained.

In preview mode F8 before a recent streak of three fresh successful SR
resolves (without reset) is ignored rather than consuming the one-shot
capture. This cannot prove gameplay; press F8 while actually driving.
After the first accepted start, all further F8 presses are ignored
until process exit. The 8-resolve RAW window and 10-second visible
trial are independent. TRIAL_END logs candidate/routed/submitted counts
and elapsed/chain-change reason. External PresentMon and screen video
remain necessary to verify actual display.


### Wide replay and PQ contract diagnosis

    Start-DebugCapture.cmd -Preview -PreviewPostFx -PreviewReplay -PreviewSRReplay -PreviewJitter -ShaderDisasm -Output 3840x1600

Keep game HDR enabled for this diagnostic. No -VisibleSR for this profile.
1280x720 -> 1920x1080 keeps its 2/3 position remap. Wide profile is
exactly 2592x1080 -> 3840x1600, with 27/40 = 0.675 in both axes.
The exact original postprocess shader is patched at its same two
SV_POSITION conversions, preserving original normalized UVs and native
auxiliary inputs. This is an offscreen experiment; wide resolution
does not itself establish HDR. Preserve the native final shader.
One wide RAW group (up to 10 files including hdrscene) stays within
the 256 MiB readback cap. SDR remains two groups and up to 22 files.

The exact HDR final pair 577d52ef / 79d0ca68 reads t0 composite,
t1 1D curve, t2 preoverlay and cb0 peak/paper/black, using s0. Its
disassembly applies sRGB decoding and curve conversion, suppresses
scene HDR boost where composite differs from scene, converts to BT.2020
primaries and applies ST 2084 PQ. Neither native postfx nor srpostfx
is a PQ buffer. POSTFX HDR_REQ and HDR_CONTRACT report each input
binding, resource dimensions, exact read-state observations, root
CBV/constant presence and sampler binding. Actual Levels values and
sampler values are not read or inferred. HDR_COPY logs exact source,
destination, region and recording when the preoverlay is copied before
composite. HDR_FINAL_GATE correlates that copy with final t2. A safe
same-recording, known read state and full-copy candidate can produce
hdrscene RAW; it is the actual t2 pixels at the observed final draw,
not a proof of alpha extraction. Unknown input state, copy lineage,
or output DXGI color space does not allow replacement. HDR output
remains native regardless of these diagnostic results.


### Independent SDR final-pixel probe on a nonadjacent tail

If the original composite and final are in the same recording but a
counted intervening work item blocks native replacement, the selected
RAW groups may still replay the merge and original SDR final shader into
private RTVs, using the already-owned private layers and current proven
native final binding/restore conditions. This produces hudprobe and
finalprobe (instead of visiblehud and visiblefinal), without touching
the game's native final RTV. A nonadjacent tail remains a hard block for
actual routing. The private probe may reflect an earlier composite if
native work intervened, so it cannot prove final parity or presentation.
It is attempted only for the two bounded SDR capture groups, not the
rolling live slots. PROBE_BEGIN / PROBE_HUD_RECORDED /
PROBE_FINAL_RECORDED, batch fence and raw file names distinguish it.
No extra run is needed to check the merge PSO/curve if the work-tail
gate alone fails. The general lineage scan now covers candidate
fullscreen draws at any destination size and creation-snapshot
backbuffer draws; this is not an exhaustive shader-use proof.
The wide shader fixture checks real compiler/disassembly at 0.675.
The original native 1920x1080 MV-magnitude literal remains unchanged:
it is a native motion-blur scale, not one of the two SV_POSITION loads.


### Automatic test readiness (no F8)

AZIMUTH_DEBUG_AUTO_TEST=1 enables automatic one-shot preview capture.
The DLL observes a recent fresh successful SR resolve and the exact
native postfx -> composite -> final PS/VS chain at the configured
render/output sizes before starting. At least eight observed chains
and a continuous warmup (AZIMUTH_DEBUG_AUTO_WARMUP_MS, default 2000)
are required. Brief gaps are tolerated, stale observations reset the
warmup. AZIMUTH_DEBUG_AUTO_RESOLVE is an optional minimum, not a fixed
trigger. AZIMUTH_DEBUG_AUTO_READY_FILE optionally requires a new
ordinary file created by an external runner after entering gameplay.
The DLL only checks existence; no key binding or synthetic hotkey.
The automatic mode bypasses F8 polling and preserves every existing
rendering gate. Readiness is observational, not a gameplay classifier,
GPU completion, or proof of actual Present. A title/garage/menu can
render the same scene chain; the external runner can use the file
gate to select real gameplay if this matters.


Use the full artifact and run the noninteractive preset (game settings
must already match the intended monitor/render/HDR settings):

    Start-DebugCapture.cmd -GameDir "F:\Games\Azimuth" -InstallProxy -AutoTest -VisibleSR -Output 1920x1080
    Start-DebugCapture.cmd -GameDir "F:\Games\Azimuth" -InstallProxy -AutoTest -Output 3840x1600

-AutoTest implies Preview, PreviewPostFx, PreviewReplay, PreviewSRReplay,
PreviewJitter, ShaderDisasm and ExitAfterCapture for a new launch.
-VisibleSR remains explicit SDR-only. The default label/render label
are filled without Read-Host. Pass -WaitSeconds 300 if loading is slow.
No forced termination: on complete DEBUG END + PREVIEW COMPLETE and
(for VisibleSR) VISIBLE TRIAL_END, keep a two-second native post-roll
then request normal window close. The total timeout preserves partial
fresh evidence after normal exit, without treating timeout as success.
The trial result and display proof are separate from capture completion.
-ReadyFile optionally waits for a nonexisting path in an existing
directory. A runner that verifies real gameplay can create this file
from another process, with no F8 or focus-dependent hotkey.
For example, pass -ReadyFile "C:\Temp\fh2-unique-ready.flag" and have
the runner create that file only after entering driving. Use a fresh
path per launch. -AutoResolve optionally sets a minimum resolve;
-AutoWarmupMs adjusts the stable scene warmup. The launcher does not
navigate game menus or automate throttle/steering. The runner can do
those separately and record video/PresentMon as before. A supplied
-PresentMonCsv is copied after game exit if present; the external
runner is responsible for ensuring the CSV has flushed.
