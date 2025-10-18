@echo off
setlocal

echo ============================================
echo Building C++ DICOM Viewer - MinSizeRel
echo (Optimized for minimum binary size)
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

REM Clean the MinSizeRel output directory
echo Cleaning MinSizeRel output directory...
if exist "MinSizeRel\MinSizeRel" (
    rmdir /S /Q "MinSizeRel\MinSizeRel"
    echo MinSizeRel directory cleaned.
)

REM Build the project with MinSizeRel configuration
echo Building project with MinSizeRel configuration...
cmake --build . --config MinSizeRel --clean-first
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)

REM Copy icons if they don't exist in output directory
if not exist "MinSizeRel\MinSizeRel\GVPIcons" (
    echo Copying GVPIcons...
    xcopy /E /I /Y "..\..\..\GVPIcons" "MinSizeRel\MinSizeRel\GVPIcons"
)

REM Ensure platform plugins are copied
if not exist "MinSizeRel\MinSizeRel\platforms" (
    echo Creating platforms directory...
    mkdir "MinSizeRel\MinSizeRel\platforms"
)

if not exist "MinSizeRel\MinSizeRel\platforms\qwindows.dll" (
    echo Copying Qt platform plugins...
    copy "%QT_DIR%\plugins\platforms\*.dll" "MinSizeRel\MinSizeRel\platforms\"
)

REM Copy Utils folder content to MinSizeRel folder (before compression)
echo Copying Utils folder content to MinSizeRel directory...
xcopy /Y "..\Utils\*" "MinSizeRel\MinSizeRel\"

REM Create compressed archive using 7za.exe (excluding unwanted files and folders)
echo Creating compressed archive...
cd MinSizeRel\MinSizeRel
REM Delete existing archives to ensure fresh creation
if exist "EikonDicomViewer.7z" del "EikonDicomViewer.7z"
if exist "EikonDicomViewer_MinSize.7z" del "EikonDicomViewer_MinSize.7z"
if exist "..\..\..\Utils\7za.exe" (
    REM Create optimized archive excluding unwanted files and folders
    "..\..\..\Utils\7za.exe" a -t7z -mx=9 -mfb=64 -md=32m -ms=on EikonDicomViewer.7z EikonDicomViewer.exe *.dll platforms imageformats 7za.exe 7za.dll 7zxa.dll autorun.inf -x!ffmpeg.exe -x!generic -x!iconengines -x!networkinformation -x!styles -x!tls -x!dxcompiler.dll -x!dxil.dll -x!Qt6Network.dll -x!Qt6Svg.dll -x!Qt6Multimedia.dll
    if %ERRORLEVEL% equ 0 (
        echo Archive EikonDicomViewer.7z created successfully!
    ) else (
        echo Warning: Archive creation failed!
    )
) else (
    echo Warning: 7za.exe not found in Utils folder!
)
cd ..\..

REM Note: ffmpeg.exe is kept separate and not included in the archive

REM Copy DicomViewerSplash.exe from splash UI build output
echo Copying DicomViewerSplash.exe...
set "SPLASH_SOURCE=%~dp0..\dicomviewerSplashUI_CPP\dist\DicomViewerSplash.exe"
if exist "%SPLASH_SOURCE%" (
    copy /Y "%SPLASH_SOURCE%" "MinSizeRel\MinSizeRel\"
    echo DicomViewerSplash.exe copied successfully!
) else (
    echo Warning: DicomViewerSplash.exe not found at %SPLASH_SOURCE%
    echo Please build the splash UI first using build_exe.bat
)

REM Show file sizes for comparison
echo ============================================
echo Build Size Comparison:
if exist "Release\Release\EikonDicomViewer.exe" (
    for %%A in ("Release\Release\EikonDicomViewer.exe") do echo Release build:    %%~zA bytes
)
if exist "MinSizeRel\MinSizeRel\EikonDicomViewer.exe" (
    for %%A in ("MinSizeRel\MinSizeRel\EikonDicomViewer.exe") do echo MinSizeRel build: %%~zA bytes
)
echo ============================================

echo ============================================
echo MinSizeRel build completed successfully!
echo Executable location: build\MinSizeRel\MinSizeRel\EikonDicomViewer.exe
echo Archive location: build\MinSizeRel\MinSizeRel\EikonDicomViewer.7z
echo Splash executable: build\MinSizeRel\MinSizeRel\DicomViewerSplash.exe
echo Note: ffmpeg.exe is separate and not included in archive
echo ============================================

REM Ask if user wants to run the application
set /p choice="Do you want to run the MinSizeRel application now? (y/n): "
if /i "%choice%"=="y" (
    cd MinSizeRel\MinSizeRel
    start EikonDicomViewer.exe
)

pause