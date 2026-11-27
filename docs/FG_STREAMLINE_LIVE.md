# FH2 HDR Streamline live integration (experimental)

Target only FH2, 2592x1080 -> 3840x1600 RGB10 PQ/BT.2020, DLSS Quality, HUD Delta, no jitter, no NR evaluation, one interpolated frame. Keep ReShade as dxgi.dll; the temporary proxy is dxgi-dlaa.dll. Do not enable Smooth Motion for azimuth.exe during either trial.

## Reproduce

Use the matching Windows artifact containing Start-DebugCapture.ps1, dxgi.dll (installed temporarily as dxgi-dlaa.dll), azimuth_ngx.dll and azimuth_streamline_live.dll. Supply official signed Streamline 2.14.1 DLLs and nvngx_dlssg.dll in a separate directory, with a byte-identical copy of the game's nvngx_dlss.dll. The build pins the SDK headers to 2122257e0fce486f91b385aa63b9a09b0a34b363. No NVIDIA runtime redistribution.

Save the existing azimuth.exe-specific NVIDIA Smooth Motion value and NR evaluation settings. Turn only that profile's Smooth Motion OFF; turn NR evaluation OFF. Verify HDR on, output and render resolution. Do not change the global driver profile. The launcher only records operator confirmation, not driver configuration. Restore both settings after both tests, including failures, and verify the original values and DLL hashes.

Run in PowerShell from the extracted artifact:

    $game = 'D:\FH2\Azimuth'
    $runtime = 'D:\SDK\Streamline-2.14.1-runtime'
    $ready = Join-Path $env:TEMP ('fh2-ready-' + [guid]::NewGuid().ToString() + '.txt')
    .\Start-DebugCapture.ps1 -GameDir $game -InstallProxy -ProxyMode ReShade -AutoTest -VisibleHDR -HDRLive -Output 3840x1600 -RenderSize 2592x1080 -Quality quality -HUD delta -NoJitter -ProducerGpuWait -PresentTrace -VisibleSeconds 10 -WaitSeconds 300 -ReadyFile $ready -StreamlineRuntime $runtime -ConfirmSmoothMotionOff -StreamlineControl -Label FH2-HDR-FG-OFF

The launcher starts the game and prints the ready path. In free roam, create the ready file from another PowerShell with New-Item -ItemType File -Path <printed path>. Repeat with a new ready path and -StreamlineFG -Label FH2-HDR-FG-ON instead of -StreamlineControl. Start an external physical-display trace before creating the ready file; retain its CSV/ETL. Use -PresentMonCsv for an externally collected CSV if available. For the initial test leave free roam only after the automatic trial exits. For a longer recovery test use -VisibleSeconds 60 and remove the ready file before pausing/loading; recreate after active driving resumes. The ReadyFile is an operator gate, not automatic game-state detection. Missing frames, discontinuity, resize and HDR changes also cause reset/off. Existing SR must remain functional when FG is off.

## Proof and limitations

The native D3D12 bridge uses the supplied ReShade unwrap and camera reconstruction convention. A packet binds exact camera recording, depth/MV producer and final HUD-less PQ producer to one Resolve and frame token. A bounded private GPU ring owns input snapshots. Only the 256-byte camera readback is CPU-mapped and only after its submitted fence is complete. No full-backbuffer CPU readback or QPC-nearest matching.

The native final write seeds a unique resource-private cookie; ordered full-resource/single-subresource RGB10 copies propagate the cookie's packet only after actual ExecuteCommandLists, with explicit queue/fence dependencies. The managed swapchain generation, actual presenting queue and current virtual buffer index must match. Known writes invalidate provenance; UAV-capable RGB10 textures are excluded. No arbitrary shader draw is promoted to a transfer. The supplied diagnostic did not prove the final-to-virtual-backbuffer transfer type, so the full-copy route is a hypothesis to verify in the first test. If a shader blit or uncovered API is involved, FG may remain OFF; record exact command-list/PSO/resource bindings before expanding the allowed route. Native D3D12 hook coverage and wrapper identities require real-game verification.

Inspect fg-live.log for FINAL, COPY/SUBMIT, PRESENT_TAG and PRESENT_RESULT; require a continuous sequence of distinct Resolve/token, matching generation/index and submitted transfer, reset on the first valid frame, later SDK status OK and framesSinceQuery > 1. The SDK counter is cumulative since last query, not a per-token proof. Require independent actual DXGI displayed events and video to prove generated frames. Capture both virtual and physical swapchain traces. If DisplayedTime/frame-type rows are missing, do not treat API Present count as display proof: retry with a physical-output ETW/PresentMon capture or external high-speed display recording. Compare FG OFF/ON HDR format, HUD, SR completion/fallback, displayed intervals and restoration. Raw captures and videos may be kept separate; ZIP only the decision-relevant logs, trace excerpt, comparison clip, manifests and restoration evidence.

No second-Present overwrite, no additional application Present, no generated-frame claim based on an SDK counter alone. Reflex low latency and per-token PCL render/present markers are experimental; actual simulation start and ReflexSleep are not hooked. The actual local validation commits were not available in the remote repository; this is a port to PR #10's published head, not a cherry-pick of those local commits. Windows CI is build/test only, not RTX GPU validation.

## Automated evidence summary

Run `python analyze_streamline_live.py <capture>/fg-live.log --presentmon <physical-chain-filtered.csv>` from the build. It checks exact packet, three submitted stages, copy destination, queue, recording, submit, generation, resolve and token; `sdk_multiple_frame_candidates` requires a non-reset consecutive FG token and consumer fences. It intentionally reports `actual_display_proof: false` until a human correlates actual displayed generated events. `retry_required: true` means display data is missing or empty. This is not a substitute for the real RTX test.
