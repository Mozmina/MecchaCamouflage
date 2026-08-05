@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ============================================================
echo   Zemi Mecchamouflage - pull latest changes from the base repo
echo ============================================================
echo Working folder: %cd%
echo.

set "UPSTREAM_URL=https://github.com/acentrist/MecchaCamouflage.git"
set "UPSTREAM_BRANCH=main"

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Git was not found on this computer.
    echo Install "Git for Windows" from https://git-scm.com/download/win
    echo then run this file again.
    pause
    exit /b 1
)

if not exist ".git" (
    echo [ERROR] No git repository here yet. Run push-to-github.bat once first.
    pause
    exit /b 1
)

echo [1/4] Setting remote "upstream" to %UPSTREAM_URL% ...
git remote add upstream "%UPSTREAM_URL%" 2>nul
if errorlevel 1 (
    git remote set-url upstream "%UPSTREAM_URL%"
)

echo [2/4] Fetching latest "%UPSTREAM_BRANCH%" from the base repo...
echo (This can take a little while, the base repo is large.)
git fetch upstream %UPSTREAM_BRANCH%
if errorlevel 1 goto :fail

echo [3/4] Making sure your local changes are committed first...
git add -A
git commit -m "Snapshot before merging upstream update" >nul 2>nul

echo [4/4] Merging upstream/%UPSTREAM_BRANCH% into your local copy...
git merge upstream/%UPSTREAM_BRANCH% --no-ff --no-commit -m "Merge upstream update" 2>merge_output.txt
set MERGE_RESULT=%errorlevel%
type merge_output.txt

if %MERGE_RESULT% neq 0 (
    findstr /C:"refusing to merge unrelated histories" merge_output.txt >nul
    if not errorlevel 1 (
        echo.
        echo [INFO] No shared history found, retrying with --allow-unrelated-histories ...
        git merge upstream/%UPSTREAM_BRANCH% --no-ff --no-commit --allow-unrelated-histories -m "Merge upstream update"
    )
)
del merge_output.txt >nul 2>nul

echo.
echo ============================================================
echo   Merge step finished (with or without conflicts - see above).
echo   Do NOT commit or push yet.
echo   Come back to Claude and say "c'est fait" - Claude will check
echo   the merged files, resolve any conflicts, and re-apply your
echo   custom features before anything is committed or pushed.
echo ============================================================
pause
exit /b 0

:fail
echo.
echo [ERROR] Something went wrong at the step above. Scroll up to see the
echo git error message. Nothing further was run.
pause
exit /b 1
