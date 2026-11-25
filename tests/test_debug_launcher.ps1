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
    'PASS: automatic launcher packaging and failure evidence'
} finally {
    $resolved = (Resolve-Path -LiteralPath $root).Path
    if ((Split-Path -Parent $resolved).TrimEnd('\') -eq $temp.TrimEnd('\') -and
        (Split-Path -Leaf $resolved) -match '^azimuth-launcher-test-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
