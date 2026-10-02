// -----------------------------------------------------------------------------
// dev_remote_stub.cpp — no-op dev_remote implementation for Linux builds.
//
// The real dev_remote.cpp speaks sockets through dev_remote_socket.h and
// grabs screenshots via dev_remote_macos.mm (macOS) or dev_remote_win32.cpp
// (Windows, #696). Linux has no screenshot TU yet, so it compiles this stub
// instead and main.cpp doesn't have to #ifdef every call site.
//
// Porting to Linux = add a dev_remote_linux.cpp with
// dev_remote_capture_window() (e.g. a GL back-buffer readback) and switch
// the CMake branch over.
// -----------------------------------------------------------------------------

#include "dev_remote.h"

namespace dev_remote {

void start(int /*port*/)                                   {}
void stop()                                                {}
void drain_commands(Camera& /*cam*/)                       {}
void publish_fps(int /*fps*/)                              {}
void publish_day(int /*day*/)                              {}
void maybe_capture_screenshot()                            {}
void publish_system_name(const char* /*name*/)             {}
void publish_render_matrices(const HMM_Mat4& /*view_proj*/,
                             const HMM_Mat4& /*model*/,
                             HMM_Vec3 /*cam_pos*/)         {}

// Snapshot publishers (issue #103 + the agentic-testing expansion). The
// real server copies these under a mutex for the HTTP thread; the stub
// just drops them.
void publish_ships(const std::vector<ShipInfo>& /*ships*/)             {}
void publish_loot(const std::vector<LootInfo>& /*loot*/)               {}
void publish_objectives(const std::vector<LeadInfo>& /*objectives*/)   {}
void publish_inventory(const std::vector<ItemInfo>& /*items*/,
                       int /*used*/, int /*cap*/,
                       const std::vector<ModInfo>& /*mods*/,
                       const std::vector<MountInfo>& /*mounts*/)       {}
void publish_player(const PlayerInfo& /*p*/)                           {}
void publish_missions(const std::vector<MissionInfo>& /*missions*/)    {}
void publish_base(const BaseInfo& /*b*/)                               {}
void publish_cinematic(const CinematicInfo& /*c*/)                     {}
void push_event(const std::string& /*category*/,
                const std::string& /*text*/)                           {}

// Host hooks — accepted and discarded; no HTTP thread ever invokes them.
void set_cargo_give_hook(std::function<void(std::string, int)> /*hook*/) {}
void set_spawn_hook(std::function<void(std::string, std::string, float)> /*hook*/) {}
void set_kill_hook(std::function<void(uint32_t)> /*hook*/)             {}
void set_damage_hook(
    std::function<void(uint32_t, float, const std::string&)> /*hook*/)  {}
void set_target_hook(std::function<void(uint32_t)> /*hook*/)           {}
void set_tractor_pull_hook(std::function<void()> /*hook*/)             {}
void set_rumor_hook(std::function<void()> /*hook*/)                    {}
void set_inventory_sell_hook(std::function<void(int)> /*hook*/)        {}
void set_inventory_give_hook(
    std::function<void(std::string, std::string,
                       std::string, int)> /*hook*/)                    {}
void set_inventory_install_hook(std::function<void(int)> /*hook*/)     {}
void set_inventory_equip_hook(std::function<void(int, int)> /*hook*/)  {}
void set_panel_hook(std::function<void(std::string)> /*hook*/)         {}
void set_comms_select_hook(std::function<void(int)> /*hook*/)          {}
void set_plot_hook(std::function<void(std::string, std::string)> /*hook*/) {}
void set_advance_day_hook(std::function<void(int)> /*hook*/)           {}
void set_base_screen_hook(std::function<void(std::string)> /*hook*/)   {}
void set_goto_hook(std::function<void(std::string)> /*hook*/)          {}
void set_dock_hook(std::function<void(std::string)> /*hook*/)          {}
void set_fixer_hook(std::function<void(std::string, std::string)> /*hook*/) {}
void set_bar_music_hook(std::function<void(int)> /*hook*/) {}
void set_dj_panel_hook(std::function<void(bool)> /*hook*/) {}
void set_autopilot_hook(std::function<void(std::string)> /*hook*/)     {}
void set_jump_hook(std::function<void(std::string)> /*hook*/)          {}

// Cinematic hooks — same deal: accepted and discarded, no HTTP thread ever
// calls them on non-macOS builds.
void set_cinematic_play_hook(
    std::function<bool(const std::string&, std::string&)> /*hook*/)   {}
void set_cinematic_reload_hook(
    std::function<bool(const std::string&, std::string&)> /*hook*/)   {}
void set_cinematic_seek_hook(
    std::function<bool(float, std::string&)> /*hook*/)                {}
void set_cinematic_stop_hook(std::function<void()> /*hook*/)           {}

} // namespace dev_remote
