cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)

set(SWITCH_APP_NAME "melonDS Switch" CACHE STRING "Title shown in hbmenu")
set(SWITCH_APP_VERSION "0.2.0" CACHE STRING "Version shown in hbmenu and in the menu")

add_executable(melonDS-switch
    main.cpp
    Gfx.cpp
    Layout.cpp
    ../ScreenLayout.cpp
    Platform.cpp
)
target_compile_definitions(melonDS-switch PRIVATE SWITCH_APP_VERSION="${SWITCH_APP_VERSION}")

# timing profilers (prof=/glprof= columns of the log); they read the system counter in the hot loops
option(SWITCH_PROFILE "Build the emulation-loop and GL profilers (MELONDS_PROF, MELONDS_GL_PROFILE)" ON)

if (ENABLE_OGLRENDERER)
    # GLES 3 renderer (Mesa/nouveau on the console): same definitions as the WebGL build
    target_compile_definitions(core PUBLIC "MELONDS_GL_HEADER=<GLES3/gl3.h>" MELONDS_GLES)
    if (SWITCH_PROFILE)
        target_compile_definitions(core PUBLIC MELONDS_GL_PROFILE)
    endif()
    target_include_directories(core PUBLIC $ENV{DEVKITPRO}/portlibs/switch/include)
endif()

# GL 2D renderer: count of mid-frame screen sections (main.cpp switches heavy games to software rendering)
target_compile_definitions(core PUBLIC MELONDS_GL_SECTION_COUNT)

# one emulator instance, always run from the main thread (see NDS::Current)
target_compile_definitions(core PUBLIC MELONDS_SINGLE_NDS_THREAD)

# optional timing of the emulation loop (src/Prof.h), read by main.cpp
if (SWITCH_PROFILE)
    target_compile_definitions(core PUBLIC MELONDS_PROF)
endif()

target_include_directories(melonDS-switch PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(melonDS-switch PRIVATE core Threads::Threads SDL2_ttf freetype harfbuzz png16 bz2 z lz4 ${SDL2_LIBRARIES} EGL GLESv2 glapi drm_nouveau nx)

# .nro (needs devkitPro: nacptool, elf2nro)
find_program(NACPTOOL nacptool HINTS $ENV{DEVKITPRO}/tools/bin)
find_program(ELF2NRO elf2nro HINTS $ENV{DEVKITPRO}/tools/bin)
if (NACPTOOL AND ELF2NRO)
    add_custom_command(TARGET melonDS-switch POST_BUILD
        COMMAND ${NACPTOOL} --create "${SWITCH_APP_NAME}" "FrcJules" "${SWITCH_APP_VERSION}" melonDS-switch.nacp
        COMMAND ${ELF2NRO} $<TARGET_FILE:melonDS-switch> melonDS-switch.nro --nacp=melonDS-switch.nacp --icon=${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
        COMMENT "Creating melonDS-switch.nro")
endif()
