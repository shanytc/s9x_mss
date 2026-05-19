// gui.cpp - Win32 GUI. Two side-by-side panes:
//   Side A (left) : SNES9x save state (any extension) -> Mesen2 .mss
//   Side B (right): Mesen2 .mss                       -> SNES9x .000-style state
//
// Workflow per side: drop a file (or click Browse). The app validates the
// header by magic bytes (not by extension), displays format + version, then
// enables the Convert button. Clicking Convert writes the output next to the
// input with a "_from_snes9x" / "_from_mesen2" suffix — toggleable via the
// "Include suffix" checkbox at the bottom of the window.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>

#include "s9x_mss.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ---- control IDs ----
enum {
    ID_BROWSE_A     = 1001,
    ID_BROWSE_B     = 1002,
    ID_CONVERT_A    = 1003,
    ID_CONVERT_B    = 1004,
    ID_OPEN_OUT_A   = 1005,
    ID_OPEN_OUT_B   = 1006,
    ID_REGION_B     = 1007,
    ID_ROM_BROWSE_B = 1008,
    ID_ROM_CLEAR_B  = 1009,
    ID_UPGRADE_A    = 1010,
    ID_SUFFIX_CB    = 1011,
};

// ---- per-pane state ----
struct Pane {
    HWND drop      = nullptr;   // drop zone (large static)
    HWND info      = nullptr;   // multi-line info: filename + format + version
    HWND status    = nullptr;   // result message
    HWND browse    = nullptr;
    HWND convert   = nullptr;
    HWND open_out  = nullptr;

    // Side A only — upgrade legacy snes9x 1.5.x to modern format
    HWND upgrade   = nullptr;
    bool is_legacy = false;

    // Side B only — region selector + optional ROM path for accurate region detection.
    HWND region_label  = nullptr;
    HWND region_combo  = nullptr;
    HWND rom_label     = nullptr;
    HWND rom_browse    = nullptr;
    HWND rom_clear     = nullptr;

    std::wstring input_path;
    bool         loaded = false;
    std::wstring last_output_path;
    std::wstring rom_path;
};

static Pane g_a;                // .009 -> .mss
static Pane g_b;                // .mss -> .009
static HWND g_main = nullptr;
static HWND g_title_a = nullptr;
static HWND g_title_b = nullptr;
static HWND g_suffix_cb = nullptr;
static HFONT g_font_big  = nullptr;
static HFONT g_font_norm = nullptr;
static HBRUSH g_drop_brush = nullptr;

static std::wstring to_w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}
static std::string from_w(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static void set_info(Pane& p, const std::wstring& text) {
    SetWindowTextW(p.info, text.c_str());
}
static void set_status(Pane& p, const std::wstring& text) {
    SetWindowTextW(p.status, text.c_str());
}

// Validate the dropped/browsed file, update the pane, enable Convert if OK.
// `forward` true == side A (expect SNES9x); false == side B (expect Mesen2).
static void load_file(Pane& p, const std::wstring& path, bool forward) {
    p.input_path = path;
    p.loaded = false;
    p.is_legacy = false;
    EnableWindow(p.convert, FALSE);
    if (p.upgrade) EnableWindow(p.upgrade, FALSE);

    std::string utf8 = from_w(path);
    std::wstring fname = fs::path(path).filename().wstring();

    ProbeResult r = forward ? probe_s9x(utf8) : probe_mss(utf8);
    if (!r.ok) {
        std::wstring info = L"File: " + fname + L"\nNot a "
                          + (forward ? std::wstring(L"SNES9x") : std::wstring(L"Mesen2"))
                          + L" save state: " + to_w(r.error);
        set_info(p, info);
        set_status(p, L"");
        return;
    }

    std::wstring info = L"File:    " + fname
                      + L"\nFormat:  " + to_w(r.label);
    if (!forward) {
        info += L"\nemu_ver: " + std::to_wstring(r.version)
              + L"   fmt_ver: " + std::to_wstring(r.fmt_version);
    }
    set_info(p, info);
    set_status(p, L"Ready. Click Convert.");
    p.loaded = true;
    EnableWindow(p.convert, TRUE);

    // Side A: enable the legacy upgrade button if the file uses the
    // pre-v6 "#!snes9x:NNNN" magic. probe_s9x signals this by storing the
    // version 1500/1510/... range; the label also contains "legacy".
    if (forward && p.upgrade) {
        p.is_legacy = (r.version >= 1000);
        EnableWindow(p.upgrade, p.is_legacy ? TRUE : FALSE);
    }
}

static void run_conversion(Pane& p, bool forward) {
    if (!p.loaded || p.input_path.empty()) return;

    std::string in_utf8 = from_w(p.input_path);
    fs::path q(in_utf8);
    std::string stem = q.stem().string();
    std::string out_utf8;
    bool include_suffix = g_suffix_cb
        && SendMessageW(g_suffix_cb, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (forward) {
        if (include_suffix) {
            std::string tag = "_from_snes9x";
            if (stem.find(tag) == std::string::npos) stem += tag;
        }
        out_utf8 = (q.parent_path() / (stem + ".mss")).string();
    } else {
        if (include_suffix) {
            std::string tag = "_from_mesen2";
            if (stem.find(tag) == std::string::npos) stem += tag;
        }
        // SNES9x states traditionally use .000-.999 numbered slots; keep the
        // user's original extension so naming feels consistent.
        std::string ext = q.extension().string();
        if (ext.empty() || ext == ".mss") ext = ".000";
        out_utf8 = (q.parent_path() / (stem + ext)).string();
    }

    set_status(p, L"Converting...");
    RedrawWindow(p.status, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    try {
        if (forward) {
            convert_s9x_to_mss(in_utf8, out_utf8);
        } else {
            int sel = (int)SendMessageW(p.region_combo, CB_GETCURSEL, 0, 0);
            RegionOverride r = (sel == 1) ? RegionOverride::ForceNTSC
                             : (sel == 2) ? RegionOverride::ForcePAL
                             :              RegionOverride::Auto;
            convert_mss_to_s9x(in_utf8, out_utf8, r, from_w(p.rom_path));
        }
    } catch (const std::exception& e) {
        set_status(p, L"Error: " + to_w(e.what()));
        EnableWindow(p.open_out, FALSE);
        return;
    }

    p.last_output_path = to_w(out_utf8);
    std::wstring msg = L"Wrote: ";
    msg += fs::path(p.last_output_path).filename().wstring();
    set_status(p, msg);
    EnableWindow(p.open_out, TRUE);
}

static void browse_for(Pane& p, bool forward) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_main;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (forward) {
        // SNES9x save states can have ANY extension: .000-.999, .oops, no
        // extension at all, anything the user named it. Default to All Files;
        // we validate by magic bytes after open.
        ofn.lpstrFilter = L"All files (*.*)\0*.*\0"
                          L"Common SNES9x extensions (*.000;*.001;...)\0*.0??;*.oops\0\0";
        ofn.lpstrTitle  = L"Pick a SNES9x save state";
    } else {
        ofn.lpstrFilter = L"Mesen2 save state (*.mss)\0*.mss\0All files\0*.*\0\0";
        ofn.lpstrTitle  = L"Pick a Mesen2 save state";
    }
    if (GetOpenFileNameW(&ofn)) {
        load_file(p, buf, forward);
    }
}

static void run_upgrade(Pane& p) {
    if (!p.loaded || !p.is_legacy || p.input_path.empty()) return;

    std::string in_utf8 = from_w(p.input_path);
    fs::path q(in_utf8);
    std::string stem = q.stem().string();
    std::string ext  = q.extension().string();
    if (ext.empty()) ext = ".000";
    std::string tag  = "_upgraded";
    if (stem.find(tag) == std::string::npos) stem += tag;
    std::string out_utf8 = (q.parent_path() / (stem + ext)).string();

    set_status(p, L"Upgrading...");
    RedrawWindow(p.status, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    try {
        upgrade_legacy_s9x_state(in_utf8, out_utf8);
    } catch (const std::exception& e) {
        set_status(p, L"Error: " + to_w(e.what()));
        EnableWindow(p.open_out, FALSE);
        return;
    }

    p.last_output_path = to_w(out_utf8);
    std::wstring msg = L"Upgraded: ";
    msg += fs::path(p.last_output_path).filename().wstring();
    set_status(p, msg);
    EnableWindow(p.open_out, TRUE);
}

static void update_rom_label(Pane& p) {
    if (!p.rom_label) return;
    if (p.rom_path.empty()) {
        SetWindowTextW(p.rom_label, L"ROM: (none — using Auto heuristic)");
    } else {
        std::wstring fname = fs::path(p.rom_path).filename().wstring();
        int pal = detect_rom_region_pal(from_w(p.rom_path));
        const wchar_t* tag = (pal == 1) ? L" [PAL]"
                           : (pal == 0) ? L" [NTSC]"
                           :              L" [region: unknown]";
        SetWindowTextW(p.rom_label, (L"ROM: " + fname + tag).c_str());
    }
}

static void browse_for_rom(Pane& p) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_main;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrFilter = L"SNES ROM (*.sfc;*.smc;*.fig;*.swc)\0*.sfc;*.smc;*.fig;*.swc\0All files\0*.*\0\0";
    ofn.lpstrTitle  = L"Pick the matching SNES ROM (used for region detection)";
    if (GetOpenFileNameW(&ofn)) {
        p.rom_path = buf;
        update_rom_label(p);
    }
}

static void clear_rom(Pane& p) {
    p.rom_path.clear();
    update_rom_label(p);
}

static void open_folder(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- subclass: drop area accepts files but doesn't display selection rect --
static WNDPROC g_static_orig = nullptr;
static LRESULT CALLBACK drop_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DROPFILES) {
        HDROP hd = HDROP(w);
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        if (n >= 1) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(hd, 0, path, MAX_PATH);
            bool forward = (h == g_a.drop);
            DragFinish(hd);
            load_file(forward ? g_a : g_b, path, forward);
            return 0;
        }
        DragFinish(hd);
        return 0;
    }
    return CallWindowProcW(g_static_orig, h, m, w, l);
}

// ---- pane layout ----------------------------------------------------------
// Per pane, top to bottom:
//   title                 (title_h)
//   drop area             (flexible)
//   info (3 lines)        (ROW_INFO)
//   status (1 line)       (ROW_STATUS)
//   buttons row           (ROW_BUTTONS)
// Plus gaps between rows.
static const int GAP         = 8;
static const int ROW_INFO    = 60;
static const int ROW_STATUS  = 22;
static const int ROW_BUTTONS = 30;

static const int DROP_MIN_H = 70;
static const int DROP_MAX_H = 110;

// Compute the drop-zone height that lets BOTH panes share the same vertical
// layout below the drop. Side B has an extra region row (combo + rom label) so
// its fixed bottom rows are taller; we use the larger of the two so the buttons
// line up across panes.
static int compute_shared_drop_h(int total_h, int title_h) {
    int fixed_a = title_h + GAP + GAP + ROW_INFO + GAP + ROW_STATUS + GAP + ROW_BUTTONS;
    int fixed_b = fixed_a + (ROW_BUTTONS + GAP + ROW_STATUS + GAP);
    int drop_h  = total_h - fixed_b;
    if (drop_h > DROP_MAX_H) drop_h = DROP_MAX_H;
    if (drop_h < DROP_MIN_H) drop_h = DROP_MIN_H;
    return drop_h;
}

static void layout_pane(const Pane& p, HWND title, int x, int top_y,
                        int pane_w, int total_h, int title_h, int drop_h) {
    (void)total_h;
    bool with_region = (p.region_combo != nullptr);

    int y = top_y;
    SetWindowPos(title, nullptr, x, y, pane_w, title_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += title_h + GAP;
    SetWindowPos(p.drop, nullptr, x, y, pane_w, drop_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += drop_h + GAP;
    SetWindowPos(p.info, nullptr, x + 8, y, pane_w - 16, ROW_INFO,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += ROW_INFO + GAP;
    SetWindowPos(p.status, nullptr, x + 8, y, pane_w - 16, ROW_STATUS,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += ROW_STATUS + GAP;
    if (with_region) {
        // The COMBOBOX's *closed* (visible) area renders at its font-derived
        // height (~24 px) and is pinned to the top of its bounding rect — it
        // doesn't fill ROW_BUTTONS the way a BUTTON does. Offset by half the
        // difference so the combo's visible edit area visually centres with
        // the 30 px buttons next to it. Same trick for the static label.
        const int combo_visible_h = 24;
        const int combo_dy = (ROW_BUTTONS - combo_visible_h) / 2;     // = 3
        const int label_h = 16;
        const int label_dy = (ROW_BUTTONS - label_h) / 2;             // = 7
        // For the combo, the height we pass is the *open* dropdown extent;
        // give it room for a few items rather than capping it at ROW_BUTTONS.
        const int combo_dropdown_h = 160;
        SetWindowPos(p.region_label, nullptr, x + 8,        y + label_dy, 60,  label_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(p.region_combo, nullptr, x + 8 + 64,   y + combo_dy, 130, combo_dropdown_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(p.rom_browse,   nullptr, x + 8 + 202,  y,            110, ROW_BUTTONS,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(p.rom_clear,    nullptr, x + 8 + 320,  y,            60,  ROW_BUTTONS,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += ROW_BUTTONS + GAP;
        SetWindowPos(p.rom_label, nullptr, x + 8, y, pane_w - 16, ROW_STATUS,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += ROW_STATUS;
    } else {
        // Pane A has no region row; instead use the equivalent vertical
        // span for the legacy-upgrade button (when present), then a blank
        // status-height gap so its bottom buttons sit at the same y as Side B.
        if (p.upgrade) {
            SetWindowPos(p.upgrade, nullptr, x + 8, y, 200, ROW_BUTTONS,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        y += ROW_BUTTONS + GAP + ROW_STATUS;
    }
    y += GAP;
    SetWindowPos(p.browse,   nullptr, x + 8,       y, 100, ROW_BUTTONS,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p.convert,  nullptr, x + 8 + 108, y, 100, ROW_BUTTONS,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p.open_out, nullptr, x + 8 + 216, y, 180, ROW_BUTTONS,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void create_pane(HWND parent, Pane& p, const wchar_t* drop_text,
                        int browse_id, int convert_id, int open_id,
                        bool with_region_controls = false,
                        bool with_upgrade_button = false) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    // SS_CENTER (no SS_CENTERIMAGE) lets the static control wrap long text;
    // SS_CENTERIMAGE would force a single line and clip the right side.
    p.drop = CreateWindowExW(
        WS_EX_ACCEPTFILES | WS_EX_CLIENTEDGE,
        L"STATIC", drop_text,
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
        0, 0, 100, 100, parent, nullptr, hi, nullptr);
    SendMessageW(p.drop, WM_SETFONT, WPARAM(g_font_big), TRUE);
    DragAcceptFiles(p.drop, TRUE);
    if (!g_static_orig)
        g_static_orig = (WNDPROC)GetWindowLongPtrW(p.drop, GWLP_WNDPROC);
    SetWindowLongPtrW(p.drop, GWLP_WNDPROC, (LONG_PTR)drop_proc);

    p.info = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 100, 60, parent, nullptr, hi, nullptr);
    SendMessageW(p.info, WM_SETFONT, WPARAM(g_font_norm), TRUE);

    p.status = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 100, 24, parent, nullptr, hi, nullptr);
    SendMessageW(p.status, WM_SETFONT, WPARAM(g_font_norm), TRUE);

    p.browse   = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 100, 32, parent, HMENU(intptr_t(browse_id)), hi, nullptr);
    SendMessageW(p.browse, WM_SETFONT, WPARAM(g_font_norm), TRUE);

    p.convert  = CreateWindowExW(0, L"BUTTON", L"Convert",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED,
        0, 0, 100, 32, parent, HMENU(intptr_t(convert_id)), hi, nullptr);
    SendMessageW(p.convert, WM_SETFONT, WPARAM(g_font_norm), TRUE);

    p.open_out = CreateWindowExW(0, L"BUTTON", L"Open output folder",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 180, 32, parent, HMENU(intptr_t(open_id)), hi, nullptr);
    SendMessageW(p.open_out, WM_SETFONT, WPARAM(g_font_norm), TRUE);

    if (with_upgrade_button) {
        // Upgrade legacy snes9x 1.5.x ('#!snes9x:NNNN') states to the modern
        // v12 format that current snes9x can load. Stays disabled unless the
        // dropped file's magic matches that legacy header.
        p.upgrade = CreateWindowExW(0, L"BUTTON",
            L"Upgrade legacy → v12",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            0, 0, 160, 32, parent,
            HMENU(intptr_t(ID_UPGRADE_A)), hi, nullptr);
        SendMessageW(p.upgrade, WM_SETFONT, WPARAM(g_font_norm), TRUE);
    }

    if (with_region_controls) {
        p.region_label = CreateWindowExW(0, L"STATIC", L"Region:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 60, 16, parent, nullptr, hi, nullptr);
        SendMessageW(p.region_label, WM_SETFONT, WPARAM(g_font_norm), TRUE);

        p.region_combo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            0, 0, 120, 200, parent,
            HMENU(intptr_t(ID_REGION_B)), hi, nullptr);
        SendMessageW(p.region_combo, WM_SETFONT, WPARAM(g_font_norm), TRUE);
        SendMessageW(p.region_combo, CB_ADDSTRING, 0, (LPARAM)L"Auto");
        SendMessageW(p.region_combo, CB_ADDSTRING, 0, (LPARAM)L"NTSC (force)");
        SendMessageW(p.region_combo, CB_ADDSTRING, 0, (LPARAM)L"PAL (force)");
        SendMessageW(p.region_combo, CB_SETCURSEL, 0, 0);

        p.rom_browse = CreateWindowExW(0, L"BUTTON", L"Browse ROM...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 110, 24, parent,
            HMENU(intptr_t(ID_ROM_BROWSE_B)), hi, nullptr);
        SendMessageW(p.rom_browse, WM_SETFONT, WPARAM(g_font_norm), TRUE);

        p.rom_clear = CreateWindowExW(0, L"BUTTON", L"Clear",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 60, 24, parent,
            HMENU(intptr_t(ID_ROM_CLEAR_B)), hi, nullptr);
        SendMessageW(p.rom_clear, WM_SETFONT, WPARAM(g_font_norm), TRUE);

        p.rom_label = CreateWindowExW(0, L"STATIC",
            L"ROM: (none — using Auto heuristic)",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            0, 0, 100, 22, parent, nullptr, hi, nullptr);
        SendMessageW(p.rom_label, WM_SETFONT, WPARAM(g_font_norm), TRUE);
    }
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font_norm = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = lf.lfHeight * 13 / 10;
        lf.lfWeight = FW_SEMIBOLD;
        g_font_big = CreateFontIndirectW(&lf);
        g_drop_brush = CreateSolidBrush(RGB(245, 245, 250));

        HINSTANCE hi = GetModuleHandleW(nullptr);
        g_title_a = CreateWindowExW(0, L"STATIC",
            L"Side A: SNES9x save state → Mesen2 .mss",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 100, 24, h, nullptr, hi, nullptr);
        SendMessageW(g_title_a, WM_SETFONT, WPARAM(g_font_big), TRUE);

        g_title_b = CreateWindowExW(0, L"STATIC",
            L"Side B: Mesen2 .mss → SNES9x save state",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 100, 24, h, nullptr, hi, nullptr);
        SendMessageW(g_title_b, WM_SETFONT, WPARAM(g_font_big), TRUE);

        create_pane(h, g_a,
            L"\nDrop SNES9x save state here\n\n"
            L"Any extension - validated by header",
            ID_BROWSE_A, ID_CONVERT_A, ID_OPEN_OUT_A,
            /*with_region_controls=*/false,
            /*with_upgrade_button=*/true);
        create_pane(h, g_b,
            L"\nDrop Mesen2 .mss here\n",
            ID_BROWSE_B, ID_CONVERT_B, ID_OPEN_OUT_B, true);

        // Footer: global toggle for the "_from_snes9x" / "_from_mesen2" tag
        // applied to output filenames. Checked by default (legacy behavior);
        // unchecked when the user wants the output to keep the input's name.
        g_suffix_cb = CreateWindowExW(0, L"BUTTON",
            L"Include \"_from_snes9x\" / \"_from_mesen2\" suffix in output filename",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            0, 0, 100, 22, h, HMENU(intptr_t(ID_SUFFIX_CB)), hi, nullptr);
        SendMessageW(g_suffix_cb, WM_SETFONT, WPARAM(g_font_norm), TRUE);
        SendMessageW(g_suffix_cb, BM_SETCHECK, BST_CHECKED, 0);
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        int margin = 10;
        int title_h = 28;
        int pane_w = (rc.right - 3 * margin) / 2;
        int pane_top = margin;
        const int FOOTER_H = 22;
        // Reserve space for the suffix-toggle checkbox at the bottom so the
        // panes (and their drop areas) lay out into the remaining height.
        int total_h = rc.bottom - margin - margin - (FOOTER_H + GAP);
        int drop_h = compute_shared_drop_h(total_h, title_h);
        layout_pane(g_a, g_title_a, margin,                  pane_top, pane_w, total_h, title_h, drop_h);
        layout_pane(g_b, g_title_b, 2 * margin + pane_w,     pane_top, pane_w, total_h, title_h, drop_h);
        if (g_suffix_cb) {
            SetWindowPos(g_suffix_cb, nullptr,
                margin, rc.bottom - margin - FOOTER_H,
                rc.right - 2 * margin, FOOTER_H,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        switch (id) {
        case ID_BROWSE_A:   browse_for(g_a, true);  break;
        case ID_BROWSE_B:   browse_for(g_b, false); break;
        case ID_CONVERT_A:  run_conversion(g_a, true);  break;
        case ID_CONVERT_B:  run_conversion(g_b, false); break;
        case ID_OPEN_OUT_A: open_folder(g_a.last_output_path); break;
        case ID_OPEN_OUT_B: open_folder(g_b.last_output_path); break;
        case ID_ROM_BROWSE_B: browse_for_rom(g_b); break;
        case ID_ROM_CLEAR_B:  clear_rom(g_b);      break;
        case ID_UPGRADE_A:    run_upgrade(g_a);    break;
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = HDC(w);
        HWND who = HWND(l);
        SetBkMode(dc, TRANSPARENT);
        if (who == g_a.drop || who == g_b.drop) {
            SetTextColor(dc, RGB(60, 60, 60));
            return LRESULT(g_drop_brush);
        }
        SetTextColor(dc, RGB(0, 0, 0));
        return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_DESTROY:
        if (g_font_big)   DeleteObject(g_font_big);
        if (g_font_norm)  DeleteObject(g_font_norm);
        if (g_drop_brush) DeleteObject(g_drop_brush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"S9xMssConverterWnd";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = main_proc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Fixed-size window — no resize border, no maximize button. Drop area is
    // capped (see DROP_MAX_H), the layout has no flexible row, so nothing
    // useful happens when stretching the window. Sizes are computed for the
    // exact client area the layout needs.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wantR = { 0, 0, 900, 390 };
    AdjustWindowRectEx(&wantR, style, FALSE, WS_EX_ACCEPTFILES);
    g_main = CreateWindowExW(
        WS_EX_ACCEPTFILES, cls,
        L"SNES9x ↔ Mesen2 Save State Converter",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        wantR.right - wantR.left, wantR.bottom - wantR.top,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}
