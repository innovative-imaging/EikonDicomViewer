# Makefile for DicomViewerSplash with embedded image data
# Use with Visual Studio Developer Command Prompt or VS Code with C++ extension

CXX = cl
CXXFLAGS = /std:c++17 /EHsc /W3 /O2 /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE
LDFLAGS = /link /SUBSYSTEM:WINDOWS
LIBS = gdiplus.lib shlwapi.lib comctl32.lib user32.lib kernel32.lib gdi32.lib

TARGET = DicomViewerSplash.exe
SOURCE = DicomViewerSplash.cpp
OUTDIR = bin

all: $(OUTDIR)/$(TARGET)

$(OUTDIR):
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"

$(OUTDIR)/$(TARGET): $(SOURCE) splash_image_data.h | $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(SOURCE) /Fe:$(OUTDIR)/$(TARGET) $(LDFLAGS) $(LIBS)

splash_image_data.h: convert_image.py
	python convert_image.py

clean:
	@if exist "$(OUTDIR)" rmdir /s /q "$(OUTDIR)"
	@if exist "*.obj" del "*.obj"
	@if exist "*.pdb" del "*.pdb"

rebuild: clean all

.PHONY: all clean rebuild