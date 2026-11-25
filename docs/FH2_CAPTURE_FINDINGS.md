# First FH2 HDR captures: evidence and next gates

Two metadata-only runs at native and 1080p settings completed two F8 windows
each, with eight recognized resolve intervals per window and normal exit.
These are aggregate observations, not uploaded game captures or assets.

| Observation | Native | 1080p setting |
| --- | --- | --- |
| Geometry viewport and scene allocation | 3840x1600 | 2592x1080 |
| Scene resource format | R11G11B10_FLOAT | R11G11B10_FLOAT |
| Depth allocation | 3840x1600 | 2592x1080 |
| Native velocity target | 1920x800 | 1296x540 |
| Resolve scissor | 0,0,32767,32767 | 0,0,32767,32767 |
| First swapchain creation | 3840x1600, 2 buffers | 3840x1600, 2 buffers |
| Second swapchain creation | 3840x1600, 3 buffers | 2592x1080, 3 buffers |

The actual scene raster dimensions decrease to 67.5% of native in each axis.
A 3840x1600 format-24 texture is still produced by a known fullscreen draw
at the low setting. This does not prove that this resource reaches a
3840x1600 presenting backbuffer. In native captures some copies reach
the second swapchain's creation-time backbuffer identities; in the low
captures the recorded copy destinations do not match those snapshots.

Observed exact PS/VS pairs (hashes only, not shader code):

| PS | VS | Observed low-setting bindings/output |
| --- | --- | --- |
| 479be24a-12d3a1cc-5ae78b8d-e91827a4 | 2098e1f9-8e0d8b8e-7bdf6434-4ec0d391 | scene identity at t0, native half-res velocity at t3, other bound textures; 2592x1080 R10G10B10A2_UNORM target |
| a7840ad0-c7ff485a-baaba36a-f2b83b22 | 2b57aba8-08891572-ded78132-0c122cb6 | prior target at t0 plus other bound textures; 2592x1080 format-24 target |
| 577d52ef-933f3888-157f3b5e-e0935945 | 79d0ca68-15da8827-24fc6096-35dd7508 | two 2592x1080 format-24 bound textures at t0 and t2; 3840x1600 format-24 target |

The first two suggest tonemapping and composition. Their exact sampling,
color space, UV math and UI semantics remain unknown. All three are
fullscreen triangle draw candidates, not verified safe replacement points.
The existing pure HDR route requires an R11G11B10_FLOAT high-res target;
do not arm it using these format-24 PS hashes. A high-resolution HDR result
likely needs game-specific postprocess/view/constant changes before final
composition, or a verified alternative pipeline. The FG HUD-less point may
precede UI composition; actual texture contents must establish it.

Both runs observed five distinct direct queue creations. The old four-entry
queue table marked tracking incomplete before F8; all 16,384 logged FG draws
reported layout=0. The old draw quota was exhausted in approximately the
first two resolve intervals, with many depth-using scene draws. The existing
consumer analyzer returned no rows. Empty candidate output and scene=0 flags
are not proof of no consumer: offline comparison of recorded resolve color
identity with bound texture identities identifies the first candidate's t0.
No camera constants, GPU textures or actual submission order were captured.

Changes in the follow-up build:
- increase direct queue observation capacity to 32 without removing the
  single-direct-queue restriction on the existing visible route;
- prioritize the three known shader pairs in their own bounded quota,
  filter most unrelated 3D draws and retain partial descriptor observations
  explicitly marked incomplete;
- capture recognized scene identity separately from the route's transient
  state, never use it as a resource lifetime or frame-order proof;
- permit covering scissor only in the explicitly requested offscreen probe.
  Existing visible-route/padded scissor checks remain strict.

Next: a low-resolution F8 metadata run with the revised build and a
two-frame GPU capture of the same scene, ideally with HUD visible. Inspect
the three fullscreen draws, actual sampled resources/CBs, pre-HUD final
color, actual queue submissions and present path. Offscreen NGX probe may
be tested separately with the matching NGX bridge and runtime, correct
output size, and a quality mode whose queried input limits include
2592x1080. No probe success or visible SR/FG result has been established.

## Follow-up: successful offscreen SR API return

The next 1080p probe and debug capture ZIPs used matching game/proxy SHA-256. Probe color, depth and reconstructed R16G16_FLOAT MV were 2592x1080; all 12 logged decisions were fresh=1, top_left=1 and decision=1. NGX Quality optimal=2560x1067, min=1920x800, max=3840x1600; actual input accepted. `SR PROBE EVALUATED 2592x1080 -> 3840x1600 (OFFSCREEN ONLY)` appeared, exit code 0, no logged evaluation failure. This verifies successful API return at least once, not GPU completion or image quality. No game output was replaced.

Three F8 windows completed, 24 resolve intervals. Each known shader pair occurred 24 times; all 24 intervals matched scene identity -> first t0 -> second t0 -> third t0, in recording order within the same observed list. The final t2 is another distinct 2592x1080 format-24 bound texture in 24/24 intervals; HUD role unknown. None of 181 recorded copy edges matches the final candidate targets or creation-time backbuffer snapshots. 1,613 FG draws all layout=0, 898 input truncation markers; no queue submissions or GPU capture. Actual sampling, camera constants and present path need verification. Targeted disassembly now available as an optional local capture; no visible SR or FG enablement.


## Third capture: selected shader disassembly and 16:9 overlay boundary

The third local capture reports actual 1920x1080 scene/depth and both
swapchains, with 960x540 native velocity, not the earlier ultrawide
2592x1080 scene -> 3840x1600 candidate path. Five completed F8 windows,
40 recognized resolve intervals, exit code zero. Six selected shader
assembly files are available privately, not in this repository.

Disassembly establishes these facts:

- Scene resolve 1549a8f9 samples/loads source color, sample mask and
  depth and writes color and depth; the branch handling multisample
  data uses additional filmic-related math. Verify the actual HDR
  dynamic range and exposure instead of assuming all resolve outputs
  are untouched linear scene radiance.
- Native velocity 8a12870f uses 2x2 depth/stencil gather, selects
  reprojection rows 30--33 plus the stencil-derived index and clamps
  vector magnitude. The existing full-resolution replay intentionally
  replaces the gather/clamp and outputs UV-space motion. Validate units,
  edge/disocclusion and jitter against actual adjacent frames. The
  native half-resolution velocity is still needed for game's motion blur.
- 479be24a really uses scene t0, bloom, light rays, velocity t3, lens
  flare, stencil, alpha buffers/depth, world depth and color grading.
  The translated shader's declared registers differ from some original
  reflection comments (e.g. structured t6 vs original t7). Trust the
  executable instructions and actual descriptor bindings, not stale
  reflection names alone. It performs normalized UV sampling, integer
  loads using SV_POSITION for render-domain auxiliary textures, and
  contains a literal 1920x1080 velocity-to-pixel conversion. Its output
  includes color transformation and an sRGB-like transfer; verify the
  actual HDR/color-space contract in the capture.
- a7840ad0 is only an LOD-zero t0 sample and return, not complex
  composition. Its VS forwards input UV. It cannot substitute directly
  for high-resolution HDR tone mapping.
- In this run, the third candidate is a93721c4-5975a343-749f00dc-e74366c0
  with VS 79d0ca68-15da8827-24fc6096-35dd7508. It consumes a7840ad0's
  output at t0 and writes another 1920x1080 format-24 texture. The
  ultrawide variant 577d52ef was not observed; neither final PS
  assembly was included. Do not equate them or presume t2 is UI.

All 40 recognized intervals have same-list observed chain from 479be24a
through a7840ad0 to a93721c4. In between first two, the original
postprocess target receives 33--56 more draws per interval; all these
observed draws use blending. Some bind small atlas-like textures, and
some use derived blurred intermediates. Dominant candidates include
e792b40f, 6fe1ad18, dceb99b4. This is stronger evidence for an overlay
boundary just after 479be24a than the prior t2 UI speculation.
It does NOT prove all such draws are HUD, nor that the first candidate
is display-space HUD-less color compatible with FG. Compare actual
texture contents before and after these draws in a GPU capture.
88 copy records reach observed creation-time backbuffer identities,
but none directly links the final candidate's two target pointers to
them; actual presentation/queue execution remains unverified.

### Next integration decision

Keep original scene/velocity/depth rendering low-resolution. First
validate offscreen SR output pixels with jitter and temporal data; then
replay the actual postprocess at display resolution into separate
resources. Bind reconstructed HDR t0, preserve original bloom/blur
inputs and native velocity in their own coordinate domain. Integer
loads that address low-resolution auxiliaries need output-to-render
pixel mapping; normalized samples should remain normalized. Do not
blindly scale the 1920x1080 reference literal or every texture/constant:
verify its artistic meaning and each resource's actual size. Preserve
alpha/depth/stencil semantics and transfer function. Use a separate
output-size RTV/view chain and restore all original state. No direct
copy of linear HDR to R10G10B10A2 or generic replacement of all SRVs.
Before enabling visible SR, prove final copy/composition and draw/UI
depth-stencil compatibility at output size.

For FG, capture display-space color before the first confirmed HUD
draw, then compare post-HUD color and identify separate overlay
resources or replayable draw state. Preserve game-generated UI rather
than temporally reconstruct UI pixels. Existing motion-vector
limitations on rotating wheels and foliage remain; use targeted
geometry replay only after identifying stable draw/object history.
Do not invent camera constants or FG frame tokens.

Descriptor trace failure now specifically occurs at azt_slot, after
the first three windows. The fourth/fifth are entirely incomplete.
This could be capacity, hash clustering or retained generations, not
proof of a specific cause. Increase bounded table/probe and improve
hash mixing; report used/capacity at each F8 begin and overflow.
No live entries are evicted and the visible-route fail-closed gates
remain unchanged. Add exact alternate final pair to priority capture
and an offline chain analyzer. These changes are diagnostics only.
No proprietary shader text, game data or GPU capture is committed.


## Fourth capture: final color path and native pre-overlay HDR copy

The latest local ZIP's label says 1920x1080, but its two creation-time
swapchains are 1920x1080 and 2560x1440. Actual scene/depth/geometry
2560x1440, native velocity 1280x720. All three F8 windows completed,
24 recognized resolve intervals, exit code zero. All 24 observed
chains are 2560x1440 -> 2560x1440 -> 1920x1080, with exactly 23
intervening same-target blend draws. This is a downscale observation,
not an SR test. The final executed shader is a93721c4. The
577d52ef PSO exists and was disassembled, but no draw using it
appears in these windows. Both final shader assembly files and
shared fullscreen VS are available privately. Descriptor occupancy
151207, 152979, 154458 / 524288; no global incomplete warning. Some
individual root layouts remain incomplete (including the first
postprocess and copy); final SDR candidate layout is complete.
No GPU frame capture, present event, or texture contents.

Final shader disassembly corrects the prior interpretation:
a93721c4 samples image t0 and a 1D calibration curve t1, converts
transfer functions and returns the curve-mapped image. 577d52ef
uses t0 image, t1 curve, t2 scene and cb0 peak/paper/black. It maps
scene and composite through the curve, compares their values,
attenuates scene-specific HDR enhancement in changed pixels, maps
color to a wide-gamut basis consistent with Rec.2020, and applies
ST 2084/PQ. These are SDR-curve and HDR-PQ path candidates, not
aspect-ratio variants. Actual swapchain color space is not recorded.

Cross-check against the earlier 2592x1080 HDR metadata:
all 24/24 matched HDR chains have a same-list, same-resolve
CopyTextureRegion from the 479be24a postprocess target (resource
format 23) to the distinct t2 texture (format 24), recorded after
postprocess and before the first recorded blend draw. t0 remains
the a7840ad0 post-overlay image. This is strong evidence that the
game already retains a pre-overlay scene image for HDR composition.
No evidence that t2 is a separate UI layer; it is the scene copy.
The new analyzer verifies copy ordering and identities, and reports
measured direction rather than trusting the filename. Copy region,
GPU ordering and actual image contents remain unverified.

### Changed implementation approach

For HDR FG, reuse this existing pre-overlay input rather than trying
to remove HUD from final PQ pixels. An initial offscreen experiment
should replay the *existing* final HDR color transform with t0 and
t2 both sourced from the native pre-overlay scene, same t1 curve,
cb0 brightness levels and display viewport. With identical samples
the scene/composite difference in the shader is zero, avoiding the
overlay-specific suppression of scene enhancement. Verify the
result against the actual game HDR color space/format and pixels.
Use separate resources/descriptors and restore state. No copying
of proprietary shader bytecode into this repository. The replay
is a plan, not implemented yet; root binding/state, fence/lifetime,
actual active swapchain, and pixels require validation.

This does not recover true UI alpha: scene/composite difference
alone is not an alpha image. Preserve actual UI draws or use the
supported SDK HUD-less mode, and test translucent HUD, non-HUD
overlays and exposure changes. SDR path does not show the same
native t2 input, so a dedicated pre-overlay copy may be needed.

For SR, select a genuine low scene -> higher output run, e.g. the
previous 2592x1080 -> 3840x1600 HDR path, rather than the latest
downscale path. Reconstruct HDR before postprocess, map low-res
auxiliary pixel coordinates, keep UI/composition and original HDR
output transform. First validate the actual offscreen NGX image;
an API success return is not an image-quality measurement. No new
runtime rendering changes in this commit. The next high-value
evidence is actual GPU pixels/constants, not another broad F8 trace.
