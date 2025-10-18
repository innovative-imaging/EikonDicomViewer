@echo off
setlocal

echo ============================================
echo Building C++ DICOM Viewer
echo ============================================

REM Set Qt path
set QT_DIR=C:\Qt_6.9\6.9.3\msvc2022_64
set CMAKE_PREFIX_PATH=%QT_DIR%

REM Navigate to project directory
cd /d "%~dp0"

REM Create build directory if it doesn't exist
if not exist "build" mkdir build

REM Navigate to build directory
cd build

REM Configure with CMake
echo Configuring with CMake...
cmake .. -DCMAKE_PREFIX_PATH="%CMAKE_PREFIX_PATH%"
if %ERRORLEVEL% neq 0 (
    echo CMake configuration failed!
    pause
    exit /b %ERRORLEVEL%
)

REM Clean the Release output directory
echo Cleaning Release output directory...
if exist "Release\Release" (
    rmdir /S /Q "Release\Release"
    echo Release directory cleaned.
)

REM Build the project
echo Building project...
cmake --build . --config Release --clean-first
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)

REM Copy icons if they don't exist in output directory
if not exist "Release\Release\GVPIcons" (
    echo Copying GVPIcons...
    xcopy /E /I /Y "..\..\..\GVPIcons" "Release\Release\GVPIcons"
)

REM Ensure platform plugins are copied
if not exist "Release\Release\platforms" (
    echo Creating platforms directory...
    mkdir "Release\Release\platforms"
)

if not exist "Release\Release\platforms\qwindows.dll" (
    echo Copying Qt platform plugins...
    copy "%QT_DIR%\plugins\platforms\*.dll" "Release\Release\platforms\"
)

REM Create compressed archive using 7za.exe (before copying Utils)
echo Creating compressed archive...
cd Release\Release
REM Delete existing archive to ensure fresh creation
if exist "EikonDicomViewer.7z" del "EikonDicomViewer.7z"
if exist "..\..\..\Utils\7za.exe" (
    "..\..\..\Utils\7za.exe" a -t7z -mx=9 -mfb=64 -md=32m -ms=on EikonDicomViewer.7z * -x!*.7z
    if %ERRORLEVEL% equ 0 (
        echo Archive EikonDicomViewer.7z created successfully!
    ) else (
        echo Warning: Archive creation failed!
    )
) else (
    echo Warning: 7za.exe not found in Utils folder!
)
cd ..\..

REM Copy Utils folder content to Release folder (after compression)
echo Copying Utils folder content to Release directory...
xcopy /Y "..\Utils\*" "Release\Release\"

REM Copy DicomViewerSplash.exe from splash UI build output
echo Copying DicomViewerSplash.exe...
set "SPLASH_SOURCE=%~dp0..\dicomviewerSplashUI_CPP\dist\DicomViewerSplash.exe"
if exist "%SPLASH_SOURCE%" (
    copy /Y "%SPLASH_SOURCE%" "Release\Release\"
    echo DicomViewerSplash.exe copied successfully!
) else (
    echo Warning: DicomViewerSplash.exe not found at %SPLASH_SOURCE%
    echo Please build the splash UI first using build_exe.bat
)

echo ============================================
echo Build completed successfully!
echo Executable location: build\Release\Release\EikonDicomViewer.exe
echo Archive location: build\Release\Release\EikonDicomViewer.7z
echo Splash executable: build\Release\Release\DicomViewerSplash.exe
echo ============================================

REM Ask if user wants to run the application
set /p choice="Do you want to run the application now? (y/n): "
if /i "%choice%"=="y" (
    cd Release\Release
    start EikonDicomViewer.exe
)

pause