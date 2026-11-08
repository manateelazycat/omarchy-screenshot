# SPDX-FileCopyrightText: 2026 Andy Stewart
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.21)
project(omarchy_screenshot VERSION 0.1.12 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Test LinguistTools)
find_package(LayerShellQt REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.37)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(VIRTUAL_POINTER_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml)
set(VIRTUAL_POINTER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-client-protocol.h)
set(VIRTUAL_POINTER_CODE ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-protocol.c)
add_custom_command(
    OUTPUT ${VIRTUAL_POINTER_HEADER} ${VIRTUAL_POINTER_CODE}
    COMMAND ${WAYLAND_SCANNER} client-header ${VIRTUAL_POINTER_XML} ${VIRTUAL_POINTER_HEADER}
    COMMAND ${WAYLAND_SCANNER} private-code ${VIRTUAL_POINTER_XML} ${VIRTUAL_POINTER_CODE}
    DEPENDS ${VIRTUAL_POINTER_XML}
)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

include(FetchContent)
set(PPOCR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PPOCR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PPOCR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PPOCR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PPOCR_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
set(PPOCR_ENABLE_METAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rapidocrlite
    GIT_REPOSITORY https://github.com/RapidAI/RapidOCRLite.git
    GIT_TAG v5.0.0a16
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rapidocrlite)
# RapidOCRLite installs its own headers, library, and licenses at the prefix
# root. Name this app's rules so installs can select only them.
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "omarchy-screenshot")

set(OMARCHY_SCREENSHOT_OCR_MODEL_DIR "${CMAKE_BINARY_DIR}/ocr-models"
    CACHE PATH "Directory containing the PP-OCRv6 Tiny models")
file(MAKE_DIRECTORY "${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}")
function(fetch_ocr_model name hash url)
    if(NOT EXISTS "${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}/${name}")
        message(STATUS "Downloading OCR model ${name}")
        file(DOWNLOAD "${url}" "${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}/${name}"
            EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Cannot download ${name}: ${status}")
        endif()
    endif()
endfunction()
fetch_ocr_model(PP-OCRv6_det_tiny.onnx
    f42c0fbd294d95eac1a550e131b277dac97462c8025fa4b6c3cec1b7894bd3d5
    "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/det/PP-OCRv6_det_tiny.onnx")
fetch_ocr_model(PP-OCRv6_rec_tiny.onnx
    e16e242de5937ad92609223f19bc2aff3727ee40b095f996907c24749bad251b
    "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_tiny.onnx")
fetch_ocr_model(ppocrv6_tiny_dict.txt
    c5cbe34ef40c29c4df07ed012bf96569cb69a2d2a01a07027e9f13cb832bd9cd
    "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/paddle/PP-OCRv6/rec/PP-OCRv6_rec_tiny/ppocrv6_tiny_dict.txt")

# The image source protocol references foreign toplevels, so its generated
# code needs that interface too.
set(capture_protocols
    staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
    staging/ext-image-capture-source/ext-image-capture-source-v1.xml
    staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
)
foreach(protocol IN LISTS capture_protocols)
    get_filename_component(protocol_name "${protocol}" NAME_WE)
    set(protocol_xml "${WAYLAND_PROTOCOLS_DIR}/${protocol}")
    set(protocol_header "${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-client-protocol.h")
    set(protocol_code "${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-protocol.c")
    add_custom_command(
        OUTPUT "${protocol_header}" "${protocol_code}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${protocol_xml}" "${protocol_header}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${protocol_xml}" "${protocol_code}"
        DEPENDS "${protocol_xml}"
        VERBATIM
    )
    list(APPEND protocol_sources "${protocol_header}" "${protocol_code}")
endforeach()

qt_add_executable(omarchy-screenshot
    src/main.cpp
    src/capturecontroller.cpp
    src/capturecontroller.h
    src/cursorcaptureguard.cpp
    src/cursorcaptureguard.h
    src/pinnedimages.cpp
    src/pinnedimages.h
    src/pinnedwindows.cpp
    src/pinnedwindows.h
    src/countdown.cpp
    src/countdown.h
    src/scrollstitcher.cpp
    src/scrollstitcher.h
    src/longimageitem.cpp
    src/longimageitem.h
    src/virtualpointer.cpp
    src/virtualpointer.h
    ${VIRTUAL_POINTER_CODE}
    ${VIRTUAL_POINTER_HEADER}
    src/daemonsocket.cpp
    src/daemonsocket.h
    src/screencapture.cpp
    src/screencapture.h
    src/ocr.cpp
    src/ocr.h
    ${protocol_sources}
)

set_source_files_properties(qml/Overlay.qml PROPERTIES QT_RESOURCE_ALIAS Overlay.qml)
set_source_files_properties(qml/ToolbarGlyph.qml PROPERTIES QT_RESOURCE_ALIAS ToolbarGlyph.qml)
set_source_files_properties(qml/AnnotationToolbar.qml PROPERTIES QT_RESOURCE_ALIAS AnnotationToolbar.qml)
set_source_files_properties(qml/LongOverlay.qml PROPERTIES QT_RESOURCE_ALIAS LongOverlay.qml)
set_source_files_properties(qml/ScrollCapture.qml PROPERTIES QT_RESOURCE_ALIAS ScrollCapture.qml)
set_source_files_properties(qml/SelectionResizeHandle.qml PROPERTIES QT_RESOURCE_ALIAS SelectionResizeHandle.qml)
set_source_files_properties(qml/PinOverlay.qml PROPERTIES QT_RESOURCE_ALIAS PinOverlay.qml)
set_source_files_properties(qml/Countdown.qml PROPERTIES QT_RESOURCE_ALIAS Countdown.qml)
qt_add_qml_module(omarchy-screenshot
    URI OmarchyScreenshot
    VERSION 1.0
    QML_FILES qml/Overlay.qml qml/ToolbarGlyph.qml qml/AnnotationToolbar.qml qml/LongOverlay.qml qml/ScrollCapture.qml qml/SelectionResizeHandle.qml qml/PinOverlay.qml qml/Countdown.qml
)

target_link_libraries(omarchy-screenshot PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test LayerShellQt::Interface
    PkgConfig::WAYLAND_CLIENT ppocr
)
target_include_directories(omarchy-screenshot PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(omarchy-screenshot PRIVATE
    OMARCHY_SCREENSHOT_OCR_MODEL_DIR="${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}")

set(screenshot_locales
    az be bg hr cs da nl et fi fr ka de el he hu is ga it ja kk ky lo
    lv lt mk nb pl pt_PT pt_BR ro ru sr sk sl es sv tg tr uk zh_CN zh_TW
)
foreach(locale IN LISTS screenshot_locales)
    list(APPEND screenshot_translations "translations/omarchy-screenshot_${locale}.ts")
endforeach()
qt_add_translations(omarchy-screenshot
    TS_FILES ${screenshot_translations}
    SOURCES src/main.cpp src/capturecontroller.cpp src/virtualpointer.cpp qml/Overlay.qml
        qml/AnnotationToolbar.qml qml/LongOverlay.qml qml/ScrollCapture.qml qml/PinOverlay.qml
    LUPDATE_OPTIONS -no-obsolete -locations none
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME translations
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/translations.py"
            --app $<TARGET_FILE:omarchy-screenshot>
            --lupdate $<TARGET_FILE:Qt6::lupdate>
            --source "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME scroll-stitch
        COMMAND omarchy-screenshot --scroll-stitch-test)
    set_tests_properties(scroll-stitch PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Builds the controller again so reset() is tested without a compositor.
    qt_add_executable(daemon-test
        tests/daemon.cpp
        src/daemonsocket.cpp
        src/capturecontroller.cpp
        src/cursorcaptureguard.cpp
        src/scrollstitcher.cpp
        src/virtualpointer.cpp
        src/ocr.cpp
        src/ocr.h
        ${VIRTUAL_POINTER_CODE}
        src/screencapture.cpp
        ${protocol_sources}
    )
    target_include_directories(daemon-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(daemon-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test PkgConfig::WAYLAND_CLIENT ppocr)
    target_compile_definitions(daemon-test PRIVATE
        OMARCHY_SCREENSHOT_OCR_MODEL_DIR="${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}")
    add_test(NAME daemon COMMAND daemon-test)
    set_tests_properties(daemon PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")

    qt_add_executable(cursor-capture-test tests/cursorcapture.cpp
        src/cursorcaptureguard.cpp src/cursorcaptureguard.h)
    target_include_directories(cursor-capture-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(cursor-capture-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME cursor-capture COMMAND cursor-capture-test)
    set_tests_properties(cursor-capture PROPERTIES TIMEOUT 30)

    # Manual compositor integration test; CTest also runs without Wayland.
    qt_add_executable(cursor-live-test tests/cursor_live.cpp
        src/capturecontroller.cpp src/cursorcaptureguard.cpp
        src/scrollstitcher.cpp src/virtualpointer.cpp ${VIRTUAL_POINTER_CODE}
        src/ocr.cpp src/ocr.h src/screencapture.cpp ${protocol_sources})
    target_include_directories(cursor-live-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(cursor-live-test PRIVATE
        Qt6::Quick Qt6::Test LayerShellQt::Interface PkgConfig::WAYLAND_CLIENT ppocr)
    target_compile_definitions(cursor-live-test PRIVATE
        OMARCHY_SCREENSHOT_OCR_MODEL_DIR="${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}")

    qt_add_executable(ocr-test tests/tst_ocr.cpp src/ocr.cpp src/ocr.h)
    target_include_directories(ocr-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ocr-test PRIVATE
        OMARCHY_SCREENSHOT_OCR_MODEL_DIR="${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}")
    target_link_libraries(ocr-test PRIVATE Qt6::Gui Qt6::Test ppocr)
    add_test(NAME ocr COMMAND ocr-test)
    set_tests_properties(ocr PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60)

    qt_add_executable(pin-test tests/pinnedimages.cpp
        src/pinnedimages.cpp src/pinnedimages.h
        src/pinnedwindows.cpp src/pinnedwindows.h)
    target_include_directories(pin-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(pin-test PRIVATE Qt6::Quick Qt6::Test LayerShellQt::Interface)
    target_compile_definitions(pin-test PRIVATE
        PIN_OVERLAY="${CMAKE_CURRENT_SOURCE_DIR}/qml/PinOverlay.qml")
    add_test(NAME pins COMMAND pin-test)
    set_tests_properties(pins PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software")

    find_program(QML_TEST_RUNNER NAMES qmltestrunner qmltestrunner6
        HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    if(QML_TEST_RUNNER)
        add_test(NAME toolbar COMMAND "${QML_TEST_RUNNER}"
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_toolbar.qml")
        set_tests_properties(toolbar PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS omarchy-screenshot RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    "${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}/PP-OCRv6_det_tiny.onnx"
    "${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}/PP-OCRv6_rec_tiny.onnx"
    "${OMARCHY_SCREENSHOT_OCR_MODEL_DIR}/ppocrv6_tiny_dict.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/omarchy-screenshot/ocr")
install(FILES "${rapidocrlite_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omarchy-screenshot"
    RENAME LICENSE.RapidOCRLite)
install(FILES "${rapidocrlite_SOURCE_DIR}/src/indra/LICENSE-indra-ocr"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omarchy-screenshot")
install(FILES "${rapidocrlite_SOURCE_DIR}/third_party/x86-simd-sort/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omarchy-screenshot"
    RENAME LICENSE.x86-simd-sort)

# systemd starts the resident daemon on the first request and lets it exit
# when idle. Relative to the install prefix, so `--prefix` relocates it; a
# per-user install (~/.local) should point this at ~/.local/share/systemd/user.
set(SYSTEMD_USER_UNIT_DIR "lib/systemd/user"
    CACHE STRING "Where to install the systemd user units")
configure_file(systemd/omarchy-screenshot.service.in
    "${CMAKE_CURRENT_BINARY_DIR}/omarchy-screenshot.service" @ONLY)
install(FILES
    systemd/omarchy-screenshot.socket
    "${CMAKE_CURRENT_BINARY_DIR}/omarchy-screenshot.service"
    DESTINATION "${SYSTEMD_USER_UNIT_DIR}")

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(tst-countdown tests/tst_countdown.cpp src/countdown.cpp src/countdown.h)
    target_include_directories(tst-countdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst-countdown PRIVATE
        Qt6::Quick Qt6::Test LayerShellQt::Interface PkgConfig::WAYLAND_CLIENT)
    qt_add_resources(tst-countdown countdown
        PREFIX /qt/qml/OmarchyScreenshot
        BASE qml
        FILES qml/Countdown.qml)
    add_test(NAME countdown COMMAND tst-countdown)
    set_tests_properties(countdown PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software"
        TIMEOUT 15)
    qt_add_executable(tst-preview-scaling tests/tst_preview_scaling.cpp)
    target_include_directories(tst-preview-scaling PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tst-preview-scaling PRIVATE Qt6::Quick Qt6::Test)
    target_compile_definitions(tst-preview-scaling PRIVATE
        PREVIEW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/preview_scaling.qml")
    foreach(scale IN ITEMS 1 1.25 1.5 2)
        add_test(NAME preview-scaling-${scale} COMMAND tst-preview-scaling)
        set_tests_properties(preview-scaling-${scale} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale}"
            TIMEOUT 15)
    endforeach()
endif()
