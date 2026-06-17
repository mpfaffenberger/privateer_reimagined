// -----------------------------------------------------------------------------
// win32_launch_dialog.cpp — minimal pre-launch resolution picker.
//
// Built with raw Win32 API (no .rc, no manifest needed) so it compiles
// on a fresh Windows box without resource tooling. The dialog blocks until
// the user clicks OK or Cancel; the chosen width/height/fullscreen is
// returned to sokol_main, which passes it into the sapp_desc.
// -----------------------------------------------------------------------------

#include "win32_launch_dialog.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <string>

namespace {

struct Mode {
    const char* label;
    int  w;
    int  h;
    bool fullscreen;
};

// Curated list. 16:9 primary, 16:10 legacy, 4:3 fallback. Windowed default
// so first-time users don't get trapped on a bad monitor.
constexpr Mode k_modes[] = {
    { "1280x720  (Windowed)",   1280,  720, false },
    { "1920x1080 (Windowed)",  1920, 1080, false },
    { "2560x1440 (Windowed)",  2560, 1440, false },
    { "3840x2160 (Windowed)",  3840, 2160, false },
    { "1920x1080 (Fullscreen)", 1920, 1080, true  },
    { "2560x1440 (Fullscreen)", 2560, 1440, true  },
    { "3840x2160 (Fullscreen)", 3840, 2160, true  },
};
constexpr int k_default_index = 1; // 1920x1080 windowed

constexpr int k_dlg_w  = 360;
constexpr int k_dlg_h  = 180;
constexpr int k_margin = 16;
constexpr int k_ctl_h  = 24;

struct DlgState {
    HWND    combo    = nullptr;
    HWND    ok       = nullptr;
    HWND    cancel   = nullptr;
    bool    accepted = false;
    int     selected = k_default_index;
};

static DlgState g_dlg{};

INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM /*lparam*/) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_dlg.combo = GetDlgItem(hwnd, 100);
        g_dlg.ok    = GetDlgItem(hwnd, 101);
        g_dlg.cancel= GetDlgItem(hwnd, 102);
        for (const auto& m : k_modes)            SendMessageA(g_dlg.combo, CB_ADDSTRING, 0, (LPARAM)m.label);
        SendMessageA(g_dlg.combo, CB_SETCURSEL, (WPARAM)k_default_index, 0);
        SetFocus(g_dlg.ok);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == 101) {             // OK
            g_dlg.selected = (int)SendMessageA(g_dlg.combo, CB_GETCURSEL, 0, 0);
            if (g_dlg.selected < 0 || g_dlg.selected >= (int)(sizeof(k_modes)/sizeof(k_modes[0])))
                g_dlg.selected = k_default_index;
            g_dlg.accepted = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        } else if (LOWORD(wparam) == 102) {      // Cancel
            g_dlg.accepted = false;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        g_dlg.accepted = false;
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// Create all child controls. We don't use a .rc, so everything is done
// programmatically. IDs: 100 = combo, 101 = OK, 102 = Cancel.
void create_controls(HWND hwnd) {
    HINSTANCE inst = GetModuleHandle(nullptr);
    const int combo_w = k_dlg_w - 2 * k_margin;
    const int btn_w   = (k_dlg_w - 3 * k_margin) / 2;

    g_dlg.combo = CreateWindowA("COMBOBOX", "",
        CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        k_margin, k_margin, combo_w, k_ctl_h * 8,
        hwnd, (HMENU)100, inst, nullptr);

    g_dlg.ok = CreateWindowA("BUTTON", "OK",
        BS_DEFPUSHBUTTON | WS_CHILD | WS_VISIBLE,
        k_margin, k_dlg_h - k_margin - k_ctl_h, btn_w, k_ctl_h,
        hwnd, (HMENU)101, inst, nullptr);

    g_dlg.cancel = CreateWindowA("BUTTON", "Cancel",
        BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE,
        k_margin + btn_w + k_margin, k_dlg_h - k_margin - k_ctl_h, btn_w, k_ctl_h,
        hwnd, (HMENU)102, inst, nullptr);
}

// Center the dialog on the primary monitor and give it a plain app look.
void center_window(HWND hwnd) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    const int dlg_w = rc.right - rc.left;
    const int dlg_h = rc.bottom - rc.top;

    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);

    SetWindowPos(hwnd, nullptr,
        (screen_w - dlg_w) / 2, (screen_h - dlg_h) / 2,
        0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == 101) {          // OK
            g_dlg.selected = (int)SendMessageA(g_dlg.combo, CB_GETCURSEL, 0, 0);
            if (g_dlg.selected < 0 || g_dlg.selected >= (int)(sizeof(k_modes)/sizeof(k_modes[0])))
                g_dlg.selected = k_default_index;
            g_dlg.accepted = true;
            DestroyWindow(hwnd);
            return 0;
        } else if (LOWORD(wparam) == 102) {   // Cancel
            g_dlg.accepted = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_dlg.accepted = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

} // namespace

namespace win32 {

bool pick_resolution(int* out_w, int* out_h, bool* out_fullscreen) {
    if (!out_w || !out_h || !out_fullscreen) return false;

    HINSTANCE inst = GetModuleHandle(nullptr);

    WNDCLASSA wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "new_privateer_launch_dialog";
    if (!RegisterClassA(&wc)) {
        // Might already be registered if called twice in one process; that's OK.
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        wc.lpszClassName,
        "new_privateer — select resolution",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        k_dlg_w, k_dlg_h,
        nullptr, nullptr, inst, nullptr);

    if (!hwnd) return false;

    center_window(hwnd);

    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageA(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        if (!IsDialogMessageA(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (!g_dlg.accepted) return false;

    const Mode& m = k_modes[g_dlg.selected];
    *out_w = m.w;
    *out_h = m.h;
    *out_fullscreen = m.fullscreen;
    return true;
}

} // namespace win32

#endif // _WIN32
