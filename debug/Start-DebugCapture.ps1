[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Label = '',
    [string]$Output = '',
    [string]$RenderSetting = '',
    [switch]$InstallProxy,
    [switch]$CollectOnly,
    [switch]$ShaderDisasm,
    [ValidateRange(0, 1000000)][int]$AutoResolve = 0,
    [switch]$ExitAfterCapture,
    [ValidateRange(10, 1800)][int]$WaitSeconds = 180,
    [string]$Destination = (Join-Path $PSScriptRoot 'debug-captures')
)
$ErrorActionPreference = 'Stop'
if ($ExitAfterCapture -and (!$AutoResolve -or $CollectOnly)) {
    throw '-ExitAfterCapture requires -AutoResolve and a new game launch.'
}
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
$autoWaitExpired = $false
function FileHashOrNull([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
    return $null
}
function Get-AutomaticCapture([string]$Content, [int]$RequestedResolve) {
    if (!$RequestedResolve) { return }
    $begin = [regex]::Match($Content, "(?m)^DEBUG AUTO_BEGIN id=(\d+) requestedResolve=$RequestedResolve\r?$")
    if (!$begin.Success) { return }
    $id = $begin.Groups[1].Value
    $end = [regex]::Match($Content, "(?m)^DEBUG END id=$id reason=(\w+) resolves=(\d+) lastResolve=\d+\r?$")
    if ($end.Success) {
        [pscustomobject]@{ id = [int]$id; reason = $end.Groups[1].Value; resolves = [int]$end.Groups[2].Value }
    }
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
        foreach ($key in @('AZIMUTH_SR_MODE','AZIMUTH_DEBUG_RENDERDOC','AZIMUTH_DEBUG_SHADER_DISASM','AZIMUTH_DEBUG_AUTO_RESOLVE')) {
            $oldEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
        }
        [Environment]::SetEnvironmentVariable('AZIMUTH_SR_MODE','debug','Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_RENDERDOC','0','Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_SHADER_DISASM',$(if($ShaderDisasm){'1'}else{'0'}),'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_AUTO_RESOLVE',[string]$AutoResolve,'Process')
        if($ShaderDisasm){Write-Host 'Selected shader assembly is included in the ZIP; review before sharing privately.'}
        if ($AutoResolve) {
            Write-Host "Enter real gameplay before resolve $AutoResolve. The trace starts automatically."
        } else {
            Write-Host 'Enter real gameplay. Press F8 once while driving. Repeat after the bounded capture for additional samples.'
        }
        Write-Host 'This launcher collects CPU metadata only. To capture GPU frames, use the RenderDoc procedure and -CollectOnly.'
        $started = [DateTime]::UtcNow
        if ($ExitAfterCapture) {
            $proc = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $finished = $null
            while (!$proc.WaitForExit(250) -and $timer.Elapsed.TotalSeconds -lt $WaitSeconds) {
                $item = Get-Item -LiteralPath $log -ErrorAction SilentlyContinue
                if ($item -and $item.LastWriteTimeUtc -ge $started.AddSeconds(-2) -and $item.LastWriteTimeUtc -ne $previous) {
                    $live = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
                    if ($live) { $finished = Get-AutomaticCapture $live $AutoResolve }
                    if ($finished) { break }
                }
            }
            if (!$proc.HasExited) {
                $autoWaitExpired = !$finished
                $null = $proc.CloseMainWindow()
                if (!$proc.WaitForExit(15000)) {
                    throw 'Azimuth did not close normally. Close it and rerun with -CollectOnly to package the log.'
                }
            }
        } else {
            $proc = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru -Wait
        }
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
    $autoCapture = Get-AutomaticCapture $content $AutoResolve
    New-Item -ItemType Directory -Path $stage | Out-Null
    $stageCreated = $true
    Copy-Item -LiteralPath $log -Destination (Join-Path $stage 'azimuth_dlaa.log')
    $shaderFiles = @()
    if($ShaderDisasm){
        $rows = [regex]::Matches($content,'(?m)^DEBUG SHADER_DUMP directory=(azimuth_shader_debug_\d+_\d+) file=([pv]s-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{8}){3}\.asm\.txt) bytes=(\d+)\r?$')
        foreach($row in $rows){
            if($shaderFiles.Count -ge 16){break}
            $folder = Join-Path $game $row.Groups[1].Value
            $name = $row.Groups[2].Value
            $source = Join-Path $folder $name
            if(!(Test-Path -LiteralPath $source -PathType Leaf)){continue}
            $dirInfo = Get-Item -LiteralPath $folder
            $fileInfo = Get-Item -LiteralPath $source
            if(($dirInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($fileInfo.Attributes -band [IO.FileAttributes]::ReparsePoint)){continue}
            if($fileInfo.Length -lt 1 -or $fileInfo.Length -gt 1048576 -or [string]$fileInfo.Length -ne $row.Groups[3].Value){continue}
            if($shaderFiles -contains $name){continue}
            $shaderDest = Join-Path $stage 'shader-disassembly'
            New-Item -ItemType Directory -Path $shaderDest -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination (Join-Path $shaderDest $name)
            $shaderFiles += $name
        }
        Write-Host "Selected shader disassembly files: $($shaderFiles.Count)"
        if(!$shaderFiles.Count){Write-Warning 'No disassembly exported. Check DEBUG SHADER_DUMP lines.'}
    }
    $meta = [ordered]@{
        label = $Label; expected_output = $Output; actual_render_setting_label = $RenderSetting
        utc = [DateTime]::UtcNow.ToString('o'); mode = 'debug'; collect_only = [bool]$CollectOnly
        process_exit_code = $exitCode; f8_begins = $begins; f8_ends = $ends
        automatic_resolve = $AutoResolve; automatic_capture = $autoCapture
        automatic_wait_expired = $autoWaitExpired
        files_sha256 = [ordered]@{ 'azimuth.exe' = (FileHashOrNull $exe); 'dxgi.dll' = (FileHashOrNull $proxy) }
        selected_shader_disassembly = $shaderFiles
        notice = 'Local evidence. Optional selected shader disassembly if requested; no game binaries, saves or raw GPU captures. Review privately.'
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
    if ($AutoResolve -and ($autoCapture.reason -ne 'resolve_limit' -or $autoWaitExpired -or ($null -ne $exitCode -and $exitCode -ne 0))) {
        throw 'Automatic capture did not finish successfully. Review the saved evidence.'
    }
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
