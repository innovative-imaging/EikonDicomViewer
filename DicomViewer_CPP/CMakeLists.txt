cmake_minimum_required(VERSION 3.16)
project(DicomViewer_CPP VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static runtime to match DCMTK libraries
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Set Qt6 installation path
set(CMAKE_PREFIX_PATH "C:/Qt_6.9/6.9.3/msvc2022_64")

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui Multimedia)

# Set DCMTK installation path
set(DCMTK_DIR "D:/Repos/dcmtk/install_lib")
set(DCMTK_ROOT "D:/Repos/dcmtk/install_lib")

# Find DCMTK
find_package(DCMTK REQUIRED)
message(STATUS "DCMTK found - using proper DCMTK parsing")

# Set TurboJPEG installation path
set(TURBOJPEG_ROOT "C:/libjpeg-turbo64")

# Check for TurboJPEG
if(EXISTS "${TURBOJPEG_ROOT}/include/turbojpeg.h")
    message(STATUS "TurboJPEG found - enabling hybrid performance mode")
    set(HAVE_TURBOJPEG ON)
else()
    message(STATUS "TurboJPEG not found - using DCMTK-only mode")
    set(HAVE_TURBOJPEG OFF)
endif()

# Find GDCM installed via vcpkg
find_package(GDCM CONFIG QUIET)

# Check for GDCM - also check vcpkg installation directly
if(GDCM_FOUND)
    message(STATUS "GDCM found via vcpkg - enabling accelerated JPEG Lossless decompression")
    set(HAVE_GDCM ON)
elseif(EXISTS "C:/vcpkg/installed/x64-windows/include/gdcmImageReader.h")
    message(STATUS "GDCM found in vcpkg installation - enabling accelerated JPEG Lossless decompression")
    set(HAVE_GDCM ON)
    # Manually set paths for vcpkg GDCM
    set(GDCM_INCLUDE_DIRS "C:/vcpkg/installed/x64-windows/include")
    set(GDCM_LIBRARY_DIRS "C:/vcpkg/installed/x64-windows/lib")
else()
    message(STATUS "GDCM not found - install via vcpkg for faster JPEG Lossless performance")
    set(HAVE_GDCM OFF)
endif()

# Enable automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Source files
set(SOURCES
    src/main.cpp
    src/dicomviewer.cpp
    src/dicomviewer.h
    src/dicomreader.cpp
    src/dicomreader.h
    src/progressiveframeloader.cpp
    src/progressiveframeloader.h
    src/DicomFrameProcessor.cpp
    src/DicomFrameProcessor.h
    src/saveimagedialog.cpp
    src/saveimagedialog.h
    src/saverundialog.cpp
    src/saverundialog.h
    # Professional Framework Classes (Simplified)
    src/DicomPlaybackController_Simple.cpp
    src/DicomPlaybackController_Simple.h
    src/DicomInputHandler_Simple.cpp
    src/DicomInputHandler_Simple.h
)

# Resources
qt6_add_resources(RESOURCES resources/resources.qrc)

# Create executable with custom name (WIN32 hides console window)
add_executable(EikonDicomViewer WIN32 ${SOURCES} ${RESOURCES})

# Link Qt6 libraries
target_link_libraries(EikonDicomViewer Qt6::Core Qt6::Widgets Qt6::Gui Qt6::Multimedia)

# Link DCMTK libraries and define HAVE_DCMTK
target_include_directories(EikonDicomViewer PRIVATE "D:/Repos/dcmtk/install_lib/include")
target_compile_definitions(EikonDicomViewer PRIVATE HAVE_DCMTK)

# Add TurboJPEG support if available
if(HAVE_TURBOJPEG)
    target_include_directories(EikonDicomViewer PRIVATE "${TURBOJPEG_ROOT}/include")
    target_compile_definitions(EikonDicomViewer PRIVATE HAVE_TURBOJPEG)
endif()

# Add GDCM support if available
if(HAVE_GDCM)
    target_compile_definitions(EikonDicomViewer PRIVATE HAVE_GDCM)
    # Add include directories if manually detected
    if(GDCM_INCLUDE_DIRS)
        target_include_directories(EikonDicomViewer PRIVATE ${GDCM_INCLUDE_DIRS})
    endif()
endif()

# Ensure proper debug/release definitions
target_compile_definitions(EikonDicomViewer PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)

# Compiler-specific optimizations
if(MSVC)
    # Explicitly remove debug definitions in Release and MinSizeRel modes
    target_compile_options(EikonDicomViewer PRIVATE
        $<$<CONFIG:Release>:/U_DEBUG>
        $<$<CONFIG:MinSizeRel>:/U_DEBUG>
    )
    
    # MinSizeRel specific optimizations for MSVC
    target_compile_options(EikonDicomViewer PRIVATE
        $<$<CONFIG:MinSizeRel>:/Os>      # Optimize for size
        $<$<CONFIG:MinSizeRel>:/GF>      # Enable string pooling
        $<$<CONFIG:MinSizeRel>:/Gy>      # Enable function-level linking
    )
    
    # MinSizeRel specific linker optimizations
    target_link_options(EikonDicomViewer PRIVATE
        $<$<CONFIG:MinSizeRel>:/OPT:REF>     # Remove unreferenced code
        $<$<CONFIG:MinSizeRel>:/OPT:ICF>     # Identical COMDAT folding
        $<$<CONFIG:MinSizeRel>:/LTCG>        # Link-time code generation
    )
    
    # Ensure Windows subsystem for all configurations (hides console)
    target_link_options(EikonDicomViewer PRIVATE
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:MinSizeRel>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>
    )
endif()

# Set different output directories for different build types
set_target_properties(EikonDicomViewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug/Debug
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release/Release
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/MinSizeRel/MinSizeRel
)
target_link_libraries(EikonDicomViewer 
    "D:/Repos/dcmtk/install_lib/lib/dcmdata.lib"
    "D:/Repos/dcmtk/install_lib/lib/dcmimgle.lib"
    "D:/Repos/dcmtk/install_lib/lib/dcmjpeg.lib"
    "D:/Repos/dcmtk/install_lib/lib/ijg8.lib"
    "D:/Repos/dcmtk/install_lib/lib/ijg12.lib"
    "D:/Repos/dcmtk/install_lib/lib/ijg16.lib"
    "D:/Repos/dcmtk/install_lib/lib/ofstd.lib"
    "D:/Repos/dcmtk/install_lib/lib/oflog.lib"
    "D:/Repos/dcmtk/install_lib/lib/oficonv.lib"
    ws2_32.lib
    netapi32.lib
    iphlpapi.lib
)

# Link TurboJPEG library if available
if(HAVE_TURBOJPEG)
    if(EXISTS "${TURBOJPEG_ROOT}/lib/turbojpeg.lib")
        target_link_libraries(EikonDicomViewer "${TURBOJPEG_ROOT}/lib/turbojpeg.lib")
        message(STATUS "Linking TurboJPEG static library")
    elseif(EXISTS "${TURBOJPEG_ROOT}/lib/turbojpeg-static.lib")
        target_link_libraries(EikonDicomViewer "${TURBOJPEG_ROOT}/lib/turbojpeg-static.lib")
        message(STATUS "Linking TurboJPEG static library (alternative name)")
    else()
        message(WARNING "TurboJPEG library file not found")
    endif()
endif()

# Link GDCM libraries if available
if(HAVE_GDCM)
    if(GDCM_FOUND)
        # Use CMake targets if found via find_package
        target_link_libraries(EikonDicomViewer 
            gdcmIOD 
            gdcmDICT 
            gdcmDSED 
            gdcmMEXD
            gdcmMSFF
            gdcmCommon
        )
    else()
        # Manually link libraries from vcpkg path
        target_link_libraries(EikonDicomViewer 
            "C:/vcpkg/installed/x64-windows/lib/gdcmIOD.lib"
            "C:/vcpkg/installed/x64-windows/lib/gdcmDICT.lib"
            "C:/vcpkg/installed/x64-windows/lib/gdcmDSED.lib"
            "C:/vcpkg/installed/x64-windows/lib/gdcmMSFF.lib"
            "C:/vcpkg/installed/x64-windows/lib/gdcmCommon.lib"
        )
    endif()
    message(STATUS "Linking GDCM libraries for accelerated JPEG decompression")
endif()



# Copy Qt6 DLLs and platform plugins to output directory (Windows specific)
if(WIN32)
    # Copy Qt6 DLLs
    add_custom_command(TARGET EikonDicomViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt6::Core>
        $<TARGET_FILE:Qt6::Widgets>
        $<TARGET_FILE:Qt6::Gui>
        $<TARGET_FILE:Qt6::Multimedia>
        $<TARGET_FILE_DIR:EikonDicomViewer>
    )
    
    # Copy platform plugins
    add_custom_command(TARGET EikonDicomViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EikonDicomViewer>/platforms
    )
    
    # Find Qt6 installation path and copy platform plugins
    get_target_property(QT6_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT6_WINDEPLOYQT_EXECUTABLE ${QT6_QMAKE_EXECUTABLE} PATH)
    set(QT6_WINDEPLOYQT_EXECUTABLE "${QT6_WINDEPLOYQT_EXECUTABLE}/windeployqt.exe")
    
    if(EXISTS ${QT6_WINDEPLOYQT_EXECUTABLE})
        add_custom_command(TARGET EikonDicomViewer POST_BUILD
            COMMAND ${QT6_WINDEPLOYQT_EXECUTABLE} --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-network --no-svg --no-multimedia --exclude-plugins tls,networkinformation,generic,iconengines,styles $<TARGET_FILE:EikonDicomViewer>
        )
    endif()
    
    # Copy TurboJPEG DLL if available
    if(HAVE_TURBOJPEG AND EXISTS "${TURBOJPEG_ROOT}/bin/turbojpeg.dll")
        add_custom_command(TARGET EikonDicomViewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TURBOJPEG_ROOT}/bin/turbojpeg.dll"
            $<TARGET_FILE_DIR:EikonDicomViewer>
            COMMENT "Copying TurboJPEG DLL"
        )
    endif()
endif()