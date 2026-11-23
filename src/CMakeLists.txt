cmake_minimum_required(VERSION 3.24)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MX_VERSION_TEXT)
string(STRIP "${MX_VERSION_TEXT}" MX_VERSION_TEXT)
project(MezinXT VERSION ${MX_VERSION_TEXT} LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(cmake/Warnings.cmake)
if(MSVC)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()
configure_file(mx_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/mx_version.h @ONLY)
# Tests write only under the repository's git-ignored work/ folder.
get_filename_component(MX_WORK "${CMAKE_CURRENT_SOURCE_DIR}/../work" ABSOLUTE)
get_filename_component(MX_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests" ABSOLUTE)

# app/mezinxt.yaml is the default configuration written on first start.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/mezinxt.yaml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/app/mezinxt.yaml MX_DEFAULT_YAML HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MX_DEFAULT_YAML "${MX_DEFAULT_YAML}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/mezinxt_default_yaml.h "static const unsigned char mx_default_yaml[]={${MX_DEFAULT_YAML}};\n")

file(GLOB MX_YAML_SOURCES third_party/libyaml/src/*.c)
add_library(mx_yaml STATIC ${MX_YAML_SOURCES})
target_include_directories(mx_yaml PUBLIC third_party/libyaml/include)
target_compile_definitions(mx_yaml PUBLIC YAML_DECLARE_STATIC PRIVATE
    YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5"
    _CRT_SECURE_NO_WARNINGS)

# Everything OS-specific: one implementation per OS.
if(WIN32)
    add_library(mx_host STATIC host/host_win32.c)
    target_link_libraries(mx_host PUBLIC ws2_32)
else()
    add_library(mx_host STATIC host/host_posix.c)
endif()
mx_set_warnings(mx_host)

# The emulator core: no SDL, no OS header, no wall clock. The chips (hw/)
# join it as they are written.
add_library(mx_core STATIC core/config.c core/yamledit.c core/romdb.c core/font8x8.c core/machine.c core/video_out.c core/stub_machine.c
    hw/cpu8086.c)
target_include_directories(mx_core PUBLIC core hw PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mx_core PUBLIC mx_yaml mx_host)
# The composite monitor's filter (video_out.c) uses the C math library, a separate one outside Windows.
if(NOT WIN32)
    target_link_libraries(mx_core PUBLIC m)
endif()
mx_set_warnings(mx_core)
target_sources(mx_core PRIVATE hw/i8255.c hw/i8237.c hw/xtkbd.c)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    URL https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL2)

# The shell's timing: the desktop loop's pacing and the sound output's clock sync, with no SDL and no clock of their own.
add_library(mx_timing STATIC app/pace.c platform/audiosync.c)
target_link_libraries(mx_timing PUBLIC mx_core)
mx_set_warnings(mx_timing)

# The shell: the platform layer and the application.
add_library(mx_platform STATIC platform/video.c platform/audio.c platform/overlay.c platform/serial.c platform/png.c)
target_link_libraries(mx_platform PUBLIC mx_core mx_timing SDL2::SDL2-static)
target_compile_definitions(mx_platform PUBLIC SDL_MAIN_HANDLED)
mx_set_warnings(mx_platform)

# The text-mode toolkit and the setup screen: no SDL.
add_library(mx_ui STATIC app/tui.c app/setup.c)
target_link_libraries(mx_ui PUBLIC mx_core)
mx_set_warnings(mx_ui)

# The host key map: SDL's scancode names only.
add_library(mx_keymap STATIC app/keymap.c)
target_link_libraries(mx_keymap PUBLIC mx_core SDL2::SDL2-static)
target_compile_definitions(mx_keymap PUBLIC SDL_MAIN_HANDLED)
mx_set_warnings(mx_keymap)

add_executable(mezinxt app/mezinxt.c app/desktop.c app/pads.c)
target_include_directories(mezinxt PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mezinxt PRIVATE mx_platform mx_ui mx_keymap)
if(WIN32)
    # A Windows program without a console window of its own (app/mezinxt.c main).
    set_target_properties(mezinxt PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(mezinxt PRIVATE app/mezinxt.manifest)   # UTF-8 file names
    enable_language(RC)
    set(MX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../assets/mezinxt.ico")
    set(MX_ICON_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/mezinxt_icon.rc")
    file(CONFIGURE OUTPUT "${MX_ICON_RESOURCE}" CONTENT "1 ICON \"${MX_ICON}\"\n")
    set_source_files_properties("${MX_ICON_RESOURCE}" PROPERTIES OBJECT_DEPENDS "${MX_ICON}")
    target_sources(mezinxt PRIVATE "${MX_ICON_RESOURCE}")
    if(MSVC)
        target_link_options(mezinxt PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()
mx_set_warnings(mezinxt)

# The chips and boards of the machine.
target_sources(mx_core PRIVATE hw/i8259.c hw/i8253.c hw/speaker.c hw/drivesnd.c
    hw/sched.c hw/bus.c hw/junior.c core/junior_machine.c
    hw/mc6845.c hw/cga.c core/cga_card.c
    hw/fdd.c hw/upd765.c hw/fdc_card.c core/disk.c
    hw/z80sio.c hw/sio_card.c hw/lpt.c core/ports_card.c
    hw/ns8250.c hw/sermouse.c core/pcserial_card.c
    hw/hdc.c hw/guest/hdrom.c
    hw/fpu8087.c hw/gameport.c core/stick.c
    hw/mda.c core/mda_card.c core/text.c)

# The tests (ctest): one short smoke test per part that needs none of the others, plain programs on the core, no SDL.
# tests/verify.py runs the program itself for the rest. They write only under work/tests.
enable_testing()
file(MAKE_DIRECTORY ${MX_WORK}/tests/config)
foreach(test cpu_test test_config test_fpu8087 test_hdc test_ports)
    add_executable(${test} ${MX_TESTS}/${test}.c)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(${test} PRIVATE mx_core)
    target_compile_definitions(${test} PRIVATE MX_WORK="${MX_WORK}")
    mx_set_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The CPU conformance runner (tests/cpu_suite.py run): not a ctest test, the suites are downloaded on request.
add_executable(cpu_suite ${MX_TESTS}/cpu_suite.c)
target_link_libraries(cpu_suite PRIVATE mx_core)
mx_set_warnings(cpu_suite)
