cmake_minimum_required(VERSION 3.21)

# installer/Orbit.iss is the one place the release version is set; the project, the
# executables' version resources and ORBIT_VERSION all follow it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/installer/Orbit.iss" ORBIT_ISS_VERSION_LINE REGEX "^#define AppVersion ")
string(REGEX REPLACE "^#define AppVersion \"([^\"]+)\".*$" "\\1" ORBIT_RELEASE_VERSION "${ORBIT_ISS_VERSION_LINE}")
if(NOT ORBIT_RELEASE_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "Couldn't read AppVersion from installer/Orbit.iss")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/installer/Orbit.iss")

project(Orbit
    VERSION ${ORBIT_RELEASE_VERSION}
    DESCRIPTION "Orbit Download Manager - multi-connection HTTP and BitTorrent downloads"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(ORBIT_BUILD_TESTS "Build the test suite (run it with ctest)" OFF)
option(ORBIT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(ORBIT_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Quick QuickControls2 Network)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(LibtorrentRasterbar CONFIG REQUIRED)
find_package(Threads REQUIRED)
if(UNIX AND NOT APPLE)
    # Power management, file manager hand-off and the settings portal go over D-Bus; the
    # startup splash draws itself on its own X connection, like the Win32 one on Windows.
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
endif()

qt_standard_project_setup()

message(STATUS "Orbit release version: ${ORBIT_RELEASE_VERSION}")


# Version resources for the Windows executables (shown in file properties, and part of
# what Windows and antivirus software use to identify the publisher's files).
string(REPLACE "." ";" _orbit_version_parts "${ORBIT_RELEASE_VERSION}")
list(LENGTH _orbit_version_parts _orbit_version_count)
while(_orbit_version_count LESS 4)
    list(APPEND _orbit_version_parts 0)
    math(EXPR _orbit_version_count "${_orbit_version_count} + 1")
endwhile()
list(SUBLIST _orbit_version_parts 0 4 _orbit_version_parts)
string(REPLACE ";" "," ORBIT_RC_VERSION "${_orbit_version_parts}")
function(orbit_version_resource target_name description output_var)
    set(ORBIT_RC_NAME "${target_name}")
    set(ORBIT_RC_DESCRIPTION "${description}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/ui/assets/version.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_version.rc" @ONLY)
    set(${output_var} "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_version.rc" PARENT_SCOPE)
endfunction()
orbit_version_resource(orbit "Orbit Download Manager" ORBIT_APP_VERSION_RC)
orbit_version_resource(orbit-native-host "Orbit browser extension connector" ORBIT_HOST_VERSION_RC)

# The download engine: HTTP (libcurl), BitTorrent (libtorrent) and media (yt-dlp, run
# through QProcess - the only reason it needs Qt Core). No GUI and no Qt Network, so it can
# back a CLI, a headless service or the test suite as easily as the app.
add_library(orbit_core STATIC
    src/core/Types.cpp
    src/core/DownloadTask.cpp
    src/core/HttpDownloader.cpp
    src/core/MediaDownloader.cpp
    include/core/MediaDownloader.hpp
    src/core/TorrentEngine.cpp
    src/core/DownloadManager.cpp
    src/core/DownloadManagerPersistence.cpp
    src/utils/Logger.cpp
    src/utils/FileUtils.cpp
    src/utils/StringUtils.cpp
    src/utils/SpeedLimiter.cpp
    include/utils/SpeedLimiter.hpp
    src/utils/Hashing.cpp
    include/utils/Hashing.hpp
)

target_include_directories(orbit_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(orbit_core PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    LibtorrentRasterbar::torrent-rasterbar
    Threads::Threads
    Qt6::Core
)

target_compile_definitions(orbit_core PUBLIC
    $<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN;_CRT_SECURE_NO_WARNINGS>
)

# App services around the engine: the web remote (with its bundled web UI), the
# single-instance guard, the orbit:// protocol handler and the scheduler.
add_library(orbit_services STATIC
    src/core/ProtocolHandler.cpp
    src/core/SingleInstanceGuard.cpp
    include/core/SingleInstanceGuard.hpp
    src/core/RemoteServer.cpp
    include/core/RemoteServer.hpp
    src/core/Scheduler.cpp
    include/core/Scheduler.hpp
    include/utils/LocalSocket.hpp
    src/web/webassets.qrc
)

target_link_libraries(orbit_services PUBLIC
    orbit_core
    Qt6::Network
)

foreach(_orbit_lib orbit_core orbit_services)
    if(MSVC)
        target_compile_options(${_orbit_lib} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${_orbit_lib} PRIVATE -Wall -Wextra)
    endif()
endforeach()

qt_add_executable(orbit
    src/ui/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.hpp
    src/ui/DownloadListModel.cpp
    src/ui/DownloadListModel.hpp
    src/ui/DownloadFilterProxyModel.cpp
    src/ui/DownloadFilterProxyModel.hpp
    src/ui/TrayController.cpp
    src/ui/TrayController.hpp
    src/ui/SplashScreen.cpp
    src/ui/SplashScreen.hpp
    src/ui/UpdateManager.cpp
    src/ui/UpdateManager.hpp
    src/ui/YtDlpUpdater.cpp
    src/ui/YtDlpUpdater.hpp
    src/ui/LocalApiServer.cpp
    src/ui/LocalApiServer.hpp
    src/ui/BrowserIntegration.cpp
    src/ui/BrowserIntegration.hpp
    src/ui/PowerManager.cpp
    src/ui/PowerManager.hpp
    src/ui/ClipboardMonitor.cpp
    src/ui/ClipboardMonitor.hpp
    src/ui/ChecksumCalculator.cpp
    src/ui/ChecksumCalculator.hpp
    src/ui/resources.qrc
    $<$<NOT:$<PLATFORM_ID:Windows>>:${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts.qrc>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/src/ui/assets/app.rc>
    $<$<PLATFORM_ID:Windows>:${ORBIT_APP_VERSION_RC}>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/src/ui/assets/orbit.manifest>
)

target_include_directories(orbit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_link_libraries(orbit PRIVATE
    orbit_services
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    $<$<PLATFORM_ID:Windows>:dwmapi>
    $<$<PLATFORM_ID:Windows>:crypt32>
    $<$<PLATFORM_ID:Windows>:powrprof>
)

if(UNIX AND NOT APPLE)
    target_sources(orbit PRIVATE src/ui/LinuxIntegration.cpp src/ui/LinuxIntegration.hpp)
    target_link_libraries(orbit PRIVATE Qt6::DBus PkgConfig::XCB OpenSSL::Crypto)
endif()

target_compile_definitions(orbit PRIVATE ORBIT_VERSION="${ORBIT_RELEASE_VERSION}")

set_target_properties(orbit PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(MSVC)
    target_compile_options(orbit PRIVATE /W4 /permissive-)
else()
    target_compile_options(orbit PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    # The browser starts this helper for native messaging; it forwards requests to the
    # running Orbit over its local API pipe. Static CRT so it runs from the install folder
    # without any runtime DLLs.
    add_executable(orbit-native-host
        src/native-host/main.cpp
        src/ui/assets/orbit.manifest
        ${ORBIT_HOST_VERSION_RC}
    )
    set_property(TARGET orbit-native-host PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(orbit-native-host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(orbit-native-host PRIVATE /W4 /permissive-)
    endif()
    add_dependencies(orbit orbit-native-host)
else()
    # Same job on Linux: the browser starts it over stdio and it forwards to Orbit's local
    # API socket. Plain POSIX, no Qt, so it starts instantly.
    add_executable(orbit-native-host src/native-host/main.cpp)
    target_compile_options(orbit-native-host PRIVATE -Wall -Wextra)
    add_dependencies(orbit orbit-native-host)
endif()

include(GNUInstallDirs)
install(TARGETS orbit
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
if(UNIX AND NOT APPLE)
    install(TARGETS orbit-native-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WIN32)
    # Only the Windows build ships Qt's plugins beside the executable; on Linux this file
    # would hide the system Qt's platform plugins and Orbit couldn't open a window.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "[Paths]\nPlugins = .\nQml2Imports = qml\n")
    add_custom_command(TARGET orbit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "$<TARGET_FILE_DIR:orbit>/qt.conf"
        COMMENT "Writing qt.conf next to orbit.exe"
    )
endif()

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET orbit POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/qml" "$<TARGET_FILE:orbit>"
            COMMENT "Running windeployqt to stage Qt runtime dependencies"
        )
    else()
        set(_qt_root "${Qt6_DIR}/../../Qt6")
        set(_qt_bin "${Qt6_DIR}/../../bin")
        if(EXISTS "${_qt_root}/plugins")
            file(GLOB _qt_runtime_dlls "${_qt_bin}/Qt6*.dll")
            add_custom_command(TARGET orbit POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_qt_runtime_dlls} "$<TARGET_FILE_DIR:orbit>"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/plugins/platforms" "$<TARGET_FILE_DIR:orbit>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/plugins/styles" "$<TARGET_FILE_DIR:orbit>/styles"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/plugins/imageformats" "$<TARGET_FILE_DIR:orbit>/imageformats"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/plugins/iconengines" "$<TARGET_FILE_DIR:orbit>/iconengines"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/plugins/networkinformation" "$<TARGET_FILE_DIR:orbit>/networkinformation"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/plugins/tls" "$<TARGET_FILE_DIR:orbit>/tls"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/qml/QtQml" "$<TARGET_FILE_DIR:orbit>/qml/QtQml"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/qml/QtQuick" "$<TARGET_FILE_DIR:orbit>/qml/QtQuick"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_qt_root}/qml/QtCore" "$<TARGET_FILE_DIR:orbit>/qml/QtCore"
                COMMENT "Staging Qt plugins and builtin QML modules next to orbit.exe (windeployqt not found)"
            )
        endif()
    endif()
endif()

if(ORBIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
