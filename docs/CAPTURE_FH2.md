# FH2 render data acquisition

This is a local-only diagnostic build. Do not use the route mode until a real
capture establishes the exact native pass and semantics. No game settings,
DLLs or saves are automatically modified by the collector.

## Run A/B trace first

1. Use a legitimate installed FH2 and Azimuth (upstream profile was tested on
   1.0.4). Exit Azimuth. Back up the existing dxgi.dll and azimuth_ngx.dll;
   install the matching experimental pair from the Windows build artifact.
   Do not mix these with another DXGI proxy or an overlay for the first test.
2. Set Azimuth anti-aliasing Off and select native rendering. Keep output
   resolution, graphics settings, HDR mode and the driving location fixed.
   Run from the repository or the unpacked capture-tools artifact:

       py -3 tools/capture_sr.py --game-dir "D:\Azimuth" --label FH2-native --render-setting native --output 3840x1600

   Drive briefly after entering the actual world; close the game normally.
3. Choose a genuinely lower Azimuth render setting (record what the launcher
   actually offers; 2560x1067 is a theoretical 2/3-scale example, not a known
   Azimuth setting). Keep window/output size fixed if Azimuth permits it:

       py -3 tools/capture_sr.py --game-dir "D:\Azimuth" --label FH2-low --render-setting low --output 3840x1600

   If only output resolution can be changed, report that fact rather than
   assuming render and display dimensions are independent. Capture one
   corresponding run at each available resolution.
4. Review the two local ZIPs in sr-captures. Send them privately to the
   developer. They contain the raw mod log, JSON summary and metadata with
   binary SHA-256 hashes, not game binaries. There is no automatic upload.
   Include Azimuth/game version, GPU/driver, actual window and render
   settings, and whether the HUD/output looked normal.

## PIX verification

Install Microsoft PIX for Windows from its official site. Launch azimuth.exe
through PIX (Launch Win32, correct working directory, Launch for GPU Capture).
Set AZIMUTH_SR_MODE=trace and AZIMUTH_SR_TRACE_BUDGET=4000 in the target process
environment if using the experimental DLL. Disable competing overlays and
avoid F11 (upstream mod hotkey); use the GPU Capture button or PrintScreen.
Capture one frame during actual driving, at each of the two settings. Save
the .wpix files locally. If PIX cannot capture the modded process, repeat
with the original game/proxy configuration. PIX replay can require the same
GPU/driver; export screenshots and shader disassembly of the key events if
sharing the full capture is not practical. Full captures may contain game
assets; do not publish them in the public repository.

At the known HDR resolve and downstream consumers, record:
- event ID, draw/dispatch and command list/queue;
- PS/VS DXBC hashes, shader disassembly and any root constants;
- input color/depth/MV dimensions, formats, sample count, SRV register/space;
- output RTV/UAV and backbuffer dimensions/formats; viewport/scissor and UV mapping;
- actual geometry draw viewport, and the postprocess chain to tonemap/HUD.

The trace's "SR GEOMETRY" record is the **resolve** viewport and allocation
size, not independent proof of main geometry cost. Native velocity logging
is observational (the trace mode does not generate the mod's full-res MV).
Use PIX to verify actual geometry rasterization. "SR CONSUMER" means a
descriptor is bound, not that the shader really samples it. Missing consumer
rows do not prove there is no pass: newer D3D12 interfaces, dispatches and
other paths may be untracked.

After exiting a PIX-launched run, package its log with the collector's
--collect-only flag; check freshness manually. Keep the .wpix separate.

Microsoft official instructions:
https://devblogs.microsoft.com/pix/taking-a-capture/
https://learn.microsoft.com/en-us/windows/pix/gpu-captures/gpu-captures-overview

## What the results decide

- Physical low-res scene + high-res HDR pure scale target: verify route
  eligibility, including actual queue/list and shader semantics.
- Full-res allocation with a smaller active top-left region: investigate
  padded mode; verify the complete pipeline's UV/subrect assumptions.
- Low-res scene and combined tonemap/upscale or only low-res outputs: design
  game-specific postprocessing mirror/redirect; do not force the existing route.
- No reduction in scene geometry: find a renderer resolution control first.
- Once true lower-res inputs are established, run mode probe using the same
  collector plus --mode probe --quality quality. Look for NGX success and the
  high-resolution UAV in PIX. Probe does not display SR or enable jitter.
- Only then validate visible SR (native UI, no crop/ghosts, valid states) and
  measure GPU frame time outside capture instrumentation.

No actual FH2 capture or visible SR result is included in this branch yet.

## Optional frame generation discovery

See [FG_INTEGRATION.md](FG_INTEGRATION.md). Add `--fg-trace` to a native trace run to collect output-sized graphics draw metadata, candidate HUD boundaries and swapchain creation snapshots. This does not enable FG.
