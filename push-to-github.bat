@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ============================================================
echo   Zemi Mecchamouflage - push local changes to your GitHub fork
echo ============================================================
echo Working folder: %cd%
echo.

rem --- Configure this if your fork lives somewhere else ---
set "REMOTE_URL=https://github.com/Mozmina/MecchaCamouflage.git"
set "BRANCH=main"

rem --- Clear any stale git lock files left over from an interrupted run ---
rem     (can happen if OneDrive, an antivirus, or another program briefly
rem     locks a file inside .git while a previous run was working)
if exist ".git\index.lock" del /f /q ".git\index.lock" >nul 2>nul
if exist ".git\HEAD.lock" del /f /q ".git\HEAD.lock" >nul 2>nul

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Git was not found on this computer.
    echo Install "Git for Windows" from https://git-scm.com/download/win
    echo then run this file again.
    pause
    exit /b 1
)

if not exist ".git" (
    echo [1/6] Initializing a new git repository here...
    git init
    if errorlevel 1 goto :fail
) else (
    echo [1/6] Git repository already initialized. Skipping.
)

echo [2/6] Setting remote "origin" to %REMOTE_URL% ...
git remote add origin "%REMOTE_URL%" 2>nul
if errorlevel 1 (
    git remote set-url origin "%REMOTE_URL%"
    if errorlevel 1 goto :fail
)

echo [3/6] Fetching latest "%BRANCH%" from GitHub...
git fetch origin %BRANCH%
if errorlevel 1 goto :fail

echo [4/6] Lining up local history with origin/%BRANCH% (your edited files are kept as-is)...
set "RESET_TRIES=0"
:reset_retry
if exist ".git\index.lock" del /f /q ".git\index.lock" >nul 2>nul
git reset origin/%BRANCH%
if not errorlevel 1 goto :reset_done
set /a RESET_TRIES+=1
if !RESET_TRIES! lss 3 (
    echo [INFO] Retrying (another program may have briefly locked a git file)...
    timeout /t 2 /nobreak >nul
    goto :reset_retry
)
goto :fail
:reset_done

echo [5/6] Staging and committing your changes...
git add -A
git commit -m "Merge upstream v1.7.2 (ESP overlay, Image Paint, WebP, Fukuyoka, rewritten per-region replay/color pipeline, GPLv3, new submodules); re-adapt natural paint 3-stage (F10/F11) and second pass (F9) onto new pipeline; move Second/Natural hotkeys to F9-F11 to free F5-F8 for Image Paint; rebrand new LiveDiagnostics project and merged files to ZemiMecchamouflage"
if errorlevel 1 (
    echo.
    echo [INFO] Nothing to commit, or git needs your identity configured.
    echo If this is the first time you use git on this PC, run these two lines
    echo once ^(with your own name/email^) and then run this file again:
    echo    git config --global user.name "Your Name"
    echo    git config --global user.email "you@example.com"
    echo.
)

echo [6/6] Pushing to origin/%BRANCH% ...
echo A browser window may open asking you to sign in to GitHub - that is normal.
git push origin HEAD:%BRANCH%
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo   Done! Your changes are now on GitHub.
echo   Go to the Actions tab of your repo and run the "Release"
echo   workflow (or push a tag) to get a compiled build.
echo ============================================================
pause
exit /b 0

:fail
echo.
echo [ERROR] Something went wrong at the step above. Scroll up to see the
echo git error message. Nothing further was run.
pause
exit /b 1
