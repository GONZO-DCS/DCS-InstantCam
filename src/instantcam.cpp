// DCS Instant Cam - OpenXR API layer
// A handheld instant camera for DCS World in VR. The mouse acts as your hand.
//
// Hooks the DCS OpenXR session, draws a camera (with a live screen) and the
// developing print as quad layers, and captures photos straight from the
// eye images DCS submits - so the camera itself never appears in the photo.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <unknwn.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <mmsystem.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shader_src.h"
#include "compose.h"

#define IC_VERSION "1.4.0"
#define LAYER_NAME "XR_APILAYER_NOVENDOR_instantcam"

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static HMODULE g_module = nullptr;
static std::wstring g_dataDir;     // Saved Games\InstantCam
static std::wstring g_installDir;  // folder of this DLL
static std::mutex g_logMutex;

static void Log(const char* fmt, ...)
{
    if (g_dataDir.empty()) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* f = _wfopen((g_dataDir + L"\\InstantCam.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static double NowSeconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

static bool IsOurProcessForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void InitPaths()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_module, buf, MAX_PATH);
    std::wstring p(buf);
    g_installDir = p.substr(0, p.find_last_of(L"\\/"));

    PWSTR saved = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)))
    {
        g_dataDir = std::wstring(saved) + L"\\InstantCam";
        CoTaskMemFree(saved);
    }
    else
    {
        g_dataDir = g_installDir;
    }
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    CreateDirectoryW((g_dataDir + L"\\Photos").c_str(), nullptr);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Config
{
    int raiseMouseButton = 1;   // 1 = Mouse 4 (back), 2 = Mouse 5 (forward), 0 = off
    float sensitivity = 0.05f;  // degrees per mouse count
    int invertY = 0;
    float minFocal = 24.f, maxFocal = 200.f, startFocal = 35.f;
    float distance = 0.36f;     // metres in front of your eyes
    float drop = 0.07f;         // metres below eye level
    float cameraWidth = 0.18f;  // metres
    float printSeconds = 8.f;
    int filmPack = 10;          // shots per film pack, 0 = unlimited
    int captionDetails = 0;     // 1 = add altitude / speed / heading line
    int flatFollow = 1;         // flat screen: 1 = camera follows the aim, 0 = fixed in the corner
    int photoSize = 1536;
    int sound = 1;
    int telemetryPort = 47811;
    int keyRaise = 0, keyShutter = 0, keyZoomIn = 0, keyZoomOut = 0, keyRecenter = 0;
    std::wstring caption;
};
static Config g_cfg;

static const char* kDefaultIni =
    "; DCS Instant Cam settings. Restart DCS after editing.\r\n"
    "\r\n"
    "[Mouse]\r\n"
    "; Which side button raises / lowers the camera: 1 = Mouse 4 (back), 2 = Mouse 5 (forward), 0 = none\r\n"
    "RaiseButton=1\r\n"
    "; Aim speed in degrees per mouse count\r\n"
    "Sensitivity=0.05\r\n"
    "InvertY=0\r\n"
    "\r\n"
    "[Keys]\r\n"
    "; Optional keyboard bindings (Windows virtual-key codes, decimal; 0 = none).\r\n"
    "; Map these to HOTAS buttons with your joystick software (Joystick Gremlin, VKB, TARGET...).\r\n"
    "; F13=124 F14=125 F15=126 F16=127 F17=128 F18=129 F19=130 F20=131 F21=132 F22=133 F23=134 F24=135\r\n"
    "Raise=0\r\n"
    "Shutter=0\r\n"
    "ZoomIn=0\r\n"
    "ZoomOut=0\r\n"
    "Recenter=0\r\n"
    "\r\n"
    "[Camera]\r\n"
    "; Lens range in mm (35mm-equivalent, square frame)\r\n"
    "MinFocal=24\r\n"
    "MaxFocal=200\r\n"
    "StartFocal=35\r\n"
    "; Where the camera is held, in metres\r\n"
    "Distance=0.36\r\n"
    "Drop=0.07\r\n"
    "Width=0.18\r\n"
    "; How long the print stays in view after a shot (seconds)\r\n"
    "PrintSeconds=8\r\n"
    "; Shots per film pack (0 = unlimited). When the pack is empty, lower and raise the camera to load a new one.\r\n"
    "FilmPack=10\r\n"
    "Sound=1\r\n"
    "\r\n"
    "[Photo]\r\n"
    "; Photo resolution in pixels (square image area): 1024, 1536 or 2048\r\n"
    "Resolution=1536\r\n"
    "; Your own note, written at the end of the second caption line, e.g. CAW UK\r\n"
    "Caption=\r\n"
    "; 1 = add a third line with altitude, speed and heading\r\n"
    "CaptionDetails=0\r\n"
    "\r\n"
    "[FlatScreen]\r\n"
    "; 1 = the camera follows where you aim (held up in front of the target), 0 = fixed in the lower-right corner\r\n"
    "FollowAim=1\r\n"
    "\r\n"
    "[Telemetry]\r\n"
    "; UDP port used by InstantCamExport.lua (must match the Lua file)\r\n"
    "Port=47811\r\n";

static float IniFloat(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
    if (!buf[0]) return def;
    return wcstof(buf, nullptr);
}

static void LoadConfig()
{
    std::wstring ini = g_dataDir + L"\\InstantCam.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        FILE* f = _wfopen(ini.c_str(), L"wb");
        if (f) { fputs(kDefaultIni, f); fclose(f); }
    }
    const wchar_t* p = ini.c_str();
    g_cfg.raiseMouseButton = GetPrivateProfileIntW(L"Mouse", L"RaiseButton", 1, p);
    g_cfg.sensitivity = IniFloat(p, L"Mouse", L"Sensitivity", 0.05f);
    g_cfg.invertY = GetPrivateProfileIntW(L"Mouse", L"InvertY", 0, p);
    g_cfg.keyRaise = GetPrivateProfileIntW(L"Keys", L"Raise", 0, p);
    g_cfg.keyShutter = GetPrivateProfileIntW(L"Keys", L"Shutter", 0, p);
    g_cfg.keyZoomIn = GetPrivateProfileIntW(L"Keys", L"ZoomIn", 0, p);
    g_cfg.keyZoomOut = GetPrivateProfileIntW(L"Keys", L"ZoomOut", 0, p);
    g_cfg.keyRecenter = GetPrivateProfileIntW(L"Keys", L"Recenter", 0, p);
    g_cfg.minFocal = std::max(8.f, IniFloat(p, L"Camera", L"MinFocal", 24.f));
    g_cfg.maxFocal = std::max(g_cfg.minFocal, IniFloat(p, L"Camera", L"MaxFocal", 200.f));
    g_cfg.startFocal = std::min(g_cfg.maxFocal, std::max(g_cfg.minFocal, IniFloat(p, L"Camera", L"StartFocal", 35.f)));
    g_cfg.distance = IniFloat(p, L"Camera", L"Distance", 0.36f);
    g_cfg.drop = IniFloat(p, L"Camera", L"Drop", 0.07f);
    g_cfg.cameraWidth = IniFloat(p, L"Camera", L"Width", 0.18f);
    g_cfg.printSeconds = IniFloat(p, L"Camera", L"PrintSeconds", 8.f);
    g_cfg.sound = GetPrivateProfileIntW(L"Camera", L"Sound", 1, p);
    g_cfg.filmPack = std::max(0, std::min(99, (int)GetPrivateProfileIntW(L"Camera", L"FilmPack", 10, p)));
    g_cfg.photoSize = std::min(4096, std::max(256, (int)GetPrivateProfileIntW(L"Photo", L"Resolution", 1536, p)));
    wchar_t cap[256];
    GetPrivateProfileStringW(L"Photo", L"Caption", L"", cap, 256, p);
    g_cfg.caption = cap;
    g_cfg.captionDetails = GetPrivateProfileIntW(L"Photo", L"CaptionDetails", 0, p);
    g_cfg.flatFollow = GetPrivateProfileIntW(L"FlatScreen", L"FollowAim", 1, p);
    g_cfg.telemetryPort = GetPrivateProfileIntW(L"Telemetry", L"Port", 47811, p);
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}
static XrQuaternionf QConj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
static V3 QRot(const XrQuaternionf& q, V3 v)
{
    XrQuaternionf p{v.x, v.y, v.z, 0};
    XrQuaternionf r = QMul(QMul(q, p), QConj(q));
    return {r.x, r.y, r.z};
}
static XrQuaternionf QAxis(V3 axis, float ang)
{
    float s = sinf(ang * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, cosf(ang * 0.5f)};
}
static XrQuaternionf YawPitch(float yaw, float pitch)
{
    return QMul(QAxis({0, 1, 0}, yaw), QAxis({1, 0, 0}, pitch));
}
static V3 ToV3(const XrVector3f& v) { return {v.x, v.y, v.z}; }
static XrVector3f ToXr(V3 v) { return {v.x, v.y, v.z}; }

// ---------------------------------------------------------------------------
// Input (mouse low-level hook + keyboard polling)
// ---------------------------------------------------------------------------

struct InputState
{
    std::atomic<bool> raised{false};
    std::atomic<int> dx{0}, dy{0}, wheel{0}, shutter{0}, recenter{0};
};
static InputState g_in;
static std::thread g_hookThread;
static std::atomic<DWORD> g_hookThreadId{0};

static LRESULT CALLBACK MouseHookProc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && IsOurProcessForeground())
    {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        if ((w == WM_XBUTTONDOWN || w == WM_XBUTTONUP) && g_cfg.raiseMouseButton > 0 &&
            HIWORD(m->mouseData) == (WORD)g_cfg.raiseMouseButton)
        {
            if (w == WM_XBUTTONDOWN) g_in.raised = !g_in.raised;
            return 1;
        }
        if (g_in.raised)
        {
            switch (w)
            {
            case WM_MOUSEMOVE:
            {
                POINT cur;
                GetCursorPos(&cur);
                g_in.dx += m->pt.x - cur.x;
                g_in.dy += m->pt.y - cur.y;
                return 1;
            }
            case WM_LBUTTONDOWN: g_in.shutter++; return 1;
            case WM_RBUTTONDOWN: g_in.raised = false; return 1;
            case WM_MBUTTONDOWN: g_in.recenter++; return 1;
            case WM_MOUSEWHEEL: g_in.wheel += (short)HIWORD(m->mouseData); return 1;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            case WM_MOUSEHWHEEL:
                return 1;
            default: break;
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

static void StartInput()
{
    if (g_hookThread.joinable()) return;
    g_hookThread = std::thread([] {
        g_hookThreadId = GetCurrentThreadId();
        HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_module, 0);
        Log("Mouse hook %s", h ? "installed" : "FAILED");
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (h) UnhookWindowsHookEx(h);
    });
}

static void StopInput()
{
    if (!g_hookThread.joinable()) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    g_hookThread.join();
}

struct KeyEdge
{
    bool prev = false;
    bool Pressed(int vk, bool fg)
    {
        if (!vk) return false;
        bool down = fg && (GetAsyncKeyState(vk) & 0x8000);
        bool r = down && !prev;
        prev = down;
        return r;
    }
};

// ---------------------------------------------------------------------------
// Telemetry from Export.lua (UDP)
// ---------------------------------------------------------------------------

struct Telemetry
{
    bool valid = false;
    std::string aircraft;
    std::string pilot;
    bool hasPos = false;
    double px = 0, pz = 0;  // DCS map metres (x north, z east)
    double lat = 0, lon = 0, altM = 0, hdgDeg = 0, iasMs = 0, missionTime = -1;
    double receivedAt = 0;
};
static std::mutex g_telMutex;
static Telemetry g_tel;
static std::thread g_telThread;
static std::atomic<bool> g_telStop{false};

static void TelemetryLoop()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_cfg.telemetryPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
    {
        Log("Telemetry: could not bind UDP port %d", g_cfg.telemetryPort);
        closesocket(s);
        WSACleanup();
        return;
    }
    DWORD tmo = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    Log("Telemetry listening on 127.0.0.1:%d", g_cfg.telemetryPort);
    char buf[512];
    while (!g_telStop)
    {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        // IC1|name|lat|lon|alt_m|hdg_deg|ias_ms|mission_time_s
        if (strncmp(buf, "IC1|", 4) != 0) continue;
        std::vector<std::string> parts;
        std::string cur;
        for (int i = 4; i <= n; ++i)
        {
            if (i == n || buf[i] == '|') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(buf[i]);
        }
        if (parts.size() < 7) continue;
        Telemetry t;
        t.valid = !parts[0].empty();
        t.aircraft = parts[0];
        t.lat = atof(parts[1].c_str());
        t.lon = atof(parts[2].c_str());
        t.altM = atof(parts[3].c_str());
        t.hdgDeg = atof(parts[4].c_str());
        t.iasMs = atof(parts[5].c_str());
        t.missionTime = atof(parts[6].c_str());
        if (parts.size() >= 8) t.pilot = parts[7];
        if (parts.size() >= 10) { t.px = atof(parts[8].c_str()); t.pz = atof(parts[9].c_str()); t.hasPos = true; }
        t.receivedAt = NowSeconds();
        std::lock_guard<std::mutex> lk(g_telMutex);
        g_tel = t;
    }
    closesocket(s);
    WSACleanup();
}

static void StartTelemetry()
{
    if (g_telThread.joinable()) return;
    g_telStop = false;
    g_telThread = std::thread(TelemetryLoop);
}
static void StopTelemetry()
{
    if (!g_telThread.joinable()) return;
    g_telStop = true;
    g_telThread.join();
}

// ---------------------------------------------------------------------------
// Photo writer (GDI caption + WIC PNG)
// ---------------------------------------------------------------------------

using card::Widen;
using card::PrettyAircraft;

// ---- caption data written by InstantCamHook.lua at mission load ----
struct Town { std::wstring name; double lat, lon; };

static std::wstring TitleCase(const std::wstring& in)
{
    std::wstring w = in;
    bool start = true;
    for (auto& c : w)
    {
        if (iswalpha(c)) { c = start ? towupper(c) : towlower(c); start = false; }
        else start = true;
    }
    return w;
}

static std::vector<std::wstring> ReadLines(const std::wstring& path)
{
    std::vector<std::wstring> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    std::string data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(Widen(line));
        pos = e + 1;
    }
    return out;
}

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep)
{
    std::vector<std::wstring> r;
    std::wstring cur;
    for (wchar_t c : s) { if (c == sep) { r.push_back(cur); cur.clear(); } else cur.push_back(c); }
    r.push_back(cur);
    return r;
}

static std::wstring PlaceText(const std::wstring& rawName, double distKm, double brgDeg)
{
    std::wstring name = rawName;
    bool allUpper = true;
    for (wchar_t c : name) if (iswlower(c)) { allUpper = false; break; }
    if (allUpper) name = TitleCase(name);
    if (distKm < 15.0) return L"Near " + name;
    static const wchar_t* dirs[] = {L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW"};
    wchar_t b[160];
    swprintf(b, 160, L"%.0f nm %ls of %ls", distKm / 1.852, dirs[(int)((brgDeg + 22.5) / 45.0) % 8], name.c_str());
    return b;
}

// Airfields of the current map in DCS metres, written by InstantCamHook.lua
static std::wstring NearestAirfield(double x, double z)
{
    auto lines = ReadLines(g_dataDir + L"\\airfields.txt");
    double best = 1e18, bx = 0, bz = 0;
    std::wstring bn;
    for (auto& l : lines)
    {
        auto p = Split(l, L'|');
        if (p.size() < 3 || p[0].empty()) continue;
        double ax = _wtof(p[1].c_str()), az = _wtof(p[2].c_str());
        double d = (ax - x) * (ax - x) + (az - z) * (az - z);
        if (d < best) { best = d; bx = ax; bz = az; bn = p[0]; }
    }
    if (bn.empty()) return L"";
    double brg = fmod(atan2(z - bz, x - bx) * 180.0 / 3.14159265358979 + 360.0, 360.0);
    return PlaceText(bn, sqrt(best) / 1000.0, brg);
}

static std::wstring NearestPlace(double lat, double lon)
{
    auto lines = ReadLines(g_dataDir + L"\\towns.txt");
    const double R = 6371.0, d2r = 3.14159265358979 / 180.0;
    double best = 1e18;
    Town bt{};
    for (auto& l : lines)
    {
        auto p = Split(l, L'|');
        if (p.size() < 3) continue;
        double tl = _wtof(p[1].c_str()), tn = _wtof(p[2].c_str());
        double dlat = (tl - lat) * d2r, dlon = (tn - lon) * d2r;
        double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat * d2r) * cos(tl * d2r) * sin(dlon / 2) * sin(dlon / 2);
        double d = 2 * R * atan2(sqrt(a), sqrt(1 - a));
        if (d < best) { best = d; bt = {p[0], tl, tn}; }
    }
    if (bt.name.empty()) return L"";
    double y = sin((lon - bt.lon) * d2r) * cos(lat * d2r);
    double x = cos(bt.lat * d2r) * sin(lat * d2r) - sin(bt.lat * d2r) * cos(lat * d2r) * cos((lon - bt.lon) * d2r);
    double brg = fmod(atan2(y, x) / d2r + 360.0, 360.0);
    return PlaceText(bt.name, best, brg);
}

// Mission date (from mission.txt) advanced by the mission clock.
static bool MissionDateTime(double missionTime, SYSTEMTIME& out)
{
    auto lines = ReadLines(g_dataDir + L"\\mission.txt");
    if (lines.empty() || missionTime < 0) return false;
    auto p = Split(lines[0], L'|');
    if (p.size() < 4) return false;
    SYSTEMTIME st{};
    st.wYear = (WORD)_wtoi(p[1].c_str());
    st.wMonth = (WORD)_wtoi(p[2].c_str());
    st.wDay = (WORD)_wtoi(p[3].c_str());
    if (st.wYear < 1601 || st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1) return false;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)(missionTime * 1e7);
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return FileTimeToSystemTime(&ft, &out) != 0;
}

// The finished card is handed back to the render thread so the print you see matches the saved photo.
static std::mutex g_cardMutex;
static std::vector<uint8_t> g_cardPixels;  // BGRA
static int g_cardW = 0, g_cardH = 0;
static uint64_t g_cardSerial = 0;

static std::vector<std::wstring> CaptionLines(const Telemetry& tel, const SYSTEMTIME& st)
{
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                      L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    SYSTEMTIME when = st;
    bool missionClock = tel.valid && MissionDateTime(tel.missionTime, when);
    if (!missionClock && tel.valid && tel.missionTime >= 0)
    {
        int t = (int)tel.missionTime % 86400;
        when.wHour = (WORD)(t / 3600);
        when.wMinute = (WORD)((t / 60) % 60);
    }
    wchar_t date[64];
    swprintf(date, 64, L"%d %ls %04d, %02d:%02d", when.wDay, months[(when.wMonth + 11) % 12], when.wYear, when.wHour,
             when.wMinute);
    std::wstring place;
    if (tel.valid && tel.hasPos) place = NearestAirfield(tel.px, tel.pz);
    if (place.empty() && tel.valid) place = NearestPlace(tel.lat, tel.lon);
    std::wstring line1 = place.empty() ? std::wstring(date) : place + L" - " + date;

    std::vector<std::wstring> parts2;
    if (tel.valid && !tel.pilot.empty()) parts2.push_back(L"Pilot " + Widen(tel.pilot));
    if (tel.valid && !tel.aircraft.empty()) parts2.push_back(PrettyAircraft(tel.aircraft));
    if (!g_cfg.caption.empty()) parts2.push_back(g_cfg.caption);
    std::wstring line2;
    for (size_t i = 0; i < parts2.size(); ++i) line2 += (i ? L" | " : L"") + parts2[i];

    std::wstring line3;
    if (g_cfg.captionDetails && tel.valid)
    {
        wchar_t b[160];
        long ft = lround(tel.altM * 3.28084);
        long kt = lround(tel.iasMs * 1.943844);
        int hdg = ((int)lround(tel.hdgDeg) % 360 + 360) % 360;
        swprintf(b, 160, L"%ld ft | %ld kt | hdg %03d", ft, kt, hdg);
        line3 = b;
    }
    return {line1, line2, line3};
}

static void WritePhoto(std::vector<uint8_t> rgba, int size, Telemetry tel, SYSTEMTIME st)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto lines = CaptionLines(tel, st);
    std::vector<uint8_t> px;
    int W = 0, H = 0;
    if (!card::Compose(rgba.data(), size, (size_t)size * 4, false, lines, g_installDir, px, W, H))
    {
        Log("Photo compose failed");
        CoUninitialize();
        return;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::wstring caption = card::JoinLines(lines);

    wchar_t name[96];
    swprintf(name, 96, L"IC_%04d%02d%02d_%02d%02d%02d_%03d.png", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring path = g_dataDir + L"\\Photos\\" + name;
    bool ok = card::SavePng(path, px.data(), W, H, caption);
    if (ok) card::SavePng(g_dataDir + L"\\Latest.png", px.data(), W, H, caption);
    Log("Photo %ls %dx%d %s", name, W, H, ok ? "saved" : "FAILED");

    {
        std::lock_guard<std::mutex> lk(g_cardMutex);
        g_cardPixels = std::move(px);
        g_cardW = W;
        g_cardH = H;
        g_cardSerial++;
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// OpenXR dispatch
// ---------------------------------------------------------------------------

struct NextFns
{
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
};
static NextFns g_next;
static bool g_active = false;  // true only in DCS with D3D11
static XrInstance g_instance = XR_NULL_HANDLE;
static XrSession g_session = XR_NULL_HANDLE;

struct SwapInfo
{
    int64_t format = 0;
    uint32_t width = 0, height = 0, arraySize = 1, samples = 1;
    bool color = false;
    std::vector<ID3D11Texture2D*> images;
    std::deque<uint32_t> acquired;
    int lastReleased = -1;
    std::map<uint32_t, ID3D11ShaderResourceView*> srvs;
    bool sampleIsLinear = false;
    bool srvFailed = false;
};
static std::mutex g_swapMutex;
static std::unordered_map<XrSwapchain, SwapInfo> g_swaps;

static bool FormatIsLinearSample(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

static DXGI_FORMAT TypedFromTypeless(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

struct CBData
{
    float cRight[4], cUp[4], cFwd[4];
    float cRight2[4], cUp2[4], cFwd2[4];
    float eyeTan[4], eyeRect[4], eyeTan2[4], eyeRect2[4];
    float p0[4], p1[4], p1b[4], p2[4], p3[4], p4[4], p5[4];
};
static_assert(sizeof(CBData) % 16 == 0, "cbuffer size");

struct QuadTarget
{
    XrSwapchain sc = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    bool outLinear = true;
    std::vector<ID3D11Texture2D*> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
};

typedef HRESULT(WINAPI* PFN_D3DCompile_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                         LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

struct Renderer
{
    bool ok = false;
    bool failed = false;
    ID3D11Device* dev = nullptr;
    ID3D11Device1* dev1 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11DeviceContext1* ctx1 = nullptr;
    ID3DDeviceContextState* state = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11BlendState* bsPremul = nullptr;
    // 2D (flat screen) scene copy
    ID3D11Texture2D* sceneTex = nullptr;
    ID3D11ShaderResourceView* sceneSrv = nullptr;
    UINT sceneW = 0, sceneH = 0;
    DXGI_FORMAT sceneFmt = DXGI_FORMAT_UNKNOWN;
    bool sceneLinear = false;
    ID3D11DepthStencilState* dss = nullptr;
    // capture
    int capSize = 1024;
    ID3D11Texture2D* capTex = nullptr;
    ID3D11RenderTargetView* capRtv = nullptr;
    ID3D11ShaderResourceView* capSrv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    // finished print card (paper + photo + caption)
    ID3D11Texture2D* cardTex = nullptr;
    ID3D11ShaderResourceView* cardSrv = nullptr;
    uint64_t cardSerial = 0;
    bool cardFresh = false;  // card belongs to the print currently shown

    void UpdateCard()
    {
        std::lock_guard<std::mutex> lk(g_cardMutex);
        if (g_cardSerial == cardSerial || g_cardPixels.empty()) return;
        cardSerial = g_cardSerial;
        if (cardSrv) { cardSrv->Release(); cardSrv = nullptr; }
        if (cardTex) { cardTex->Release(); cardTex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = g_cardW;
        td.Height = g_cardH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{g_cardPixels.data(), (UINT)g_cardW * 4, 0};
        if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &cardTex))) dev->CreateShaderResourceView(cardTex, nullptr, &cardSrv);
        cardFresh = cardSrv != nullptr;
    }

    // camera body artwork
    ID3D11Texture2D* bodyTex = nullptr;
    ID3D11ShaderResourceView* bodySrv = nullptr;
    // quad layers
    QuadTarget camQ, printQ;

    void LoadBody()
    {
        std::wstring path = g_installDir + L"\\body.png";
        std::vector<uint8_t> pixels;
        UINT bw = 0, bh = 0;
        // decode on a helper thread so we never touch COM on DCS's render thread
        std::thread([&] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
            IWICImagingFactory* fac = nullptr;
            IWICBitmapDecoder* dec = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
                SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &dec)) &&
                SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(conv->GetSize(&bw, &bh)))
            {
                pixels.resize((size_t)bw * bh * 4);
                if (FAILED(conv->CopyPixels(nullptr, bw * 4, (UINT)pixels.size(), pixels.data()))) pixels.clear();
            }
            if (conv) conv->Release();
            if (frame) frame->Release();
            if (dec) dec->Release();
            if (fac) fac->Release();
            CoUninitialize();
        }).join();
        if (pixels.empty()) { Log("body.png not loaded - using plain camera body"); return; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = bw;
        td.Height = bh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{pixels.data(), bw * 4, 0};
        if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &bodyTex)))
            dev->CreateShaderResourceView(bodyTex, nullptr, &bodySrv);
        Log("Camera body art %ux%u %s", bw, bh, bodySrv ? "loaded" : "FAILED");
    }

    bool Init(ID3D11Device* d)
    {
        dev = d;
        dev->AddRef();
        dev->GetImmediateContext(&ctx);
        if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1)) ||
            FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)))
        {
            Log("D3D11.1 interfaces unavailable");
            return false;
        }
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        UINT stFlags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                           ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                           : 0;
        if (FAILED(dev1->CreateDeviceContextState(stFlags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1),
                                                  nullptr, &state)))
        {
            Log("CreateDeviceContextState failed");
            return false;
        }

        HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = comp ? (PFN_D3DCompile_)GetProcAddress(comp, "D3DCompile") : nullptr;
        if (!compile) { Log("d3dcompiler_47.dll not found"); return false; }
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        HRESULT hr = compile(kShaderSrc, strlen(kShaderSrc), "instantcam.hlsl", nullptr, nullptr, "VS", "vs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
        if (FAILED(hr)) { Log("VS compile: %s", err ? (char*)err->GetBufferPointer() : "?"); return false; }
        hr = compile(kShaderSrc, strlen(kShaderSrc), "instantcam.hlsl", nullptr, nullptr, "PS", "ps_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
        if (FAILED(hr)) { Log("PS compile: %s", err ? (char*)err->GetBufferPointer() : "?"); return false; }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        vsb->Release();
        psb->Release();
        if (!vs || !ps) return false;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CBData);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return false;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &samp);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);

        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = FALSE;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bld, &bs);
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        dev->CreateBlendState(&bld, &bsPremul);

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = FALSE;
        dsd.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&dsd, &dss);

        if (!CreateCapture(g_cfg.photoSize)) return false;
        LoadBody();
        ok = capRtv && capSrv;
        Log("Renderer ready (feature level %x)", (unsigned)fl);
        return ok;
    }

    bool CreateCapture(int size)
    {
        if (capRtv) { capRtv->Release(); capRtv = nullptr; }
        if (capSrv) { capSrv->Release(); capSrv = nullptr; }
        if (capTex) { capTex->Release(); capTex = nullptr; }
        if (staging) { staging->Release(); staging = nullptr; }
        capSize = size;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = capSize;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &capTex))) return false;
        dev->CreateRenderTargetView(capTex, nullptr, &capRtv);
        dev->CreateShaderResourceView(capTex, nullptr, &capSrv);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return false;
        if (capRtv)
        {
            float black[4] = {0, 0, 0, 1};
            ctx->ClearRenderTargetView(capRtv, black);
        }
        Log("Photo resolution %d", capSize);
        return capRtv && capSrv;
    }

    bool CreateQuad(XrSession s, QuadTarget& q, uint32_t w, uint32_t h)
    {
        if (q.sc) return true;
        uint32_t n = 0;
        g_next.EnumerateSwapchainFormats(s, 0, &n, nullptr);
        std::vector<int64_t> fmts(n);
        g_next.EnumerateSwapchainFormats(s, n, &n, fmts.data());
        int64_t pick = 0;
        const int64_t pref[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
        for (int64_t p : pref)
        {
            for (int64_t f : fmts) if (f == p) { pick = p; break; }
            if (pick) break;
        }
        if (!pick) { Log("No usable quad swapchain format"); return false; }
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = pick;
        ci.sampleCount = 1;
        ci.width = w;
        ci.height = h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrResult r = g_next.CreateSwapchain(s, &ci, &q.sc);
        if (XR_FAILED(r)) { Log("Quad swapchain create failed %d", (int)r); q.sc = XR_NULL_HANDLE; return false; }
        q.w = w;
        q.h = h;
        q.outLinear = FormatIsLinearSample((DXGI_FORMAT)pick);
        uint32_t cnt = 0;
        g_next.EnumerateSwapchainImages(q.sc, 0, &cnt, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(cnt, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        g_next.EnumerateSwapchainImages(q.sc, cnt, &cnt, (XrSwapchainImageBaseHeader*)imgs.data());
        for (auto& im : imgs)
        {
            q.images.push_back(im.texture);
            D3D11_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = (DXGI_FORMAT)pick;
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            dev->CreateRenderTargetView(im.texture, &rv, &rtv);
            q.rtvs.push_back(rtv);
        }
        Log("Quad swapchain %ux%u format %lld, %u images", w, h, (long long)pick, cnt);
        return true;
    }

    void DestroyQuad(QuadTarget& q)
    {
        for (auto* r : q.rtvs) if (r) r->Release();
        q.rtvs.clear();
        q.images.clear();
        if (q.sc) g_next.DestroySwapchain(q.sc);
        q.sc = XR_NULL_HANDLE;
    }

    void Draw(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, const CBData& data,
              ID3D11ShaderResourceView* eye, ID3D11ShaderResourceView* focus, ID3D11ShaderResourceView* print)
    {
        DrawRect(rtv, 0, 0, (float)w, (float)h, data, eye, focus, print, false);
    }

    void DrawRect(ID3D11RenderTargetView* rtv, float x, float y, float w, float h, const CBData& data,
                  ID3D11ShaderResourceView* eye, ID3D11ShaderResourceView* focus, ID3D11ShaderResourceView* print,
                  bool blend)
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, &data, sizeof(data));
            ctx->Unmap(cb, 0);
        }
        D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(blend ? bsPremul : bs, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetSamplers(0, 1, &samp);
        ID3D11ShaderResourceView* srvs[4] = {eye, print, focus, bodySrv};
        ctx->PSSetShaderResources(0, 4, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[4] = {nullptr, nullptr, nullptr, nullptr};
        ctx->PSSetShaderResources(0, 4, nulls);
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    }

    bool DrawQuad(QuadTarget& q, const CBData& data, ID3D11ShaderResourceView* eye,
                  ID3D11ShaderResourceView* focus, ID3D11ShaderResourceView* print)
    {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(g_next.AcquireSwapchainImage(q.sc, &ai, &idx))) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(g_next.WaitSwapchainImage(q.sc, &wi)))
        {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            g_next.ReleaseSwapchainImage(q.sc, &ri);
            return false;
        }
        if (idx < q.rtvs.size() && q.rtvs[idx])
        {
            CBData d = data;
            d.p3[0] = q.outLinear ? 1.f : 0.f;
            Draw(q.rtvs[idx], q.w, q.h, d, eye, focus, print);
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        g_next.ReleaseSwapchainImage(q.sc, &ri);
        return true;
    }

    void Shutdown()
    {
        DestroyQuad(camQ);
        DestroyQuad(printQ);
#define REL(x) if (x) { x->Release(); x = nullptr; }
        REL(cardSrv) REL(cardTex) REL(sceneSrv) REL(sceneTex) REL(bsPremul) REL(bodySrv) REL(bodyTex) REL(capRtv) REL(capSrv) REL(capTex) REL(staging) REL(cb) REL(samp) REL(rs) REL(bs) REL(dss)
        REL(vs) REL(ps) REL(state) REL(ctx1) REL(ctx) REL(dev1) REL(dev)
#undef REL
        ok = false;
        failed = false;
    }
};
static Renderer g_r;
static ID3D11Device* g_device = nullptr;

// ---------------------------------------------------------------------------
// Camera state
// ---------------------------------------------------------------------------

struct CameraState
{
    bool wasRaised = false;
    float yaw = 0, pitch = 0;
    float focal = 35.f;
    float raise = 0;  // 0..1 animation
    int shotsLeft = 10;
    double lastTime = 0;
    double flashUntil = 0;
    // print
    bool printActive = false;
    double printStart = 0;
    XrPosef printBase{};
    // readback
    int readbackIn = -1;
    SYSTEMTIME shotTime{};
    Telemetry shotTel;
    KeyEdge kRaise, kShutter, kRecenter;
};
static CameraState g_cam;

static ID3D11ShaderResourceView* GetEyeSrv(XrSwapchain sc, bool& linear, uint32_t& texW, uint32_t& texH)
{
    std::lock_guard<std::mutex> lk(g_swapMutex);
    auto it = g_swaps.find(sc);
    if (it == g_swaps.end()) return nullptr;
    SwapInfo& si = it->second;
    if (si.lastReleased < 0 || si.lastReleased >= (int)si.images.size() || si.samples > 1 || si.srvFailed)
        return nullptr;
    uint32_t idx = (uint32_t)si.lastReleased;
    texW = si.width;
    texH = si.height;
    auto s = si.srvs.find(idx);
    if (s != si.srvs.end())
    {
        linear = si.sampleIsLinear;
        return s->second;
    }
    ID3D11Texture2D* tex = si.images[idx];
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    texW = td.Width;
    texH = td.Height;
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    {
        Log("DCS eye texture is not sampleable (bind flags %x)", td.BindFlags);
        si.srvFailed = true;
        return nullptr;
    }
    DXGI_FORMAT candidates[2] = {TypedFromTypeless((DXGI_FORMAT)si.format), TypedFromTypeless(td.Format)};
    ID3D11ShaderResourceView* srv = nullptr;
    for (DXGI_FORMAT f : candidates)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.MostDetailedMip = 0;
        sd.Texture2DArray.FirstArraySlice = 0;
        sd.Texture2DArray.ArraySize = td.ArraySize;
        if (SUCCEEDED(g_device->CreateShaderResourceView(tex, &sd, &srv)))
        {
            si.sampleIsLinear = FormatIsLinearSample(f);
            Log("Eye SRV created: format %d, %ux%u x%u", (int)f, td.Width, td.Height, td.ArraySize);
            break;
        }
        srv = nullptr;
    }
    if (!srv)
    {
        Log("Could not create SRV for DCS eye texture (format %d / %d)", (int)si.format, (int)td.Format);
        si.srvFailed = true;
        return nullptr;
    }
    si.srvs[idx] = srv;
    linear = si.sampleIsLinear;
    return srv;
}

static void SetVec(float* d, V3 v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = 0; }

static void FillEye(const XrCompositionLayerProjectionView& view, const XrQuaternionf& camQ, bool focus, CBData& cb,
                    ID3D11ShaderResourceView*& srvOut)
{
    bool linear = false;
    uint32_t tw = 0, th = 0;
    srvOut = GetEyeSrv(view.subImage.swapchain, linear, tw, th);
    float* valid = focus ? &cb.p1b[3] : &cb.p1[3];
    *valid = 0;
    if (!srvOut || !tw || !th) return;
    XrQuaternionf inv = QConj(view.pose.orientation);
    V3 r = QRot(inv, QRot(camQ, {1, 0, 0}));
    V3 u = QRot(inv, QRot(camQ, {0, 1, 0}));
    V3 f = QRot(inv, QRot(camQ, {0, 0, -1}));
    SetVec(focus ? cb.cRight2 : cb.cRight, r);
    SetVec(focus ? cb.cUp2 : cb.cUp, u);
    SetVec(focus ? cb.cFwd2 : cb.cFwd, f);
    float* tan = focus ? cb.eyeTan2 : cb.eyeTan;
    tan[0] = tanf(view.fov.angleLeft);
    tan[1] = tanf(view.fov.angleRight);
    tan[2] = tanf(view.fov.angleUp);
    tan[3] = tanf(view.fov.angleDown);
    float* rect = focus ? cb.eyeRect2 : cb.eyeRect;
    rect[0] = (float)view.subImage.imageRect.offset.x / tw;
    rect[1] = (float)view.subImage.imageRect.offset.y / th;
    rect[2] = (float)view.subImage.imageRect.extent.width / tw;
    rect[3] = (float)view.subImage.imageRect.extent.height / th;
    float* p = focus ? cb.p1b : cb.p1;
    p[0] = (float)view.subImage.imageArrayIndex;
    p[1] = linear ? 1.f : 0.f;
    *valid = 1;
}

static void PlayShutter()
{
    if (!g_cfg.sound) return;
    std::wstring wav = g_installDir + L"\\shutter.wav";
    PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static void ProcessReadback()
{
    if (!(g_cam.readbackIn >= 0 && g_cam.readbackIn-- == 0 && g_r.ok)) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_r.ctx->Map(g_r.staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        int sz = g_r.capSize;
        std::vector<uint8_t> px((size_t)sz * sz * 4);
        for (int y = 0; y < sz; ++y)
            memcpy(&px[(size_t)y * sz * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)sz * 4);
        g_r.ctx->Unmap(g_r.staging, 0);
        std::thread(WritePhoto, std::move(px), sz, g_cam.shotTel, g_cam.shotTime).detach();
    }
    g_cam.readbackIn = -1;
}

// Handles the shutter: film counter, capture render, sounds. Call inside our context state.
static bool TryShutter(int shots, bool raised, const CBData& cb, ID3D11ShaderResourceView* eyeSrv,
                       ID3D11ShaderResourceView* focusSrv, double now)
{
    if (shots <= 0 || !raised || g_cam.raise <= 0.9f) return false;
    bool outOfFilm = g_cfg.filmPack > 0 && g_cam.shotsLeft <= 0;
    if (outOfFilm)
    {
        if (g_cfg.sound)
            PlaySoundW((g_installDir + L"\\empty.wav").c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return false;
    }
    if (g_cam.readbackIn >= 0) return false;
    if (g_cfg.photoSize != g_r.capSize && !g_r.CreateCapture(g_cfg.photoSize)) return false;
    if (g_cfg.filmPack > 0) g_cam.shotsLeft--;
    CBData c = cb;
    c.p0[3] = 1;
    c.p3[0] = 1;
    c.p3[2] = 1;                    // high-quality sampling (bicubic + supersampling)
    c.p3[3] = (float)g_r.capSize;
    g_r.Draw(g_r.capRtv, g_r.capSize, g_r.capSize, c, eyeSrv, focusSrv, nullptr);
    g_r.ctx->CopyResource(g_r.staging, g_r.capTex);
    g_cam.readbackIn = 2;
    GetLocalTime(&g_cam.shotTime);
    {
        std::lock_guard<std::mutex> lk(g_telMutex);
        g_cam.shotTel = g_tel;
        if (now - g_tel.receivedAt > 5.0) g_cam.shotTel.valid = false;
    }
    g_cam.flashUntil = now + 0.15;
    g_r.cardFresh = false;
    g_cam.printActive = true;
    g_cam.printStart = now;
    PlayShutter();
    Log("Shutter at %.0f mm (eye %s, focus %s)", g_cam.focal, eyeSrv ? "ok" : "none", focusSrv ? "ok" : "none");
    return true;
}

// Common per-frame input: hotkeys, raise transition, aim deltas. Returns shots requested.
struct FrameInput { int dx, dy, wheel, shots, rec; bool raised, justRaised; double now, dt; bool fg; };
static FrameInput PollInput()
{
    FrameInput fi{};
    fi.now = NowSeconds();
    fi.dt = g_cam.lastTime > 0 ? std::min(0.1, fi.now - g_cam.lastTime) : 0.0;
    g_cam.lastTime = fi.now;
    fi.fg = IsOurProcessForeground();
    {
        // settings changed (tray app or editor)? reload them
        static double lastCheck = 0;
        static FILETIME lastWrite{};
        if (fi.now - lastCheck > 2.0)
        {
            lastCheck = fi.now;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW((g_dataDir + L"\\InstantCam.ini").c_str(), GetFileExInfoStandard, &fa))
            {
                if ((lastWrite.dwLowDateTime || lastWrite.dwHighDateTime) && CompareFileTime(&fa.ftLastWriteTime, &lastWrite) != 0)
                {
                    LoadConfig();
                    if (g_cfg.filmPack == 0) g_cam.shotsLeft = 0;
                    else if (g_cam.shotsLeft > g_cfg.filmPack || g_cam.shotsLeft <= 0) g_cam.shotsLeft = g_cfg.filmPack;
                    Log("Settings reloaded");
                }
                lastWrite = fa.ftLastWriteTime;
            }
        }
    }
    if (g_cam.kRaise.Pressed(g_cfg.keyRaise, fi.fg)) g_in.raised = !g_in.raised;
    if (g_cam.kShutter.Pressed(g_cfg.keyShutter, fi.fg) && g_in.raised) g_in.shutter++;
    if (g_cam.kRecenter.Pressed(g_cfg.keyRecenter, fi.fg) && g_in.raised) g_in.recenter++;
    fi.raised = g_in.raised;
    fi.justRaised = fi.raised && !g_cam.wasRaised;
    g_cam.wasRaised = fi.raised;
    if (fi.justRaised)
    {
        if (g_cfg.filmPack > 0 && g_cam.shotsLeft <= 0)
        {
            g_cam.shotsLeft = g_cfg.filmPack;
            Log("New film pack loaded");
        }
        g_in.dx = 0; g_in.dy = 0; g_in.wheel = 0; g_in.shutter = 0;
    }
    fi.dx = g_in.dx.exchange(0); fi.dy = g_in.dy.exchange(0); fi.wheel = g_in.wheel.exchange(0);
    fi.shots = g_in.shutter.exchange(0); fi.rec = g_in.recenter.exchange(0);
    if (fi.raised)
    {
        if (fi.wheel) g_cam.focal *= powf(1.12f, fi.wheel / 120.f);
        if (fi.fg && g_cfg.keyZoomIn && (GetAsyncKeyState(g_cfg.keyZoomIn) & 0x8000)) g_cam.focal *= powf(1.8f, (float)fi.dt);
        if (fi.fg && g_cfg.keyZoomOut && (GetAsyncKeyState(g_cfg.keyZoomOut) & 0x8000)) g_cam.focal /= powf(1.8f, (float)fi.dt);
        g_cam.focal = std::max(g_cfg.minFocal, std::min(g_cfg.maxFocal, g_cam.focal));
    }
    float target = fi.raised ? 1.f : 0.f;
    float step = (float)fi.dt / 0.22f;
    g_cam.raise = g_cam.raise < target ? std::min(target, g_cam.raise + step) : std::max(target, g_cam.raise - step);
    return fi;
}

static void FillCommon(CBData& cb, double now)
{
    cb.p0[1] = (float)fmod(now, 1000.0);
    cb.p2[0] = g_cam.focal;
    cb.p2[1] = (logf(g_cam.focal) - logf(g_cfg.minFocal)) / std::max(1e-3f, logf(g_cfg.maxFocal) - logf(g_cfg.minFocal));
    cb.p2[2] = (g_cam.readbackIn >= 0 || (g_cam.printActive && now - g_cam.printStart < 5.0)) ? 1.f : 0.f;
    cb.p2[3] = g_cfg.filmPack > 0 ? (float)g_cam.shotsLeft : -1.f;
    cb.p3[1] = 1.f;
    cb.p4[0] = g_r.bodySrv ? 1.f : 0.f;
}

// Returns quad layers to append (pointers into static storage).
static void UpdateCamera(const XrFrameEndInfo* info, std::vector<const XrCompositionLayerBaseHeader*>& extra)
{
    static XrCompositionLayerQuad camLayer, printLayer;

    const XrCompositionLayerProjection* proj = nullptr;
    for (uint32_t i = 0; i < info->layerCount; ++i)
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
        {
            proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
            break;
        }

    FrameInput fi = PollInput();
    double now = fi.now;
    ProcessReadback();

    if (!proj || proj->viewCount < 1 || !g_r.ok)
    {
        g_in.dx = 0; g_in.dy = 0; g_in.wheel = 0; g_in.shutter = 0; g_in.recenter = 0;
        return;
    }

    const XrCompositionLayerProjectionView& v0 = proj->views[0];
    V3 headPos = ToV3(v0.pose.position);
    if (proj->viewCount >= 2) headPos = (ToV3(proj->views[0].pose.position) + ToV3(proj->views[1].pose.position)) * 0.5f;
    XrQuaternionf headQ = v0.pose.orientation;

    bool raised = fi.raised;
    if (fi.justRaised || (raised && fi.rec))
    {
        V3 f = QRot(headQ, {0, 0, -1});
        g_cam.yaw = atan2f(-f.x, -f.z);
        g_cam.pitch = asinf(std::max(-1.f, std::min(1.f, f.y)));
    }
    const float deg = 3.14159265f / 180.f;
    if (raised && !fi.justRaised)
    {
        g_cam.yaw -= fi.dx * g_cfg.sensitivity * deg;
        g_cam.pitch += (g_cfg.invertY ? fi.dy : -fi.dy) * g_cfg.sensitivity * deg;
        g_cam.pitch = std::max(-80.f * deg, std::min(80.f * deg, g_cam.pitch));
    }

    // camera pose
    float e = g_cam.raise * g_cam.raise * (3 - 2 * g_cam.raise);
    XrQuaternionf camQ = YawPitch(g_cam.yaw, g_cam.pitch);
    V3 offset = {0, -g_cfg.drop - (1 - e) * 0.25f, -g_cfg.distance};
    V3 camPos = headPos + QRot(camQ, offset);
    float camW = g_cfg.cameraWidth, camH = camW * 704.f / 1024.f;

    CBData cb{};
    ID3D11ShaderResourceView* eyeSrv = nullptr;
    ID3D11ShaderResourceView* focusSrv = nullptr;
    FillEye(proj->views[0], camQ, false, cb, eyeSrv);
    if (proj->viewCount >= 4) FillEye(proj->views[2], camQ, true, cb, focusSrv);  // quad views: focus (left)
    cb.p0[0] = 18.f / g_cam.focal;  // tan(half fov) of square 36mm frame
    FillCommon(cb, now);

    ID3DDeviceContextState* prev = nullptr;
    g_r.ctx1->SwapDeviceContextState(g_r.state, &prev);

    // shutter
    if (TryShutter(fi.shots, raised, cb, eyeSrv, focusSrv, now))
        g_cam.printBase = XrPosef{camQ, ToXr(camPos)};
    cb.p0[2] = g_cam.flashUntil > now ? (float)((g_cam.flashUntil - now) / 0.15) : 0.f;

    bool showCam = g_cam.raise > 0.001f;
    if (showCam && g_r.CreateQuad(g_session, g_r.camQ, 1536, 1056))
    {
        CBData c = cb;
        c.p0[3] = 0;
        if (g_r.DrawQuad(g_r.camQ, c, eyeSrv, focusSrv, nullptr))
        {
            camLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            camLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            camLayer.space = proj->space;
            camLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            camLayer.subImage.swapchain = g_r.camQ.sc;
            camLayer.subImage.imageRect = {{0, 0}, {(int32_t)g_r.camQ.w, (int32_t)g_r.camQ.h}};
            camLayer.pose = XrPosef{camQ, ToXr(camPos)};
            camLayer.size = {camW, camH};
            extra.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&camLayer));
        }
    }

    if (g_cam.printActive)
    {
        double t = now - g_cam.printStart;
        double total = g_cfg.printSeconds;
        if (t > total)
        {
            g_cam.printActive = false;
        }
        else if (g_r.CreateQuad(g_session, g_r.printQ, 512, 622))
        {
            float slide = (float)std::min(1.0, t / 0.8);
            slide = 1 - (1 - slide) * (1 - slide);
            float pw = 0.088f, ph = 0.107f;
            const XrQuaternionf& pq = g_cam.printBase.orientation;
            V3 base = ToV3(g_cam.printBase.position);
            V3 up = QRot(pq, {0, 1, 0});
            V3 back = QRot(pq, {0, 0, 1});
            V3 pos = base + up * ((camH * 0.5f + ph * 0.5f + 0.01f) * slide) + back * 0.01f;
            CBData c = cb;
            c.p0[3] = 2;
            c.p1[2] = (float)std::max(0.0, std::min(1.0, (t - 0.8) / 4.5));
            c.p3[1] = (float)std::min(1.0, std::max(0.0, (total - t) / 0.5));
            g_r.UpdateCard();
            c.p4[1] = g_r.cardFresh ? 1.f : 0.f;
            if (g_r.DrawQuad(g_r.printQ, c, nullptr, nullptr, g_r.cardFresh ? g_r.cardSrv : g_r.capSrv))
            {
                printLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                printLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                printLayer.space = proj->space;
                printLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                printLayer.subImage.swapchain = g_r.printQ.sc;
                printLayer.subImage.imageRect = {{0, 0}, {(int32_t)g_r.printQ.w, (int32_t)g_r.printQ.h}};
                printLayer.pose = XrPosef{pq, ToXr(pos)};
                printLayer.size = {pw, ph};
                extra.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&printLayer));
            }
        }
    }

    g_r.ctx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
}

// ---------------------------------------------------------------------------
// Flat-screen (2D) mode: hook IDXGISwapChain::Present inside DCS
// ---------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present g_origPresent = nullptr;
static PFN_Present1 g_origPresent1 = nullptr;
static std::atomic<bool> g_2dInstalled{false};
static float g_aimX = 0, g_aimY = 0;  // 2D aim point in screen tangent units (screen height = 2)

static bool EnsureSceneCopy(ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd)
{
    if (g_r.sceneTex && g_r.sceneW == bd.Width && g_r.sceneH == bd.Height && g_r.sceneFmt == bd.Format)
        return g_r.sceneSrv != nullptr;
    if (g_r.sceneSrv) { g_r.sceneSrv->Release(); g_r.sceneSrv = nullptr; }
    if (g_r.sceneTex) { g_r.sceneTex->Release(); g_r.sceneTex = nullptr; }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = bd.Width;
    td.Height = bd.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = bd.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_r.dev->CreateTexture2D(&td, nullptr, &g_r.sceneTex))) { Log("2D: scene copy texture failed"); return false; }
    DXGI_FORMAT f = TypedFromTypeless(bd.Format);
    if (bd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (bd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = 1;
    if (FAILED(g_r.dev->CreateShaderResourceView(g_r.sceneTex, &sd, &g_r.sceneSrv))) { Log("2D: scene SRV failed"); return false; }
    g_r.sceneW = bd.Width;
    g_r.sceneH = bd.Height;
    g_r.sceneFmt = bd.Format;
    g_r.sceneLinear = FormatIsLinearSample(f);
    Log("2D: screen %ux%u format %d", bd.Width, bd.Height, (int)bd.Format);
    return true;
}

static void Draw2D(IDXGISwapChain* sc)
{
    if (g_session || g_r.failed) return;  // VR session owns the camera
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    if (!g_r.ok)
    {
        if (!g_r.Init(dev))
        {
            Log("2D renderer init failed");
            g_r.Shutdown();
            g_r.failed = true;
            dev->Release();
            return;
        }
    }
    bool sameDev = (g_r.dev == dev);
    dev->Release();
    if (!sameDev) return;

    FrameInput fi = PollInput();
    double now = fi.now;
    ProcessReadback();

    bool visible = g_cam.raise > 0.001f || g_cam.printActive;
    if (!visible && fi.shots == 0) return;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    if (!EnsureSceneCopy(bb, bd)) { bb->Release(); return; }

    const float W = (float)bd.Width, H = (float)bd.Height, asp = W / H;
    // zoom -> half height of the capture frame, in tangent units (full screen height = 2)
    float th = std::min(1.f, g_cfg.minFocal / g_cam.focal);
    if (fi.justRaised || (fi.raised && fi.rec)) { g_aimX = 0; g_aimY = 0; }
    if (fi.raised && !fi.justRaised)
    {
        float k = 2.f / H * (g_cfg.sensitivity / 0.05f);
        g_aimX += fi.dx * k;
        g_aimY -= (g_cfg.invertY ? -fi.dy : fi.dy) * k;
    }
    g_aimX = std::max(-asp + th, std::min(asp - th, g_aimX));
    g_aimY = std::max(-1.f + th, std::min(1.f - th, g_aimY));

    // copy the finished frame (before our overlay) for the live screen and the photo
    DXGI_FORMAT rf = bd.Format;
    if (rf == DXGI_FORMAT_R8G8B8A8_TYPELESS) rf = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (rf == DXGI_FORMAT_B8G8R8A8_TYPELESS) rf = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (rf == DXGI_FORMAT_R16G16B16A16_TYPELESS) rf = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (rf == DXGI_FORMAT_R10G10B10A2_TYPELESS) rf = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (bd.SampleDesc.Count > 1)
        g_r.ctx->ResolveSubresource(g_r.sceneTex, 0, bb, 0, rf);
    else
        g_r.ctx->CopyResource(g_r.sceneTex, bb);

    CBData cb{};
    SetVec(cb.cRight, {1, 0, 0});
    SetVec(cb.cUp, {0, 1, 0});
    SetVec(cb.cFwd, {g_aimX, g_aimY, -1});
    cb.eyeTan[0] = -asp; cb.eyeTan[1] = asp; cb.eyeTan[2] = 1; cb.eyeTan[3] = -1;
    cb.eyeRect[0] = 0; cb.eyeRect[1] = 0; cb.eyeRect[2] = 1; cb.eyeRect[3] = 1;
    cb.p1[0] = 0;
    cb.p1[1] = g_r.sceneLinear ? 1.f : 0.f;
    cb.p1[3] = 1;
    cb.p0[0] = th;
    FillCommon(cb, now);
    cb.p4[2] = W;
    cb.p4[3] = H;
    cb.p5[0] = ((g_aimX - th) + asp) / (2 * asp);
    cb.p5[1] = (1 - (g_aimY + th)) / 2;
    cb.p5[2] = ((g_aimX + th) + asp) / (2 * asp);
    cb.p5[3] = (1 - (g_aimY - th)) / 2;

    ID3DDeviceContextState* prev = nullptr;
    g_r.ctx1->SwapDeviceContextState(g_r.state, &prev);

    TryShutter(fi.shots, fi.raised, cb, g_r.sceneSrv, nullptr, now);
    cb.p0[2] = g_cam.flashUntil > now ? (float)((g_cam.flashUntil - now) / 0.15) : 0.f;

    // render target on the back buffer
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = rf;
    rv.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_r.dev->CreateRenderTargetView(bb, &rv, &rtv)) && rtv)
    {
        bool outLinear = FormatIsLinearSample(rf);
        float e = g_cam.raise * g_cam.raise * (3 - 2 * g_cam.raise);
        float camH = H * 0.40f, camW = camH * 1024.f / 704.f;
        float camX = W - camW - H * 0.04f;
        float camY = H - camH - H * 0.03f + (1 - e) * camH * 1.15f;
        static float camCx = -1, camCy = -1;
        if (g_cfg.flatFollow)
        {
            // hold the camera up in front of the target: its screen sits on the aim point
            float tx = (g_aimX + asp) / (2 * asp) * W, ty = (1 - g_aimY) / 2 * H;
            if (fi.justRaised || camCx < 0) { camCx = tx; camCy = ty; }
            float k = std::min(1.f, (float)fi.dt * 14.f);
            camCx += (tx - camCx) * k;
            camCy += (ty - camCy) * k;
            camX = camCx - camW * (300.f / 1024.f);
            camY = camCy - camH * (412.f / 704.f) + (1 - e) * H * 0.7f;
        }

        // viewfinder frame on the scene (corner mode only; in follow mode the camera screen is the viewfinder)
        if (g_cam.raise > 0.001f && !g_cfg.flatFollow)
        {
            CBData c = cb;
            c.p0[3] = 4;
            c.p3[0] = outLinear ? 1.f : 0.f;
            c.p3[1] = e;
            g_r.DrawRect(rtv, 0, 0, W, H, c, nullptr, nullptr, nullptr, true);
        }
        // developing print, emerging from the slot on top of the camera
        if (g_cam.printActive)
        {
            double t = now - g_cam.printStart;
            double total = g_cfg.printSeconds;
            if (t > total) g_cam.printActive = false;
            else
            {
                float slide = (float)std::min(1.0, t / 0.8);
                slide = 1 - (1 - slide) * (1 - slide);
                float ph = camH * 0.95f, pw = ph * 88.f / 107.f;
                float px = camX + camW * 0.5f - pw * 0.5f;
                float py = camY + camH * 0.06f - slide * (ph + camH * 0.02f);
                CBData c = cb;
                c.p0[3] = 2;
                c.p1[2] = (float)std::max(0.0, std::min(1.0, (t - 0.8) / 4.5));
                c.p3[0] = outLinear ? 1.f : 0.f;
                c.p3[1] = (float)std::min(1.0, std::max(0.0, (total - t) / 0.5));
                g_r.UpdateCard();
                c.p4[1] = g_r.cardFresh ? 1.f : 0.f;
                g_r.DrawRect(rtv, px, py, pw, ph, c, nullptr, nullptr, g_r.cardFresh ? g_r.cardSrv : g_r.capSrv, true);
            }
        }
        // the camera
        if (g_cam.raise > 0.001f)
        {
            CBData c = cb;
            c.p0[3] = 0;
            c.p3[0] = outLinear ? 1.f : 0.f;
            g_r.DrawRect(rtv, camX, camY, camW, camH, c, g_r.sceneSrv, nullptr, nullptr, true);
        }
        rtv->Release();
    }

    g_r.ctx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    bb->Release();
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) Draw2D(sc);
    return g_origPresent(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    if (!(flags & DXGI_PRESENT_TEST)) Draw2D(sc);
    return g_origPresent1(sc, sync, flags, params);
}

static bool PatchVtable(void** vt, int index, void* hook, void** orig)
{
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (vt[index] != hook)
    {
        *orig = vt[index];
        vt[index] = hook;
    }
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return true;
}

static void Install2D()
{
    if (g_2dInstalled.exchange(true)) return;
    HWND wnd = CreateWindowExW(0, L"STATIC", L"InstantCamDummy", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!wnd) { Log("2D: dummy window failed"); return; }
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 64;
    scd.BufferDesc.Height = 64;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = wnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    if (SUCCEEDED(hr) && sc)
    {
        void** vt = *reinterpret_cast<void***>(sc);
        bool a = PatchVtable(vt, 8, (void*)&HookPresent, (void**)&g_origPresent);
        IDXGISwapChain1* sc1 = nullptr;
        bool b = false;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1)
        {
            void** vt1 = *reinterpret_cast<void***>(sc1);
            b = PatchVtable(vt1, 22, (void*)&HookPresent1, (void**)&g_origPresent1);
            sc1->Release();
        }
        Log("2D: Present hook %s, Present1 hook %s", a ? "ok" : "FAILED", b ? "ok" : "FAILED");
    }
    else
    {
        Log("2D: could not create dummy swap chain (0x%08lx)", (unsigned long)hr);
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(wnd);
}

// Loaded by Saved Games\DCS\Scripts\Hooks\InstantCamHook.lua via require("instantcam").
// Same DLL file as the OpenXR layer, so in VR both paths share one module and the VR path wins.
extern "C" __declspec(dllexport) int luaopen_instantcam(void* /*L*/)
{
    static bool done = false;
    if (done) return 0;
    done = true;
    if (g_dataDir.empty()) InitPaths();
    Log("---- DCS Instant Cam " IC_VERSION " (flat-screen hook) ----");
    LoadConfig();
    if (g_cam.focal <= 0 || g_session == XR_NULL_HANDLE)
    {
        g_cam.focal = g_cfg.startFocal;
        g_cam.shotsLeft = g_cfg.filmPack;
    }
    StartInput();
    StartTelemetry();
    Install2D();
    return 0;
}

// ---------------------------------------------------------------------------
// Hooked functions
// ---------------------------------------------------------------------------

static XrResult XRAPI_CALL L_CreateSession(XrInstance inst, const XrSessionCreateInfo* info, XrSession* session)
{
    XrResult r = g_next.CreateSession(inst, info, session);
    if (XR_FAILED(r)) return r;
    const XrBaseInStructure* p = reinterpret_cast<const XrBaseInStructure*>(info->next);
    while (p)
    {
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p);
            g_session = *session;
            g_device = b->device;
            g_device->AddRef();
            Log("D3D11 session created");
            g_cam = CameraState{};
            g_cam.focal = g_cfg.startFocal;
            g_cam.shotsLeft = g_cfg.filmPack;
            g_in.raised = false;
            StartInput();
            StartTelemetry();
            break;
        }
        p = p->next;
    }
    if (!g_device) Log("Session is not D3D11 - Instant Cam stays inactive");
    return r;
}

static XrResult XRAPI_CALL L_DestroySession(XrSession session)
{
    if (session == g_session)
    {
        g_r.Shutdown();
        {
            std::lock_guard<std::mutex> lk(g_swapMutex);
            for (auto& kv : g_swaps)
                for (auto& s : kv.second.srvs) if (s.second) s.second->Release();
            g_swaps.clear();
        }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_session = XR_NULL_HANDLE;
        StopInput();
        StopTelemetry();
        Log("Session destroyed");
    }
    return g_next.DestroySession(session);
}

static XrResult XRAPI_CALL L_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* sc)
{
    if (session != g_session || !g_device) return g_next.CreateSwapchain(session, info, sc);
    XrSwapchainCreateInfo ci = *info;
    bool color = (ci.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    if (color) ci.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrResult r = g_next.CreateSwapchain(session, &ci, sc);
    if (XR_FAILED(r) && color && !(info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT))
    {
        Log("Swapchain create with SAMPLED failed (%d), retrying without", (int)r);
        r = g_next.CreateSwapchain(session, info, sc);
    }
    if (XR_SUCCEEDED(r))
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        SwapInfo& si = g_swaps[*sc];
        si.format = info->format;
        si.width = info->width;
        si.height = info->height;
        si.arraySize = info->arraySize;
        si.samples = info->sampleCount;
        si.color = color;
    }
    return r;
}

static XrResult XRAPI_CALL L_DestroySwapchain(XrSwapchain sc)
{
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        auto it = g_swaps.find(sc);
        if (it != g_swaps.end())
        {
            for (auto& s : it->second.srvs) if (s.second) s.second->Release();
            g_swaps.erase(it);
        }
    }
    return g_next.DestroySwapchain(sc);
}

static XrResult XRAPI_CALL L_EnumerateSwapchainImages(XrSwapchain sc, uint32_t cap, uint32_t* count,
                                                     XrSwapchainImageBaseHeader* images)
{
    XrResult r = g_next.EnumerateSwapchainImages(sc, cap, count, images);
    if (XR_SUCCEEDED(r) && images && cap > 0 && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        auto it = g_swaps.find(sc);
        if (it != g_swaps.end())
        {
            auto* d3d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            it->second.images.clear();
            for (uint32_t i = 0; i < *count; ++i) it->second.images.push_back(d3d[i].texture);
        }
    }
    return r;
}

static XrResult XRAPI_CALL L_AcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo* ai, uint32_t* idx)
{
    XrResult r = g_next.AcquireSwapchainImage(sc, ai, idx);
    if (XR_SUCCEEDED(r))
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        auto it = g_swaps.find(sc);
        if (it != g_swaps.end()) it->second.acquired.push_back(*idx);
    }
    return r;
}

static XrResult XRAPI_CALL L_ReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo* ri)
{
    XrResult r = g_next.ReleaseSwapchainImage(sc, ri);
    if (XR_SUCCEEDED(r))
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        auto it = g_swaps.find(sc);
        if (it != g_swaps.end() && !it->second.acquired.empty())
        {
            it->second.lastReleased = (int)it->second.acquired.front();
            it->second.acquired.pop_front();
        }
    }
    return r;
}

static XrResult XRAPI_CALL L_EndFrame(XrSession session, const XrFrameEndInfo* info)
{
    if (session != g_session || !g_device || !info || g_r.failed) return g_next.EndFrame(session, info);
    if (!g_r.ok)
    {
        if (!g_r.Init(g_device))
        {
            Log("Renderer init failed - Instant Cam disabled for this session");
            g_r.Shutdown();
            g_r.failed = true;
            return g_next.EndFrame(session, info);
        }
    }
    std::vector<const XrCompositionLayerBaseHeader*> extra;
    UpdateCamera(info, extra);
    if (extra.empty()) return g_next.EndFrame(session, info);
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    layers.insert(layers.end(), extra.begin(), extra.end());
    XrFrameEndInfo fi = *info;
    fi.layerCount = (uint32_t)layers.size();
    fi.layers = layers.data();
    return g_next.EndFrame(session, &fi);
}

static XrResult XRAPI_CALL L_DestroyInstance(XrInstance inst)
{
    XrResult r = g_next.DestroyInstance(inst);
    g_active = false;
    g_instance = XR_NULL_HANDLE;
    return r;
}

static XrResult XRAPI_CALL L_GetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn)
{
    if (!g_next.GetInstanceProcAddr) return XR_ERROR_HANDLE_INVALID;
    if (g_active && name)
    {
#define HOOK(n) if (strcmp(name, "xr" #n) == 0) { *fn = reinterpret_cast<PFN_xrVoidFunction>(L_##n); return XR_SUCCESS; }
        HOOK(DestroyInstance)
        HOOK(CreateSession)
        HOOK(DestroySession)
        HOOK(CreateSwapchain)
        HOOK(DestroySwapchain)
        HOOK(EnumerateSwapchainImages)
        HOOK(AcquireSwapchainImage)
        HOOK(ReleaseSwapchainImage)
        HOOK(EndFrame)
#undef HOOK
    }
    return g_next.GetInstanceProcAddr(inst, name, fn);
}

static bool IsDcsProcess()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe(buf);
    exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    for (auto& c : exe) c = towlower(c);
    return exe.rfind(L"dcs", 0) == 0 && exe.size() > 4 && exe.substr(exe.size() - 4) == L".exe";
}

static XrResult XRAPI_CALL L_CreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                                   XrInstance* instance)
{
    if (!layerInfo || !layerInfo->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    g_next.GetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (XR_FAILED(r)) return r;

    g_instance = *instance;
    bool d3d11 = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (strcmp(info->enabledExtensionNames[i], XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) d3d11 = true;
    g_active = IsDcsProcess() && d3d11;
    if (IsDcsProcess())
    {
        InitPaths();
        Log("---- DCS Instant Cam " IC_VERSION " ----");
        Log("App: %s, D3D11: %s", info->applicationInfo.applicationName, d3d11 ? "yes" : "no");
    }
    if (!g_active) return r;

    LoadConfig();
#define GET(n) g_next.GetInstanceProcAddr(*instance, "xr" #n, reinterpret_cast<PFN_xrVoidFunction*>(&g_next.n))
    GET(DestroyInstance);
    GET(CreateSession);
    GET(DestroySession);
    GET(EnumerateSwapchainFormats);
    GET(CreateSwapchain);
    GET(DestroySwapchain);
    GET(EnumerateSwapchainImages);
    GET(AcquireSwapchainImage);
    GET(WaitSwapchainImage);
    GET(ReleaseSwapchainImage);
    GET(EndFrame);
#undef GET
    Log("Layer active");
    return r;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* layerName, XrNegotiateApiLayerRequest* req)
{
    if (!loaderInfo || !req || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
    req->getInstanceProcAddr = L_GetInstanceProcAddr;
    req->createApiLayerInstance = L_CreateApiLayerInstance;
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
