@echo off
REM ================================================================
REM Simple ISO Creation and Testing Script for EikonDicomViewer
REM ================================================================

echo ================================================================
echo  EikonDicomViewer ISO Creation and Testing Script
echo ================================================================
echo.

REM Set paths
set "SOURCE_FOLDER=D:\Repos\EikonDicomViewer\Binaries"
set "TEST_FOLDER=D:\Repos\EikonDicomViewer\test_dvd_content_1"
set "ISO_PATH=D:\Repos\EikonDicomViewer\DicomDir.iso"
set "OSCDIMG_PATH=C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\amd64\Oscdimg\oscdimg.exe"

echo Step 1: Copying Binaries to test folder...
if not exist "%TEST_FOLDER%" mkdir "%TEST_FOLDER%"
xcopy "%SOURCE_FOLDER%\*" "%TEST_FOLDER%\" /E /Y /Q > nul
if errorlevel 1 (
    echo ERROR: Failed to copy files
    pause
    exit /b 1
)
echo ✓ Files copied successfully
echo.

echo Step 2: Removing old ISO if it exists...
if exist "%ISO_PATH%" (
    echo Dismounting existing ISO...
    powershell -Command "Dismount-DiskImage -ImagePath '%ISO_PATH%' -ErrorAction SilentlyContinue"
    timeout /t 2 /nobreak > nul
    del "%ISO_PATH%" /Q
)
echo.

echo Step 3: Creating ISO image...
echo Running: oscdimg.exe -m -o -u2 "%TEST_FOLDER%" "%ISO_PATH%"
"%OSCDIMG_PATH%" -m -o -u2 "%TEST_FOLDER%" "%ISO_PATH%"
if errorlevel 1 (
    echo ERROR: ISO creation failed
    pause
    exit /b 1
)
echo ✓ ISO created successfully
echo.

echo Step 4: Mounting ISO and launching application...
echo This will open PowerShell to mount the ISO and launch the app...
echo.
powershell -Command "& { Mount-DiskImage -ImagePath '%ISO_PATH%'; Start-Sleep 3; $drive = (Get-Volume | Where-Object DriveType -eq 'CD-ROM' | Select-Object -Last 1).DriveLetter + ':'; Write-Host 'ISO mounted at:' $drive -ForegroundColor Green; Start-Process -FilePath \"$drive\DicomViewerSplash.exe\" -WorkingDirectory $drive; Write-Host 'DicomViewerSplash.exe launched!' -ForegroundColor Green }"

echo.
echo ================================================================
echo  SCRIPT COMPLETED!
echo ================================================================
echo.
echo The ISO has been created and the application launched.
echo To unmount later, run: powershell "Dismount-DiskImage -ImagePath '%ISO_PATH%'"
echo.
pause