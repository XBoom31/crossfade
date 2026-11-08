# Windows desktop-integration smoke test.
#
# Drives build/test_win_platform.exe (built by the CMake test target) and
# verifies the two Windows integrations end-to-end through the same APIs the
# shell uses:
#
#   * System Media Transport Controls session appears in
#     GlobalSystemMediaTransportControlsSessionManager with the published
#     title/artist/album and a usable timeline.
#   * Pause / Play / Next / Previous / seek commands sent by the shell reach
#     the app (checked in the harness command log).
#   * The notification platform accepted the toast.
#
# Usage (from the repo root, after a CMake build):
#     powershell -ExecutionPolicy Bypass -File scripts/windows-platform-smoke.ps1
#     powershell ... -Exe build\test_win_platform.exe -Seconds 20

param(
    [string]$Exe = "",
    [int]$Seconds = 25
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $candidates = @(
        (Join-Path $root 'build\test_win_platform.exe'),
        (Join-Path $root 'build-win\test_win_platform.exe')
    )
    $Exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Exe -or -not (Test-Path $Exe)) {
    throw "test_win_platform.exe not found; build the test target first (cmake --build build)"
}

$stem = [System.IO.Path]::GetFileNameWithoutExtension($Exe)
$log = Join-Path $env:TEMP "$stem-commands.log"
$out = Join-Path $env:TEMP "$stem-output.log"
Remove-Item $log, $out -ErrorAction SilentlyContinue

function Await-WinRt($asyncOp, $resultType) {
    $methods = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    }
    $task = $methods[0].MakeGenericMethod($resultType).Invoke($null, @($asyncOp))
    $task.Wait(-1) | Out-Null
    return $task.Result
}

function Fail($message) {
    Write-Host "FAIL: $message" -ForegroundColor Red
    exit 1
}

$process = Start-Process -FilePath $Exe `
    -ArgumentList @("--seconds=$Seconds", "--log=$log") `
    -RedirectStandardOutput $out -PassThru

try {
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $out) -and ((Get-Content $out -Raw) -match 'READY')) { break }
        Start-Sleep -Milliseconds 200
    }
    if (-not ((Get-Content $out -Raw -ErrorAction SilentlyContinue) -match 'READY')) {
        Get-Content $out -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        Fail 'harness never reached READY'
    }

    [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType=WindowsRuntime] | Out-Null
    $manager = Await-WinRt ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) `
        ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

    $session = $null
    for ($i = 0; $i -lt 20 -and -not $session; $i++) {
        $session = $manager.GetSessions() |
            Where-Object { $_.SourceAppUserModelId -like "*$stem*" } |
            Select-Object -First 1
        if (-not $session) { Start-Sleep -Milliseconds 250 }
    }
    if (-not $session) { Fail "SMTC session for $stem not found" }

    $info = $session.GetPlaybackInfo()
    $timeline = $session.GetTimelineProperties()
    $propsType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType=WindowsRuntime]
    $props = Await-WinRt ($session.TryGetMediaPropertiesAsync()) $propsType

    Write-Host ("SMTC session: " + $session.SourceAppUserModelId)
    Write-Host ("  title:    " + $props.Title)
    Write-Host ("  artist:   " + $props.Artist)
    Write-Host ("  album:    " + $props.AlbumTitle)
    Write-Host ("  status:   " + $info.PlaybackStatus)
    Write-Host ("  timeline: " + $timeline.Position + " / " + $timeline.EndTime)

    if ($props.Title -ne 'Crossfade Smoke Test') { Fail 'unexpected title' }
    if ($props.Artist -ne 'Platform Test Artist') { Fail 'unexpected artist' }
    if ($props.AlbumTitle -ne 'Platform Test Album') { Fail 'unexpected album' }
    if ($info.PlaybackStatus -ne [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionPlaybackStatus]::Playing) {
        Fail 'playback status is not Playing'
    }
    if ($timeline.EndTime.TotalSeconds -lt 1) { Fail 'timeline was not published' }

    function Expect-Command($name) {
        for ($i = 0; $i -lt 20; $i++) {
            if ((Test-Path $log) -and ((Get-Content $log) -match "CMD $name")) {
                Write-Host "  command ok: $name"
                return
            }
            Start-Sleep -Milliseconds 150
        }
        Fail "command $name was not delivered"
    }

    if (-not (Await-WinRt ($session.TryPauseAsync()) ([bool]))) { Fail 'TryPauseAsync returned false' }
    Expect-Command 'Pause'
    if (-not (Await-WinRt ($session.TryPlayAsync()) ([bool]))) { Fail 'TryPlayAsync returned false' }
    Expect-Command 'Play'
    if (-not (Await-WinRt ($session.TrySkipNextAsync()) ([bool]))) { Fail 'TrySkipNextAsync returned false' }
    Expect-Command 'Next'
    if (-not (Await-WinRt ($session.TrySkipPreviousAsync()) ([bool]))) { Fail 'TrySkipPreviousAsync returned false' }
    Expect-Command 'Previous'
    if (-not (Await-WinRt ($session.TryChangePlaybackPositionAsync([long](120 * 10000000))) ([bool]))) {
        Fail 'TryChangePlaybackPositionAsync returned false'
    }
    Expect-Command 'SeekTo'
    Start-Sleep -Milliseconds 400
    $timeline = $session.GetTimelineProperties()
    if ($timeline.Position.TotalSeconds -lt 110) { Fail 'timeline did not follow the seek' }
    Write-Host ("  timeline after seek: " + $timeline.Position)

    $harness = Get-Content $out -Raw
    if ($harness -notmatch 'toasts_shown=([0-9]+)' -or [int]$matches[1] -lt 1) {
        Fail 'the notification platform did not accept a toast'
    }
    Write-Host ("  toasts accepted: " + $matches[1])

    Write-Host 'WINDOWS PLATFORM SMOKE: PASS' -ForegroundColor Green
}
finally {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
}
