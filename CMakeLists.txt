cmake_minimum_required (VERSION 3.31)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT_SLUG "bt")

project ("Browser Tamer"
        VERSION 6.3.2
        HOMEPAGE_URL "https://www.aloneguid.uk/projects/bt/"
        LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Browser Torero builds for Windows only.")
endif()

# UNICODE affects the Win32 API, _UNICODE the CRT entry point
add_compile_definitions(UNICODE _UNICODE NOMINMAX)

string(TIMESTAMP CURRENT_YEAR "%Y")
configure_file(globals.h.in ${CMAKE_CURRENT_BINARY_DIR}/globals.h @ONLY)
configure_file(bt.rc.in "${CMAKE_CURRENT_SOURCE_DIR}/bt.rc" @ONLY)

# Static CRT unless the dynamic vcpkg triplet is used, so bt.exe runs without the VC++ redistributable.
string(TOLOWER "$ENV{VCPKG_TRIPLET}" VCPKG_TRIPLET_LOWER)
if(VCPKG_TRIPLET_LOWER STREQUAL "x64-windows")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
    add_compile_options(/utf-8 /W3)
    if(CMAKE_BUILD_TYPE MATCHES Release)
        add_link_options(/OPT:REF /OPT:ICF)
    endif()
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(fkYAML CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_path(P_RANAV_CSV2_INCLUDE_DIRS "csv2/mio.hpp")
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

enable_testing()

# Everything except the entry point, so the tests can link it.
file(GLOB core_src
        "res.cpp"
        "globals.cpp"
        "cmdline.cpp"
        "state.cpp"
        "app/*.cpp"
        "app/ui/*.cpp"
        "app/pipeline/*.cpp"
        "common/*.cpp"
        "common/win32/*.cpp")
add_library(bt_core STATIC ${core_src})
target_include_directories(bt_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${P_RANAV_CSV2_INCLUDE_DIRS}
        ${SIMPLEINI_INCLUDE_DIRS}
        ${LUA_INCLUDE_DIR})
target_link_libraries(bt_core PUBLIC
        nlohmann_json::nlohmann_json
        tinyxml2::tinyxml2
        unofficial::sqlite3::sqlite3
        fkYAML::fkYAML
        magic_enum::magic_enum
        ${LUA_LIBRARIES}
        comctl32 gdiplus shlwapi dwmapi uxtheme winhttp psapi pdh version comdlg32 shcore msimg32)

add_executable(${PROJECT_SLUG} WIN32 bt.cpp bt.rc bt.manifest)
target_link_libraries(${PROJECT_SLUG} PRIVATE bt_core)

if(MSVC)
    # bt.exe starts for every click and usually only forwards it to the resident process.
    # Delay-loading keeps those short-lived starts from mapping DLLs that only the UI needs.
    set(BT_DELAY_LOAD comctl32 gdiplus shlwapi dwmapi uxtheme winhttp psapi pdh version comdlg32 shcore msimg32
            shell32 ole32 oleaut32)
    foreach(dll ${BT_DELAY_LOAD})
        target_link_options(${PROJECT_SLUG} PRIVATE "/DELAYLOAD:${dll}.dll")
    endforeach()
    target_link_libraries(${PROJECT_SLUG} PRIVATE delayimp)
endif()

file(GLOB test_src "tests/*.cpp")
add_executable(test1 ${test_src})
target_link_libraries(test1 PRIVATE bt_core GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
include(GoogleTest)
gtest_discover_tests(test1)

# ClearURLs database deployed next to the executable.
set(RULES_URL https://rules2.clearurls.xyz/data.minify.json)
set(RULES_JSON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/app/pipeline/clearurls_rules.json)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    file(DOWNLOAD "${RULES_URL}" "${RULES_JSON_PATH}" SHOW_PROGRESS)
endif()

foreach(target ${PROJECT_SLUG} test1)
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RULES_JSON_PATH}"
            "$<TARGET_FILE_DIR:${target}>/clearurls_rules.json")
endforeach()

# "Install" target
set(CPACK_PACKAGE_NAME "BrowserTamer")
set(CPACK_PACKAGE_VENDOR "@aloneguid")
set(CPACK_PACKAGE_CONTACT "@aloneguid")
# Shown as "Browser Torero"; the project, package and folder names stay "Browser Tamer" so upgrades and
# the existing config keep working.
set(CPACK_PACKAGE_DISPLAY_NAME "Browser Torero")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "BrowserTamer")
install(TARGETS ${PROJECT_SLUG} RUNTIME DESTINATION ".")
install(FILES "${RULES_JSON_PATH}" DESTINATION ".")

# to use WIX generator (for producing .msi files) you can install wix toolset with .net sdk:
# dotnet tool install --global wix
#
# then run the silly "eula accept" command:
# wix eula accept wix7
#
# then install wix ui extension required by cpack:
# wix extension add --global WixToolset.UI.wixext

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
    set(CPACK_SYSTEM_NAME "win-arm64")
else()
    set(CPACK_SYSTEM_NAME "win64")
endif()

set(CPACK_GENERATOR WIX)

# "4" is required in order not to use the legacy WIX 3.x generator. It did not matter that I was using WIX 7.
set(CPACK_WIX_VERSION 4)

# WiX requires a stable Upgrade GUID — generate ONE and hardcode it (don't regenerate per build)
set(CPACK_WIX_UPGRADE_GUID "C7630F02-56B6-41A6-8166-9D15AF6961E7")
# needs to re-generate
set(CPACK_WIX_PRODUCT_GUID "*")

set(CPACK_WIX_INSTALL_SCOPE "perUser")
set(CPACK_WIX_LICENSE_RTF "${CMAKE_SOURCE_DIR}/resources/LICENSE.rtf")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/icon.ico")
set(CPACK_WIX_UI_BANNER "${CMAKE_SOURCE_DIR}/resources/wix/wix_banner.png")
set(CPACK_WIX_UI_DIALOG "${CMAKE_SOURCE_DIR}/resources/wix/wix_dialog.png")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Browser Torero")
set(CPACK_WIX_PROPERTY_APPHELPLINK "${PROJECT_HOMEPAGE_URL}")
set(CPACK_WIX_PATCH_FILE "${CMAKE_SOURCE_DIR}/resources/wix/patch.xml")

set_property(INSTALL "$<TARGET_FILE_NAME:bt>"
        PROPERTY CPACK_START_MENU_SHORTCUTS "Browser Torero"
)
include(CPack) # needs to be at the end of install configurations
