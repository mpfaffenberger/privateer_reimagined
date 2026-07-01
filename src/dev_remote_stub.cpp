// -----------------------------------------------------------------------------
// dev_remote_stub.cpp — no-op dev_remote implementation for non-macOS builds.
//
// The real dev_remote.cpp uses POSIX sockets (<sys/socket.h>, <arpa/inet.h>,
// etc) and dev_remote_macos.mm uses Cocoa to grab window screenshots. Both
// are macOS-only conveniences for the Code-Puppy capture loop. On Windows /
// Linux we just compile this stub instead so the link succeeds and main.cpp
// doesn't have to #ifdef every call site.
//
// If we ever want the HTTP control channel on Windows, this file gets
// replaced with a winsock2 port (and the screenshot path with a D3D11 RT
// readback) and dev_remote.cpp's POSIX guts get pulled into a unix-only
// branch the same way.
// -----------------------------------------------------------------------------

#include "dev_remote.h"

namespace dev_remote {

void start(int /*port*/)                                   {}
void stop()                                                {}
void drain_commands(Camera& /*cam*/)                       {}
void publish_fps(int /*fps*/)                              {}
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
void push_event(const std::string& /*category*/,
                const std::string& /*text*/)                           {}

// Host hooks — accepted and discarded; no HTTP thread ever invokes them.
void set_cargo_give_hook(std::function<void(std::string, int)> /*hook*/) {}
void set_spawn_hook(std::function<void(std::string, std::string, float)> /*hook*/) {}
void set_kill_hook(std::function<void(uint32_t)> /*hook*/)             {}
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

} // namespace dev_remote
