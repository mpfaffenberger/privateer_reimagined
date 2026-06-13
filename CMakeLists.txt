cmake_minimum_required(VERSION 3.20)
project(new_privateer LANGUAGES C CXX OBJC OBJCXX)

# ---------------------------------------------------------------------------
# Global config
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Platform-specific backend selection for sokol
#   Mac    -> Metal      (SOKOL_METAL)
#   Linux  -> Vulkan? not yet — stick with GL for now (SOKOL_GLCORE)
#   Win    -> D3D11      (SOKOL_D3D11)
# ---------------------------------------------------------------------------
if(APPLE)
    set(SOKOL_BACKEND SOKOL_METAL)
    set(SHADER_SLANG metal_macos)
elseif(WIN32)
    set(SOKOL_BACKEND SOKOL_D3D11)
    set(SHADER_SLANG hlsl5)
else()
    set(SOKOL_BACKEND SOKOL_GLCORE)
    set(SHADER_SLANG glsl430)
endif()

message(STATUS "sokol backend    : ${SOKOL_BACKEND}")
message(STATUS "shader target    : ${SHADER_SLANG}")

# ---------------------------------------------------------------------------
# sokol-shdc: cross-compile shaders/*.glsl -> src/generated/*.glsl.h
# Each .glsl file produces one header you can `#include`.
# ---------------------------------------------------------------------------
set(SHDC ${CMAKE_SOURCE_DIR}/third_party/bin/sokol-shdc)
set(GEN_DIR ${CMAKE_SOURCE_DIR}/src/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

function(compile_shader SHADER_NAME)
    set(IN  ${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.glsl)
    set(OUT ${GEN_DIR}/${SHADER_NAME}.glsl.h)
    add_custom_command(
        OUTPUT  ${OUT}
        # NOTE: each generated header contains `static const uint8_t
        # vs_source_<slang>[...]` arrays named by shader stage, not by
        # program. That means two shader headers can't share a TU without
        # link conflicts. We handle this by including each shader header in
        # exactly one .cpp file — see src/sun.cpp vs src/sun_corona.cpp.
        COMMAND ${SHDC} --input ${IN} --output ${OUT} --slang ${SHADER_SLANG} --format sokol
        DEPENDS ${IN} ${SHDC}
        COMMENT "sokol-shdc: ${SHADER_NAME}.glsl -> ${SHADER_NAME}.glsl.h"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${OUT})
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

compile_shader(skybox)
compile_shader(sun)
compile_shader(sun_glow)
compile_shader(sun_gas)
compile_shader(dust)
compile_shader(asteroid)
compile_shader(mesh)
compile_shader(atmosphere)
compile_shader(sprite)
compile_shader(sprite_spot)
compile_shader(post_blur)
compile_shader(post_composite)

# ---------------------------------------------------------------------------
# Target: new_privateer
# ---------------------------------------------------------------------------
# The Obj-C++ TU exists only on Apple; elsewhere we compile sokol_impl as
# plain C++ (we'll rename the file extension at add_executable time).
if(APPLE)
    set(SOKOL_IMPL_SRC src/sokol_impl.mm)
else()
    # On non-Apple, we just set a compile-definition and reuse main.cpp's
    # SOKOL_IMPL block via a tiny .cpp shim. For now, only Apple is wired
    # because that's what Mike is on. Add Linux/Win later.
    set(SOKOL_IMPL_SRC src/sokol_impl.mm)
endif()

# Dear ImGui — pulled into third_party/imgui/ by hand (see commit log). We
# compile it in unity-of-sorts: the 4 core .cpp files get added to the
# target and sokol_imgui.h is header-only (included once with IMPL macro
# from debug_panel.cpp).
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SRC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)

add_executable(new_privateer
    src/main.cpp
    src/audio.cpp
    src/sfx.cpp
    src/camera.cpp
    src/skybox.cpp
    src/sun.cpp
    src/sun_corona.cpp
    src/sun_gas.cpp
    src/dust.cpp
    src/rendertargets.cpp
    src/postprocess.cpp
    src/postprocess_composite.cpp
    src/star_presets.cpp
    src/mesh.cpp
    src/mesh_cache.cpp
    src/material.cpp
    src/asteroid.cpp
    src/json.cpp
    src/obj_loader.cpp
    src/system_def.cpp
    src/galaxy.cpp
    src/commodity.cpp
    src/economy.cpp
    src/outfitting.cpp
    src/missions.cpp
    src/faction.cpp
    src/comm.cpp
    src/game_state.cpp
    src/docking.cpp
    src/autopilot.cpp
    src/threat.cpp
    src/jump.cpp
    src/encounters.cpp
    src/base_screens.cpp
    src/savegame.cpp
    src/player.cpp
    src/mobility.cpp
    src/gun.cpp
    src/shield.cpp
    src/armor.cpp
    src/ship_class.cpp
    src/ship.cpp
    src/ship_registry.cpp
    src/ship_ai.cpp
    src/perception.cpp
    src/firing.cpp
    src/projectile.cpp
    src/missile.cpp
    src/repair.cpp
    src/explosion.cpp
    src/mesh_render.cpp
    src/mesh_render_atmosphere.cpp
    src/sprite.cpp
    src/ship_sprite.cpp
    src/sprite_light_editor.cpp
    src/mesh_orient_editor.cpp
    src/sprite_generation_tool.cpp
    src/planet_texture.cpp
    src/debug_panel.cpp
    src/atlas_grid_viewer.cpp
    src/cockpit_hud.cpp
    src/dev_remote.cpp
    src/dev_remote_macos.mm
    ${IMGUI_SRC}
    ${SOKOL_IMPL_SRC}
    ${SHADER_OUTPUTS}
)

# Bake the asset path into the runtime so we can launch from anywhere. For
# now we just use a relative path + chdir-to-source; for shipping we'd copy
# assets next to the binary. Good enough for Stage 2.
add_custom_command(TARGET new_privateer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:new_privateer>/assets
    COMMENT "symlinking assets/ next to binary"
)

target_include_directories(new_privateer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/generated
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
)

# Dear ImGui is a big codebase that triggers lots of unused-function /
# unused-parameter warnings under -Wall -Wextra. We trust upstream, so
# silence those *just for the imgui TUs* — our own code stays strict.
set_source_files_properties(${IMGUI_SRC} PROPERTIES
    COMPILE_OPTIONS "-w"
)

target_compile_definitions(new_privateer PRIVATE ${SOKOL_BACKEND})

target_compile_options(new_privateer PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
)

# ---------------------------------------------------------------------------
# Platform libraries
# ---------------------------------------------------------------------------
if(APPLE)
    target_link_libraries(new_privateer PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework MetalKit"
        "-framework AudioToolbox"
    )
elseif(WIN32)
    target_link_libraries(new_privateer PRIVATE d3d11 dxgi)
else()
    find_package(OpenGL REQUIRED)
    find_package(X11    REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(new_privateer PRIVATE OpenGL::GL ${X11_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Headless integration test harness: test_full_loop (np-zte.3)
# ---------------------------------------------------------------------------
# The END-TO-END regression spine for the sandbox loop. It links a SUBSET of
# the real game modules — no GPU, no audio, no ImGui — under the same
# *_HEADLESS guards the per-feature harnesses (test_economy / test_outfitting /
# test_missions / test_reputation) already use, then walks the whole loop
# programmatically: new_game -> encounter spawn -> kill (rep + bounty) -> dock
# + autosave -> commodity arbitrage -> outfitting -> missions -> repair ->
# homing-missile kill + afterburner -> jump (round-trip + hostile gate) ->
# save/reload round-trip -> death/respawn. dev_remote can't inject keystrokes
# (np-w68), so this is the scriptable proxy for the manual playtest.
#
# EXCLUDE_FROM_ALL so the normal `cmake --build build -j` stays a pure game
# build; opt in explicitly and run from the repo root (asset paths are
# relative):
#   cmake --build build --target test_full_loop && ./build/test_full_loop
#
# The other tools/test_*.cpp harnesses document a standalone clang++ recipe in
# their header; test_full_loop carries one too (see the file header) for the
# no-CMake path. This target is the wired-in convenience.
add_executable(test_full_loop EXCLUDE_FROM_ALL
    tools/test_full_loop.cpp
    src/game_state.cpp src/player.cpp src/faction.cpp src/comm.cpp
    src/commodity.cpp src/economy.cpp src/outfitting.cpp src/missions.cpp
    src/galaxy.cpp src/system_def.cpp src/json.cpp src/savegame.cpp
    src/docking.cpp src/autopilot.cpp src/threat.cpp src/jump.cpp
    src/encounters.cpp src/repair.cpp src/missile.cpp src/ship.cpp
    src/ship_class.cpp src/ship_registry.cpp src/ship_ai.cpp
    src/perception.cpp src/gun.cpp src/shield.cpp src/armor.cpp
    src/mobility.cpp src/camera.cpp
)
target_include_directories(test_full_loop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_full_loop PRIVATE
    ECONOMY_HEADLESS OUTFITTING_HEADLESS MISSIONS_HEADLESS COMM_HEADLESS
)
