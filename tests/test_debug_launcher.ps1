$ErrorActionPreference = 'Stop'
$launcher = Join-Path $PSScriptRoot '..\debug\Start-DebugCapture.ps1'
$temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root = Join-Path $temp ('azimuth-launcher-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

try {
    $game = Join-Path $root 'game'
    New-Item -ItemType Directory -Path $game | Out-Null
    New-Item -ItemType File -Path (Join-Path $game 'azimuth.exe') | Out-Null
    $log = Join-Path $game 'azimuth_dlaa.log'
    @(
        'DEBUG READY: capture test',
        'DEBUG BEGIN id=1 firstResolveAfter=2001 limit=8 timeoutMs=8000',
        'DEBUG AUTO_BEGIN id=1 requestedResolve=2000',
        'DEBUG END id=1 reason=resolve_limit resolves=8 lastResolve=2010'
    ) | Set-Content -LiteralPath $log

    $options = @{
        GameDir = $game; Label = 'auto-fixture'; Output = '1920x1080'
        RenderSetting = 'test'; AutoResolve = 2000; CollectOnly = $true
    }
    $success = Join-Path $root 'success'
    & $launcher @options -Destination $success
    $zip = @(Get-ChildItem -LiteralPath $success -Filter '*.zip')
    if ($zip.Count -ne 1) { throw 'Expected a single successful capture archive.' }
    $contents = Join-Path $root 'contents'
    Expand-Archive -LiteralPath $zip[0].FullName -DestinationPath $contents
    $capture = Get-Content -LiteralPath (Join-Path $contents 'capture.json') -Raw | ConvertFrom-Json
    if ($capture.automatic_resolve -ne 2000 -or $capture.automatic_capture.resolves -ne 8) {
        throw 'Automatic capture results were not packaged.'
    }

    (Get-Content -LiteralPath $log) -replace 'reason=resolve_limit resolves=8', 'reason=timeout resolves=2' |
        Set-Content -LiteralPath $log
    $failure = Join-Path $root 'failure'
    $failedAsExpected = $false
    try { & $launcher @options -Destination $failure }
    catch {
        if ($_.Exception.Message -notmatch 'Automatic capture did not finish') { throw }
        $failedAsExpected = $true
    }
    if (!$failedAsExpected -or @(Get-ChildItem -LiteralPath $failure -Filter '*.zip').Count -ne 1) {
        throw 'An incomplete automatic capture must fail and preserve its evidence.'
    }
    # Preview collect-only fixture: package only exact reported raw sizes.
    $rawDir=Join-Path $game 'azimuth_preview_123_456'
    New-Item -ItemType Directory -Path $rawDir | Out-Null
    $rawName='f2002-sr-2x2-fmt26.raw'
    [IO.File]::WriteAllBytes((Join-Path $rawDir $rawName),[byte[]]::new(16))
    @('SR PREVIEW: fixture','DEBUG READY: fixture',
      'DEBUG BEGIN id=1 firstResolveAfter=2001 limit=8 timeoutMs=8000',
      "PREVIEW FILE directory=azimuth_preview_123_456 file=$rawName bytes=16",
      'PREVIEW FILE directory=azimuth_preview_123_456 file=f2002-color-2x2-fmt26.raw bytes=16',
      'PREVIEW COMPLETE groups=1 saved=1 failed=0 recordedFiles=1',
      'DEBUG END id=1 reason=resolve_limit resolves=8 lastResolve=2010') | Set-Content -LiteralPath $log
    $previewDest=Join-Path $root 'preview'
    $presentCsv=Join-Path $root 'present.csv'
    'Application,ProcessID,SwapChainAddress,CPUStartQPC' | Set-Content -LiteralPath $presentCsv
    'azimuth.exe,1,0x1234,5678' | Add-Content -LiteralPath $presentCsv
    & $launcher -GameDir $game -Label 'preview-fixture' -Output '3840x1600' -RenderSetting 'test' -Preview -CollectOnly -PresentMonCsv $presentCsv -Destination $previewDest
    $previewZip=@(Get-ChildItem -LiteralPath $previewDest -Filter '*.zip')
    $previewContents=Join-Path $root 'preview-contents'
    Expand-Archive -LiteralPath $previewZip[0].FullName -DestinationPath $previewContents
    if(!(Test-Path -LiteralPath (Join-Path $previewContents ('gpu-raw\'+$rawName)))){throw 'Missing preview raw texture.'}
    $previewMeta=Get-Content -LiteralPath (Join-Path $previewContents 'capture.json') -Raw | ConvertFrom-Json
    if($previewMeta.mode -ne 'preview' -or @($previewMeta.gpu_raw).Count -ne 1){throw 'Wrong preview metadata.'}
    if(!$previewMeta.presentmon_csv_packaged -or !(Test-Path -LiteralPath (Join-Path $previewContents 'presentmon.csv'))){throw 'Missing optional external Present evidence.'}
    # AutoTest collect-only: resolve zero is a valid scene-ready trigger.
    # All completion markers are needed for a VisibleSR test verdict;
    # the mere presence of PREVIEW COMPLETE is not enough.
    @('SR PREVIEW: fixture','DEBUG READY: fixture',
      'PREVIEW AUTO_CONFIG minimumResolve=0 warmupMs=2000 gateRequired=0 keys=disabled profile=1920x1080',
      'VISIBLE TRIAL_BEGIN firstResolveAfter=2001 qpc=100 retainedSwapchainRefs=0',
      'DEBUG BEGIN id=1 firstResolveAfter=2001 limit=8 timeoutMs=8000',
      'DEBUG AUTO_BEGIN id=1 requestedResolve=0',
      'PREVIEW AUTO_READY resolve=2001 chains=120 stableMs=2010 gateOpen=1 qpc=101',
      "PREVIEW FILE directory=azimuth_preview_123_456 file=$rawName bytes=16",
      'DEBUG END id=1 reason=resolve_limit resolves=8 lastResolve=2010',
      'PREVIEW COMPLETE groups=1 saved=1 failed=0 recordedFiles=1') | Set-Content -LiteralPath $log
    $autoOpts=@{GameDir=$game;Output='1920x1080';AutoTest=$true;VisibleSR=$true;CollectOnly=$true}
    $incomplete=Join-Path $root 'auto-incomplete'
    $failedAsExpected=$false
    try{& $launcher @autoOpts -Destination $incomplete}catch{
        if($_.Exception.Message -notmatch 'Automatic capture did not finish'){throw}
        $failedAsExpected=$true
    }
    if(!$failedAsExpected -or @(Get-ChildItem -LiteralPath $incomplete -Filter '*.zip').Count -ne 1){throw 'Missing trial-end must preserve evidence and fail.'}
    'VISIBLE TRIAL_END resolve=3000 reason=duration_elapsed candidates=999 routed=0 nativeCandidates=999 routedSubmitted=0 retainedSwapchainRefs=0 actualPresent=external_etw_or_video qpc=200' | Add-Content -LiteralPath $log
    $complete=Join-Path $root 'auto-complete'
    & $launcher @autoOpts -Destination $complete
    $autoZip=@(Get-ChildItem -LiteralPath $complete -Filter '*.zip')
    $autoContents=Join-Path $root 'auto-contents'
    Expand-Archive -LiteralPath $autoZip[0].FullName -DestinationPath $autoContents
    $autoMeta=Get-Content -LiteralPath (Join-Path $autoContents 'capture.json') -Raw | ConvertFrom-Json
    if(!$autoMeta.auto_test -or !$autoMeta.auto_ready -or !$autoMeta.visible_trial_complete -or $autoMeta.automatic_capture.id -ne 1 -or $autoMeta.automatic_resolve -ne 0 -or $autoMeta.capture_begins -ne 1){throw 'Wrong keyless auto-test metadata.'}
    'PASS: automatic launcher packaging, failure evidence, bounded preview RAW, and keyless Visible SR completion fixtures'
} finally {
    $resolved = (Resolve-Path -LiteralPath $root).Path
    if ((Split-Path -Parent $resolved).TrimEnd('\') -eq $temp.TrimEnd('\') -and
        (Split-Path -Leaf $resolved) -match '^azimuth-launcher-test-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
