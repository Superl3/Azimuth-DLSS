[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Label = '',
    [string]$Output = '',
    [string]$RenderSize = '',
    [ValidateSet('default','J','K','L','M')][string]$Preset = 'default',
    [switch]$NoJitter,
    [switch]$NRGuideProbe,
    [switch]$NRTemporalProbe,
    [ValidateSet('native','delta')][string]$HUD = 'native',
    [ValidateSet(0,1)][int]$DepthInverted=0,
    [ValidateSet(0,1)][int]$MVJittered=0,
    [double]$MVScaleX=1,
    [double]$MVScaleY=1,
    [double]$PreExposure=1,
    [double]$ExposureScale=1,
    [string]$LodBias='off',
    [string]$RenderSetting = '',
    [switch]$InstallProxy,
    [ValidateSet('Auto','Direct','ReShade')][string]$ProxyMode='Auto',
    [string]$NRLogPath='',
    [string]$RenderConfigPath='',
    [ValidateRange(0,4320)][int]$RenderHeight=0,
    [switch]$CollectOnly,
    [switch]$ShaderDisasm,
    [switch]$Preview,
    [switch]$PreviewPostFx,
    [switch]$PreviewJitter,
    [switch]$PreviewReplay,
    [switch]$PreviewSRReplay,
    [switch]$VisibleSR,
    [switch]$VisibleHDR,
    [switch]$HDRLive,
    [switch]$PresentTrace,
    [switch]$ProducerGpuWait,
    [switch]$VisibleMarker,
    [ValidateRange(10,120)][int]$VisibleSeconds = 10,
    [switch]$AutoTest,
    [ValidateRange(0,12)][int]$ProducerWaitMs = 0,
    [ValidateRange(500,15000)][int]$AutoWarmupMs = 2000,
    [string]$ReadyFile = '',
    [ValidateRange(0,10)][int]$PostRollSeconds = 2,
    [string]$PresentMonCsv,
    [ValidateSet('quality','balanced','performance','ultraperformance','auto')][string]$Quality = 'quality',
    [ValidateRange(0, 1000000)][int]$AutoResolve = 0,
    [switch]$ExitAfterCapture,
    [ValidateRange(10, 1800)][int]$WaitSeconds = 180,
    [string]$Destination = (Join-Path $PSScriptRoot 'debug-captures')
)
$ErrorActionPreference = 'Stop'
if($LodBias -notin @('off','auto')){
    $lodNumber=0.0
    if(![double]::TryParse($LodBias,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$lodNumber) -or [double]::IsNaN($lodNumber) -or $lodNumber -lt -2.5 -or $lodNumber -gt 0){throw 'LodBias must be off, auto or a finite number in -2.5..0.'}
}
if($HDRLive -and !$VisibleHDR){throw '-HDRLive requires -VisibleHDR.'}
if($VisibleHDR){$VisibleSR=$true}
# Explicit automatic test preset: no debug hotkeys, no install prompts.
# The caller controls actual game/monitor settings and any game menus.
if($AutoTest){
    if(!$Output -or (!$RenderSize -and $Output -notin @('1920x1080','3840x1600'))){throw '-AutoTest requires measured -Output, with -RenderSize for other resolutions.'}
    if(!$CollectOnly -and !$InstallProxy){throw '-AutoTest requires explicit -InstallProxy consent for temporary DLL installation.'}
    $Preview=$true; $PreviewPostFx=$true; $PreviewReplay=$true; $PreviewSRReplay=$true
    $PreviewJitter=!$NoJitter; $ShaderDisasm=$true
    if(!$CollectOnly){$ExitAfterCapture=$true}
    if(!$PSBoundParameters.ContainsKey('ProducerWaitMs')){$ProducerWaitMs=8}
    if(!$Label){$Label='FH2-auto-'+$Output}
    if(!$RenderSetting){$RenderSetting=$(if($RenderSize){"$RenderSize expected (verify measured size)"}elseif($Output -eq '1920x1080'){'1280x720 expected (verify measured size)'}else{'2592x1080 expected (verify measured size)'})}
    if(!$GameDir -and !(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'azimuth.exe') -PathType Leaf)){throw '-AutoTest requires -GameDir when launched outside the game folder.'}
}
if($ReadyFile -and (!$AutoTest -or $CollectOnly)){throw '-ReadyFile requires a new -AutoTest launch.'}
if($ReadyFile){
    $ReadyFile=[IO.Path]::GetFullPath($ReadyFile)
    if($ReadyFile.Length -ge 260 -or (Test-Path -LiteralPath $ReadyFile) -or !(Test-Path -LiteralPath (Split-Path -Parent $ReadyFile) -PathType Container)){
        throw '-ReadyFile must be a new, absent file in an existing directory (path under 260 characters).'
    }
}
if(($PreviewPostFx -or $PreviewJitter -or $PreviewReplay) -and !$Preview){throw 'Preview options require -Preview.'}
if($VisibleMarker -and !$VisibleSR){throw '-VisibleMarker requires -VisibleSR.'}
if($PSBoundParameters.ContainsKey('VisibleSeconds') -and !$VisibleSR){throw '-VisibleSeconds requires -VisibleSR.'}
if($NoJitter){$PreviewJitter=$false}
if($VisibleSR -and (!$PreviewSRReplay -or (!$PreviewJitter -and !$NoJitter))){throw '-VisibleSR requires -PreviewSRReplay and either -PreviewJitter or -NoJitter.'}
if($VisibleSR -and $ExitAfterCapture -and !$AutoTest){throw 'For automatic VisibleSR shutdown use -AutoTest; otherwise omit -ExitAfterCapture.'}
if($NRTemporalProbe){
    if(!$VisibleSR -or ($VisibleHDR -and !$HDRLive)){throw '-NRTemporalProbe requires live VisibleSR or VisibleHDR + HDRLive.'}
    $NRGuideProbe=$true
}
if($NRGuideProbe -and !$Preview){throw '-NRGuideProbe requires -Preview or -AutoTest.'}
if($PreviewSRReplay -and !$PreviewReplay){throw '-PreviewSRReplay requires -PreviewReplay.'}
if($ProducerGpuWait -and !$PreviewSRReplay){throw '-ProducerGpuWait requires -PreviewSRReplay.'}
if($PresentTrace -and !$VisibleSR){throw '-PresentTrace requires -VisibleSR or -VisibleHDR.'}
if($PreviewReplay -and !$PreviewPostFx){throw '-PreviewReplay requires -PreviewPostFx.'}
if ($ExitAfterCapture -and ((!$AutoResolve -and !$AutoTest) -or $CollectOnly)) {
    throw '-ExitAfterCapture requires -AutoResolve or -AutoTest and a new game launch.'
}
if (!$GameDir) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'azimuth.exe') -PathType Leaf) { $GameDir = $PSScriptRoot }
    else { $GameDir = (Read-Host 'Path to the Azimuth folder containing azimuth.exe').Trim('"') }
}
$game = (Resolve-Path -LiteralPath $GameDir).Path
$exe = Join-Path $game 'azimuth.exe'
$frontProxy = Join-Path $game 'dxgi.dll'
$chainProxy = Join-Path $game 'dxgi-dlaa.dll'
$chainExists = Test-Path -LiteralPath $chainProxy -PathType Leaf
$frontExists = Test-Path -LiteralPath $frontProxy -PathType Leaf
$selectedProxyMode = $ProxyMode
if($ProxyMode -eq 'Auto'){
    if($chainExists){
        if(!$frontExists){throw 'Incomplete ReShade chain: dxgi-dlaa.dll exists but dxgi.dll is absent.'}
        $selectedProxyMode='ReShade'
    }elseif(!$CollectOnly -and (Test-Path -LiteralPath (Join-Path $game 'ReShade.ini') -PathType Leaf)){
        throw 'ReShade configuration exists but dxgi-dlaa.dll is absent. Verify chain; explicit Direct is for a deliberate standalone test.'
    }else{$selectedProxyMode='Direct'}
}
if($selectedProxyMode -eq 'ReShade' -and (!$frontExists -or !$chainExists)){throw 'ReShade mode requires both existing dxgi.dll and dxgi-dlaa.dll.'}
$proxyName = if($selectedProxyMode -eq 'ReShade'){'dxgi-dlaa.dll'}else{'dxgi.dll'}
$proxy = Join-Path $game $proxyName
if(!$CollectOnly -and $selectedProxyMode -eq 'ReShade'){
    foreach($file in @($frontProxy,$chainProxy)){
        if((Get-Item -LiteralPath $file).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing reparse-point DLL in ReShade chain.'}
    }
}
if($NRLogPath){$NRLogPath=[IO.Path]::GetFullPath($NRLogPath)}
if([bool]$RenderConfigPath -ne [bool]$RenderHeight){throw '-RenderConfigPath and -RenderHeight must be supplied together.'}
if($RenderConfigPath){
    if($CollectOnly -or !$InstallProxy -or $RenderHeight -lt 720){throw 'Render config editing requires new launch, -InstallProxy and height 720..4320.'}
    $RenderConfigPath=(Resolve-Path -LiteralPath $RenderConfigPath).Path
    $fi=Get-Item -LiteralPath $RenderConfigPath
    if($fi.PSIsContainer -or $fi.Length -gt 1048576 -or ($fi.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Render config must be a regular file <=1 MiB.'}
    if($RenderConfigPath -in @($frontProxy,$chainProxy,$exe,(Join-Path $game 'azimuth_ngx.dll'),(Join-Path $game 'azimuth_sr.ini'),(Join-Path $game 'nvngx_dlss.dll'))){throw 'Render config must be a separate per-game settings file.'}
}
Write-Host "Proxy mode: $selectedProxyMode; target: $proxyName"
$log = Join-Path $game 'azimuth_dlaa.log'
$artifact = Join-Path $PSScriptRoot 'dxgi.dll'
$bridge = Join-Path $game 'azimuth_ngx.dll'
$bridgeArtifact = Join-Path $PSScriptRoot 'azimuth_ngx.dll'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'azimuth.exe not found' }
if (Get-Process -Name azimuth -ErrorAction SilentlyContinue) { throw 'Close existing Azimuth instances before launching or collecting.' }
if (!$Label) { $Label = Read-Host 'Capture label (for example FH2-native-HUD-on)' }
if ($Label -notmatch '^[A-Za-z0-9_-]{1,48}$') { throw 'Label must contain 1-48 letters, numbers, underscores or hyphens.' }
if (!$Output) { $Output = Read-Host 'Expected display resolution, e.g. 3840x1600 (or unknown)' }
if ($Output -ne 'unknown' -and $Output -notmatch '^[1-9]\d*x[1-9]\d*$') { throw 'Output must be WxH or unknown.' }
if($Preview -and $Output -eq 'unknown'){throw 'Preview requires an explicit higher output resolution.'}
if($RenderSize -eq 'auto'){
    if(!$PreviewReplay -or $Output -notmatch '^([1-9]\d{2,3})x([1-9]\d{2,3})$'){throw '-RenderSize auto requires preview replay and explicit output WxH.'}
    if([int]$Matches[1] -gt 3840 -or [int]$Matches[2] -gt 2160 -or [int]$Matches[1] -lt 320 -or [int]$Matches[2] -lt 200){throw 'Unsupported automatic output size.'}
}elseif($RenderSize){
    if(!$PreviewReplay -or $RenderSize -notmatch '^([1-9]\d{2,3})x([1-9]\d{2,3})$'){throw '-RenderSize requires preview replay and explicit WxH.'}
    $rw=[int]$Matches[1];$rh=[int]$Matches[2]
    if($Output -notmatch '^([1-9]\d{2,3})x([1-9]\d{2,3})$'){throw 'Explicit profile requires output WxH.'}
    $ow=[int]$Matches[1];$oh=[int]$Matches[2]
    if($rw -lt 320 -or $rh -lt 200 -or $ow -gt 3840 -or $oh -gt 2160 -or $rw -ge $ow -or $rh -ge $oh -or $rw*3 -lt $ow -or $rh*3 -lt $oh -or [math]::Abs(($rw*$oh)/($ow*$rh)-1) -gt 0.01){throw 'Unsupported fixed profile dimensions or aspect.'}
}elseif($PreviewReplay -and $Output -notin @('1920x1080','3840x1600')){throw 'Use -RenderSize with other output resolutions.'}
if (!$RenderSetting) { $RenderSetting = Read-Host 'Actual Azimuth render setting (native/low/etc.; this is a label only)' }
$dest = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $dest -Force | Out-Null
$tag = $Label + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$stage = Join-Path $dest ($tag + '.tmp')
$zip = Join-Path $dest ($tag + '.zip')
$backup = $null
$installedHash = $null
$installed = $false
$bridgeBackup = $null
$bridgeHash = $null
$bridgeInstalled = $false
$oldEnv = @{}
$stageCreated = $false
$exitCode = $null
$autoWaitExpired = $false
$automaticCompletionSeen = $false
$automaticCloseRequested = $false
$runPid = $null
function FileHashOrNull([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
    return $null
}
$frontInitialHash=FileHashOrNull $frontProxy
$proxyInitialHash=FileHashOrNull $proxy
$bridgeInitialHash=FileHashOrNull $bridge
$nrInitialHash=$null
$nrPackaged=$false
$configBackup=$null
$configOriginalHash=$null
$configInstalledHash=$null
$configInstalled=$false
$configOriginalHeight=$null
if($NRLogPath -and (Test-Path -LiteralPath $NRLogPath -PathType Leaf)){
    $nrFile=Get-Item -LiteralPath $NRLogPath
    if(($nrFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $nrFile.Length -gt 134217728){throw 'NR log must be a regular file <=128 MiB.'}
    $nrInitialHash=FileHashOrNull $NRLogPath
}
# Only edit a single explicit decimal resolution= entry. Preserve all other
# UTF-8 bytes (including BOM/CRLF); never guess XML or a binary format.
function Get-RenderConfigEdit([byte[]]$Bytes,[int]$Height){
    if($Height -lt 720 -or $Height -gt 4320 -or $Bytes.Length -gt 1048576){throw 'Invalid render height or config size.'}
    $utf8=[Text.UTF8Encoding]::new($false,$true)
    $source=$utf8.GetString($Bytes)
    if($source.Contains([char]0)){throw 'Binary/UTF-16 render config is unsupported.'}
    $hits=[regex]::Matches($source,'(?mi)^([\uFEFF \t]*resolution[ \t]*=[ \t]*)([0-9]+)([ \t]*(?:[;#][^\r\n]*)?)(?=\r?$)')
    if($hits.Count -ne 1){throw 'Expected exactly one decimal resolution= line; supply a verified per-game config, not a guessed format.'}
    $m=$hits[0].Groups[2]
    $old=[int]$m.Value
    $next=$source.Substring(0,$m.Index)+$Height.ToString([Globalization.CultureInfo]::InvariantCulture)+$source.Substring($m.Index+$m.Length)
    [pscustomobject]@{ OriginalHeight=$old; Bytes=$utf8.GetBytes($next) }
}
function Get-AutomaticCapture([string]$Content, [int]$RequestedResolve) {
    if (!$RequestedResolve -and !$AutoTest) { return }
    $begin = [regex]::Match($Content, "(?m)^DEBUG AUTO_BEGIN id=(\d+) requestedResolve=$RequestedResolve\r?$")
    if (!$begin.Success) { return }
    $id = $begin.Groups[1].Value
    $end = [regex]::Match($Content, "(?m)^DEBUG END id=$id reason=(\w+) resolves=(\d+) lastResolve=\d+\r?$")
    if ($end.Success) {
        [pscustomobject]@{ id = [int]$id; reason = $end.Groups[1].Value; resolves = [int]$end.Groups[2].Value }
    }
}
# Keep only the completion state and a partial line between polls. Limit
# each poll so sustained log writes cannot delay the timeout check.
function Read-AutoCompletion([IO.StreamReader]$Reader, [char[]]$Buffer, [hashtable]$Progress, [int]$RequestedResolve) {
    $markerPattern = '(?m)^(?:DEBUG AUTO_BEGIN |DEBUG END |PREVIEW COMPLETE |VISIBLE TRIAL_END )[^\r\n]*(?=\r?\n)'
    for ($i = 0; $i -lt 64; $i++) {
        $count = $Reader.ReadBlock($Buffer, 0, $Buffer.Length)
        if (!$count) { break }
        $text = $Progress.PartialLine + [string]::new($Buffer, 0, $count)
        $tail = $text.Substring($text.LastIndexOf([char]10) + 1)
        # Native log lines are limited to 1024 bytes. Keep an oversized
        # fragment ineligible for a start-of-line match on the next poll.
        $Progress.PartialLine = if ($tail.Length -le 1024) { $tail } else { '!' }
        foreach ($marker in [regex]::Matches($text, $markerPattern)) {
            $line = $marker.Value
            if (!$Progress.Id -and $line -match "^DEBUG AUTO_BEGIN id=(\d+) requestedResolve=$RequestedResolve$") {
                $Progress.Id = $Matches[1]
            } elseif ($Progress.Id -and !$Progress.Capture -and $line -match "^DEBUG END id=$($Progress.Id) reason=(\w+) resolves=(\d+) lastResolve=\d+$") {
                $Progress.Capture = [pscustomobject]@{ id = [int]$Progress.Id; reason = $Matches[1]; resolves = [int]$Matches[2] }
            } elseif ($line.StartsWith('PREVIEW COMPLETE ', [StringComparison]::Ordinal)) {
                $Progress.PreviewComplete = $true
            } elseif ($line.StartsWith('VISIBLE TRIAL_END ', [StringComparison]::Ordinal)) {
                $Progress.VisibleComplete = $true
            }
        }
        # Markers allow normal shutdown, including when the completed
        # capture reports native fallback or only partial RAW evidence.
        if ($Progress.Capture -and (!$Preview -or $Progress.PreviewComplete) -and (!$VisibleSR -or $Progress.VisibleComplete)) {
            return $Progress.Capture
        }
        if ($count -lt $Buffer.Length) { break }
    }
}
try {
    if (!$CollectOnly) {
        if($RenderConfigPath){
            $configOriginalHash=FileHashOrNull $RenderConfigPath
            $edit=Get-RenderConfigEdit ([IO.File]::ReadAllBytes($RenderConfigPath)) $RenderHeight
            $configOriginalHeight=$edit.OriginalHeight
            $configBackup=Join-Path $dest ($tag+'-original-render-config.bak')
            Copy-Item -LiteralPath $RenderConfigPath -Destination $configBackup
            if((FileHashOrNull $configBackup) -ne $configOriginalHash -or (FileHashOrNull $RenderConfigPath) -ne $configOriginalHash){throw 'Render config changed during backup.'}
            $temp=Join-Path $dest ($tag+'-requested-render-config.tmp')
            [IO.File]::WriteAllBytes($temp,[byte[]]$edit.Bytes)
            $configInstalledHash=FileHashOrNull $temp
            if($configInstalledHash -ne $configOriginalHash){
                Copy-Item -LiteralPath $temp -Destination $RenderConfigPath -Force
                $configInstalled=$true
                if((FileHashOrNull $RenderConfigPath) -ne $configInstalledHash){throw 'Render config edit verification failed.'}
            }
            Remove-Item -LiteralPath $temp
            Write-Host "Requested Azimuth resolution=$RenderHeight (was $configOriginalHeight); actual GPU size must be measured."
        }
        if($Preview){
            if(!(Test-Path -LiteralPath $bridgeArtifact -PathType Leaf) -or !(Test-Path -LiteralPath $artifact -PathType Leaf)){throw 'Preview requires the full azimuth-sr-experimental-windows-x64 artifact (matching bridge and proxy).'}
            if(!(Test-Path -LiteralPath (Join-Path $game 'nvngx_dlss.dll') -PathType Leaf)){throw 'Preview requires your existing NVIDIA nvngx_dlss.dll runtime.'}
            $bridgeHash = FileHashOrNull $bridgeArtifact
            $oldBridgeHash = FileHashOrNull $bridge
            if($oldBridgeHash -ne $bridgeHash){
                if(!$InstallProxy){
                    $reply=Read-Host 'Temporarily install matching NGX bridge? Existing bridge will be backed up and restored. [y/N]'
                    if($reply -notmatch '^[Yy]$'){throw 'Bridge install declined.'}
                }
                if($oldBridgeHash){
                    $bridgeBackup=Join-Path $dest ($tag+'-original-azimuth_ngx.dll')
                    Copy-Item -LiteralPath $bridge -Destination $bridgeBackup
                    if((FileHashOrNull $bridgeBackup) -ne $oldBridgeHash){throw 'Bridge backup verification failed.'}
                }
                Copy-Item -LiteralPath $bridgeArtifact -Destination $bridge
                $bridgeInstalled=$true
                if((FileHashOrNull $bridge) -ne $bridgeHash){throw 'Bridge copy verification failed.'}
            }
        }
        $currentHash = FileHashOrNull $proxy
        $artifactHash = FileHashOrNull $artifact
        if ($artifactHash -and $artifactHash -ne $currentHash) {
            if (!$InstallProxy) {
                $reply = Read-Host "Temporarily install debug proxy $proxyName? Existing proxy will be backed up and restored after exit. [y/N]"
                if ($reply -notmatch '^[Yy]$') { throw 'Install declined. No game DLL changed.' }
            }
            if ($currentHash) {
                $backup = Join-Path $dest ($tag + '-original-' + $proxyName)
                Copy-Item -LiteralPath $proxy -Destination $backup
                if ((FileHashOrNull $backup) -ne $currentHash) { throw 'Original proxy backup verification failed.' }
            }
            Copy-Item -LiteralPath $artifact -Destination $proxy
            $installedHash = $artifactHash
            $installed = $true
            if ((FileHashOrNull $proxy) -ne $installedHash) { throw 'Debug proxy copy verification failed.' }
        }
        if (!(Test-Path -LiteralPath $proxy -PathType Leaf)) { throw "Install the latest debug proxy as $proxyName next to azimuth.exe first." }
        $previous = $null
        if (Test-Path -LiteralPath $log -PathType Leaf) {
            $previous = (Get-Item -LiteralPath $log).LastWriteTimeUtc
            Copy-Item -LiteralPath $log -Destination (Join-Path $dest ($tag + '-previous.log'))
        }
        foreach ($key in @('AZIMUTH_SR_MODE','AZIMUTH_DEBUG_RENDERDOC','AZIMUTH_DEBUG_SHADER_DISASM','AZIMUTH_DEBUG_AUTO_RESOLVE','AZIMUTH_DEBUG_AUTO_TEST','AZIMUTH_DEBUG_PRODUCER_WAIT_MS','AZIMUTH_DEBUG_PRODUCER_GPU_WAIT','AZIMUTH_DEBUG_PRESENT_TRACE','AZIMUTH_DEBUG_AUTO_WARMUP_MS','AZIMUTH_DEBUG_AUTO_READY_FILE','AZIMUTH_SR_OUTPUT','AZIMUTH_SR_RENDER','AZIMUTH_SR_PRESET','AZIMUTH_DEBUG_NR_GUIDES','AZIMUTH_DEBUG_NR_TEMPORAL','AZIMUTH_SR_HUD','AZIMUTH_SR_DEPTH_INVERTED','AZIMUTH_SR_MV_JITTERED','AZIMUTH_SR_MV_SCALE_X','AZIMUTH_SR_MV_SCALE_Y','AZIMUTH_SR_PRE_EXPOSURE','AZIMUTH_SR_EXPOSURE_SCALE','AZIMUTH_SR_LOD_BIAS','AZIMUTH_SR_QUALITY','AZIMUTH_DEBUG_PREVIEW_POSTFX','AZIMUTH_DEBUG_PREVIEW_JITTER','AZIMUTH_DEBUG_POSTFX_REPLAY','AZIMUTH_DEBUG_SR_REPLAY','AZIMUTH_DEBUG_VISIBLE_SR','AZIMUTH_DEBUG_VISIBLE_HDR','AZIMUTH_DEBUG_HDR_LIVE','AZIMUTH_DEBUG_VISIBLE_MARKER','AZIMUTH_DEBUG_VISIBLE_SECONDS','AZIMUTH_DEBUG_QUEUE_TRACE')) {
            $oldEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
        }
        [Environment]::SetEnvironmentVariable('AZIMUTH_SR_MODE',$(if($Preview){'preview'}else{'debug'}),'Process')
        if($Preview){
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_OUTPUT',$Output,'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_RENDER',$(if($RenderSize){$RenderSize}else{$null}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_PRESET',$Preset.ToLowerInvariant(),'Process')
            $culture=[Globalization.CultureInfo]::InvariantCulture
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_NR_GUIDES',$(if($NRGuideProbe){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_NR_TEMPORAL',$(if($NRTemporalProbe){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_HUD',$HUD.ToLowerInvariant(),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_DEPTH_INVERTED',[string]$DepthInverted,'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_MV_JITTERED',[string]$MVJittered,'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_MV_SCALE_X',$MVScaleX.ToString('R',$culture),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_MV_SCALE_Y',$MVScaleY.ToString('R',$culture),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_PRE_EXPOSURE',$PreExposure.ToString('R',$culture),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_EXPOSURE_SCALE',$ExposureScale.ToString('R',$culture),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_LOD_BIAS',$LodBias.ToLowerInvariant(),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_SR_QUALITY',$Quality,'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_PREVIEW_POSTFX',$(if($PreviewPostFx){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_PREVIEW_JITTER',$(if($PreviewJitter){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_POSTFX_REPLAY',$(if($PreviewReplay){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_SR_REPLAY',$(if($PreviewSRReplay){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_VISIBLE_SR',$(if($VisibleSR){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_VISIBLE_HDR',$(if($VisibleHDR){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_HDR_LIVE',$(if($HDRLive){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_VISIBLE_MARKER',$(if($VisibleMarker){'1'}else{'0'}),'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_VISIBLE_SECONDS',[string]$VisibleSeconds,'Process')
            [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_QUEUE_TRACE','1','Process')
        }
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_RENDERDOC','0','Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_SHADER_DISASM',$(if($ShaderDisasm){'1'}else{'0'}),'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_AUTO_RESOLVE',[string]$AutoResolve,'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_AUTO_TEST',$(if($AutoTest){'1'}else{'0'}),'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_PRODUCER_WAIT_MS',[string]$ProducerWaitMs,'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_PRODUCER_GPU_WAIT',$(if($ProducerGpuWait){'1'}else{'0'}),'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_PRESENT_TRACE',$(if($PresentTrace){'1'}else{'0'}),'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_AUTO_WARMUP_MS',[string]$AutoWarmupMs,'Process')
        [Environment]::SetEnvironmentVariable('AZIMUTH_DEBUG_AUTO_READY_FILE',$(if($ReadyFile){$ReadyFile}else{$null}),'Process')
        if($ShaderDisasm){Write-Host 'Selected shader assembly is included in the ZIP; review before sharing privately.'}
        if ($AutoTest) {
            Write-Host "Automatic test: no F8. Stable native scene + fresh SR for $AutoWarmupMs ms; minimum resolve $AutoResolve."
            if($ReadyFile){Write-Host "After navigating to gameplay, create this ready file: $ReadyFile"}
            Write-Host 'The game must reach the correct scene; this launcher does not navigate menus or drive the car.'
        } elseif ($AutoResolve) {
            Write-Host "Enter real gameplay before resolve $AutoResolve. The trace starts automatically."
        } else {
            Write-Host 'Enter real gameplay. Press F8 once while driving. Preview ignores early input before fresh scene readiness and all further F8 inputs after acceptance.'
        }
        if($Preview){
            if($VisibleHDR){Write-Warning "HDR SR alpha: exact native PQ shader replay; live=$([bool]$HDRLive); enable game HDR, verify output signal externally. Default is private probe only."}
            elseif($VisibleSR){Write-Warning "Visible SR SDR alpha: after capture start, up to $VisibleSeconds seconds of gated native final-target replacement. Native HUD pixels retained; SDR only: explicitly disable HDR in the game. No swapchain references are retained; actual Present needs external ETW or screen evidence."}
            else{Write-Host 'Preview captures offscreen SR and selected GPU textures; no visible SR replacement.'}
            if($PreviewJitter){Write-Warning 'Experimental viewport jitter can affect the displayed native fallback scene.'}
            if($ProducerGpuWait){Write-Warning 'Experimental deferred producer dependency: queue Wait at actual submission; exceptional failure can block for safe completion.'}
            if($PresentTrace){Write-Warning 'Experimental passive Present/Resize/ColorSpace vtable instrumentation; no retained interfaces, no FG or timing changes requested.'}
        }
        else{Write-Host 'This launcher collects CPU metadata only. To capture GPU frames, use the RenderDoc procedure and -CollectOnly.'}
        $started = [DateTime]::UtcNow
        if ($ExitAfterCapture) {
            $proc = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
            $runPid=$proc.Id
            Write-Host "Azimuth PID=$runPid"
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $finished = $null
            $postRollStart = $null
            $reader = $null
            $buffer = [char[]]::new(65536)
            $progress = @{ PartialLine = ''; Id = $null; Capture = $null; PreviewComplete = $false; VisibleComplete = $false }
            try {
                while (!$proc.WaitForExit(250) -and $timer.Elapsed.TotalSeconds -lt $WaitSeconds) {
                    if($null -ne $postRollStart){
                        if($timer.Elapsed.TotalSeconds-$postRollStart -ge $PostRollSeconds){break}
                        continue
                    }
                    if (!$reader) {
                        $item = Get-Item -LiteralPath $log -ErrorAction SilentlyContinue
                        if (!$item -or $item.LastWriteTimeUtc -lt $started.AddSeconds(-2) -or $item.LastWriteTimeUtc -eq $previous) { continue }
                        try {
                            $stream = [IO.File]::Open($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                            $reader = [IO.StreamReader]::new($stream)
                        } catch [IO.IOException] { continue }
                    }
                    $finished = Read-AutoCompletion $reader $buffer $progress $AutoResolve
                    if ($finished) {
                        $automaticCompletionSeen=$true
                        $postRollStart=$timer.Elapsed.TotalSeconds
                        Write-Host 'Capture completion markers observed; keeping the native post-roll before normal exit.'
                        if(!$AutoTest -or !$PostRollSeconds){break}
                    }
                }
            } finally {
                if ($reader) { $reader.Dispose() }
            }
            if (!$proc.HasExited) {
                $autoWaitExpired = !$finished -or ($AutoTest -and $null -ne $postRollStart -and $timer.Elapsed.TotalSeconds-$postRollStart -lt $PostRollSeconds)
                $automaticCloseRequested=$true
                $null = $proc.CloseMainWindow()
                if (!$proc.WaitForExit(15000)) {
                    throw 'Azimuth did not close normally. No force kill or DLL restore while running. Close it and rerun with -CollectOnly to package the log.'
                }
            }
        } else {
            $proc = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru -Wait
        }
        $runPid=$proc.Id
        $exitCode = $proc.ExitCode
        if (Get-Process -Name azimuth -ErrorAction SilentlyContinue) { throw 'Azimuth child is still running; exit and run with -CollectOnly.' }
        if (!(Test-Path -LiteralPath $log -PathType Leaf)) { throw 'No trace log produced.' }
        $stamp = (Get-Item -LiteralPath $log).LastWriteTimeUtc
        if ($stamp -lt $started.AddSeconds(-2) -or ($previous -and $stamp -eq $previous)) { throw 'Trace log is not fresh.' }
    }
    if (!(Test-Path -LiteralPath $log -PathType Leaf)) { throw 'No azimuth_dlaa.log found.' }
    $content = Get-Content -LiteralPath $log -Raw
    if ($content -notmatch 'DEBUG READY:') {
        if(!$AutoTest){throw 'This log is not from the debug build. Check the installed DLL and launch mode.'}
        Write-Warning 'Startup ended before DEBUG READY; preserving the fresh partial log.'
    }
    $begins = [regex]::Matches($content,'(?m)^DEBUG BEGIN ').Count
    $ends = [regex]::Matches($content,'(?m)^DEBUG END ').Count
    $autoCapture = Get-AutomaticCapture $content $AutoResolve
    New-Item -ItemType Directory -Path $stage | Out-Null
    $stageCreated = $true
    Copy-Item -LiteralPath $log -Destination (Join-Path $stage 'azimuth_dlaa.log')
    if($NRLogPath -and (Test-Path -LiteralPath $NRLogPath -PathType Leaf)){
        $nrFile=Get-Item -LiteralPath $NRLogPath
        if(!($nrFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -and $nrFile.Length -le 134217728){
            $nrHash=FileHashOrNull $NRLogPath
            if($CollectOnly -or $nrHash -ne $nrInitialHash){
                Copy-Item -LiteralPath $NRLogPath -Destination (Join-Path $stage 'nr-external.log')
                if((FileHashOrNull (Join-Path $stage 'nr-external.log')) -ne $nrHash){throw 'NR log changed during copy.'}
                $nrPackaged=$true
            }
        }
        if(!$nrPackaged){Write-Warning 'NR log unchanged, invalid or over budget; not packaged.'}
    }
    $shaderFiles = @()
    if($ShaderDisasm){
        $rows = [regex]::Matches($content,'(?m)^DEBUG SHADER_DUMP directory=(azimuth_shader_debug_\d+_\d+) file=([pv]s-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{8}){3}\.asm\.txt) bytes=(\d+)\r?$')
        foreach($row in $rows){
            if($shaderFiles.Count -ge 40){break}
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
    $presentMonPackaged=$false
    if($PresentMonCsv){
        $pm=Get-Item -LiteralPath $PresentMonCsv -ErrorAction SilentlyContinue
        if($pm -and !$pm.PSIsContainer -and !($pm.Attributes -band [IO.FileAttributes]::ReparsePoint) -and $pm.Length -gt 0 -and $pm.Length -le 67108864){
            try{
                Copy-Item -LiteralPath $pm.FullName -Destination (Join-Path $stage 'presentmon.csv')
                $presentMonPackaged=$true
            }catch{Write-Warning 'PresentMon CSV could not be copied; preserving render evidence.'}
        }else{Write-Warning 'PresentMon CSV missing or invalid; preserving in-process evidence.'}
    }
    $previewFiles=@()
    if($Preview){
        if($content -notmatch 'SR PREVIEW:' -and !$AutoTest){throw 'Log is not from preview mode.'}
        $total=0L
        $rawRows=[regex]::Matches($content,'(?m)^PREVIEW FILE directory=(azimuth_preview_\d+_\d+) file=(f\d+-(?:color|depth|mv|sr|preoverlay|composite|final|replay|srpostfx|visiblehud|visiblefinal|hdrscene|hudprobe|finalprobe|hdrsrscene|stencil|guidecb)-(\d+)x(\d+)-fmt(\d+)\.raw) bytes=(\d+)\r?$')
        foreach($row in $rawRows){
            if($previewFiles.Count -ge $(if($NRGuideProbe){34}elseif($VisibleHDR){28}else{22})){break}
            $folder=Join-Path $game $row.Groups[1].Value
            $name=$row.Groups[2].Value
            if($previewFiles -contains $name){continue}
            $w=[long]$row.Groups[3].Value;$h=[long]$row.Groups[4].Value;$size=[long]$row.Groups[6].Value
            if($w -lt 1 -or $w -gt 7680 -or $h -lt 1 -or $h -gt 4320 -or $size -ne ($w*$h*4) -or $size -gt 67108864 -or $total+$size -gt 268435456){continue}
            if([int]$row.Groups[5].Value -notin @(23,24,26,34,39,40,41)){continue}
            $source=Join-Path $folder $name
            if(!(Test-Path -LiteralPath $source -PathType Leaf)){continue}
            $dirInfo=Get-Item -LiteralPath $folder;$fileInfo=Get-Item -LiteralPath $source
            if(($dirInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($fileInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $fileInfo.Length -ne $size){continue}
            $imageDir=Join-Path $stage 'gpu-raw'
            New-Item -ItemType Directory -Path $imageDir -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination (Join-Path $imageDir $name)
            $previewFiles+=$name;$total+=$size
        }
        Write-Host "Fence-completed texture files: $($previewFiles.Count)"
        if(!$previewFiles.Count){Write-Warning 'No raw texture files; inspect PREVIEW log lines. Partial evidence will still be packaged.'}
    }
    $acceptedProfiles=@([regex]::Matches($content,'(?m)^SR PROFILE_ACCEPTED render=(\d+x\d+) output=(\d+x\d+) quality=(\d+) ') | ForEach-Object {
        [ordered]@{render=$_.Groups[1].Value;output=$_.Groups[2].Value;quality=[int]$_.Groups[3].Value}
    })
    $measuredFrames=@([regex]::Matches($content,'(?m)^SR frame: color=(\d+x\d+) depth=(\d+x\d+) mv=(\d+x\d+) resolve=(\d+x\d+) ') | ForEach-Object {
        "$($_.Groups[1].Value)/$($_.Groups[2].Value)/$($_.Groups[3].Value)/$($_.Groups[4].Value)"
    } | Sort-Object -Unique)
    $meta = [ordered]@{
        label = $Label; expected_output = $Output; actual_render_setting_label = $RenderSetting
        utc = [DateTime]::UtcNow.ToString('o'); mode = $(if($Preview){'preview'}else{'debug'}); collect_only = [bool]$CollectOnly
        process_exit_code = $exitCode; process_id = $runPid; f8_begins = $begins; f8_ends = $ends
        capture_begins = $begins; capture_ends = $ends; auto_test = [bool]$AutoTest
        auto_warmup_ms = $(if($AutoTest){$AutoWarmupMs}else{$null}); auto_gate_requested = [bool]$ReadyFile
        auto_gate_open = [bool]($content -match '(?m)^PREVIEW AUTO_GATE_OPEN ')
        auto_ready = [bool]($content -match '(?m)^PREVIEW AUTO_READY ')
        visible_hdr_requested = [bool]$VisibleHDR; hdr_live_requested = [bool]$HDRLive
        sr_render_requested = $RenderSize; sr_preset_requested = $Preset; sr_no_jitter_requested = [bool]$NoJitter
        nr_guide_probe_requested=[bool]$NRGuideProbe; nr_temporal_probe_requested=[bool]$NRTemporalProbe; sr_lod_bias_requested=$LodBias; sr_hud_requested=$HUD; sr_depth_inverted_requested=$DepthInverted; sr_mv_jittered_requested=$MVJittered; sr_mv_scale_requested=@($MVScaleX,$MVScaleY); sr_pre_exposure_requested=$PreExposure; sr_exposure_scale_requested=$ExposureScale
        present_trace_requested = [bool]$PresentTrace; producer_gpu_wait_requested = [bool]$ProducerGpuWait
        visible_trial_complete = [bool]($content -match '(?m)^VISIBLE TRIAL_END ')
        visible_seconds_requested = $(if($VisibleSR){$VisibleSeconds}else{$null})
        visible_duration_ms_observed = $(if($content -match '(?m)^VISIBLE CONFIG enabled=1 durationMs=(\d+) '){[int]$Matches[1]}else{$null})
        automatic_completion_seen = $automaticCompletionSeen; automatic_close_requested = $automaticCloseRequested
        post_roll_seconds = $(if($AutoTest){$PostRollSeconds}else{0}); producer_wait_ms = $ProducerWaitMs
        automatic_resolve = $AutoResolve; automatic_capture = $autoCapture
        automatic_wait_expired = $autoWaitExpired
        render_height_requested=$(if($RenderConfigPath){$RenderHeight}else{$null}); render_height_original=$configOriginalHeight
        render_config_original_sha256=$configOriginalHash; render_config_requested_sha256=$configInstalledHash
        accepted_sr_profiles=$acceptedProfiles; observed_color_depth_mv_resolve=$measuredFrames
        proxy_mode=$selectedProxyMode; proxy_target=$proxyName; front_proxy_original_sha256=$frontInitialHash
        front_proxy_preserved=$(if($selectedProxyMode -eq 'ReShade'){(FileHashOrNull $frontProxy) -eq $frontInitialHash}else{$null})
        nr_log_packaged=$nrPackaged; nr_log_scope='whole supplied log, may include prior sessions; correlate timestamps'
        files_sha256 = [ordered]@{ 'azimuth.exe' = (FileHashOrNull $exe); 'dxgi.dll' = (FileHashOrNull $frontProxy); 'dxgi-dlaa.dll' = (FileHashOrNull $chainProxy); 'azimuth_ngx.dll' = (FileHashOrNull $bridge) }
        selected_shader_disassembly = $shaderFiles
        gpu_raw = $previewFiles; preview_complete = [bool]($content -match '(?m)^PREVIEW COMPLETE '); preview_quality = $(if($Preview){$Quality}else{$null}); preview_jitter_requested = [bool]$PreviewJitter; preview_native_replay_requested = [bool]$PreviewReplay; preview_sr_replay_requested = [bool]$PreviewSRReplay; visible_sr_requested = [bool]$VisibleSR; visible_marker_requested = [bool]$VisibleMarker; presentmon_csv_packaged = $presentMonPackaged
        notice = 'Local evidence. Explicit preview includes selected raw game textures. Optional selected shader assembly. No game binaries or saves. Review privately.'
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
    if($Preview -and ($content -notmatch '(?m)^PREVIEW COMPLETE ' -or !$previewFiles.Count)){throw 'Preview did not collect GPU textures successfully. Review the saved evidence.'}
    if (($AutoResolve -or $AutoTest) -and ($autoCapture.reason -ne 'resolve_limit' -or $autoWaitExpired -or ($VisibleSR -and $content -notmatch '(?m)^VISIBLE TRIAL_END ') -or ($null -ne $exitCode -and $exitCode -ne 0))) {
        throw 'Automatic capture did not finish successfully. Review the saved evidence.'
    }
    if ($begins -eq 0) { Write-Warning 'No capture began. Inspect scene readiness, optional gate, and game startup evidence.' }
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
    if($bridgeInstalled){
        if((Get-Process -Name azimuth -ErrorAction SilentlyContinue) -or (FileHashOrNull $bridge) -ne $bridgeHash){
            Write-Warning "Bridge not restored. Backup: $bridgeBackup"
        }elseif($bridgeBackup){
            Copy-Item -LiteralPath $bridgeBackup -Destination $bridge -Force
        }else{Remove-Item -LiteralPath $bridge -Force}
    }
    if($configInstalled){
        if((Get-Process -Name azimuth -ErrorAction SilentlyContinue) -or (FileHashOrNull $RenderConfigPath) -ne $configInstalledHash){
            Write-Warning "Render config not restored (running or changed). Original backup: $configBackup"
        }else{
            Copy-Item -LiteralPath $configBackup -Destination $RenderConfigPath -Force
            if((FileHashOrNull $RenderConfigPath) -ne $configOriginalHash){Write-Warning "Render config restore hash mismatch. Backup: $configBackup"}
        }
    }
    if(!$CollectOnly){
        $restored=[ordered]@{
            render_config_original_sha256=$configOriginalHash
            render_config_after_sha256=$(if($RenderConfigPath){FileHashOrNull $RenderConfigPath}else{$null})
            proxy_mode=$selectedProxyMode;proxy_target=$proxyName
            front_sha256_before=$frontInitialHash;front_sha256_after=(FileHashOrNull $frontProxy)
            front_preserved=$(if($selectedProxyMode -eq 'ReShade'){(FileHashOrNull $frontProxy) -eq $frontInitialHash}else{$null})
            proxy_original_sha256=$proxyInitialHash;proxy_after_sha256=(FileHashOrNull $proxy)
            bridge_original_sha256=$bridgeInitialHash;bridge_after_sha256=(FileHashOrNull $bridge)
            game_running=[bool](Get-Process -Name azimuth -ErrorAction SilentlyContinue)
        }
        $restored | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $dest ($tag+'-restoration.json')) -Encoding UTF8
        if($selectedProxyMode -eq 'ReShade' -and !$restored.front_preserved){Write-Warning 'Front ReShade DLL changed externally; never overwrite it automatically.'}
    }
    if ($stageCreated -and (Test-Path -LiteralPath $zip -PathType Leaf)) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
