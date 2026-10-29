cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)

set(SWITCH_APP_NAME "melonDS Switch" CACHE STRING "Title shown in hbmenu")
set(SWITCH_APP_VERSION "0.2.0" CACHE STRING "Version shown in hbmenu and in the menu")

add_executable(melonDS-switch
    main.cpp
    Platform.cpp
)
target_compile_definitions(melonDS-switch PRIVATE SWITCH_APP_VERSION="${SWITCH_APP_VERSION}")

# deko3d presentation (render=deko3d in config.txt): shaders compiled by uam at build time, embedded in the binary
option(SWITCH_DEKO3D "Build the deko3d presentation path" ON)
if (SWITCH_DEKO3D)
    find_program(UAM uam HINTS $ENV{DEVKITPRO}/tools/bin REQUIRED)
    set(DK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/deko3d)
    set(DK_OUT ${CMAKE_CURRENT_BINARY_DIR}/deko3d)
    add_custom_command(OUTPUT ${DK_OUT}/gfx_shaders.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DK_OUT}
        COMMAND ${UAM} -s vert -o ${DK_OUT}/gfx_vsh.dksh ${DK_SRC}/gfx_vsh.glsl
        COMMAND ${UAM} -s frag -o ${DK_OUT}/gfx_fsh.dksh ${DK_SRC}/gfx_fsh.glsl
        COMMAND ${CMAKE_COMMAND} -DOUT=${DK_OUT}/gfx_shaders.h -DINPUTS=GfxVshDksh=${DK_OUT}/gfx_vsh.dksh|GfxFshDksh=${DK_OUT}/gfx_fsh.dksh -P ${DK_SRC}/EmbedDksh.cmake
        DEPENDS ${DK_SRC}/gfx_vsh.glsl ${DK_SRC}/gfx_fsh.glsl ${DK_SRC}/EmbedDksh.cmake
        COMMENT "Compiling deko3d shaders (uam)"
        VERBATIM)
    # compute renderer (3D on deko3d): the 33 variants of src/GPU3D_Compute_shaders.h written at 1x by a host tool
    find_program(HOST_CXX NAMES g++ c++ clang++ PATHS /usr/bin /usr/local/bin NO_DEFAULT_PATH REQUIRED)
    set(DK_COMPUTE InterpXSpansZ InterpXSpansW BinCombined DepthBlendZ DepthBlendW
        RasteriseNoTextureZ RasteriseNoTextureW RasteriseNoTextureToonZ RasteriseNoTextureToonW
        RasteriseNoTextureHighlightZ RasteriseNoTextureHighlightW RasteriseUseTextureDecalZ RasteriseUseTextureDecalW
        RasteriseUseTextureModulateZ RasteriseUseTextureModulateW RasteriseUseTextureToonZ RasteriseUseTextureToonW
        RasteriseUseTextureHighlightZ RasteriseUseTextureHighlightW RasteriseShadowMaskZ RasteriseShadowMaskW
        ClearCoarseBinMask ClearIndirectWorkCount CalculateWorkOffsets SortWork
        FinalPass FinalPassEdge FinalPassFog FinalPassEdgeFog FinalPassAA FinalPassAAEdge FinalPassAAFog FinalPassAAEdgeFog)
    set(DK_UAM_CMDS)
    set(DK_EMBED)
    foreach(n ${DK_COMPUTE})
        list(APPEND DK_UAM_CMDS COMMAND ${UAM} -s comp -o ${DK_OUT}/cs/${n}.dksh ${DK_OUT}/cs/${n}.glsl)
        list(APPEND DK_EMBED "Cs${n}=${DK_OUT}/cs/${n}.dksh")
    endforeach()
    string(REPLACE ";" "|" DK_EMBED "${DK_EMBED}")
    add_custom_command(OUTPUT ${DK_OUT}/compute_shaders.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DK_OUT}/cs
        COMMAND ${HOST_CXX} -O1 -std=c++17 -o ${DK_OUT}/GenComputeShaders ${DK_SRC}/GenComputeShaders.cpp
        COMMAND ${DK_OUT}/GenComputeShaders ${DK_OUT}/cs
        ${DK_UAM_CMDS}
        COMMAND ${CMAKE_COMMAND} -DOUT=${DK_OUT}/compute_shaders.h -DINPUTS=${DK_EMBED} -P ${DK_SRC}/EmbedDksh.cmake
        DEPENDS ${DK_SRC}/GenComputeShaders.cpp ${CMAKE_SOURCE_DIR}/src/GPU3D_Compute_shaders.h ${DK_SRC}/EmbedDksh.cmake
        COMMENT "Compiling the deko3d compute renderer shaders (33 variants, uam)"
        VERBATIM)
    target_sources(melonDS-switch PRIVATE deko3d/GfxDeko.cpp deko3d/DekoRenderer.cpp ${DK_OUT}/gfx_shaders.h ${DK_OUT}/compute_shaders.h)
    target_include_directories(melonDS-switch PRIVATE ${DK_OUT})
    target_compile_definitions(melonDS-switch PRIVATE SWITCH_DEKO3D)
    # debug build of deko3d (more checks, reported through the debug callback) for development
    option(SWITCH_DEKO3D_DEBUG "Link the debug build of deko3d (libdeko3dd)" OFF)
    if (SWITCH_DEKO3D_DEBUG)
        target_link_libraries(melonDS-switch PRIVATE deko3dd)
    else()
        target_link_libraries(melonDS-switch PRIVATE deko3d)
    endif()
endif()

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
