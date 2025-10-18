@echo off
setlocal

echo ============================================
echo  Building All EikonDicomViewer Projects
echo ============================================
echo.

REM Get the script directory (project root)
set "PROJECT_ROOT=%~dp0"
set "DICOM_VIEWER_DIR=%PROJECT_ROOT%DicomViewer_CPP"
set "SPLASH_UI_DIR=%PROJECT_ROOT%dicomviewerSplashUI_CPP"
set "BINARIES_DIR=%PROJECT_ROOT%Binaries"

echo Project Root: %PROJECT_ROOT%
echo DICOM Viewer: %DICOM_VIEWER_DIR%
echo Splash UI:    %SPLASH_UI_DIR%
echo Binaries:     %BINARIES_DIR%
echo.

REM Create or clean Binaries directory
echo ============================================
echo Preparing Binaries Directory
echo ============================================
if exist "%BINARIES_DIR%" (
    echo Cleaning existing Binaries directory...
    rmdir /S /Q "%BINARIES_DIR%"
)
mkdir "%BINARIES_DIR%"
echo Binaries directory ready.
echo.

REM Build DicomViewer_CPP with MinSizeRel configuration
echo ============================================
echo Building DicomViewer_CPP (MinSizeRel)
echo ============================================
cd /d "%DICOM_VIEWER_DIR%"

REM Clean build directory to avoid cache conflicts
if exist "build" (
    echo Cleaning build directory...
    rmdir /S /Q "build"
)

REM Build directly without interactive prompts
echo Configuring with CMake...
set QT_DIR=C:\Qt_6.9\6.9.3\msvc2022_64
set CMAKE_PREFIX_PATH=%QT_DIR%

REM Create build directory
if not exist "build" mkdir build
pushd build

REM Configure with CMake
cmake .. -DCMAKE_PREFIX_PATH="%CMAKE_PREFIX_PATH%"
if %ERRORLEVEL% neq 0 (
    echo CMake configuration failed!
    popd
    pause
    exit /b %ERRORLEVEL%
)

REM Clean the MinSizeRel output directory
if exist "MinSizeRel\MinSizeRel" (
    rmdir /S /Q "MinSizeRel\MinSizeRel"
)

REM Build the project with MinSizeRel configuration
echo Building project with MinSizeRel configuration...
cmake --build . --config MinSizeRel --clean-first
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    popd
    pause
    exit /b %ERRORLEVEL%
)

REM Copy Utils folder content to MinSizeRel folder
echo Copying Utils folder content to MinSizeRel directory...
xcopy /Y "..\Utils\*" "MinSizeRel\MinSizeRel\"

REM Create compressed archive using 7za.exe
echo Creating compressed archive...
pushd MinSizeRel\MinSizeRel
if exist "EikonDicomViewer.7z" del "EikonDicomViewer.7z"
if exist "..\..\..\Utils\7za.exe" (
    "..\..\..\Utils\7za.exe" a -t7z -mx=9 -mfb=64 -md=32m -ms=on EikonDicomViewer.7z * -x!*.7z -x!ffmpeg.exe > nul
    if %ERRORLEVEL% equ 0 (
        echo Archive EikonDicomViewer.7z created successfully!
    ) else (
        echo Warning: Archive creation failed!
    )
) else (
    echo Warning: 7za.exe not found in Utils folder!
)
popd
popd
echo ✓ DicomViewer_CPP build completed successfully.
echo.

REM Build dicomviewerSplashUI_CPP
echo ============================================
echo Building dicomviewerSplashUI_CPP
echo ============================================
pushd "%SPLASH_UI_DIR%"

REM Ensure CompanySplashScreen.png is in parent directory
if not exist "..\CompanySplashScreen.png" (
    if exist "CompanySplashScreen.png" (
        echo Copying CompanySplashScreen.png to parent directory...
        copy /Y "CompanySplashScreen.png" "..\CompanySplashScreen.png"
    ) else (
        echo ⚠️  Warning: CompanySplashScreen.png not found!
    )
)

REM Build the splash UI with Visual Studio environment
echo. | cmd /c ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && build.bat"
popd
if %ERRORLEVEL% neq 0 (
    echo ✗ dicomviewerSplashUI_CPP build failed!
    pause
    exit /b %ERRORLEVEL%
)
echo ✓ dicomviewerSplashUI_CPP build completed successfully.
echo.

REM Copy builds to Binaries folder (optimized)
echo ============================================
echo Creating Optimized Binaries Folder
echo ============================================
pushd "%PROJECT_ROOT%"

REM Use the optimization script approach for better deployment
set "SOURCE_BUILD=%DICOM_VIEWER_DIR%\build\MinSizeRel\MinSizeRel"

REM Copy main executable
echo Copying main executable...
copy /Y "%SOURCE_BUILD%\EikonDicomViewer.exe" "%BINARIES_DIR%\"

REM Copy essential Qt DLLs (only the ones directly required)
echo Copying essential Qt DLLs...
copy /Y "%SOURCE_BUILD%\Qt6Core.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\Qt6Gui.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\Qt6Widgets.dll" "%BINARIES_DIR%\"

REM Copy all GDCM DLLs (they're interdependent)
echo Copying GDCM libraries...
copy /Y "%SOURCE_BUILD%\gdcmCommon.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmDSED.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmMSFF.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmDICT.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmIOD.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmjpeg8.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmjpeg12.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmjpeg16.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\gdcmcharls.dll" "%BINARIES_DIR%\"

REM Copy compression libraries
echo Copying compression libraries...
copy /Y "%SOURCE_BUILD%\zlib1.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\openjp2.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\libexpat.dll" "%BINARIES_DIR%\"

REM Copy TurboJPEG
echo Copying TurboJPEG...
copy /Y "%SOURCE_BUILD%\turbojpeg.dll" "%BINARIES_DIR%\"

REM Copy essential Qt plugins (platforms is critical)
echo Copying essential Qt plugins...
if not exist "%BINARIES_DIR%\platforms" mkdir "%BINARIES_DIR%\platforms"
copy /Y "%SOURCE_BUILD%\platforms\qwindows.dll" "%BINARIES_DIR%\platforms\"

REM Copy image format plugins (for DICOM image display)
if not exist "%BINARIES_DIR%\imageformats" mkdir "%BINARIES_DIR%\imageformats"
copy /Y "%SOURCE_BUILD%\imageformats\qjpeg.dll" "%BINARIES_DIR%\imageformats\"

REM Copy utilities
echo Copying utilities...
copy /Y "%SOURCE_BUILD%\7za.exe" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\7za.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\7zxa.dll" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\ffmpeg.exe" "%BINARIES_DIR%\"
copy /Y "%SOURCE_BUILD%\autorun.inf" "%BINARIES_DIR%\"

REM Copy the compressed archive
echo Copying compressed archive...
if exist "%SOURCE_BUILD%\EikonDicomViewer.7z" (
    copy /Y "%SOURCE_BUILD%\EikonDicomViewer.7z" "%BINARIES_DIR%\"
    echo ✓ Archive copied successfully
) else (
    echo ⚠️ Warning: EikonDicomViewer.7z not found!
)

echo ✓ Optimized DicomViewer_CPP files copied.

REM Copy DicomViewerSplash.exe
echo Copying DicomViewerSplash.exe...
if exist "%SPLASH_UI_DIR%\bin\DicomViewerSplash.exe" (
    copy /Y "%SPLASH_UI_DIR%\bin\DicomViewerSplash.exe" "%BINARIES_DIR%\"
    echo ✓ DicomViewerSplash.exe copied.
) else (
    echo ✗ DicomViewerSplash.exe not found!
    pause
    exit /b 1
)

REM List final contents
echo.
echo ============================================
echo Final Binaries Directory Contents
echo ============================================
dir "%BINARIES_DIR%" /B
echo.

REM Show file sizes for key executables
echo ============================================
echo Executable Sizes
echo ============================================
if exist "%BINARIES_DIR%\EikonDicomViewer.exe" (
    for %%A in ("%BINARIES_DIR%\EikonDicomViewer.exe") do echo EikonDicomViewer.exe:  %%~zA bytes
)
if exist "%BINARIES_DIR%\DicomViewerSplash.exe" (
    for %%A in ("%BINARIES_DIR%\DicomViewerSplash.exe") do echo DicomViewerSplash.exe:  %%~zA bytes
)
if exist "%BINARIES_DIR%\EikonDicomViewer.7z" (
    for %%A in ("%BINARIES_DIR%\EikonDicomViewer.7z") do echo Archive ^(7z^):          %%~zA bytes
)
echo.

echo ============================================
echo ✓ ALL BUILDS COMPLETED SUCCESSFULLY! ✓
echo ============================================
echo.
echo The following executables are ready for distribution:
echo • %BINARIES_DIR%\EikonDicomViewer.exe
echo • %BINARIES_DIR%\DicomViewerSplash.exe
echo • %BINARIES_DIR%\EikonDicomViewer.7z (optimized compressed distribution)
echo • %BINARIES_DIR%\ffmpeg.exe (separate utility, not in archive)
echo.
echo All dependencies and utilities are included in the Binaries folder.
echo.

popd

echo.
echo Build script completed successfully.
echo Ready for distribution from the Binaries folder.