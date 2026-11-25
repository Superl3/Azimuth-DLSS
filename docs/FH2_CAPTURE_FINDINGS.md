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
