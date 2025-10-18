@echo off
title Building DicomViewerSplash C++ Application

echo ============================================================
echo    Building DicomViewerSplash C++ Application
echo ============================================================
echo.

REM Check if Visual Studio Developer Command Prompt is available
where cl >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: Visual Studio C++ compiler not found in PATH
    echo.
    echo Please run this from a Visual Studio Developer Command Prompt or
    echo make sure you have Visual Studio C++ Build Tools installed.
    echo.
    echo Alternatives:
    echo 1. Open "Developer Command Prompt for VS" from Start Menu
    echo 2. Run "vcvarsall.bat x64" from VS installation directory
    echo 3. Install "C++ Build Tools" workload in Visual Studio Installer
    echo.
    pause
    exit /b 1
)

echo Found Visual Studio C++ compiler
echo.

REM Create output directory
if not exist "bin" mkdir "bin"

REM Check if the image file exists and generate header if needed
if not exist "..\CompanySplashScreen.png" (
    echo.
    echo ✗ CompanySplashScreen.png not found in parent directory!
    echo   Expected location: ..\CompanySplashScreen.png
    echo   Please ensure the image file exists before building.
    echo.
    pause
    exit /b 1
)

REM Generate embedded image header file if it doesn't exist or is older than the image
if not exist "splash_image_data.h" (
    echo Generating embedded image data...
    python convert_image.py
    if %ERRORLEVEL% NEQ 0 (
        echo ✗ Failed to generate image data!
        pause
        exit /b 1
    )
) else (
    echo Image data header already exists, skipping conversion.
)

echo Compiling DicomViewerSplash.cpp...
cl /std:c++17 /EHsc /W3 /O2 /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE DicomViewerSplash.cpp /Fe:bin/DicomViewerSplash.exe /link /SUBSYSTEM:WINDOWS gdiplus.lib shlwapi.lib comctl32.lib user32.lib kernel32.lib gdi32.lib ole32.lib

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ✓ Build successful!
    echo   Output: bin\DicomViewerSplash.exe
    echo   Splash screen image embedded in executable
    
    REM Clean up intermediate files
    if exist "*.obj" del "*.obj"
    
    echo.
    echo You can now run: bin\DicomViewerSplash.exe
    echo No external image file required - splash screen is embedded!
) else (
    echo.
    echo ✗ Build failed!
    echo Check the error messages above.
)

echo.
pause