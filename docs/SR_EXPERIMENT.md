# Azimuth-DLSS v0.2 experimental integration

**Development prototype, not a validated release.** Based on `Superl3/Azimuth-DLSS` (identical to upstream `a42-git/Azimuth-DLAA`) at `3bcaf0fa9bf108505c1e415bd8100c1febb6266e`. Native DLAA is the unchanged default. FH2 is the only eligible SR profile; FH3 keeps upstream DLAA behavior. No game assets or NVIDIA runtime DLL are bundled. Preserve both upstream license files.

## What changed from the first experiment

1. NGX render/output extents, quality modes, optimal size validation, render-space MVs, fixed-size temporal state, and the padded/offscreen paths remain.
2. A CPU-side D3D12 consumer tracer follows SRV creation, descriptor copies, heap generations, root-signature 1.0/1.1 descriptor ranges, PS/VS hashes, target RTV, viewport, and legacy barriers. It logs **bound** scene SRVs, not proof of shader sampling. A `trace` mode does not run NGX or inject MV replay/jitter.
3. A narrow visible `route` path can replace a *verified, pure HDR spatial upscale draw* with the full-resolution NGX output. It is not a general postprocess redirect. It uses a same-size `CopyResource`, not a copy between differently sized textures or a mutation of game descriptor heaps.
4. The route requires explicit verified PS and VS hashes, register/space, matching source identity, single observed direct queue, same command-list recording, same-size R11G11B10_FLOAT output/RTV, full viewport/scissor, simple views, triangle-list fullscreen draw shape, known target RT state, no active predication/query, and no detected intervening scene writes. Unknown or invalid cases leave the original draw intact. It disables further SR evaluation after 16 consecutive unresolved outputs. No Present hook is added.
5. `tools/analyze_sr_trace.py` groups logged candidates for manual GPU-capture review. Windows CI builds both DLLs against pinned official SDK/toolchain inputs and runs the host tests. The workflow must still be executed in the actual repository.

## Modes

| `AZIMUTH_SR_MODE` | Behavior | Displayed output |
| --- | --- | --- |
| `dlaa` (default) | Original DLAA path, no extra tracing vtable hooks | Upstream DLAA |
| `trace` | Observe native scene/consumer bindings; no injected MV replay, jitter or NGX | Original game |
| `diagnostic` | Original DLAA plus resource and consumer logs | Upstream DLAA |
| `probe` | NGX SR into a separate output UAV, no injected scene jitter | Original game, SR is offscreen |
| `padded` | Active low-res top-left subrect in output-sized scene allocation; same-size copy | Conditional experimental write-back |
| `route` | Low-res scene -> separate SR output -> exact, independently confirmed pure HDR upscale consumer | Conditional experimental write-back |

The game must provide genuine low-resolution rendering. No mode changes Azimuth's internal render resolution. No mode is proof of FPS gains. If Azimuth uses a combined tonemap+upscale pass, no suitable pure HDR upscale pass, or separate command lists, **this route is intentionally not applicable**. Do not bypass the guards to force output.

## Apply to the repository

Use a disposable branch from the pinned commit. The cumulative patch already includes v0.1; do not apply both v0.1 and v0.2. The Python installer verifies exact original Git blob hashes and every replacement anchor before modifying anything. It does not download, commit or push.

```powershell
git clone https://github.com/Superl3/Azimuth-DLSS.git
cd Azimuth-DLSS
git switch -c experiment/dlss-sr-v02 3bcaf0fa9bf108505c1e415bd8100c1febb6266e
py C:\path\to\azimuth-sr-v02\apply_v02.py . --check
py C:\path\to\azimuth-sr-v02\apply_v02.py .
git diff --check
git status --short
git add README.md src bridge docs tests tools .github/workflows/sr-build.yml
git commit -m "Add experimental DLSS SR consumer tracing and guarded HDR routing"
git push -u origin experiment/dlss-sr-v02
```

Alternatively, use `git apply --check /path/to/azimuth-sr-v02.patch` and `git apply /path/to/azimuth-sr-v02.patch` on the pristine pinned branch. The installer is preferred because it checks the full original blob hashes. Neither method changes the upstream main branch. Review the diff before pushing. If the repository already contains other changes, stop and integrate via a separate branch rather than overwriting files.

## Build

GitHub Actions: enable Actions on the fork if necessary, then open **Experimental SR Windows build**. A push to `experiment/**` or manual dispatch invokes Windows 2022 runner, Zig 0.15.2 with its official published SHA256, NVIDIA/DLSS SDK commit `9981c6350dc58c0ce21f024fd65e31e851c31689`, and installed Visual Studio x64 tools. The artifact `azimuth-sr-experimental-windows-x64` contains the two DLLs, licenses, guide and checksums, not `nvngx_dlss.dll` or game assets. A CI success proves compilation/tests, not real GPU support.

Manual build: follow upstream instructions: x64 MSVC Native Tools Command Prompt, official DLSS SDK in `third_party/DLSS` or `DLSS_SDK`, `bridge\build_bridge.bat`; then Git Bash `ZIG=/path/to/zig ./build.sh`. Use the two DLLs together. Keep a backup of the existing DLLs. Supply your own official `nvngx_dlss.dll` for NGX modes. Disable Azimuth AA, launch directly rather than via Steam for initial tests. No binaries have been built in the authoring environment.

## Runtime validation protocol

### 1. Native trace, no injected SR

Choose FH2 / Azimuth 1.0.4 first. Run at two actual Azimuth render settings with the same window/output size if possible. In the Azimuth installation directory:

```powershell
$env:AZIMUTH_SR_MODE = 'trace'
.\azimuth.exe
```

Drive for a short period and capture `azimuth_dlaa.log` (rename after each run). Record the actual window/render settings. Inspect `SR TRACE RESOLVE`, `SR CONSUMER`, `SR TRACE observed direct queues`, `SR TRACE incomplete` and swapchain lines. The trace uses bounded tables and logs (up to 160 consumer rows), so capture early gameplay. A missing consumer is **not** proof no consumer exists: command-list or descriptor APIs may be untracked.

```powershell
py C:\path\to\azimuth-sr-v02\tools\analyze_sr_trace.py .\azimuth_dlaa.log --output 3840x1600
```

The analyzer is a ranking aid only. It must not arm a shader based on dimensions or counts alone. Use a PIX GPU capture to inspect the exact PS/VS, render target, original SRV, actual sampling region, draw coverage and target contents. Confirm the shader is only a linear-HDR resize with no tone mapping, grading, blending, bloom, other material contributions, UAV side effects, or depth/stencil dependence. Verify the output target is consumed at full output resolution and that HUD and tone mapping still happen in the original order. Confirm resource and queue ordering, including actual command-list boundaries. If no matching pure pass exists, retain trace/probe and implement a game-specific shader/resource redirect instead.

### 2. Real NGX offscreen probe

Use the actual low-resolution allocation dimensions, not a guessed ratio. Example target `3840x1600`:

```powershell
$env:AZIMUTH_SR_MODE = 'probe'
$env:AZIMUTH_SR_OUTPUT = '3840x1600'
$env:AZIMUTH_SR_QUALITY = 'quality'
.\azimuth.exe
```

Look for `SR frame`, `SR NGX optimal`, `SR PROBE EVALUATED`. Inspect the named `AzimuthSR Experimental Output` UAV in PIX. Verify it contains a valid reconstructed HDR image. The source/target ratio must be inside the selected NGX mode's supported range. The mod does not force the game's internal resolution; if the source allocation is still native, the probe rejects it. This mode leaves the display unmodified; its lack of scene jitter is intentional.

### 3. Visible route, only after independent capture verification

Do not copy placeholder hashes. The log emits four little-endian DXBC header checksum words for PS and VS, e.g. `xxxxxxxx-xxxxxxxx-xxxxxxxx-xxxxxxxx`. The following example requires you to replace placeholders with values verified in step 1. Start with jitter OFF.

```powershell
$env:AZIMUTH_SR_MODE = 'route'
$env:AZIMUTH_SR_OUTPUT = '3840x1600'
$env:AZIMUTH_SR_QUALITY = 'quality'
$env:AZIMUTH_SR_ROUTE_HASH = '<verified PS hash>'
$env:AZIMUTH_SR_ROUTE_VS_HASH = '<verified VS hash>'
$env:AZIMUTH_SR_ROUTE_REGISTER = '<verified decimal t register>'
$env:AZIMUTH_SR_ROUTE_SPACE = '<verified decimal register space>'
$env:AZIMUTH_SR_ROUTE_CONFIRMED = '1' # only after manual PIX verification
$env:AZIMUTH_SR_ROUTE_JITTER = '0'
.\azimuth.exe
```

Look for `SR ROUTE EVALUATED` (NGX succeeded) and `SR ROUTE COPIED` (the selected draw was replaced). Verify the actual final output in PIX and gameplay: no stale edges, incorrect tone mapping, cropped image, missing HUD, device errors, ghosting or changes to the render chain. A copy log is not proof that the final image is correct. Once visible output is independently verified, optionally restart with `AZIMUTH_SR_ROUTE_JITTER=1` to test temporal quality. Jitter is gated on a prior recorded successful route, but if a later route fails, an already recorded jittered frame may remain; this is experimental. F11 toggles the existing feature. Clear all SR environment variables and restart to restore native DLAA. If the renderer uses padded full-size allocations, the older `padded` path is separate; only enable it after verifying the full-size downstream sampling region.

## Remaining risks and follow-up

- This route works only for a pure HDR upscale pass with an existing compatible output target. It does not yet mirror the full postprocessing chain, resize engine resources, or redirect complex consumers. No real FH2 consumer hash is supplied.
- Descriptor/root tracking covers the hooked base D3D12 interfaces and legacy barriers; pipeline-state streams, newer device/list interfaces, bindless/unbounded tables, cross-list SR consumers, multiple queues, and enhanced barriers are not supported. Capacity overflow disables route. Host metadata tests do not establish correctness on real D3D12 interfaces or execution timelines. The direct-queue count is an observed count, not proof of submission ordering. GPU capture remains mandatory.
- The inherited DLAA/NGX backend still relies on upstream command-list, resource lifetime and state assumptions. The new route does not fix all of them; only fixed-size experiments are supported. Pointer identities and copied descriptor metadata are conservative observations. No general fence-managed multi-queue lifetime system has been implemented.
- Native DLAA, probe, padded and route must each be tested separately. Compare native render load, GPU frame time, FPS and image quality only after a working visible route. For physically low-resolution scene textures with complex downstream passes, follow the Luma FFXV resource mirror model using verified shaders and fence-managed resources instead of relaxing the current gates.

## Verification performed locally

Host C policy tests; host doubles executing the actual consumer metadata/route code including a positive copy and rejection cases; GCC/Clang warnings and AddressSanitizer/UndefinedBehaviorSanitizer for the trace test; Python trace analyzer unit tests; Python installer syntax; generated patch apply and exact output comparison against sparse verified original source excerpts. All six edited original GitHub blob hashes and all 51 non-new-file patch hunk old-line ranges were checked against the pinned remote source. The full checkout was not downloaded into the authoring container. Full Windows SDK/Zig DLL build, GitHub Actions execution, actual NVIDIA NGX evaluation, runtime GPU capture and final screen output have **not** been performed here.

## Capture collection helper

See [FH2 render capture guide](CAPTURE_FH2.md) and `tools/capture_sr.py` for local trace/probe evidence collection. The collector never enables visible routing or uploads game data.
