cmake_minimum_required(VERSION 3.20)
# OBJC / OBJCXX are macOS-only here (the sokol_impl TU is built as .mm on
# Apple to bridge Cocoa + Metal). MSVC has no ObjC compiler, so we'd fail
# CMake's CheckOBJCCompilerFlag try-compile the moment we list it. Gate it
# by APPLE so Linux/Windows builds get plain C/C++.
if(APPLE)
    project(new_privateer LANGUAGES C CXX OBJC OBJCXX)
else()
    project(new_privateer LANGUAGES C CXX)
endif()

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
    # HLSL has no gl_PointSize / gl_PointCoord builtin (SPIRV-Cross errors
    # on them). Tell the shaders to compile out the point-size path. We use
    # our own define instead of sokol-shdc's SOKOL_D3D11 because this shdc
    # build doesn't reliably set the backend macros in the GLSL preprocessor.
    set(SHADER_DEFINES NP_NO_POINT_SIZE)
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
set(SHDC ${CMAKE_SOURCE_DIR}/third_party/bin/sokol-shdc${CMAKE_EXECUTABLE_SUFFIX})
set(GEN_DIR ${CMAKE_SOURCE_DIR}/src/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

function(compile_shader SHADER_NAME)
    set(IN  ${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.glsl)
    set(OUT ${GEN_DIR}/${SHADER_NAME}.glsl.h)
    # Pass optional per-backend preprocessor defines (e.g. NP_NO_POINT_SIZE
    # on D3D11). Empty on platforms that don't set SHADER_DEFINES.
    set(_shdc_defines "")
    if(SHADER_DEFINES)
        set(_shdc_defines --defines ${SHADER_DEFINES})
    endif()
    add_custom_command(
        OUTPUT  ${OUT}
        # NOTE: each generated header contains `static const uint8_t
        # vs_source_<slang>[...]` arrays named by shader stage, not by
        # program. That means two shader headers can't share a TU without
        # link conflicts. We handle this by including each shader header in
        # exactly one .cpp file — see src/sun.cpp vs src/sun_corona.cpp.
        COMMAND ${SHDC} --input ${IN} --output ${OUT} --slang ${SHADER_SLANG} ${_shdc_defines} --format sokol
        DEPENDS ${IN} ${SHDC}
        COMMENT "sokol-shdc: ${SHADER_NAME}.glsl -> ${SHADER_NAME}.glsl.h"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${OUT})
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

compile_shader(skybox)
compile_shader(skybox_gen)
compile_shader(sun)
compile_shader(sun_glow)
compile_shader(sun_gas)
compile_shader(dust)
compile_shader(warp_streaks)
compile_shader(jump_gate)
compile_shader(asteroid)
compile_shader(mesh)
compile_shader(atmosphere)
compile_shader(sprite)
compile_shader(sprite_spot)
compile_shader(post_blur)
compile_shader(post_composite)

# ---------------------------------------------------------------------------

# Mission Computer briefing-text gen header (issue #10).
# tools/gen_mission_templates.py reads re/mission_text.json (gitignored;
# user-recovered via re/extract_mission_text.py) and bakes the per-type
# summary + copy[] variants + ENMY/CORP name pools into
# src/mission_templates.gen.h.
#
# The generator rule exists ONLY when the recovered JSON is present. Fresh
# clones don't have it, so mission_templates_gen becomes a no-op and the
# checked-in header is compiled as-is (issue #427). Re-run CMake after
# recovering re/mission_text.json to switch regeneration on.
set(MISSION_TEXT_JSON ${CMAKE_SOURCE_DIR}/re/mission_text.json)
set(MISSION_TEMPLATES_H ${CMAKE_SOURCE_DIR}/src/mission_templates.gen.h)
if(EXISTS ${MISSION_TEXT_JSON})
    add_custom_command(
        OUTPUT  ${MISSION_TEMPLATES_H}
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${CMAKE_SOURCE_DIR}/tools/gen_mission_templates.py
                ${MISSION_TEXT_JSON} ${MISSION_TEMPLATES_H}
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_mission_templates.py
                ${MISSION_TEXT_JSON}
        COMMENT "gen_mission_templates: re/mission_text.json -> src/mission_templates.gen.h"
        VERBATIM
    )
    # Every target that compiles missions.cpp depends on this, so a stale
    # header is regenerated on the next build (see add_dependencies below).
    add_custom_target(mission_templates_gen DEPENDS ${MISSION_TEMPLATES_H})
else()
    message(STATUS "mission templates: re/mission_text.json not found; "
                   "using checked-in src/mission_templates.gen.h")
    add_custom_target(mission_templates_gen)
endif()
# Target: new_privateer
# ---------------------------------------------------------------------------
# The Obj-C++ TU exists only on Apple (Metal + Foundation headers force ObjC
# syntax). On Windows / Linux the same SOKOL_IMPL block lives in plain C++
# in src/sokol_impl.cpp. Exactly one of the two is compiled per platform to
# keep the SOKOL_IMPL one-definition rule.
if(APPLE)
    set(SOKOL_IMPL_SRC src/sokol_impl.mm)
else()
    set(SOKOL_IMPL_SRC src/sokol_impl.cpp)
endif()

# dev_remote: macOS gets the real POSIX-sockets + Cocoa-screenshot pair
# (dev_remote.cpp + dev_remote_macos.mm); other platforms get a tiny stub
# that satisfies the namespace and no-ops every call. See
# src/dev_remote_stub.cpp for the rationale + porting note.
if(APPLE)
    set(DEV_REMOTE_SRC src/dev_remote.cpp src/dev_remote_macos.mm)
else()
    set(DEV_REMOTE_SRC src/dev_remote_stub.cpp)
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

# The savegame module: public API + its PlayerState <-> JSON codec (#525).
# Every target that links savegame links all three.
set(SAVEGAME_SRC
    src/savegame.cpp
    src/savegame_read.cpp
    src/savegame_write.cpp
)

add_executable(new_privateer
    src/main.cpp
    src/app_cli.cpp
    src/audio.cpp
    src/sfx.cpp
    src/music.cpp
    src/music_dj.cpp
    src/tracelog.cpp
    src/camera.cpp
    src/skybox.cpp
    src/skybox_gen.cpp
    src/sky_family.cpp
    src/sun.cpp
    src/sun_corona.cpp
    src/sun_gas.cpp
    src/dust.cpp
    src/warp_streaks.cpp
    src/jump_gate.cpp
    src/rendertargets.cpp
    src/postprocess.cpp
    src/postprocess_composite.cpp
    src/star_presets.cpp
    src/title_screen.cpp
    src/title_scene.cpp
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
    src/commodity_ui.cpp
    src/outfitting.cpp
    src/scanner.cpp
    src/dealer_ui.cpp
    src/equipment_hardpoints.cpp
    src/equipment_panels.cpp
    src/equipment_ui.cpp
    src/inventory.cpp
    src/missions.cpp
    src/missions_ui.cpp
    src/mission_tracker.cpp
    src/faction.cpp
    src/comm.cpp
    src/voice.cpp
    src/hailing.cpp
    src/scripted_encounters.cpp
    src/cinematic.cpp
    src/cinematic_parse.cpp
    src/cinematic_triggers.cpp
    src/cinematic_studio.cpp
    src/cinematic_studio_io.cpp
    src/game_state.cpp
    src/world_clock.cpp
    src/docking.cpp
    src/autopilot.cpp
    src/threat.cpp
    src/jump.cpp
    src/hazards.cpp
    src/loot.cpp
    src/objectives.cpp
    src/encounters.cpp
    src/escort.cpp
    src/drone.cpp
    src/base_screens.cpp
    src/room_anim.cpp
    src/room_anim_data.cpp
    src/base_art_studio.cpp
    ${SAVEGAME_SRC}
    src/player.cpp
    src/plot.cpp
    src/fixers.cpp
    src/campaign.cpp
    src/mobility.cpp
    src/gun.cpp
    src/shield.cpp
    src/armor.cpp
    src/ship_class.cpp
    src/ship.cpp
    src/ship_registry.cpp
    src/ship_ai.cpp
    src/ai_brain.cpp
    src/perception.cpp
    src/firing.cpp
    src/projectile.cpp
src/bolt_art.cpp
    src/missile.cpp
    src/repair.cpp
    src/explosion.cpp
    src/mesh_render.cpp
    src/mesh_render_atmosphere.cpp
    src/sprite.cpp
    src/ship_sprite.cpp
    src/sprite_light_editor.cpp
    src/sprite_light_rec.cpp
    src/mesh_orient_editor.cpp
    src/navmap_auditor.cpp
    src/sprite_generation_tool.cpp
    src/planet_texture.cpp
    src/debug_panel.cpp
    src/atlas_grid_viewer.cpp
    src/sound_labeler.cpp
    src/music_labeler.cpp
    src/speech_labeler.cpp
    src/cockpit_hud.cpp
    src/cockpit_mfd.cpp
    src/cockpit_overlay.cpp
    src/cockpit_armaments.cpp
    src/hud_text_fit.cpp
    src/cockpit_damage.cpp
    src/comms_menu.cpp
    ${DEV_REMOTE_SRC}
    ${IMGUI_SRC}
    ${SOKOL_IMPL_SRC}
    ${SHADER_OUTPUTS}
)

# Platform-specific helpers (kept out of main.cpp so the core stays portable).
# (Currently none — win32 helpers are header-only.)

# Bake the asset path into the runtime so we can launch from anywhere. For
# now we just use a relative path + chdir-to-source; for shipping we'd copy
# assets next to the binary. Good enough for Stage 2.
# The link goes next to the real binary, whatever the build dir is called
# (issue #497). In-source builds already have assets/ beside the binary, and
# linking there would collide with the real directory, so skip them.
if(NOT CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(ASSETS_LINK "$<TARGET_FILE_DIR:new_privateer>/assets")
    if(WIN32)
        # Directory junctions work without administrator privileges, unlike
        # symbolic links on Windows systems where Developer Mode is disabled.
        add_custom_command(TARGET new_privateer POST_BUILD
            COMMAND powershell -NoProfile -Command
                    "if (-not (Test-Path '${ASSETS_LINK}')) { New-Item -ItemType Junction -Path '${ASSETS_LINK}' -Target '${CMAKE_SOURCE_DIR}/assets' }"
            COMMENT "junctioning assets/ next to binary"
            VERBATIM
        )
    else()
        add_custom_command(TARGET new_privateer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    ${CMAKE_SOURCE_DIR}/assets ${ASSETS_LINK}
            COMMENT "symlinking assets/ next to binary"
            VERBATIM
        )
    endif()
endif()

target_include_directories(new_privateer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/generated
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
)
add_dependencies(new_privateer mission_templates_gen)

# Dear ImGui is a big codebase that triggers lots of unused-function /
# unused-parameter warnings under -Wall -Wextra. We trust upstream, so
# silence those *just for the imgui TUs* — our own code stays strict.
# (MSVC uses /w; GCC/Clang use -w. set() picks the right one.)
if(MSVC)
    set_source_files_properties(${IMGUI_SRC} PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${IMGUI_SRC} PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_compile_definitions(new_privateer PRIVATE ${SOKOL_BACKEND})

# MSVC's <cmath> hides M_PI, M_E, etc. behind _USE_MATH_DEFINES per the C
# standard (which says those constants are non-standard). Define it for
# the whole target so existing code reading M_PI doesn't have to know.
# Also bump NOMINMAX / WIN32_LEAN_AND_MEAN to keep <windows.h> from
# stomping on std::min / std::max if anything pulls it in transitively.
if(MSVC)
    target_compile_definitions(new_privateer PRIVATE
        _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Ship as a normal GUI application: the existing in-window loading screen
    # should be the first thing players see, not a debug terminal firehose.
    # Developers can opt back into main()+SUBSYSTEM:CONSOLE when diagnosing
    # early startup failures with: -DNP_WINDOWS_CONSOLE=ON.
    option(NP_WINDOWS_CONSOLE "Open a debug console with the Windows game" OFF)
    if(NP_WINDOWS_CONSOLE)
        target_compile_definitions(new_privateer PRIVATE SOKOL_WIN32_FORCE_MAIN)
        set(NP_WINDOWS_SUBSYSTEM CONSOLE)
    else()
        set_target_properties(new_privateer PROPERTIES WIN32_EXECUTABLE TRUE)
        set(NP_WINDOWS_SUBSYSTEM WINDOWS)
    endif()
    # Statically link the MSVC C runtime (/MT) so the shipped exe has NO
    # dependency on the VC++ Redistributable (MSVCP140.dll / VCRUNTIME140*.dll).
    # Without this a portable build fails to launch on a clean Windows install
    # that never had a VC++ redist installed. CMP0091 (NEW since 3.15; we
    # require 3.20) routes this through MSVC_RUNTIME_LIBRARY rather than manual
    # /MD->/MT flag scrubbing. imgui + sokol are compiled INTO this target, so
    # they inherit the same runtime — no mismatched-CRT link errors.
    set_property(TARGET new_privateer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # /Zi (C7-compatible PDB) + /DEBUG (linker) generates symbols even in
    # the Release config so our in-process stack walker (dbghelp) can
    # resolve names instead of bare addresses. Pairs with /OPT:REF + ICF
    # so we keep the symbol table without bloating the exe.
    target_compile_options(new_privateer PRIVATE /Zi)
    set_target_properties(new_privateer PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:${NP_WINDOWS_SUBSYSTEM} /DEBUG /OPT:REF /OPT:ICF"
    )
endif()

# Warning flags split by toolchain. GCC/Clang use -W*, MSVC uses /W*. We
# keep both vocal but don't promote anything to errors here so a fresh
# Windows port isn't blocked by hundreds of style warnings on day one.
if(MSVC)
    target_compile_options(new_privateer PRIVATE /W3 /wd4244 /wd4267 /wd4305 /wd4146 /permissive-)
else()
    target_compile_options(new_privateer PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
        # HandmadeMath vectors/quats are unions with nested scalar/array views;
        # Clang's -Wmissing-braces complains about clean HMM_Vec3{...} syntax.
        # Keep the useful warnings, drop the aggregate-layout noise.
        -Wno-missing-braces
    )
endif()

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

# Focused commodity market discovery/pricing/transaction harness.
add_executable(test_economy EXCLUDE_FROM_ALL
    tools/test_economy.cpp src/economy.cpp src/commodity.cpp src/player.cpp src/json.cpp)
target_include_directories(test_economy PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(test_economy PRIVATE ECONOMY_HEADLESS)

# Focused outfitting transaction harness + visual hardpoint data harness.
add_executable(test_outfitting EXCLUDE_FROM_ALL
    tools/test_outfitting.cpp
    src/outfitting.cpp src/ship_class.cpp src/gun.cpp src/shield.cpp
    src/armor.cpp src/player.cpp src/mobility.cpp src/faction.cpp src/json.cpp
    src/commodity.cpp
)
target_include_directories(test_outfitting PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(test_outfitting PRIVATE OUTFITTING_HEADLESS)

# Scanner catalog + fit/sell transactions (#143).
#   cmake --build build --target test_scanner && ./build/test_scanner
add_executable(test_scanner EXCLUDE_FROM_ALL
    tools/test_scanner.cpp src/scanner.cpp
    src/outfitting.cpp src/ship_class.cpp src/gun.cpp src/shield.cpp
    src/armor.cpp src/player.cpp src/mobility.cpp src/faction.cpp src/json.cpp
    src/commodity.cpp
)
target_include_directories(test_scanner PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(test_scanner PRIVATE OUTFITTING_HEADLESS)

add_executable(test_equipment_hardpoints EXCLUDE_FROM_ALL
    tools/test_equipment_hardpoints.cpp src/equipment_hardpoints.cpp src/json.cpp)
target_include_directories(test_equipment_hardpoints PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Pure weapon-group indexing harness: catches unreachable ALL modes without
# dragging the renderer, audio, or projectile simulation into a math test.
add_executable(test_gun_modes EXCLUDE_FROM_ALL tools/test_gun_modes.cpp)
target_include_directories(test_gun_modes PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Player auto-turret regression: a disarmed forward-gun mask must not suppress
# autonomous turret target acquisition/fire, while fixed guns remain gated.
add_executable(test_turret_firing EXCLUDE_FROM_ALL
    tools/test_turret_firing.cpp
    src/firing.cpp src/ship_registry.cpp src/gun.cpp src/json.cpp)
target_include_directories(test_turret_firing PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Pure launcher hardware/ammo selection harness.
add_executable(test_launcher_modes EXCLUDE_FROM_ALL tools/test_launcher_modes.cpp)
target_include_directories(test_launcher_modes PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Damped fly-by-wire response and actual camera integration (#433).
add_executable(test_turn_response EXCLUDE_FROM_ALL
    tools/test_turn_response.cpp src/camera.cpp)
target_include_directories(test_turn_response PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Animated base-room data harness (#515): sprite-sheet manifests, timeline
# wrapping, star-tile scrolling, plus the shipped New Con manifests.
add_executable(test_room_anim EXCLUDE_FROM_ALL
    tools/test_room_anim.cpp src/room_anim_data.cpp src/json.cpp)
target_include_directories(test_room_anim PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Pure Flight pause-overlay visibility harness.
add_executable(test_pause_overlay EXCLUDE_FROM_ALL tools/test_pause_overlay.cpp)
target_include_directories(test_pause_overlay PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Pure cockpit-art fit + MFD-hole geometry harness (#426).
# Real ImGui compositing with GPU texture allocation stubbed (#429).
# Also covers the V cockpit-off toggle's classic HUD placement (#554).
add_executable(test_cockpit_layers EXCLUDE_FROM_ALL
    tools/test_cockpit_layers.cpp src/cockpit_overlay.cpp
    src/cockpit_mfd.cpp src/hud_text_fit.cpp ${IMGUI_SRC})
target_include_directories(test_cockpit_layers PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party ${IMGUI_DIR})

# Dense HUD lines fit every hull's MFD glass, real ImGui + game font (#430).
# Run from the repo root.
add_executable(test_hud_text_fit EXCLUDE_FROM_ALL
    tools/test_hud_text_fit.cpp src/hud_text_fit.cpp src/cockpit_mfd.cpp
    src/cockpit_overlay.cpp ${IMGUI_SRC})
target_include_directories(test_hud_text_fit PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party ${IMGUI_DIR})

add_executable(test_cockpit_overlay EXCLUDE_FROM_ALL tools/test_cockpit_overlay.cpp)
target_include_directories(test_cockpit_overlay PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Hardpoint swaps + the player loadout -> live mounts build (#514).
add_executable(test_armament_loadout EXCLUDE_FROM_ALL
    tools/test_armament_loadout.cpp src/player.cpp src/commodity.cpp src/json.cpp
    src/firing.cpp src/ship_registry.cpp src/gun.cpp)
target_include_directories(test_armament_loadout PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# Focused jump-eligibility regression harness. Jump deliberately does not link
# threat.cpp: combat proximity belongs to autopilot, not gate eligibility.
add_executable(test_jump EXCLUDE_FROM_ALL
    tools/test_jump.cpp src/jump.cpp src/galaxy.cpp src/json.cpp)
target_include_directories(test_jump PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Headless integration test harness: test_full_loop (np-zte.3)
# ---------------------------------------------------------------------------
# The END-TO-END regression spine for the sandbox loop. It links a SUBSET of
# the real game modules — no GPU, no audio, no ImGui — under the same
# *_HEADLESS guards the per-feature harnesses (test_economy / test_outfitting /
# test_missions / test_reputation) already use, then walks the whole loop
# programmatically: new_game -> encounter spawn -> kill (rep + bounty) -> dock
# + autosave -> commodity arbitrage -> outfitting -> missions -> repair ->
# homing-missile kill + afterburner -> jump (round-trip + escape under fire) ->
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
    src/galaxy.cpp src/system_def.cpp src/json.cpp ${SAVEGAME_SRC} src/world_clock.cpp
    src/docking.cpp src/autopilot.cpp src/threat.cpp src/jump.cpp
    src/encounters.cpp src/repair.cpp src/missile.cpp src/ship.cpp
    src/ship_class.cpp src/ship_registry.cpp src/ship_ai.cpp
    src/ai_brain.cpp src/hazards.cpp
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
# comm.cpp's voice:: calls live in functions this harness never reaches;
# dead-strip drops them at link time (same trick as test_missions).
target_link_options(test_full_loop PRIVATE -Wl,-dead_strip)
add_dependencies(test_full_loop mission_templates_gen)

# Headless proof harness for the data-driven combat brain (docs/ai_maneuver_system.md).
#   cmake --build build --target test_ai_brain && ./build/test_ai_brain
add_executable(test_ai_brain EXCLUDE_FROM_ALL
    tools/test_ai_brain.cpp
    src/game_state.cpp src/player.cpp src/faction.cpp src/comm.cpp
    src/commodity.cpp src/economy.cpp src/outfitting.cpp src/missions.cpp
    src/galaxy.cpp src/system_def.cpp src/json.cpp ${SAVEGAME_SRC} src/world_clock.cpp
    src/docking.cpp src/autopilot.cpp src/threat.cpp src/jump.cpp
    src/encounters.cpp src/repair.cpp src/missile.cpp src/ship.cpp
    src/ship_class.cpp src/ship_registry.cpp src/ship_ai.cpp
    src/ai_brain.cpp src/hazards.cpp
    src/perception.cpp src/gun.cpp src/shield.cpp src/armor.cpp
    src/mobility.cpp src/camera.cpp
)
target_include_directories(test_ai_brain PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_ai_brain PRIVATE
    ECONOMY_HEADLESS OUTFITTING_HEADLESS MISSIONS_HEADLESS COMM_HEADLESS
)
target_link_options(test_ai_brain PRIVATE -Wl,-dead_strip)
add_dependencies(test_ai_brain mission_templates_gen)


# Round-trip proof for the Mission Computer + #9/#11 nav/distance/pay helpers.
# Exercises generate() / accept() / complete_delivery() / bounty progress
# AND the pure anon-namespace helpers from issue #9 (nonbase_navs /
# bases_in_system / hops_between / bounty_region) against the real
# assets/galaxy.json. Built MISSIONS_HEADLESS + COMM_HEADLESS so no UI/audio.
#   cmake --build build --target test_missions && ./build/test_missions
add_executable(test_missions EXCLUDE_FROM_ALL
    tools/test_missions.cpp
    src/missions.cpp src/comm.cpp src/player.cpp src/faction.cpp
    src/commodity.cpp
    src/galaxy.cpp src/system_def.cpp src/json.cpp ${SAVEGAME_SRC} src/world_clock.cpp
    src/ship_class.cpp src/ship_registry.cpp
    src/encounters.cpp
    src/mobility.cpp
    src/gun.cpp src/armor.cpp src/shield.cpp
)
target_include_directories(test_missions PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_missions PRIVATE
    MISSIONS_HEADLESS COMM_HEADLESS
)
# encounters.cpp references ship_ai::from_name (in init()), which the test
# never calls. -dead_strip lets the linker drop that unreachable code path
# so we don't need to pull in ship_ai.cpp's full dependency chain.
target_link_options(test_missions PRIVATE -Wl,-dead_strip)
add_dependencies(test_missions mission_templates_gen)

# Focused proof for #13 in-flight nav-reach + clear tracking. Drives
# mission_tracker::tick() against a hand-built PlayerState + nav list:
# Scout far->not done / at-nav->complete+credited; Patrol completes only
# when ALL navs reached. Built MISSIONS_HEADLESS + COMM_HEADLESS (no UI/audio).
#   cmake --build build --target test_mission_tracker && ./build/test_mission_tracker
add_executable(test_mission_tracker EXCLUDE_FROM_ALL
    tools/test_mission_tracker.cpp
    src/mission_tracker.cpp
    src/missions.cpp src/comm.cpp src/player.cpp src/faction.cpp
    src/threat.cpp src/commodity.cpp
    src/galaxy.cpp src/system_def.cpp src/json.cpp ${SAVEGAME_SRC} src/world_clock.cpp
    src/ship_class.cpp src/ship_registry.cpp
    src/mobility.cpp
    src/gun.cpp src/armor.cpp src/shield.cpp
)
target_include_directories(test_mission_tracker PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_mission_tracker PRIVATE
    MISSIONS_HEADLESS COMM_HEADLESS
)
target_link_options(test_mission_tracker PRIVATE -Wl,-dead_strip)
add_dependencies(test_mission_tracker mission_templates_gen)

# Round-trip proof for the save system (np-ymp.1 + np-3dp.19 timestamped
# unlimited saves + faction kills + ship-damage snapshot).
#   cmake --build build --target test_savegame && ./build/test_savegame
add_executable(test_savegame EXCLUDE_FROM_ALL
    tools/test_savegame.cpp
    ${SAVEGAME_SRC} src/world_clock.cpp src/player.cpp src/plot.cpp src/json.cpp src/faction.cpp
    src/commodity.cpp
    src/ship_class.cpp src/gun.cpp src/shield.cpp src/mobility.cpp   # pre-v9 turret grandfathering
)
target_include_directories(test_savegame PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

# Engine-free launch argument grammar.
#   cmake --build build --target test_app_cli && ./build/test_app_cli
add_executable(test_app_cli EXCLUDE_FROM_ALL
    tools/test_app_cli.cpp
    src/app_cli.cpp
)
target_include_directories(test_app_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Pure calendar arithmetic for Gemini Lives (#171). No engine dependencies.
#   cmake --build build --target test_world_clock && ./build/test_world_clock
add_executable(test_world_clock EXCLUDE_FROM_ALL
    tools/test_world_clock.cpp
    src/world_clock.cpp
)
target_include_directories(test_world_clock PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Docking lifecycle proof, including the once-per-landing day advance. Save
# and audio boundaries are stubbed by the harness; no real user files touched.
#   cmake --build build --target test_docking && ./build/test_docking
add_executable(test_docking EXCLUDE_FROM_ALL
    tools/test_docking.cpp
    src/docking.cpp src/game_state.cpp src/camera.cpp
)
target_include_directories(test_docking PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

# Fixer framework proof (#137): placement (base + archetype predicate),
# plot gating, action grammar, campaign-handler seam. FIXERS_HEADLESS
# compiles out the ImGui bar body.
#   cmake --build build --target test_fixers && ./build/test_fixers
add_executable(test_fixers EXCLUDE_FROM_ALL
    tools/test_fixers.cpp
    src/fixers.cpp src/plot.cpp src/player.cpp src/json.cpp src/faction.cpp
    src/commodity.cpp
)
target_include_directories(test_fixers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_fixers PRIVATE FIXERS_HEADLESS)

# Campaign mission-logic proof (epic #136): drives M01 (#113) headlessly
# against the SHIPPED fixers.json + commodity catalog.
#   cmake --build build --target test_campaign && ./build/test_campaign
add_executable(test_campaign EXCLUDE_FROM_ALL
    tools/test_campaign.cpp
    src/campaign.cpp src/fixers.cpp src/plot.cpp src/player.cpp
    src/faction.cpp src/json.cpp src/commodity.cpp src/comm.cpp
    src/ship_class.cpp src/gun.cpp src/shield.cpp src/mobility.cpp
)
target_include_directories(test_campaign PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(test_campaign PRIVATE FIXERS_HEADLESS COMM_HEADLESS)
# Same trick as test_missions: comm.cpp's voice:: calls live in functions
# this harness never references; dead-strip drops them at link time.
target_link_options(test_campaign PRIVATE -Wl,-dead_strip)

# Cinematic parser hardening proof (np-cinematic Phase 6.2 + Studio Phase A):
# feeds a battery of deliberately-malformed inline JSON at the ENGINE-FREE
# parsers (cinematic doc + outcome block + trigger table) and asserts they
# never throw; also exercises the trigger evaluator's once/cooldown latches
# through a fake play hook. Links only the engine-free TUs (+ plot.cpp for
# the flag gates) — no imgui / sokol / GL / audio.
#   cmake --build build --target test_cinematic && ./build/test_cinematic
add_executable(test_mesh_indices EXCLUDE_FROM_ALL tools/test_mesh_indices.cpp
    src/obj_loader.cpp src/mesh_cache.cpp src/json.cpp)
target_include_directories(test_mesh_indices PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

add_executable(test_cinematic_dialogue EXCLUDE_FROM_ALL tools/test_cinematic_dialogue.cpp)
target_include_directories(test_cinematic_dialogue PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)

add_executable(test_cinematic EXCLUDE_FROM_ALL
    tools/test_cinematic.cpp
    src/cinematic_parse.cpp src/cinematic_triggers.cpp
    src/cinematic_studio_io.cpp
    src/plot.cpp src/json.cpp
)
target_include_directories(test_cinematic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

# ---------------------------------------------------------------------------
# Harnesses revived from tools/ (#493): each exits non-zero on failure.
add_executable(test_autopilot EXCLUDE_FROM_ALL
    tools/test_autopilot.cpp src/autopilot.cpp src/camera.cpp src/threat.cpp
    src/hazards.cpp src/faction.cpp)
add_executable(test_inventory112 EXCLUDE_FROM_ALL
    tools/test_inventory112.cpp src/inventory.cpp src/player.cpp src/json.cpp
    src/commodity.cpp src/ship_class.cpp src/gun.cpp src/shield.cpp src/mobility.cpp
    src/faction.cpp)
target_compile_definitions(test_inventory112 PRIVATE INVENTORY_HEADLESS)
add_executable(test_missile EXCLUDE_FROM_ALL
    tools/test_missile.cpp src/missile.cpp src/ship.cpp src/player.cpp
    src/repair.cpp src/ship_class.cpp src/gun.cpp src/shield.cpp src/armor.cpp
    src/mobility.cpp src/faction.cpp src/ship_ai.cpp src/perception.cpp
    src/json.cpp src/commodity.cpp src/ai_brain.cpp src/hazards.cpp
    src/ship_registry.cpp src/comm.cpp)
target_compile_definitions(test_missile PRIVATE COMM_HEADLESS)
# Per-component damage (#141): pure model + ship/perception/repair wiring.
add_executable(test_ship_systems EXCLUDE_FROM_ALL
    tools/test_ship_systems.cpp src/ship.cpp src/player.cpp src/repair.cpp
    src/ship_class.cpp src/gun.cpp src/shield.cpp src/armor.cpp
    src/mobility.cpp src/faction.cpp src/ship_ai.cpp src/perception.cpp
    src/json.cpp src/commodity.cpp src/ai_brain.cpp src/hazards.cpp
    src/ship_registry.cpp src/comm.cpp)
target_compile_definitions(test_ship_systems PRIVATE COMM_HEADLESS)
add_executable(test_reputation EXCLUDE_FROM_ALL
    tools/test_reputation.cpp src/comm.cpp src/faction.cpp src/player.cpp
    src/json.cpp src/commodity.cpp)
target_compile_definitions(test_reputation PRIVATE COMM_HEADLESS)
add_executable(test_salvage EXCLUDE_FROM_ALL
    tools/test_salvage.cpp src/economy.cpp src/commodity.cpp src/player.cpp
    src/json.cpp)
target_compile_definitions(test_salvage PRIVATE ECONOMY_HEADLESS)
# Voice-line selection (#640): real voice.cpp vs the shipped voice bank, with
# the audio mixer stubbed in the harness so plays are counted, not heard.
add_executable(test_voice EXCLUDE_FROM_ALL
    tools/test_voice.cpp src/voice.cpp src/faction.cpp src/json.cpp)
foreach(harness test_autopilot test_inventory112 test_missile test_ship_systems
                test_reputation test_salvage test_voice)
    target_include_directories(${harness} PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
endforeach()

# ---------------------------------------------------------------------------
# `dist` — portable, relocatable game folder (build/dist/).
#
# Copies the executable + a pruned real-file assets/ copy (drops sprite-gen
# intermediates, Blender renders, and unused imported meshes). Combined with
# the static-CRT link on MSVC, the result is a folder you can zip and run on
# any Win10/11 box with zero prerequisites — no VC++ redist, no source tree,
# no symlinks.
#
#   cmake --build build --target dist
# ---------------------------------------------------------------------------
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND}
            -DNP_EXE=$<TARGET_FILE:new_privateer>
            -DNP_SRC=${CMAKE_SOURCE_DIR}
            -DNP_OUT=${CMAKE_BINARY_DIR}/dist
            -P ${CMAKE_SOURCE_DIR}/cmake/make_dist.cmake
    DEPENDS new_privateer
    VERBATIM
    COMMENT "Packaging portable dist/ (static-CRT exe + pruned real-file assets)"
)
