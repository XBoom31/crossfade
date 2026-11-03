@echo off
rem Manual YouTube Music sign-in for crossfade (browser cookies method).
rem
rem 1. Open https://music.youtube.com and make sure you are SIGNED IN
rem    (your profile avatar shows top-right, not a "Sign in" button).
rem 2. DevTools (F12) -> Network -> click any POST request to
rem    music.youtube.com -> Copy -> Copy request headers.
rem 3. Just come back here: the headers are taken straight from the
rem    CLIPBOARD (no pasting needed). Falls back to manual paste if the
rem    clipboard has no headers.
rem
rem Writes %APPDATA%\crossfade\browser.json and verifies the login.

set PY=%LOCALAPPDATA%\crossfade\share\crossfade\venv\Scripts\python.exe
set SCRIPT=%LOCALAPPDATA%\crossfade\share\crossfade\make_browser_json.py

%PY% %SCRIPT% clipboard
if errorlevel 4 (
  echo.
  echo Falling back to manual paste:
  %PY% %SCRIPT%
)
echo.
pause
