cmake_minimum_required(VERSION 3.16)
project(DicomViewerSplash)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define Unicode support
add_definitions(-DUNICODE -D_UNICODE)

# Set output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Create the executable
add_executable(DicomViewerSplash 
    DicomViewerSplash.cpp
)

# Link required Windows libraries
target_link_libraries(DicomViewerSplash
    gdiplus
    shlwapi
    comctl32
)

# Set subsystem to Windows (GUI application)
if(WIN32)
    set_target_properties(DicomViewerSplash PROPERTIES
        WIN32_EXECUTABLE TRUE
        LINK_FLAGS "/SUBSYSTEM:WINDOWS"
    )
endif()