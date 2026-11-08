# crossfade Windows installer (per-user, no admin rights needed).
#
# Installs everything under %LOCALAPPDATA%\crossfade:
#   bin\crossfade.exe (+ bundled SDL2/mpv/MinGW runtime DLLs)
#   share\crossfade\p100-backend*.py + fonts
#   share\crossfade\venv\  (Python 3.12 + ytmusicapi + yt-dlp)
# and creates Start Menu shortcuts for the app and the login helper.
#
# It always pulls the newest published release, so re-running it upgrades an
# existing install in place.

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Release = "https://github.com/XBoom31/crossfade/releases/latest/download"
$Root    = "$env:LOCALAPPDATA\crossfade"
$Tmp     = "$env:TEMP\crossfade-install"
$PyDir   = "$env:LOCALAPPDATA\Programs\crossfade-python"
$PyExe   = "$PyDir\python.exe"

Write-Host ""
Write-Host "==== crossfade installer ====" -ForegroundColor Green

# ---- 1. App payload -------------------------------------------------------
Write-Host "[1/4] Downloading crossfade ..."
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null
try {
    Invoke-WebRequest -Uri "$Release/crossfade-windows.zip" -OutFile "$Tmp\app.zip"
} catch {
    Write-Host "Download failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Check your internet connection and try again."
    exit 1
}
Write-Host "[1/4] Unpacking ..."
Remove-Item "$Tmp\crossfade-windows" -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive "$Tmp\app.zip" -DestinationPath $Tmp -Force
$payload = "$Tmp\crossfade-windows"
if (-not (Test-Path "$payload\bin\crossfade.exe")) {
    Write-Host "Downloaded package is incomplete (crossfade.exe is missing)." -ForegroundColor Red
    exit 1
}
$newVer = Get-Content "$payload\version.txt" -ErrorAction SilentlyContinue
$oldVer = Get-Content "$Root\version.txt" -ErrorAction SilentlyContinue
if ($newVer) {
    if ($oldVer) {
        Write-Host "[1/4] Updating crossfade $oldVer -> $newVer ..."
    } else {
        Write-Host "[1/4] Installing crossfade $newVer ..."
    }
}

# A running copy locks crossfade.exe and its DLLs, so the in-place upgrade
# cannot replace them. Close the app (and its backend/mpv children) first.
$running = Get-Process -Name crossfade -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[1/4] Closing the running crossfade ..."
    & taskkill.exe /IM crossfade.exe /T /F 2>$null | Out-Null
    Start-Sleep -Milliseconds 700
}

New-Item -ItemType Directory -Force -Path $Root | Out-Null
try {
    Copy-Item "$payload\*" $Root -Recurse -Force
} catch {
    Write-Host "Could not update the installed files: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Close any running crossfade window and run the installer again."
    exit 1
}

# ---- 2. Python runtime (per-user, silent) ---------------------------------
if (-not (Test-Path $PyExe)) {
    Write-Host "[2/4] Installing Python 3.12 (per-user, this is silent) ..."
    $installer = "$Tmp\python-installer.exe"
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe" -OutFile $installer
    Start-Process -FilePath $installer -ArgumentList `
        "/quiet", "InstallAllUsers=0", "PrependPath=0", "Include_test=0", `
        "TargetDir=$PyDir" -Wait
    if (-not (Test-Path $PyExe)) {
        Write-Host "Python installation failed. Re-run this installer." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "[2/4] Python already installed - skipping."
}

# ---- 3. Backend environment ------------------------------------------------
$venv = "$Root\share\crossfade\venv"
if (-not (Test-Path "$venv\Scripts\python.exe")) {
    Write-Host "[3/4] Setting up the YouTube Music backend (downloads ~30 MB) ..."
    & $PyExe -m venv $venv
    & "$venv\Scripts\python.exe" -m pip install --quiet --upgrade pip
    & "$venv\Scripts\python.exe" -m pip install --quiet -r "$Root\share\crossfade\requirements.txt"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Backend setup failed. Re-run this installer." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "[3/4] Backend environment already set up - skipping."
}

# ---- 4. Start Menu shortcuts ----------------------------------------------
Write-Host "[4/4] Creating Start Menu shortcuts ..."
$ws = New-Object -ComObject WScript.Shell
$sm = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs"

$lnk = $ws.CreateShortcut("$sm\crossfade.lnk")
$lnk.TargetPath    = "$Root\bin\crossfade.exe"
$lnk.WorkingDirectory = "$Root\bin"
$lnk.Description   = "crossfade - YouTube Music desktop client"
$lnk.Save()

$lnk2 = $ws.CreateShortcut("$sm\crossfade-login.lnk")
$lnk2.TargetPath   = "$Root\bin\crossfade-login.cmd"
$lnk2.WorkingDirectory = "$Root\bin"
$lnk2.Description  = "Sign in to YouTube Music for crossfade"
$lnk2.Save()

# ---- 5. Optional: hidden warm start at sign-in -----------------------------
if ($Host.Name -eq "ConsoleHost") {
    $runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
    $warmValue = "`"$Root\bin\crossfade.exe`" --warm"
    $warmAnswer = Read-Host "Start crossfade hidden in the background when you sign in? [Y/n]"
    if ($warmAnswer -match '^(n|no)$') {
        Remove-ItemProperty -Path $runKey -Name crossfade -ErrorAction SilentlyContinue
        Write-Host "  Background start disabled. Launch crossfade from the Start Menu."
    }
    else {
        New-ItemProperty -Path $runKey -Name crossfade -Value $warmValue `
            -PropertyType String -Force | Out-Null
        Write-Host "  crossfade will start hidden at sign-in; open it from the Start Menu."
    }
}

Write-Host ""
Write-Host "Installed." -ForegroundColor Green
Write-Host "  - Start crossfade from the Start Menu."
Write-Host "  - Sign in: Start Menu -> crossfade-login, or Settings -> Sign in with browser."
Write-Host "    (Copy the request headers it asks for; it takes them from the clipboard.)"
Write-Host ""
if ($Host.Name -eq "ConsoleHost") {
    $answer = Read-Host "Restart crossfade now? [Y/n]"
    if ($answer -notmatch '^(n|no)$') {
        Start-Process -FilePath "$Root\bin\crossfade.exe" -WorkingDirectory "$Root\bin"
    }
    else {
        Read-Host "Press Enter to close" | Out-Null
    }
}
