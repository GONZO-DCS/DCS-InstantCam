// DCS Instant Cam - tray app: quick settings, photo folder, and the caption editor.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <vector>

#include "compose.h"

#define IC_VERSION L"1.4.2"

static HINSTANCE g_hinst;
static std::wstring g_dataDir, g_installDir, g_ini;
static HWND g_hidden = nullptr, g_editor = nullptr;
static HFONT g_uiFont = nullptr;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static UINT WM_TASKBARCREATED = 0;

enum
{
    WM_TRAY = WM_APP + 1,
    WM_OPEN_EDITOR = WM_APP + 2,
    ID_HEADER = 1000,
    ID_PHOTOS,
    ID_EDITOR,
    ID_NOTE,
    ID_FOLLOW,
    ID_FILM,
    ID_DETAILS,
    ID_SOUND,
    ID_RES_1024,
    ID_RES_1536,
    ID_RES_2048,
    ID_SETTINGS,
    ID_LOG,
    ID_AUTOSTART,
    ID_EXIT,
};

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------
static int IniInt(const wchar_t* sec, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, g_ini.c_str());
}
static void IniSetInt(const wchar_t* sec, const wchar_t* key, int v)
{
    WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), g_ini.c_str());
}
static std::wstring IniStr(const wchar_t* sec, const wchar_t* key)
{
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, L"", buf, 512, g_ini.c_str());
    return buf;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"DCS Instant Cam";

static bool AutoStartOn()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}
static void SetAutoStart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    }
    else
        RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
}

static void OpenPath(const std::wstring& p, const wchar_t* verb = L"open")
{
    ShellExecuteW(nullptr, verb, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// small text input dialog (for the default caption note)
// ---------------------------------------------------------------------------
struct InputState { std::wstring text; bool done = false, ok = false; HWND edit = nullptr; };

static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* st = (InputState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m)
    {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)
        {
            if (LOWORD(w) == IDOK)
            {
                int n = GetWindowTextLengthW(st->edit);
                std::wstring t(n, 0);
                GetWindowTextW(st->edit, &t[0], n + 1);
                st->text = t;
                st->ok = true;
            }
            st->done = true;
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        st->done = true;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool InputBox(const wchar_t* title, const wchar_t* prompt, std::wstring& value)
{
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = InputProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = L"ICInput";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        reg = true;
    }
    InputState st;
    RECT r{0, 0, S(460), S(130)};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"ICInput", title, WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_hinst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);
    HWND lbl = CreateWindowW(L"STATIC", prompt, WS_CHILD | WS_VISIBLE, S(12), S(12), S(436), S(36), h, nullptr, g_hinst, nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              S(12), S(52), S(436), S(26), h, nullptr, g_hinst, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(262), S(90), S(90), S(28),
                            h, (HMENU)IDOK, g_hinst, nullptr);
    HWND ca = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(358), S(90), S(90), S(28), h,
                            (HMENU)IDCANCEL, g_hinst, nullptr);
    for (HWND c : {lbl, st.edit, ok, ca}) SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    RECT wr;
    GetWindowRect(h, &wr);
    SetWindowPos(h, nullptr, (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (st.ok) value = st.text;
    return st.ok;
}

// ---------------------------------------------------------------------------
// caption editor
// ---------------------------------------------------------------------------
enum { IDC_LIST = 100, IDC_L1, IDC_L2, IDC_L3, IDC_SAVE = 110, IDC_REVERT, IDC_OPENIMG, IDC_FOLDER, IDC_REFRESH, IDC_STATUS = 120 };

struct EditorState
{
    std::vector<std::wstring> files;  // full paths, newest first
    int cur = -1;
    int size = 0;                     // photo size of the current card
    std::vector<uint8_t> photo;       // BGRA photo crop (size x size)
    std::vector<uint8_t> preview;     // BGRA card
    int pw = 0, ph = 0;
    bool dirty = false;
    HWND list = nullptr, ed[3] = {}, status = nullptr;
    RECT prevRect{};
};
static EditorState g_ed;

static std::wstring PrettyFileName(const std::wstring& path)
{
    std::wstring n = path.substr(path.find_last_of(L"\\/") + 1);
    int Y, M, D, h, m, s;
    if (swscanf(n.c_str(), L"IC_%4d%2d%2d_%2d%2d%2d", &Y, &M, &D, &h, &m, &s) == 6)
    {
        static const wchar_t* mo[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        wchar_t b[64];
        swprintf(b, 64, L"%02d %ls %04d   %02d:%02d:%02d", D, mo[(M + 11) % 12], Y, h, m, s);
        return b;
    }
    return n;
}

static void SetStatus(const std::wstring& s) { SetWindowTextW(g_ed.status, s.c_str()); }

static std::vector<std::wstring> EditorLines()
{
    std::vector<std::wstring> lines;
    for (HWND e : g_ed.ed)
    {
        int n = GetWindowTextLengthW(e);
        std::wstring t(n, 0);
        GetWindowTextW(e, &t[0], n + 1);
        lines.push_back(t);
    }
    return lines;
}

static void RebuildPreview()
{
    if (g_ed.photo.empty()) { g_ed.preview.clear(); InvalidateRect(g_editor, &g_ed.prevRect, FALSE); return; }
    card::Compose(g_ed.photo.data(), g_ed.size, (size_t)g_ed.size * 4, true, EditorLines(), g_installDir, g_ed.preview,
                  g_ed.pw, g_ed.ph);
    InvalidateRect(g_editor, &g_ed.prevRect, FALSE);
}

static void LoadPhoto(int idx)
{
    g_ed.cur = idx;
    g_ed.photo.clear();
    g_ed.dirty = false;
    if (idx < 0 || idx >= (int)g_ed.files.size())
    {
        for (HWND e : g_ed.ed) SetWindowTextW(e, L"");
        RebuildPreview();
        return;
    }
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    std::wstring caption;
    if (!card::LoadPngBGRA(g_ed.files[idx], px, w, h, &caption))
    {
        SetStatus(L"Could not open this photo.");
        RebuildPreview();
        return;
    }
    int size = card::SizeFromCardW((int)w);
    if (std::abs(card::CardH(size) - (int)h) > 2 || size < 64)
    {
        SetStatus(L"This file isn't an Instant Cam print.");
        RebuildPreview();
        return;
    }
    g_ed.size = size;
    g_ed.photo.resize((size_t)size * size * 4);
    int ox = card::ImgX(size), oy = card::ImgY(size);
    for (int y = 0; y < size; ++y)
        memcpy(&g_ed.photo[(size_t)y * size * 4], &px[((size_t)(y + oy) * w + ox) * 4], (size_t)size * 4);
    auto lines = card::SplitLines(caption);
    for (int i = 0; i < 3; ++i) SetWindowTextW(g_ed.ed[i], i < (int)lines.size() ? lines[i].c_str() : L"");
    g_ed.dirty = false;
    SetStatus(caption.empty() ? L"No saved caption in this photo (taken before v1.4) - type new lines and Save."
                              : L"Edit the lines, then Save.");
    RebuildPreview();
}

static void RefreshList()
{
    std::wstring keep = (g_ed.cur >= 0 && g_ed.cur < (int)g_ed.files.size()) ? g_ed.files[g_ed.cur] : L"";
    g_ed.files.clear();
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((g_dataDir + L"\\Photos\\IC_*.png").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do g_ed.files.push_back(g_dataDir + L"\\Photos\\" + fd.cFileName);
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(g_ed.files.begin(), g_ed.files.end(), std::greater<std::wstring>());
    SendMessageW(g_ed.list, LB_RESETCONTENT, 0, 0);
    int sel = g_ed.files.empty() ? -1 : 0;
    for (size_t i = 0; i < g_ed.files.size(); ++i)
    {
        SendMessageW(g_ed.list, LB_ADDSTRING, 0, (LPARAM)PrettyFileName(g_ed.files[i]).c_str());
        if (g_ed.files[i] == keep) sel = (int)i;
    }
    SendMessageW(g_ed.list, LB_SETCURSEL, sel, 0);
    LoadPhoto(sel);
    if (g_ed.files.empty()) SetStatus(L"No photos yet - take some in DCS first.");
}

static void SaveCurrent()
{
    if (g_ed.cur < 0 || g_ed.photo.empty()) return;
    auto lines = EditorLines();
    std::vector<uint8_t> px;
    int W, H;
    if (!card::Compose(g_ed.photo.data(), g_ed.size, (size_t)g_ed.size * 4, true, lines, g_installDir, px, W, H)) return;
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::wstring cap = card::JoinLines(lines);
    bool ok = card::SavePng(g_ed.files[g_ed.cur], px.data(), W, H, cap);
    if (ok && g_ed.cur == 0) card::SavePng(g_dataDir + L"\\Latest.png", px.data(), W, H, cap);
    g_ed.dirty = !ok;
    SetStatus(ok ? L"Saved." : L"Could not save - is the photo open in another program?");
}

static bool ConfirmDiscard()
{
    if (!g_ed.dirty) return true;
    int r = MessageBoxW(g_editor, L"Save the changed caption first?", L"DCS Instant Cam", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) SaveCurrent();
    return true;
}

static void PaintPreview(HDC dc)
{
    RECT r = g_ed.prevRect;
    HBRUSH bg = CreateSolidBrush(RGB(60, 62, 66));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (g_ed.preview.empty() || !g_ed.pw) return;
    int aw = r.right - r.left - S(16), ah = r.bottom - r.top - S(16);
    double sc = std::min((double)aw / g_ed.pw, (double)ah / g_ed.ph);
    int dw = (int)(g_ed.pw * sc), dh = (int)(g_ed.ph * sc);
    int dx = r.left + (r.right - r.left - dw) / 2, dy = r.top + (r.bottom - r.top - dh) / 2;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_ed.pw;
    bi.bmiHeader.biHeight = -g_ed.ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, g_ed.pw, g_ed.ph, g_ed.preview.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK EditorProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_CREATE:
    {
        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int cw, int ch, int id, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, S(x), S(y), S(cw), S(ch), h,
                                     (HMENU)(INT_PTR)id, g_hinst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            return c;
        };
        mk(L"STATIC", L"Photos (newest first)", 0, 12, 10, 240, 20, 0);
        g_ed.list = mk(L"LISTBOX", L"", WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 12, 32, 240, 486, IDC_LIST,
                       WS_EX_CLIENTEDGE);
        mk(L"BUTTON", L"Refresh", WS_TABSTOP, 12, 526, 116, 28, IDC_REFRESH);
        mk(L"BUTTON", L"Open folder", WS_TABSTOP, 136, 526, 116, 28, IDC_FOLDER);
        g_ed.prevRect = {S(264), S(10), S(704), S(554)};
        const wchar_t* labels[3] = {L"Line 1 (place, date, time)", L"Line 2 (pilot, aircraft, note)", L"Line 3 (optional)"};
        for (int i = 0; i < 3; ++i)
        {
            mk(L"STATIC", labels[i], 0, 718, 10 + i * 64, 330, 20, 0);
            g_ed.ed[i] = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 718, 32 + i * 64, 330, 26, IDC_L1 + i, WS_EX_CLIENTEDGE);
        }
        mk(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 718, 212, 106, 30, IDC_SAVE);
        mk(L"BUTTON", L"Undo changes", WS_TABSTOP, 830, 212, 106, 30, IDC_REVERT);
        mk(L"BUTTON", L"Open image", WS_TABSTOP, 942, 212, 106, 30, IDC_OPENIMG);
        g_ed.status = mk(L"STATIC", L"", 0, 718, 256, 330, 60, IDC_STATUS);
        mk(L"STATIC",
           L"Tip: the preview updates as you type. Saving rewrites the photo with the new handwriting; "
           L"the picture itself is never changed.",
           0, 718, 470, 330, 80, 0);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case IDC_LIST:
            if (HIWORD(w) == LBN_SELCHANGE)
            {
                int sel = (int)SendMessageW(g_ed.list, LB_GETCURSEL, 0, 0);
                if (sel != g_ed.cur)
                {
                    if (ConfirmDiscard()) LoadPhoto(sel);
                    else SendMessageW(g_ed.list, LB_SETCURSEL, g_ed.cur, 0);
                }
            }
            break;
        case IDC_L1:
        case IDC_L2:
        case IDC_L3:
            if (HIWORD(w) == EN_CHANGE && GetFocus() == (HWND)l)
            {
                g_ed.dirty = true;
                SetStatus(L"Unsaved changes.");
                SetTimer(h, 1, 150, nullptr);
            }
            break;
        case IDC_SAVE: SaveCurrent(); break;
        case IDC_REVERT: LoadPhoto(g_ed.cur); break;
        case IDC_OPENIMG:
            if (g_ed.cur >= 0) OpenPath(g_ed.files[g_ed.cur]);
            break;
        case IDC_FOLDER: OpenPath(g_dataDir + L"\\Photos"); break;
        case IDC_REFRESH:
            if (ConfirmDiscard()) RefreshList();
            break;
        }
        return 0;
    case WM_TIMER:
        KillTimer(h, 1);
        RebuildPreview();
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintPreview(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        if (ConfirmDiscard()) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_editor = nullptr;
        g_ed = EditorState{};
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenEditor()
{
    if (g_editor)
    {
        ShowWindow(g_editor, SW_RESTORE);
        SetForegroundWindow(g_editor);
        RefreshList();
        return;
    }
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = L"ICEditor";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        reg = true;
    }
    RECT r{0, 0, S(1060), S(566)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_editor = CreateWindowW(L"ICEditor", L"DCS Instant Cam - Edit photo captions", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_hinst, nullptr);
    ShowWindow(g_editor, SW_SHOW);
    SetForegroundWindow(g_editor);
    RefreshList();
}

// ---------------------------------------------------------------------------
// tray
// ---------------------------------------------------------------------------
static void AddTrayIcon(bool balloon)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hidden;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(nid.szTip, L"DCS Instant Cam");
    if (balloon)
    {
        nid.uFlags |= NIF_INFO;
        wcscpy(nid.szInfoTitle, L"DCS Instant Cam");
        wcscpy(nid.szInfo, L"Running in the tray. Right-click the camera icon for settings and the caption editor.");
        nid.dwInfoFlags = NIIF_INFO;
    }
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hidden;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowMenu()
{
    HMENU m = CreatePopupMenu();
    auto chk = [](bool b) { return (UINT)(MF_STRING | (b ? MF_CHECKED : MF_UNCHECKED)); };
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_HEADER, L"DCS Instant Cam " IC_VERSION);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_PHOTOS, L"Open photos folder");
    AppendMenuW(m, MF_STRING, ID_EDITOR, L"Edit photo captions...");
    AppendMenuW(m, MF_STRING, ID_NOTE, L"My note on new photos...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, chk(IniInt(L"FlatScreen", L"FollowAim", 1) != 0), ID_FOLLOW, L"Flat screen: camera follows aim");
    AppendMenuW(m, chk(IniInt(L"Camera", L"FilmPack", 10) > 0), ID_FILM, L"Film packs (10 shots)");
    AppendMenuW(m, chk(IniInt(L"Photo", L"CaptionDetails", 0) != 0), ID_DETAILS, L"Caption: altitude, speed, heading line");
    AppendMenuW(m, chk(IniInt(L"Camera", L"Sound", 1) != 0), ID_SOUND, L"Shutter sound");
    HMENU res = CreatePopupMenu();
    int cur = IniInt(L"Photo", L"Resolution", 1536);
    AppendMenuW(res, chk(cur == 1024), ID_RES_1024, L"1024 (faster)");
    AppendMenuW(res, chk(cur == 1536), ID_RES_1536, L"1536 (default)");
    AppendMenuW(res, chk(cur == 2048), ID_RES_2048, L"2048 (sharpest)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)res, L"Photo resolution");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"All settings (InstantCam.ini)...");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Log file...");
    AppendMenuW(m, chk(AutoStartOn()), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_EDITOR, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hidden);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hidden, nullptr);
    PostMessageW(g_hidden, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK HiddenProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TASKBARCREATED && WM_TASKBARCREATED) { AddTrayIcon(false); return 0; }
    switch (m)
    {
    case WM_TRAY:
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) ShowMenu();
        else if (l == WM_LBUTTONDBLCLK) OpenEditor();
        return 0;
    case WM_OPEN_EDITOR: OpenEditor(); return 0;
    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case ID_PHOTOS: OpenPath(g_dataDir + L"\\Photos"); break;
        case ID_EDITOR: OpenEditor(); break;
        case ID_NOTE:
        {
            std::wstring v = IniStr(L"Photo", L"Caption");
            if (InputBox(L"My note on new photos",
                         L"Written at the end of the second line on every new photo, e.g. CAW UK. Leave empty for none.", v))
                WritePrivateProfileStringW(L"Photo", L"Caption", v.c_str(), g_ini.c_str());
            break;
        }
        case ID_FOLLOW: IniSetInt(L"FlatScreen", L"FollowAim", IniInt(L"FlatScreen", L"FollowAim", 1) ? 0 : 1); break;
        case ID_FILM: IniSetInt(L"Camera", L"FilmPack", IniInt(L"Camera", L"FilmPack", 10) > 0 ? 0 : 10); break;
        case ID_DETAILS: IniSetInt(L"Photo", L"CaptionDetails", IniInt(L"Photo", L"CaptionDetails", 0) ? 0 : 1); break;
        case ID_SOUND: IniSetInt(L"Camera", L"Sound", IniInt(L"Camera", L"Sound", 1) ? 0 : 1); break;
        case ID_RES_1024: IniSetInt(L"Photo", L"Resolution", 1024); break;
        case ID_RES_1536: IniSetInt(L"Photo", L"Resolution", 1536); break;
        case ID_RES_2048: IniSetInt(L"Photo", L"Resolution", 2048); break;
        case ID_SETTINGS: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_ini + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
        case ID_LOG: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_dataDir + L"\\InstantCam.log\"").c_str(), nullptr, SW_SHOWNORMAL); break;
        case ID_AUTOSTART: SetAutoStart(!AutoStartOn()); break;
        case ID_EXIT:
            if (g_editor) SendMessageW(g_editor, WM_CLOSE, 0, 0);
            if (!g_editor) DestroyWindow(h);
            break;
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR cmd, int)
{
    g_hinst = hinst;
    bool wantEditor = cmd && wcsstr(cmd, L"/edit");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"DCSInstantCamTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = FindWindowW(L"ICTrayHidden", nullptr);
        if (other) PostMessageW(other, WM_OPEN_EDITOR, 0, 0);
        return 0;
    }

    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(nullptr, sdc);
    g_uiFont = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_installDir = exe;
    g_installDir = g_installDir.substr(0, g_installDir.find_last_of(L"\\/"));
    PWSTR saved = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)))
    {
        g_dataDir = std::wstring(saved) + L"\\InstantCam";
        CoTaskMemFree(saved);
    }
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    CreateDirectoryW((g_dataDir + L"\\Photos").c_str(), nullptr);
    g_ini = g_dataDir + L"\\InstantCam.ini";

    bool firstRun = IniInt(L"Tray", L"Configured", 0) == 0;
    if (firstRun)
    {
        SetAutoStart(true);
        IniSetInt(L"Tray", L"Configured", 1);
    }

    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc{};
    wc.lpfnWndProc = HiddenProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"ICTrayHidden";
    RegisterClassW(&wc);
    g_hidden = CreateWindowW(L"ICTrayHidden", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, nullptr);
    AddTrayIcon(firstRun);
    if (wantEditor) OpenEditor();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (g_editor && IsDialogMessageW(g_editor, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
