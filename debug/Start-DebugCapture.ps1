[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Label = '',
    [string]$Output = '',
    [string]$RenderSetting = '',
    [switch]$InstallProxy,
    [switch]$CollectOnly,
    [string]$Destination = (Join-Path $PSScriptRoot 'debug-captures')
)
$ErrorActionPreference = 'Stop'
if (!$GameDir) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'azimuth.exe') -PathType Leaf) { $GameDir = $PSScriptRoot }
    else { $GameDir = (Read-Host 'Path to the Azimuth folder containing azimuth.exe').Trim('"') }
}
$game = (Resolve-Path -LiteralPath $GameDir).Path
$exe = Join-Path $game 'azimuth.exe'
$proxy = Join-Path $game 'dxgi.dll'
$log = Join-Path $game 'azimuth_dlaa.log'
$artifact = Join-Path $PSScriptRoot 'dxgi.dll'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'azimuth.exe not found' }
if (Get-Process -Name azimuth -ErrorAction SilentlyContinue) { throw 'Close existing Azimuth instances before launching or collecting.' }
if (!$Label) { $Label = Read-Host 'Capture label (for example FH2-native-HUD-on)' }
if ($Label -notmatch '^[A-Za-z0-9_-]{1,48}$') { throw 'Label must contain 1-48 letters, numbers, underscores or hyphens.' }
if (!$Output) { $Output = Read-Host 'Expected display resolution, e.g. 3840x1600 (or unknown)' }
if ($Output -ne 'unknown' -and $Output -notmatch '^[1-9]\d*x[1-9]\d*$') { throw 'Output must be WxH or unknown.' }
if (!$RenderSetting) { $RenderSetting = Read-Host 'Actual Azimuth render setting (native/low/etc.; this is a label only)' }
$dest = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $dest -Force | Out-Null
$tag = $Label + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$stage = Join-Path $dest ($tag + '.tmp')
$zip = Join-Path $dest ($tag + '.zip')
$backup = $null
$installedHash = $null
$installed = $false
$oldEnv = @{}
$stageCreated = $false
$exitCode = $null
function FileHashOrNull([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
    return $null
}
try {
    if (!$CollectOnly) {
        $currentHash = FileHashOrNull $proxy
        $artifactHash = FileHashOrNull $artifact
        if ($artifactHash -and $artifactHash -ne $currentHash) {
            if (!$InstallProxy) {
                $reply = Read-Host 'Temporarily install the debug dxgi.dll? Existing proxy will be backed up and restored after exit. [y/N]'
                if ($reply -notmatch '^[Yy]$') { throw 'Install declined. No game DLL changed.' }
            }
            if ($currentHash) {
                $backup = Join-Path $dest ($tag + '-original-dxgi.dll')
                Copy-Item -LiteralPath $proxy -Destination $backup
                if ((FileHashOrNull $backup) -ne $currentHash) { throw 'Original proxy backup verification failed.' }
            }
            Copy-Item -LiteralPath $artifact -Destination $proxy
            $installedHash = $artifactHash
            $installed = $true
            if ((FileHashOrNull $proxy) -ne $installedHash) { throw 'Debug proxy copy verification failed.' }
        }
        if (!(Test-Path -LiteralPath $proxy -PathType Leaf)) { throw 'Install the latest debug dxgi.dll next to azimuth.exe first.' }
        $previous = $null
        if (Test-Path -LiteralPath $log -PathType Leaf) {
            $previous = (Get-Item -LiteralPath $log).LastWriteTimeUtc
            Copy-Item -LiteralPath $log -Destination (Join-Path $dest ($tag + '-previous.log'))
        }
        foreach ($key in @('AZIMUTH_SR_MODE','AZIMUTH_DEBUG_RENDERDOC')) {
            $oldEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
        }
        [Environment]::SetEnvironmentVariable('AZIMUTH_SR_MODE','debug','Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_RENDERDOC','0','Process')
        Write-Host 'Enter real gameplay. Press F8 once while driving. Capture stops after a bounded interval; repeat F8 for additional samples. Exit normally.'
        Write-Host 'This launcher collects CPU metadata only. To capture GPU frames, use the RenderDoc procedure and -CollectOnly.'
        $started = [DateTime]::UtcNow
        $proc = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru -Wait
        $exitCode = $proc.ExitCode
        if (Get-Process -Name azimuth -ErrorAction SilentlyContinue) { throw 'Azimuth child is still running; exit and run with -CollectOnly.' }
        if (!(Test-Path -LiteralPath $log -PathType Leaf)) { throw 'No trace log produced.' }
        $stamp = (Get-Item -LiteralPath $log).LastWriteTimeUtc
        if ($stamp -lt $started.AddSeconds(-2) -or ($previous -and $stamp -eq $previous)) { throw 'Trace log is not fresh.' }
    }
    if (!(Test-Path -LiteralPath $log -PathType Leaf)) { throw 'No azimuth_dlaa.log found.' }
    $content = Get-Content -LiteralPath $log -Raw
    if ($content -notmatch 'DEBUG READY:') { throw 'This log is not from the F8 debug build. Check the installed DLL and launch mode.' }
    $begins = [regex]::Matches($content,'(?m)^DEBUG BEGIN ').Count
    $ends = [regex]::Matches($content,'(?m)^DEBUG END ').Count
    New-Item -ItemType Directory -Path $stage | Out-Null
    $stageCreated = $true
    Copy-Item -LiteralPath $log -Destination (Join-Path $stage 'azimuth_dlaa.log')
    $meta = [ordered]@{
        label = $Label; expected_output = $Output; actual_render_setting_label = $RenderSetting
        utc = [DateTime]::UtcNow.ToString('o'); mode = 'debug'; collect_only = [bool]$CollectOnly
        process_exit_code = $exitCode; f8_begins = $begins; f8_ends = $ends
        files_sha256 = [ordered]@{ 'azimuth.exe' = (FileHashOrNull $exe); 'dxgi.dll' = (FileHashOrNull $proxy) }
        notice = 'Local metadata only. No game binaries, saves, or raw GPU captures in this archive.'
    }
    $meta | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'capture.json') -Encoding UTF8
    $analyzer = Join-Path $PSScriptRoot 'tools\analyze_debug_trace.py'
    if (!(Test-Path -LiteralPath $analyzer)) { $analyzer = Join-Path $PSScriptRoot '..\tools\analyze_debug_trace.py' }
    $py = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($py -and (Test-Path -LiteralPath $analyzer -PathType Leaf)) {
        try {
            $argsForAnalyzer = @('-3',$analyzer,(Join-Path $stage 'azimuth_dlaa.log'))
            if ($Output -ne 'unknown') { $argsForAnalyzer += @('--output',$Output) }
            $result = & $py.Source @argsForAnalyzer
            if ($LASTEXITCODE -eq 0) { $result | Set-Content -LiteralPath (Join-Path $stage 'summary.json') -Encoding UTF8 }
            else { Write-Warning 'Optional Python summary failed; raw log remains included.' }
        } catch { Write-Warning 'Optional Python summary failed; raw log remains included.' }
    }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    if (!(Test-Path -LiteralPath $zip -PathType Leaf)) { throw 'Capture ZIP not created.' }
    Write-Host "Local evidence: $zip"
    if ($begins -eq 0) { Write-Warning 'No F8 capture found. Enter actual gameplay and press F8 in the next run.' }
    elseif ($begins -ne $ends) { Write-Warning 'One or more capture windows were interrupted.' }
    Write-Host 'Review the ZIP before sharing it. Save RenderDoc .rdc files separately and share privately only.'
}
finally {
    foreach ($key in $oldEnv.Keys) { [Environment]::SetEnvironmentVariable($key,$oldEnv[$key],'Process') }
    if ($installed) {
        if ((Get-Process -Name azimuth -ErrorAction SilentlyContinue) -or (FileHashOrNull $proxy) -ne $installedHash) {
            Write-Warning "Proxy not restored (game running or file changed). Original backup: $backup"
        } elseif ($backup) {
            Copy-Item -LiteralPath $backup -Destination $proxy -Force
            Write-Host "Original proxy restored; backup retained at $backup"
        } else {
            Remove-Item -LiteralPath $proxy
            Write-Host 'Temporary proxy removed.'
        }
    }
    if ($stageCreated -and (Test-Path -LiteralPath $zip -PathType Leaf)) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
