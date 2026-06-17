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

} // namespace dev_remote
