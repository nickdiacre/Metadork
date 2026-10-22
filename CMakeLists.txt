cmake_minimum_required(VERSION 3.20)
project(SdiMetaViewer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SDIMETA_BUILD_APP   "Build the GUI app (needs the DeckLink SDK)" ON)
option(SDIMETA_BUILD_TESTS "Build parser tests (no SDK or hardware needed)" ON)
set(DECKLINK_SDK_DIR "" CACHE PATH
    "Blackmagic DeckLink SDK root, i.e. the folder containing Win/, Mac/ and Linux/")

# ---------------------------------------------------------------- core (no SDK)
add_library(sdimeta_core STATIC
    src/anc/AncRegistry.cpp
    src/anc/Rp214.cpp
    src/anc/ArriMetadata.cpp
    src/anc/BmdCameraControl.cpp
    src/anc/Rdd18.cpp
    src/anc/MetadataProcessor.cpp
    src/anc/TakeTracker.cpp
    src/log/JsonLogger.cpp
    src/meta/MetaRecord.cpp
    src/meta/Adapters.cpp
    src/out/CameraSample.cpp
    src/out/Protocols.cpp
    src/out/UdpSender.cpp
    src/out/OutputManager.cpp
    src/out/Settings.cpp
    src/out/SensorModes.cpp)
target_include_directories(sdimeta_core PUBLIC src)
if(WIN32)
    target_link_libraries(sdimeta_core PUBLIC ws2_32)
endif()

if(SDIMETA_BUILD_TESTS)
    enable_testing()
    add_executable(sdimeta_tests tests/test_parsers.cpp)
    target_link_libraries(sdimeta_tests PRIVATE sdimeta_core)
    add_test(NAME parsers COMMAND sdimeta_tests)
endif()

# Field registry + per-camera coverage report from captures (no SDK needed)
add_executable(sdimeta_coverage src/tools/Coverage.cpp)
target_link_libraries(sdimeta_coverage PRIVATE sdimeta_core)

if(NOT SDIMETA_BUILD_APP)
    return()
endif()

if(NOT DECKLINK_SDK_DIR OR NOT EXISTS "${DECKLINK_SDK_DIR}")
    message(FATAL_ERROR "Set DECKLINK_SDK_DIR to the unpacked DeckLink SDK "
                        "(e.g. -DDECKLINK_SDK_DIR=\"C:/SDKs/Blackmagic DeckLink SDK 14.4\"), "
                        "or configure with -DSDIMETA_BUILD_APP=OFF to build only the tests.")
endif()

# ---------------------------------------------------------------- dependencies
include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# X11 only by default (runs fine under XWayland). Pass -DGLFW_BUILD_WAYLAND=ON for native Wayland.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "")
FetchContent_Declare(glfw  GIT_REPOSITORY https://github.com/glfw/glfw.git   GIT_TAG 3.4)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
FetchContent_MakeAvailable(glfw imgui)
find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------- DeckLink SDK per platform
set(APP_SOURCES src/main.cpp src/capture/Capture.cpp src/capture/DeckLinkPlatform.cpp)

if(WIN32)
    # The Windows SDK ships IDL; generate the C++ header + IID/CLSID definitions with MIDL.
    set(DL_IDL_DIR "${DECKLINK_SDK_DIR}/Win/include")
    set(DL_GEN "${CMAKE_BINARY_DIR}/decklink")
    file(MAKE_DIRECTORY "${DL_GEN}")
    add_custom_command(
        OUTPUT "${DL_GEN}/DeckLinkAPI_h.h" "${DL_GEN}/DeckLinkAPI_i.c"
        COMMAND midl /nologo /env x64 /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c
                /out "${DL_GEN}" /I "${DL_IDL_DIR}" "${DL_IDL_DIR}/DeckLinkAPI.idl"
        DEPENDS "${DL_IDL_DIR}/DeckLinkAPI.idl"
        COMMENT "Generating DeckLink COM headers with MIDL")
    list(APPEND APP_SOURCES "${DL_GEN}/DeckLinkAPI_i.c")
    set(DL_INCLUDE "${DL_GEN}" "${DL_IDL_DIR}")  # generated COM headers + DeckLinkAPIVersion.h
elseif(APPLE)
    set(DL_INCLUDE "${DECKLINK_SDK_DIR}/Mac/include")
    list(APPEND APP_SOURCES "${DL_INCLUDE}/DeckLinkAPIDispatch.cpp")
else()
    set(DL_INCLUDE "${DECKLINK_SDK_DIR}/Linux/include")
    list(APPEND APP_SOURCES "${DL_INCLUDE}/DeckLinkAPIDispatch.cpp")
endif()

add_executable(SdiMetaViewer ${APP_SOURCES})
target_include_directories(SdiMetaViewer PRIVATE ${DL_INCLUDE} src)
target_link_libraries(SdiMetaViewer PRIVATE sdimeta_core imgui_lib)

# Headless check of which ANC / timecode / raw VANC data reaches the API.
set(PROBE_SOURCES ${APP_SOURCES})
list(REMOVE_ITEM PROBE_SOURCES src/main.cpp src/capture/Capture.cpp)
add_executable(sdimeta_probe src/tools/Probe.cpp ${PROBE_SOURCES})
target_include_directories(sdimeta_probe PRIVATE ${DL_INCLUDE} src)
target_link_libraries(sdimeta_probe PRIVATE sdimeta_core)

foreach(t SdiMetaViewer sdimeta_probe)
    if(WIN32)
        target_link_libraries(${t} PRIVATE ole32 oleaut32)
        set_property(TARGET ${t} PROPERTY WIN32_EXECUTABLE FALSE)
    elseif(APPLE)
        target_link_libraries(${t} PRIVATE "-framework CoreFoundation" "-framework Foundation" objc)
    else()
        target_link_libraries(${t} PRIVATE ${CMAKE_DL_LIBS} pthread)
    endif()
endforeach()
