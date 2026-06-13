// -----------------------------------------------------------------------------
// dev_remote.cpp — HTTP server + command queue for the dev remote.
//
// The HTTP server is deliberately tiny (~one-at-a-time connections, HTTP/1.0
// style, no keep-alive). This is a dev tool, not a production service.
// The code here avoids any third-party deps — it's just BSD sockets and
// std::thread. Platform-specific bits (window screenshot) live in
// dev_remote_macos.mm.
// -----------------------------------------------------------------------------

#include "dev_remote.h"
#include "camera.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>   // std::clamp for the quat→euler arcsin guard
#include <atomic>
#include <cmath>        // std::asin / std::atan2 for telemetry conversion
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Declared in dev_remote_macos.mm — captures the game's NSWindow via
// screencapture -l<wid>. Returns true on success. Uses C linkage so
// we don't have to coordinate C++ mangling with the .mm TU.
extern "C" bool dev_remote_capture_window(const char* path);

namespace dev_remote {
namespace {

// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------
struct ScreenshotWaiter {
    std::string path;
    std::mutex mu;
    std::condition_variable cv;
    bool done    = false;
    bool success = false;
};

struct Command {
    enum class Kind { SetCamera, Screenshot };
    Kind kind;

    // SetCamera — any optional field is encoded with `has_*`.
    bool has_pos = false, has_euler = false;
    HMM_Vec3 pos{};
    HMM_Vec3 euler{};

    // Screenshot — the waiter is owned by the HTTP thread; the main
    // thread only sets fields on it and notifies.
    ScreenshotWaiter* waiter = nullptr;
};

std::mutex          g_queue_mu;
std::deque<Command> g_queue;

// Published state for /state responses. Plain atomics for the cheap
// scalars; a short mutex for the string.
std::atomic<int>    g_last_fps{0};
std::mutex          g_system_mu;
std::string         g_system_name;

// Shared between main thread (publishes camera) and HTTP thread (reads
// for /state). The main thread writes this in drain_commands so the
// state endpoint always reports the *current* camera even when no new
// commands have been issued.
std::mutex g_cam_snapshot_mu;
HMM_Vec3   g_cam_pos{};
HMM_Vec3   g_cam_euler{};

// Render-matrix snapshot for /project. Published once per frame by the
// main thread (publish_render_matrices); read by the HTTP thread inside
// handle_project. Guarded by its own mutex so a /project request never
// races a mid-frame matrix write.
std::mutex g_proj_mu;
HMM_Mat4   g_view_proj{};
HMM_Mat4   g_model{};
HMM_Vec3   g_proj_cam_pos{};
bool       g_proj_ready = false;   // false until first publish

std::thread       g_thread;
std::atomic<bool> g_running{false};
int               g_listen_fd = -1;

// The currently-pending screenshot request. Written only from the main
// thread in drain_commands(); read/cleared only from the main thread in
// maybe_capture_screenshot(). Single-threaded access → no lock needed.
ScreenshotWaiter* g_pending_shot = nullptr;

// ---------------------------------------------------------------------------
// Tiny JSON helpers — just enough for our 3 endpoints. We don't need a
// real parser; requests are always flat objects with known keys and
// numeric or string values. Extracting with string scans keeps the code
// dependency-free.
// ---------------------------------------------------------------------------
bool extract_float(const std::string& body, const char* key, float* out) {
    std::string needle = std::string("\"") + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return false;
    *out = std::strtof(body.c_str() + pos + 1, nullptr);
    return true;
}

// Scan a string for every floating-point token and return them in order.
// Used by /project: the client sends a flat array of numbers and we just
// read them all sequentially (6 per point: x y z nx ny nz). Brackets,
// commas, and whitespace are skipped by strtof's own tokenising — we
// step the cursor past each parsed number and resume scanning for the
// next numeric character.
std::vector<float> extract_all_floats(const std::string& body) {
    std::vector<float> out;
    const char* p   = body.c_str();
    const char* end = p + body.size();
    while (p < end) {
        // Advance to the next character that could start a number.
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {
            char* next = nullptr;
            float v = std::strtof(p, &next);
            if (next == p) { ++p; continue; }   // not actually a number
            out.push_back(v);
            p = next;
        } else {
            ++p;
        }
    }
    return out;
}

std::string json_state() {
    HMM_Vec3 pos, euler;
    {
        std::lock_guard lk(g_cam_snapshot_mu);
        pos   = g_cam_pos;
        euler = g_cam_euler;
    }
    std::string sys;
    {
        std::lock_guard lk(g_system_mu);
        sys = g_system_name;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"pos\":[%.2f,%.2f,%.2f],"
        "\"euler\":[%.2f,%.2f,%.2f],"
        "\"fps\":%d,"
        "\"system\":\"%s\"}",
        pos.X, pos.Y, pos.Z,
        euler.X, euler.Y, euler.Z,
        g_last_fps.load(),
        sys.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
void send_all(int fd, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, data + sent, n - sent, 0);
        if (r <= 0) return;
        sent += (size_t)r;
    }
}

void send_response(int fd, int status, const char* status_text,
                   const char* content_type, const std::string& body) {
    char hdr[512];
    int n = std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body.size());
    send_all(fd, hdr, (size_t)n);
    if (!body.empty()) send_all(fd, body.data(), body.size());
}

void send_json(int fd, const std::string& body) {
    send_response(fd, 200, "OK", "application/json", body);
}

void send_404(int fd) {
    send_response(fd, 404, "Not Found", "text/plain",
                  "unknown endpoint\n");
}

// ---------------------------------------------------------------------------
// Endpoint handlers
// ---------------------------------------------------------------------------
void handle_state(int fd) {
    send_json(fd, json_state());
}

void handle_camera_set(int fd, const std::string& body) {
    Command c;
    c.kind = Command::Kind::SetCamera;

    float x, y, z;
    if (extract_float(body, "x", &x) &
        extract_float(body, "y", &y) &
        extract_float(body, "z", &z)) {
        c.has_pos = true;
        c.pos = { x, y, z };
    }
    float yaw, pitch, roll;
    // Degrees. Accepts any subset — missing fields preserve current
    // orientation when drained by the main thread.
    bool has_any_euler = false;
    HMM_Vec3 eul{};
    if (extract_float(body, "pitch", &pitch)) { eul.X = pitch; has_any_euler = true; }
    if (extract_float(body, "yaw",   &yaw))   { eul.Y = yaw;   has_any_euler = true; }
    if (extract_float(body, "roll",  &roll))  { eul.Z = roll;  has_any_euler = true; }
    if (has_any_euler) {
        c.has_euler = true;
        c.euler = eul;
    }

    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }
    send_json(fd, "{\"ok\":true}");
}

// Project mesh-local 3D points into screen UV using the published render
// matrices. Body is a flat float array [x,y,z,nx,ny,nz, ...]; we read 6
// floats per point (position + outward normal). Pure read of the snapshot
// — no command queued, no main-thread round trip needed, because the
// matrices are already published each frame.
void handle_project(int fd, const std::string& body) {
    HMM_Mat4 vp, model;
    HMM_Vec3 cam;
    bool ready;
    {
        std::lock_guard lk(g_proj_mu);
        vp = g_view_proj; model = g_model; cam = g_proj_cam_pos;
        ready = g_proj_ready;
    }
    if (!ready) {
        send_json(fd, "{\"results\":[],\"error\":\"no render matrices yet\"}");
        return;
    }

    const std::vector<float> nums = extract_all_floats(body);
    std::string out = "{\"results\":[";
    const size_t n_pts = nums.size() / 6;
    for (size_t i = 0; i < n_pts; ++i) {
        const float px = nums[i*6+0], py = nums[i*6+1], pz = nums[i*6+2];
        const float nx = nums[i*6+3], ny = nums[i*6+4], nz = nums[i*6+5];

        // Local → world → clip. w=1 for the position (affine transform).
        const HMM_Vec4 world = HMM_MulM4V4(model, HMM_V4(px, py, pz, 1.0f));
        const HMM_Vec4 clip  = HMM_MulM4V4(vp, world);
        const bool front = clip.W > 1e-4f;
        float u = 0.0f, v = 0.0f;
        if (front) {
            const float ndc_x = clip.X / clip.W;
            const float ndc_y = clip.Y / clip.W;
            u = ndc_x * 0.5f + 0.5f;
            v = 1.0f - (ndc_y * 0.5f + 0.5f);   // image row 0 = top = +ndc.y
        }

        // Normal-facing visibility: transform the outward normal by the
        // model matrix (w=0 → rotation+scale only) and check it points
        // toward the camera. Uniform scale keeps the direction valid.
        const HMM_Vec4 nworld = HMM_MulM4V4(model, HMM_V4(nx, ny, nz, 0.0f));
        const HMM_Vec3 to_cam = HMM_SubV3(cam, HMM_V3(world.X, world.Y, world.Z));
        const float dot = nworld.X*to_cam.X + nworld.Y*to_cam.Y + nworld.Z*to_cam.Z;
        const bool facing = dot > 0.0f;

        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "%s{\"u\":%.5f,\"v\":%.5f,\"front\":%s,\"facing\":%s}",
            i ? "," : "", u, v,
            front  ? "true" : "false",
            facing ? "true" : "false");
        out += buf;
    }
    out += "]}";
    send_json(fd, out);
}

void handle_screenshot(int fd) {
    ScreenshotWaiter w;
    w.path = "/tmp/np_shot.png";

    Command c;
    c.kind   = Command::Kind::Screenshot;
    c.waiter = &w;
    {
        std::lock_guard lk(g_queue_mu);
        g_queue.push_back(c);
    }

    // Wait up to ~2 seconds for the main thread to take the shot.
    // Timeout means something's wrong with the render loop; we return
    // an error response rather than block the HTTP thread forever.
    std::unique_lock lk(w.mu);
    w.cv.wait_for(lk, std::chrono::seconds(2), [&]{ return w.done; });

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"path\":\"%s\"}",
        w.success ? "true" : "false",
        w.path.c_str());
    send_json(fd, buf);
}

// ---------------------------------------------------------------------------
// Request parsing and dispatch
// ---------------------------------------------------------------------------
// Read until CRLFCRLF. Returns true on success. For simplicity we cap
// the request at 64 KB — way beyond anything a dev tool should send.
bool read_request(int fd, std::string& out) {
    out.clear();
    char buf[4096];
    while (out.size() < 65536) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) return !out.empty();
        out.append(buf, buf + r);
        if (out.find("\r\n\r\n") != std::string::npos) return true;
    }
    return false;
}

void handle_connection(int fd) {
    std::string req;
    if (!read_request(fd, req)) { ::close(fd); return; }

    // Parse first line: "METHOD PATH HTTP/1.x"
    auto first_crlf = req.find("\r\n");
    std::string line = req.substr(0, first_crlf);
    std::string method, path;
    {
        auto sp1 = line.find(' ');
        auto sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            send_response(fd, 400, "Bad Request", "text/plain", "bad line\n");
            ::close(fd);
            return;
        }
        method = line.substr(0, sp1);
        path   = line.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    // Extract body (after blank line).
    auto body_sep = req.find("\r\n\r\n");
    std::string body = (body_sep == std::string::npos)
                     ? std::string{}
                     : req.substr(body_sep + 4);

    // Read the Content-Length bytes if we don't already have them all.
    // This matters for POSTs that arrive in >1 TCP segment.
    size_t content_length = 0;
    if (auto p = req.find("Content-Length:"); p != std::string::npos) {
        content_length = (size_t)std::strtoul(req.c_str() + p + 15, nullptr, 10);
    }
    while (body.size() < content_length) {
        char buf[4096];
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        body.append(buf, buf + r);
    }

    if      (method == "GET"  && path == "/state")      handle_state(fd);
    else if (method == "POST" && path == "/camera/set") handle_camera_set(fd, body);
    else if (method == "POST" && path == "/screenshot") handle_screenshot(fd);
    else if (method == "POST" && path == "/project")    handle_project(fd, body);
    else                                                send_404(fd);

    ::close(fd);
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------
void server_loop(int port) {
    g_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        std::fprintf(stderr, "[dev_remote] socket() failed\n");
        return;
    }
    int yes = 1;
    ::setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only
    if (::bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "[dev_remote] bind(%d) failed — port in use?\n", port);
        ::close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }
    if (::listen(g_listen_fd, 4) < 0) {
        std::fprintf(stderr, "[dev_remote] listen() failed\n");
        ::close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }
    std::printf("[dev_remote] listening on 127.0.0.1:%d\n", port);

    while (g_running.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int fd = ::accept(g_listen_fd, (sockaddr*)&peer, &peer_len);
        if (fd < 0) {
            if (g_running.load()) continue;  // shutdown raced us
            break;
        }
        handle_connection(fd);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void start(int port) {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(server_loop, port);
}

void stop() {
    if (!g_running.exchange(false)) return;
    if (g_listen_fd >= 0) {
        ::shutdown(g_listen_fd, SHUT_RDWR);
        ::close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_thread.joinable()) g_thread.join();
}

void publish_fps(int fps) {
    g_last_fps.store(fps);
}

void drain_commands(Camera& cam) {
    std::deque<Command> local;
    {
        std::lock_guard lk(g_queue_mu);
        local.swap(g_queue);
    }

    for (auto& c : local) {
        switch (c.kind) {
        case Command::Kind::SetCamera:
            if (c.has_pos) {
                cam.position = c.pos;
                cam.velocity = { 0, 0, 0 };   // teleport → zero inertia
            }
            if (c.has_euler) {
                // API speaks Euler angles (degrees) for human convenience;
                // Camera now stores a quaternion. Build the equivalent
                // orientation as q = qyaw * qpitch (matches the old
                // ry(yaw) * rx(pitch) matrix order). Roll is folded in
                // for full coverage even though most callers send 0.
                const float yaw_r   = c.euler.Y * HMM_DegToRad;
                const float pitch_r = c.euler.X * HMM_DegToRad;
                const float roll_r  = c.euler.Z * HMM_DegToRad;
                const HMM_Quat qy = HMM_QFromAxisAngle_RH(HMM_V3(0,1,0), yaw_r);
                const HMM_Quat qp = HMM_QFromAxisAngle_RH(HMM_V3(1,0,0), pitch_r);
                const HMM_Quat qr = HMM_QFromAxisAngle_RH(HMM_V3(0,0,1), roll_r);
                cam.orientation = HMM_NormQ(
                    HMM_MulQ(HMM_MulQ(qy, qp), qr));
            }
            break;
        case Command::Kind::Screenshot:
            // Stash for end-of-frame hook. Only one in flight at a time;
            // if two come back-to-back the earlier one is signalled as
            // failed so its HTTP thread unblocks cleanly.
            if (g_pending_shot) {
                std::lock_guard lk(g_pending_shot->mu);
                g_pending_shot->done    = true;
                g_pending_shot->success = false;
                g_pending_shot->cv.notify_all();
            }
            g_pending_shot = c.waiter;
            break;
        }
    }

    // Publish camera snapshot for /state queries. Quaternion → approximate
    // yaw/pitch for human-readable telemetry. Computed from the forward
    // vector: pitch = arcsin(fwd.y), yaw = atan2(-fwd.x, -fwd.z) — same
    // sign convention as the old Euler camera so /state output is
    // backward-compatible for callers that compare past values. Roll
    // can be reconstructed from the up vector but isn't currently
    // surfaced because no caller asks for it.
    const HMM_Vec3 fwd = cam.forward();
    const float pitch_deg = std::asin(std::clamp(fwd.Y, -1.0f, 1.0f)) * HMM_RadToDeg;
    const float yaw_deg   = std::atan2(-fwd.X, -fwd.Z) * HMM_RadToDeg;

    std::lock_guard lk(g_cam_snapshot_mu);
    g_cam_pos   = cam.position;
    g_cam_euler = { pitch_deg, yaw_deg, 0.0f };
}

void maybe_capture_screenshot() {
    if (!g_pending_shot) return;

    ScreenshotWaiter* w = g_pending_shot;
    g_pending_shot = nullptr;

    bool ok = dev_remote_capture_window(w->path.c_str());
    {
        std::lock_guard lk(w->mu);
        w->done    = true;
        w->success = ok;
    }
    w->cv.notify_all();
}

void publish_system_name(const char* name) {
    std::lock_guard lk(g_system_mu);
    g_system_name = name ? name : "";
}

void publish_render_matrices(const HMM_Mat4& view_proj,
                             const HMM_Mat4& model,
                             HMM_Vec3 cam_pos) {
    std::lock_guard lk(g_proj_mu);
    g_view_proj   = view_proj;
    g_model       = model;
    g_proj_cam_pos = cam_pos;
    g_proj_ready  = true;
}

} // namespace dev_remote
