#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include "splash_image_data.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;
using namespace std;
namespace fs = std::filesystem;

// Configuration constants
const wstring VIEWER_EXE = L"EikonDicomViewer.exe";
const wstring SEVENZA_EXE = L"7za.exe";
const wstring APP_TITLE = L"Eikon DicomViewer";
// Window size will be determined by splash image dimensions
int g_windowWidth = 800;   // Default fallback
int g_windowHeight = 600;  // Default fallback

// Global source directory (will be set from command line or auto-detected)
wstring g_sourceDir;

// Window class name
const wchar_t* CLASS_NAME = L"DicomViewerSplashClass";

// Control IDs
const int ID_STATUS_LABEL = 1001;
const int ID_TIMEOUT_TIMER = 1002;

// Timeout constants (3 minutes = 180,000 milliseconds)
const UINT TIMEOUT_DURATION_MS = 180000;

// Global variables
HWND g_hMainWnd = nullptr;
HWND g_hStatusLabel = nullptr;
wstring g_tempDir;
wstring g_logFile;
bool g_isRunning = true;
unique_ptr<Image> g_splashImage = nullptr;

// GDI+ token
ULONG_PTR g_gdiplusToken;

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void InitializeApplication();
void CleanupApplication();
wstring GetTempDirectory();
void LogMessage(const wstring& level, const wstring& message);
void UpdateStatus(const wstring& message);
bool CreateDestinationDirectory();
bool CopyDicomDir();
bool StartRobocopy();
bool CopyFfmpegExe();
bool Extract7zArchive();
bool LaunchViewer();
void BringViewerToFront();
void ExecutePipeline();
bool FileExists(const wstring& path);
bool DirectoryExists(const wstring& path);
wstring GetExecutableDirectory();

// Image loading functions
bool LoadSplashImageFromResource();
IStream* CreateStreamFromByteArray(const unsigned char* data, unsigned int size);

// DVD Detection functions
wstring DetectSourceDrive();
bool HasDicomData(const wstring& drivePath);
vector<wstring> FindDvdDrives();
vector<wstring> FindRemovableDrives();
vector<wstring> FindAllDrives();
wstring GetExecutableDrive();

// Utility function to convert string to wstring
wstring StringToWString(const string& str) {
    if (str.empty()) return wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// Utility function to convert wstring to string
string WStringToString(const wstring& wstr) {
    if (wstr.empty()) return string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
    
    // Initialize common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);
    
    // Determine source directory from command line or auto-detect
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        // Convert command line argument to wstring
        g_sourceDir = StringToWString(string(lpCmdLine));
        // Remove quotes if present
        if (g_sourceDir.front() == L'"' && g_sourceDir.back() == L'"') {
            g_sourceDir = g_sourceDir.substr(1, g_sourceDir.length() - 2);
        }
    } else {
        // Auto-detect DVD/CD drive
        g_sourceDir = DetectSourceDrive();
        if (g_sourceDir.empty()) {
            MessageBox(NULL, 
                L"No DICOM DVD detected\n\n"
                L"Could not find a DVD or drive containing DICOM data.\n\n"
                L"Please ensure:\n"
                L"• DICOM DVD is inserted\n"
                L"• Drive contains DICOMDIR or DicomFiles folder\n"
                L"• EikonDicomViewer.exe is present\n\n"
                L"Alternatively, run with path argument:\n"
                L"DicomViewerSplash.exe \"C:\\Path\\To\\DicomData\"",
                L"No DICOM DVD detected", MB_OK | MB_ICONERROR);
            return 1;
        }
    }
    
    InitializeApplication();
    
    // Register window class
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    
    if (!RegisterClass(&wc)) {
        MessageBox(NULL, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Create window
    g_hMainWnd = CreateWindowEx(
        0,
        CLASS_NAME,
        APP_TITLE.c_str(),
        WS_POPUP | WS_VISIBLE,
        (GetSystemMetrics(SM_CXSCREEN) - g_windowWidth) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - g_windowHeight) / 2,
        g_windowWidth,
        g_windowHeight,
        NULL,
        NULL,
        hInstance,
        NULL
    );
    
    if (!g_hMainWnd) {
        MessageBox(NULL, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);
    
    // Start the processing pipeline in a separate thread
    thread pipelineThread(ExecutePipeline);
    pipelineThread.detach();
    
    // Message loop
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) && g_isRunning) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    CleanupApplication();
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        {
            // Create status label at the bottom with solid black background
            const int LABEL_HEIGHT = 25;
            const int MARGIN = 10;
            g_hStatusLabel = CreateWindow(
                L"STATIC",
                L"Loading DicomViewer...",
                WS_VISIBLE | WS_CHILD | SS_CENTER,
                MARGIN, g_windowHeight - LABEL_HEIGHT - MARGIN, 
                g_windowWidth - (2 * MARGIN), LABEL_HEIGHT,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS_LABEL)), NULL, NULL
            );
            
            // Set font for status label with crisp rendering
            HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessage(g_hStatusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
            
            // Set solid black background to prevent transparency issues
            SetClassLongPtr(g_hStatusLabel, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
            
            // Start timeout timer (3 minutes)
            SetTimer(hwnd, ID_TIMEOUT_TIMER, TIMEOUT_DURATION_MS, NULL);
        }
        break;
        
    case WM_CTLCOLORSTATIC:
        {
            if ((HWND)lParam == g_hStatusLabel) {
                SetTextColor((HDC)wParam, RGB(255, 255, 255)); // White text
                SetBkColor((HDC)wParam, RGB(0, 0, 0)); // Black background
                SetBkMode((HDC)wParam, OPAQUE); // Solid background
                return (INT_PTR)GetStockObject(BLACK_BRUSH);
            }
        }
        break;
        
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            Graphics graphics(hdc);
            graphics.SetSmoothingMode(SmoothingModeHighQuality);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            
            if (g_splashImage && g_splashImage->GetLastStatus() == Ok) {
                UINT imgWidth = g_splashImage->GetWidth();
                UINT imgHeight = g_splashImage->GetHeight();
                
                if (imgWidth > 0 && imgHeight > 0) {
                    // Fill background with black first
                    SolidBrush blackBrush(Color(255, 0, 0, 0));
                    graphics.FillRectangle(&blackBrush, 0, 0, g_windowWidth, g_windowHeight);
                    
                    // Draw the image at original size, centered horizontally
                    // Reserve space at bottom for status text
                    const int TEXT_AREA_HEIGHT = 60;
                    int drawX = (g_windowWidth - (int)imgWidth) / 2;
                    int drawY = 0; // Top of window
                    
                    // Draw the image at its native resolution (no scaling to avoid blurriness)
                    graphics.DrawImage(g_splashImage.get(), drawX, drawY, (int)imgWidth, (int)imgHeight);
                } else {
                    // Image has invalid dimensions, use fallback
                    goto fallback_display;
                }
            } else {
            fallback_display:
                // Fallback: fill with black background only (no text)
                SolidBrush blackBrush(Color(255, 0, 0, 0));
                graphics.FillRectangle(&blackBrush, 0, 0, g_windowWidth, g_windowHeight);
                // Note: Status text is handled by the Windows static control, not GDI+
            }
            
            EndPaint(hwnd, &ps);
        }
        break;
        
    case WM_TIMER:
        if (wParam == ID_TIMEOUT_TIMER) {
            // 3 minutes have passed, show error message and exit
            KillTimer(hwnd, ID_TIMEOUT_TIMER);
            MessageBox(hwnd, L"Error in loading the DicomViewer", L"Timeout Error", MB_OK | MB_ICONERROR);
            g_isRunning = false;
            PostQuitMessage(0);
        }
        break;
        
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            PostQuitMessage(0);
        }
        break;
        
    case WM_DESTROY:
        // Clean up the timeout timer
        KillTimer(hwnd, ID_TIMEOUT_TIMER);
        g_isRunning = false;
        PostQuitMessage(0);
        break;
        
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    
    return 0;
}

void InitializeApplication() {
    g_tempDir = GetTempDirectory();
    g_logFile = g_tempDir + L"\\DVD_Copy_Log.txt";
    
    // Load splash image from resources
    if (!LoadSplashImageFromResource()) {
        // Fallback: try to load from external file
        wstring imagePath = GetExecutableDirectory() + L"\\..\\CompanySplashScreen.png";
        if (FileExists(imagePath)) {
            g_splashImage = make_unique<Image>(imagePath.c_str());
            if (g_splashImage->GetLastStatus() == Ok) {
                LogMessage(L"INFO", L"Splash image loaded from external file as fallback");
            } else {
                LogMessage(L"ERROR", L"Failed to load splash image from external file");
                g_splashImage.reset();
            }
        }
    }
    
    // Set window dimensions based on loaded image
    if (g_splashImage && g_splashImage->GetLastStatus() == Ok) {
        UINT imgWidth = g_splashImage->GetWidth();
        UINT imgHeight = g_splashImage->GetHeight();
        
        if (imgWidth > 0 && imgHeight > 0) {
            // Add some padding for the status text at the bottom
            const int TEXT_AREA_HEIGHT = 60;
            g_windowWidth = (int)imgWidth;
            g_windowHeight = (int)imgHeight + TEXT_AREA_HEIGHT;
            
            LogMessage(L"INFO", L"Window sized to image: " + to_wstring(g_windowWidth) + L"x" + to_wstring(g_windowHeight));
        }
    }
    
    // Create log file
    wofstream logFile(g_logFile);
    if (logFile.is_open()) {
        logFile << L"DICOM Viewer Splash Log - " << L"2025-10-18" << endl;
        logFile << L"============================================================" << endl;
        logFile.close();
    }
}

void CleanupApplication() {
    // Cleanup code here if needed
}

wstring GetTempDirectory() {
    wchar_t tempPath[MAX_PATH];
    GetTempPath(MAX_PATH, tempPath);
    return wstring(tempPath) + L"Ekn_TempData";
}

wstring GetExecutableDirectory() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    PathRemoveFileSpec(exePath);
    return wstring(exePath);
}

void LogMessage(const wstring& level, const wstring& message) {
    wofstream logFile(g_logFile, ios::app);
    if (logFile.is_open()) {
        logFile << level << L" " << message << endl;
        logFile.close();
    }
}

void UpdateStatus(const wstring& message) {
    if (g_hStatusLabel) {
        // Only update if different to prevent unnecessary redraws
        wchar_t currentText[256];
        GetWindowText(g_hStatusLabel, currentText, 256);
        if (wcscmp(currentText, L"Loading DicomViewer...") != 0) {
            SetWindowText(g_hStatusLabel, L"Loading DicomViewer...");
        }
    }
}

bool FileExists(const wstring& path) {
    return fs::exists(path) && fs::is_regular_file(path);
}

bool DirectoryExists(const wstring& path) {
    return fs::exists(path) && fs::is_directory(path);
}

bool CreateDestinationDirectory() {
    LogMessage(L"INFO", L"Creating destination directory: " + g_tempDir);
    
    try {
        // Remove existing directory if it exists
        if (fs::exists(g_tempDir)) {
            // Try to terminate any running viewer processes first
            STARTUPINFO si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            
            wstring killCmd = L"taskkill /F /IM EikonDicomViewer.exe";
            if (CreateProcess(NULL, const_cast<wchar_t*>(killCmd.c_str()), NULL, NULL, 
                             FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 5000); // 5 second timeout
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            Sleep(1000);
            
            fs::remove_all(g_tempDir);
        }
        
        // Create new directory
        fs::create_directories(g_tempDir);
        
        UpdateStatus(L"Destination prepared");
        return true;
    }
    catch (const exception& e) {
        LogMessage(L"ERROR", L"Failed to create destination directory: " + StringToWString(e.what()));
        return false;
    }
}

bool CopyDicomDir() {
    LogMessage(L"INFO", L"Copying DICOMDIR file");
    UpdateStatus(L"Copying DICOMDIR");
    
    wstring sourcePath = g_sourceDir + L"\\DICOMDIR";
    wstring destPath = g_tempDir + L"\\DICOMDIR";
    
    if (!FileExists(sourcePath)) {
        LogMessage(L"WARNING", L"DICOMDIR file not found at " + sourcePath);
        return false;
    }
    
    try {
        fs::copy_file(sourcePath, destPath, fs::copy_options::overwrite_existing);
        LogMessage(L"INFO", L"DICOMDIR file copied successfully");
        return true;
    }
    catch (const exception& e) {
        LogMessage(L"ERROR", L"Failed to copy DICOMDIR: " + StringToWString(e.what()));
        return false;
    }
}

bool StartRobocopy() {
    LogMessage(L"INFO", L"Starting robocopy for DicomFiles");
    UpdateStatus(L"Copying DICOM files");
    
    wstring sourceDir = g_sourceDir + L"\\DicomFiles";
    wstring destDir = g_tempDir + L"\\DicomFiles";
    
    if (!DirectoryExists(sourceDir)) {
        LogMessage(L"WARNING", L"DicomFiles directory not found at " + sourceDir);
        return false;
    }
    
    // Build robocopy command
    wstring robocopyCmd = L"robocopy \"" + sourceDir + L"\" \"" + destDir + 
                         L"\" /E /R:3 /W:1 /LOG+:\"" + g_logFile + L"\"";
    
    // Start robocopy in background
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    if (CreateProcess(NULL, const_cast<wchar_t*>(robocopyCmd.c_str()), NULL, NULL, 
                     FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        LogMessage(L"INFO", L"Robocopy started successfully");
        return true;
    }
    
    LogMessage(L"ERROR", L"Failed to start robocopy");
    return false;
}

bool CopyFfmpegExe() {
    LogMessage(L"INFO", L"Starting ffmpeg.exe copy");
    UpdateStatus(L"Copying ffmpeg.exe");
    
    wstring sourcePath = g_sourceDir + L"\\ffmpeg.exe";
    wstring destPath = g_tempDir + L"\\ffmpeg.exe";
    
    if (!FileExists(sourcePath)) {
        LogMessage(L"WARNING", L"ffmpeg.exe not found at " + sourcePath);
        return false;
    }
    
    // Build copy command to run in background
    wstring copyCmd = L"copy /Y \"" + sourcePath + L"\" \"" + destPath + L"\"";
    
    // Start copy in background
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    if (CreateProcess(NULL, const_cast<wchar_t*>(copyCmd.c_str()), NULL, NULL, 
                     FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        LogMessage(L"INFO", L"ffmpeg.exe copy started successfully");
        return true;
    }
    
    LogMessage(L"ERROR", L"Failed to start ffmpeg.exe copy");
    return false;
}

bool Extract7zArchive() {
    LogMessage(L"INFO", L"Extracting 7z archive");
    UpdateStatus(L"Extracting files");
    
    wstring sevenzaPath = g_sourceDir + L"\\" + SEVENZA_EXE;
    wstring archivePath = g_sourceDir + L"\\EikonDicomViewer.7z";
    
    if (!FileExists(sevenzaPath)) {
        LogMessage(L"WARNING", L"7za.exe not found at " + sevenzaPath);
        return false;
    }
    
    if (!FileExists(archivePath)) {
        LogMessage(L"WARNING", L"Archive not found at " + archivePath);
        return false;
    }
    
    // Build 7za extraction command
    wstring extractCmd = L"\"" + sevenzaPath + L"\" x \"" + archivePath + 
                        L"\" -o\"" + g_tempDir + L"\" -y";
    
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    if (CreateProcess(NULL, const_cast<wchar_t*>(extractCmd.c_str()), NULL, NULL,
                     FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        
        // Wait for extraction to complete
        WaitForSingleObject(pi.hProcess, 30000); // 30 second timeout
        
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (exitCode == 0) {
            LogMessage(L"INFO", L"7z extraction completed successfully");
            return true;
        } else {
            LogMessage(L"ERROR", L"7z extraction failed with exit code: " + to_wstring(exitCode));
        }
    }
    
    LogMessage(L"ERROR", L"Failed to start 7za extraction");
    return false;
}

void BringViewerToFront() {
    // Find EikonDicomViewer window and bring to front
    Sleep(2000); // Wait for viewer to start
    
    // First try to find by process name
    HWND viewerWnd = nullptr;
    
    // Enumerate windows to find EikonDicomViewer
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        HWND* pViewerWnd = reinterpret_cast<HWND*>(lParam);
        
        DWORD processId;
        GetWindowThreadProcessId(hwnd, &processId);
        
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
        if (hProcess) {
            wchar_t processName[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageName(hProcess, 0, processName, &size)) {
                wstring name = fs::path(processName).filename().wstring();
                if (name == L"EikonDicomViewer.exe") {
                    *pViewerWnd = hwnd;
                    CloseHandle(hProcess);
                    return FALSE; // Stop enumeration
                }
            }
            CloseHandle(hProcess);
        }
        return TRUE; // Continue enumeration
    }, reinterpret_cast<LPARAM>(&viewerWnd));
    
    if (viewerWnd) {
        SetForegroundWindow(viewerWnd);
        ShowWindow(viewerWnd, SW_RESTORE);
        LogMessage(L"INFO", L"Brought viewer to front");
    } else {
        LogMessage(L"WARNING", L"Could not find EikonDicomViewer window to bring to front");
    }
}

bool LaunchViewer() {
    LogMessage(L"INFO", L"Launching DICOM viewer");
    UpdateStatus(L"Starting viewer");
    
    wstring viewerPath = g_tempDir + L"\\" + VIEWER_EXE;
    
    if (!FileExists(viewerPath)) {
        LogMessage(L"ERROR", L"Viewer not found at " + viewerPath);
        MessageBox(g_hMainWnd, L"DICOM Viewer not found. Copy operation may have failed.", 
                  L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    
    // Terminate any existing viewer processes
    STARTUPINFO killSi = { sizeof(killSi) };
    PROCESS_INFORMATION killPi = {};
    killSi.dwFlags = STARTF_USESHOWWINDOW;
    killSi.wShowWindow = SW_HIDE;
    
    wstring killCmd = L"taskkill /F /IM EikonDicomViewer.exe";
    if (CreateProcess(NULL, const_cast<wchar_t*>(killCmd.c_str()), NULL, NULL, 
                     FALSE, CREATE_NO_WINDOW, NULL, NULL, &killSi, &killPi)) {
        WaitForSingleObject(killPi.hProcess, 3000); // 3 second timeout
        CloseHandle(killPi.hProcess);
        CloseHandle(killPi.hThread);
    }
    Sleep(1000);
    
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;
    
    if (CreateProcess(viewerPath.c_str(), NULL, NULL, NULL, FALSE, 0, 
                     NULL, g_tempDir.c_str(), &si, &pi)) {
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        LogMessage(L"INFO", L"Viewer launched successfully");
        
        // Bring viewer to front in a separate thread
        thread bringToFrontThread(BringViewerToFront);
        bringToFrontThread.detach();
        
        return true;
    }
    
    LogMessage(L"ERROR", L"Failed to launch viewer");
    MessageBox(g_hMainWnd, L"Failed to start DICOM Viewer", L"Error", MB_OK | MB_ICONERROR);
    return false;
}

// Image loading from embedded byte array
bool LoadSplashImageFromResource() {
    try {
        // Create stream from embedded byte array
        IStream* pStream = CreateStreamFromByteArray(SPLASH_IMAGE_DATA, SPLASH_IMAGE_SIZE);
        if (pStream) {
            g_splashImage = make_unique<Image>(pStream);
            pStream->Release();
            
            if (g_splashImage->GetLastStatus() == Ok) {
                LogMessage(L"INFO", L"Splash image loaded from embedded byte array successfully");
                return true;
            } else {
                LogMessage(L"ERROR", L"Failed to create image from embedded byte array, GDI+ status: " + to_wstring(g_splashImage->GetLastStatus()));
                g_splashImage.reset();
            }
        } else {
            LogMessage(L"ERROR", L"Failed to create stream from embedded byte array");
        }
    }
    catch (const exception& e) {
        LogMessage(L"ERROR", L"Exception loading splash image: " + StringToWString(e.what()));
    }
    
    LogMessage(L"WARNING", L"Falling back to default splash screen");
    return false;
}

IStream* CreateStreamFromByteArray(const unsigned char* data, unsigned int size) {
    if (!data || size == 0) return nullptr;
    
    // Create a copy of the data for the stream
    HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hBuffer) return nullptr;
    
    void* pBuffer = GlobalLock(hBuffer);
    if (!pBuffer) {
        GlobalFree(hBuffer);
        return nullptr;
    }
    
    memcpy(pBuffer, data, size);
    GlobalUnlock(hBuffer);
    
    IStream* pStream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hBuffer, TRUE, &pStream);
    if (FAILED(hr)) {
        GlobalFree(hBuffer);
        return nullptr;
    }
    
    return pStream;
}

// DVD Detection Implementation
wstring DetectSourceDrive() {
    LogMessage(L"INFO", L"Detecting source drive...");
    
    // Method 1: Get executable path and extract drive
    wstring exeDrive = GetExecutableDrive();
    if (HasDicomData(exeDrive)) {
        LogMessage(L"INFO", L"Found DICOM data on executable drive: " + exeDrive);
        return exeDrive;
    }
    
    // Method 2: Check all DVD drives for DICOM data
    vector<wstring> dvdDrives = FindDvdDrives();
    for (const auto& drive : dvdDrives) {
        if (HasDicomData(drive)) {
            LogMessage(L"INFO", L"Found DICOM data on DVD drive: " + drive);
            return drive;
        }
    }
    
    // Method 3: Check all removable drives
    vector<wstring> removableDrives = FindRemovableDrives();
    for (const auto& drive : removableDrives) {
        if (HasDicomData(drive)) {
            LogMessage(L"INFO", L"Found DICOM data on removable drive: " + drive);
            return drive;
        }
    }
    
    // Method 4: Fallback - check all drives
    vector<wstring> allDrives = FindAllDrives();
    for (const auto& drive : allDrives) {
        if (HasDicomData(drive)) {
            LogMessage(L"INFO", L"Found DICOM data on drive: " + drive);
            return drive;
        }
    }
    
    LogMessage(L"ERROR", L"No DICOM data found on any drive");
    return L"";
}

bool HasDicomData(const wstring& drivePath) {
    try {
        wstring dicomdirPath = drivePath + L"\\DICOMDIR";
        wstring dicomfilesPath = drivePath + L"\\DicomFiles";
        wstring viewerPath = drivePath + L"\\" + VIEWER_EXE;
        
        // Must have either DICOMDIR or DicomFiles, and preferably the viewer
        bool hasDicom = FileExists(dicomdirPath) || DirectoryExists(dicomfilesPath);
        bool hasViewer = FileExists(viewerPath);
        
        return hasDicom && hasViewer;
    }
    catch (...) {
        return false;
    }
}

vector<wstring> FindDvdDrives() {
    vector<wstring> dvdDrives;
    
    for (wchar_t drive = L'A'; drive <= L'Z'; drive++) {
        wstring drivePath = wstring(1, drive) + L":\\";
        
        UINT driveType = GetDriveType(drivePath.c_str());
        if (driveType == DRIVE_CDROM) {
            // Check if drive is accessible
            if (DirectoryExists(drivePath)) {
                dvdDrives.push_back(drivePath);
            }
        }
    }
    
    return dvdDrives;
}

vector<wstring> FindRemovableDrives() {
    vector<wstring> removableDrives;
    
    for (wchar_t drive = L'A'; drive <= L'Z'; drive++) {
        wstring drivePath = wstring(1, drive) + L":\\";
        
        UINT driveType = GetDriveType(drivePath.c_str());
        if (driveType == DRIVE_REMOVABLE || driveType == DRIVE_CDROM) {
            // Check if drive is accessible
            if (DirectoryExists(drivePath)) {
                removableDrives.push_back(drivePath);
            }
        }
    }
    
    return removableDrives;
}

vector<wstring> FindAllDrives() {
    vector<wstring> allDrives;
    
    for (wchar_t drive = L'A'; drive <= L'Z'; drive++) {
        wstring drivePath = wstring(1, drive) + L":\\";
        
        // Check if drive exists and is accessible
        if (DirectoryExists(drivePath)) {
            allDrives.push_back(drivePath);
        }
    }
    
    return allDrives;
}

wstring GetExecutableDrive() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    
    // Extract drive letter (e.g., "C:" from "C:\path\to\exe")
    wstring fullPath(exePath);
    if (fullPath.length() >= 2 && fullPath[1] == L':') {
        return fullPath.substr(0, 2) + L"\\";  // Return "C:\"
    }
    
    return L"";
}

void ExecutePipeline() {
    Sleep(1000); // Initial delay to show splash screen
    
    try {
        // Log the detected source directory
        LogMessage(L"INFO", L"Using source directory: " + g_sourceDir);
        
        // Step 1: Create destination directory
        UpdateStatus(L"Preparing destination");
        if (!CreateDestinationDirectory()) {
            MessageBox(g_hMainWnd, L"Failed to prepare destination directory", 
                      L"Error", MB_OK | MB_ICONERROR);
            return;
        }
        
        // Step 2: Start robocopy for DicomFiles (background)
        UpdateStatus(L"Starting file copy");
        StartRobocopy();
        
        // Step 2b: Copy ffmpeg.exe (background)
        CopyFfmpegExe();
        
        // Step 3: Copy DICOMDIR file
        UpdateStatus(L"Copying DICOMDIR");
        CopyDicomDir();
        
        // Step 4: Extract 7z archive
        UpdateStatus(L"Extracting files");
        Extract7zArchive();
        
        // Step 5: Launch viewer
        UpdateStatus(L"Starting viewer");
        if (LaunchViewer()) {
            // Success! Kill the timeout timer and exit normally
            KillTimer(g_hMainWnd, ID_TIMEOUT_TIMER);
            Sleep(2000); // Brief pause to show success
            g_isRunning = false;
            PostMessage(g_hMainWnd, WM_DESTROY, 0, 0);
        }
    }
    catch (const exception& e) {
        LogMessage(L"ERROR", L"Pipeline error: " + StringToWString(e.what()));
        MessageBox(g_hMainWnd, L"An error occurred during processing", L"Error", MB_OK | MB_ICONERROR);
    }
}