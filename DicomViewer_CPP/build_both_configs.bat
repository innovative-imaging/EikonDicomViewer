@echo off
setlocal

echo ============================================
echo Building C++ DICOM Viewer - All Configurations
echo (Release + MinSizeRel for size comparison)
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

echo ============================================
echo Building Release configuration...
echo ============================================

REM Clean the Release output directory
if exist "Release\Release" (
    rmdir /S /Q "Release\Release"
)

REM Build Release
cmake --build . --config Release --clean-first
if %ERRORLEVEL% neq 0 (
    echo Release build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo ============================================
echo Building MinSizeRel configuration...
echo ============================================

REM Clean the MinSizeRel output directory
if exist "MinSizeRel\MinSizeRel" (
    rmdir /S /Q "MinSizeRel\MinSizeRel"
)

REM Build MinSizeRel
cmake --build . --config MinSizeRel --clean-first
if %ERRORLEVEL% neq 0 (
    echo MinSizeRel build failed!
    pause
    exit /b %ERRORLEVEL%
)

REM Copy dependencies for Release
if not exist "Release\Release\platforms" (
    mkdir "Release\Release\platforms"
    copy "%QT_DIR%\plugins\platforms\*.dll" "Release\Release\platforms\"
)

REM Copy dependencies for MinSizeRel
if not exist "MinSizeRel\MinSizeRel\platforms" (
    mkdir "MinSizeRel\MinSizeRel\platforms"
    copy "%QT_DIR%\plugins\platforms\*.dll" "MinSizeRel\MinSizeRel\platforms\"
)

REM Copy Utils to both builds
xcopy /Y "..\Utils\*" "Release\Release\" >nul 2>&1
xcopy /Y "..\Utils\*" "MinSizeRel\MinSizeRel\" >nul 2>&1

echo ============================================
echo Build Size Comparison:
echo ============================================
if exist "Release\Release\EikonDicomViewer.exe" (
    for %%A in ("Release\Release\EikonDicomViewer.exe") do (
        set /A release_size=%%~zA
        echo Release build:       %%~zA bytes ^(%%~zA KB^)
    )
)
if exist "MinSizeRel\MinSizeRel\EikonDicomViewer.exe" (
    for %%A in ("MinSizeRel\MinSizeRel\EikonDicomViewer.exe") do (
        set /A minsize_size=%%~zA
        echo MinSizeRel build:     %%~zA bytes ^(%%~zA KB^)
    )
)

REM Calculate size difference (approximate)
if exist "Release\Release\EikonDicomViewer.exe" if exist "MinSizeRel\MinSizeRel\EikonDicomViewer.exe" (
    echo.
    echo MinSizeRel is optimized for smaller binary size.
    echo Both builds have identical functionality.
)

echo ============================================
echo Both builds completed successfully!
echo.
echo Release build:    build\Release\Release\EikonDicomViewer.exe
echo MinSizeRel build: build\MinSizeRel\MinSizeRel\EikonDicomViewer.exe
echo ============================================

echo.
echo Which build would you like to run?
echo 1. Release (optimized for speed)
echo 2. MinSizeRel (optimized for size)
echo 3. None
set /p choice="Enter choice (1-3): "

if "%choice%"=="1" (
    echo Running Release build...
    cd Release\Release
    start EikonDicomViewer.exe
) else if "%choice%"=="2" (
    echo Running MinSizeRel build...
    cd MinSizeRel\MinSizeRel
    start EikonDicomViewer.exe
) else (
    echo No application started.
)

pause