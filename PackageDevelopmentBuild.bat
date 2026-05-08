@echo off
setlocal

REM ── Edit UE_ROOT if your engine install lives elsewhere ──
set "UE_ROOT=C:\Program Files\Epic Games\UE_5.7"
set "PROJECT_PATH=%~dp0CatVentures.uproject"
set "ARCHIVE_DIR=%~dp0ArchivedBuilds"

echo.
echo ===============================================================
echo  CatVentures - Win64 Development Build
echo  Project: %PROJECT_PATH%
echo  Archive: %ARCHIVE_DIR%
echo ===============================================================
echo.

call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
    -project="%PROJECT_PATH%" ^
    -noP4 ^
    -platform=Win64 ^
    -clientconfig=Development ^
    -serverconfig=Development ^
    -build ^
    -cook ^
    -stage ^
    -pak ^
    -archive ^
    -archivedirectory="%ARCHIVE_DIR%" ^
    -prereqs ^
    -utf8output

if errorlevel 1 (
    echo.
    echo *** UAT BuildCookRun FAILED with exit code %errorlevel% ***
    exit /b %errorlevel%
)

echo.
echo *** UAT BuildCookRun SUCCEEDED ***
echo Staged build: %ARCHIVE_DIR%\Windows\
echo Next step:    cd SteamPipeScripts ^&^& steamcmd +login ^<user^> +run_app_build app_build_4567540.vdf +quit
endlocal
