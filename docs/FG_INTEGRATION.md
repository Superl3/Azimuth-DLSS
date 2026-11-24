# Frame generation integration: discovery first

**Status: metadata-only FG discovery, not a frame generation implementation.**
The branch has no generated frames, FG backend, swapchain replacement or
Present hook. This is deliberately independent of whether SR works. Start
with native rendering (or eventually DLAA) and a stable frame rate.

## References and architecture

Luma's DLSS implementation separates input/output sizes and tracks resource
mirrors for postprocess chains. OptiScaler's OptiFG collects upscaler inputs
and finds HUD-less candidates, then uses a presentation backend. Our existing
NGX bridge is an SR evaluator, not a presentation backend.

- https://github.com/Filoppi/Luma-Framework/blob/main/Source/Core/dlss/DLSS.cpp
- https://github.com/Filoppi/Luma-Framework/blob/main/Source/Core/includes/resource_upgrades.hpp
- https://github.com/optiscaler/OptiScaler/wiki/OptiFG
- https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
- https://gpuopen.com/fidelityfx-super-resolution-3/

Prefer NVIDIA Streamline DLSS-G as a first intended vendor backend for the
RTX 4080, with hardware/runtime capability checks (start at one generated
frame per real frame, not an assumed MFG unlock). FSR FG is a useful alternative
backend/reference for swapchain management and independent validation.
Use the vendor's supported SDKs, keep runtime DLLs external, and do not
copy third-party implementation code into this project without license review.

## Data still missing

1. Main swapchain and its actual present queue, buffer index, resize and
   lifetime. A CreateSwapChainForHwnd observation is not sufficient to identify
   the active chain. Do not attach a proxy to every swapchain.
2. Per-render-frame identity. Existing resolve counters and MV counters are
   recording-order heuristics; they do not prove execution order or match
   frame tokens. Establish queue submissions, fence completion, present and
   a single authoritative frame identity before running FG.
3. Display-sized, post-tonemap, pre-HUD color in the **same color space,
   transfer function and postprocessing state as final output**. The existing
   linear HDR R11G11B10 scene at SR time is not automatically HUD-less FG color.
   Capture the first true HUD draw and its prior image. The pass may render to
   a separate intermediate rather than the backbuffer.
4. Dense depth/MV for the same real frame; verify units, direction, resolution,
   dynamic objects, reversed depth, jitter cancellation, discontinuities.
   Current reconstructed stencil/camera motion lacks proof of moving wheels
   and independently animated objects. SR success does not prove FG quality.
5. Valid common camera constants: current/previous projection and reprojection,
   near/far, camera basis, position, jitter, MV scale and flags, reset/cut.
   Recover these from game constant buffers/shaders, with PIX verification;
   do not supply fabricated identity matrices or arbitrary near/far.
6. HUD alpha/color when feasible, or a validated HUD-less image for the
   backend's supported separation method. Menus, replay, photo mode, pause,
   loading, focus changes and resize must be handled separately.

## Passive discovery capture

Use the latest experimental artifact and native rendering. Existing capture
script supports:

    py -3 tools/capture_sr.py --game-dir "D:\Azimuth" --label FH2-FG --render-setting native --output 3840x1600 --fg-trace

The default window is the first eight recognized resolve sequences; if these
occur in menus/loading, repeat with e.g. --fg-start 300 --fg-resolves 12.
The counter is a *resolve* count, not an exact real frame number. Observe
the current capture log while testing to choose the window. Run the same
scene once with HUD visible and, if a game setting allows, once without HUD.
Do not use another overlay in the first test. Use PIX to confirm the real
queue order, shader sampling, resource contents, camera constants and
draw-before-HUD boundary. For PIX-launched captures set AZIMUTH_SR_MODE=trace
and AZIMUTH_FG_TRACE=1 before launch; optional AZIMUTH_FG_TRACE_START,
AZIMUTH_FG_TRACE_RESOLVES and AZIMUTH_FG_TRACE_BUDGET (64..20000).

Trace output:
- FG SWAPCHAIN and FG BACKBUFFER: creation-time descriptions, queue identity
  and buffer pointer snapshots. Not refreshed on resize; rerun after resizing.
  GetBuffer references are released immediately; no proxy is introduced.
- FG PASS: single-RTV draws at observed output size, PS/VS hashes, blend,
  depth, viewport, approximate CPU recording sequence, possible backbuffer.
- FG INPUT: up to eight distinct bound texture SRVs per logged draw;
  bindings are not proof of actual sampling. Unknown layouts are not guessed.
- summary.json -> fg_discovery: ordered observations and grouped candidates.
  No classifier decides which draw is HUD. Limited scope and explicit budget;
  dispatch, multi-RTV, copy-only output, newer interfaces and untracked PSOs
  may be missed. Missing rows do not prove absence of a pass.

This capture makes no GPU copies and loads no FG SDK. PIX evidence is still
necessary to determine a safe capture point. Never publish full game captures
or proprietary shader/binary dumps in the public repository.

## Planned implementation gates

A. Capture and verify main queue/swapchain, camera/MV and HUD boundary in
   native-resolution gameplay. Reproduce capture in HUD-off, pause, resize.
B. Add an FG frame contract and fence-protected input ring, tag exact real
   frame IDs; initially prepare without generated presents. Use a debug
   visualization for depth, velocity and pre-HUD image.
C. Add optional separate Streamline bridge (initialization before device/
   selected swapchain creation, or documented manual-hooking path).
   Verify adapter and supported modes via SDK; tag depth/MV/hud-less and
   constants for the matching frame token. Use a single presentation owner.
   Integrate Reflex with actual render/simulation markers where available;
   do not claim true input latency reduction from a synthetic marker.
D. Enable 2x FG at native/DLAA only after A-C pass. Disable on invalid
   input, pause/loading, menu, device/chain changes or low base FPS; reset
   history; implement SDK-controlled recreation and fence-safe shutdown.
   Do not revive the old global Present-vtable hook: upstream had an overlay
   recursion crash. Verify with overlays off before testing combinations.
E. Validate real versus displayed FPS, base GPU time, pacing/1% lows, visual
   artifacts, HUD stability, input latency, alt-tab, resize and shutdown.
F. Combine independently validated SR and FG. Post-SR HUD-less images must
   still agree with final backbuffer; do not use stale low-res pre-tonemap
   HDR as a substitute. Alternate FSR backend only after the frame contract
   works. Support for >2x is capability-dependent, not promised.

## SR audit findings to address

The existing SR NGX path uses distinct input/output sizes, optimal settings,
render subrect and pixel-space MV scales. It has three evidence-gated paths
(probe/padded/route) and does not itself control the engine's render resolution.
Luma's resource mirror and downstream view/constant updates are still missing.
The pure-scale route is not a general postprocess replacement.

The current viewport jitter, hard-coded exposure, depth convention,
mip bias, reconstructed dynamic object motion, cut reset, descriptor/command
list tracking and resource retirement need actual GPU validation. The upstream
fixed-call-count retirement is not a GPU fence. Never use that strategy for
FG inputs; keep per-frame resources alive to the backend's real completion.

For bounded F8 metadata collection and optional actual RenderDoc GPU frame capture, use [DEBUG_CAPTURE.md](DEBUG_CAPTURE.md) rather than relying on startup resolve windows.
