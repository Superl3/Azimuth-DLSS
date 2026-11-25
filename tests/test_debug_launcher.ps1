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
    & $launcher -GameDir $game -Label 'preview-fixture' -Output '3840x1600' -RenderSetting 'test' -Preview -CollectOnly -Destination $previewDest
    $previewZip=@(Get-ChildItem -LiteralPath $previewDest -Filter '*.zip')
    $previewContents=Join-Path $root 'preview-contents'
    Expand-Archive -LiteralPath $previewZip[0].FullName -DestinationPath $previewContents
    if(!(Test-Path -LiteralPath (Join-Path $previewContents ('gpu-raw\'+$rawName)))){throw 'Missing preview raw texture.'}
    $previewMeta=Get-Content -LiteralPath (Join-Path $previewContents 'capture.json') -Raw | ConvertFrom-Json
    if($previewMeta.mode -ne 'preview' -or @($previewMeta.gpu_raw).Count -ne 1){throw 'Wrong preview metadata.'}
    'PASS: automatic launcher packaging, failure evidence, and bounded preview raw texture fixture'
} finally {
    $resolved = (Resolve-Path -LiteralPath $root).Path
    if ((Split-Path -Parent $resolved).TrimEnd('\') -eq $temp.TrimEnd('\') -and
        (Split-Path -Leaf $resolved) -match '^azimuth-launcher-test-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
