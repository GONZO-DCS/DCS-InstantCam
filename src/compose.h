// DCS Instant Cam - print card composer, shared by the camera DLL and the tray app.
// Paper + photo + handwritten caption, PNG save/load with the caption stored inside the PNG.
#pragma once

#include <windows.h>
#include <wincodec.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace card {

// Instant film proportions: 88 x 107 mm card, 79 x 79 mm image at (4.5, 6) mm
inline int CardW(int size) { return (int)lround(size * 88.0 / 79.0); }
inline int CardH(int size) { return (int)lround(size * 107.0 / 79.0); }
inline int ImgX(int size) { return (int)lround(size * 4.5 / 79.0); }
inline int ImgY(int size) { return (int)lround(size * 6.0 / 79.0); }
inline int SizeFromCardW(int w) { return (int)lround(w * 79.0 / 88.0); }

inline std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// DCS type name -> name a pilot would write
inline std::wstring PrettyAircraft(const std::string& raw)
{
    static const struct { const char* id; const wchar_t* name; } table[] = {
        {"FA-18C_hornet", L"F/A-18C Hornet"}, {"F-14B", L"F-14B Tomcat"}, {"F-14A-135-GR", L"F-14A Tomcat"},
        {"F-16C_50", L"F-16C Viper"}, {"F-15ESE", L"F-15E Strike Eagle"}, {"F-15C", L"F-15C Eagle"},
        {"A-10C_2", L"A-10C II"}, {"A-10C", L"A-10C"}, {"AV8BNA", L"AV-8B Harrier"}, {"M-2000C", L"Mirage 2000C"},
        {"F-4E-45MC", L"F-4E Phantom II"}, {"F-5E-3", L"F-5E Tiger II"}, {"AJS37", L"AJS 37 Viggen"},
        {"JF-17", L"JF-17 Thunder"}, {"MiG-29A", L"MiG-29A"}, {"MiG-21Bis", L"MiG-21bis"}, {"Su-27", L"Su-27"},
        {"Su-33", L"Su-33"}, {"AH-64D_BLK_II", L"AH-64D Apache"}, {"UH-1H", L"UH-1H Huey"}, {"Mi-24P", L"Mi-24P Hind"},
        {"Mi-8MT", L"Mi-8MTV2"}, {"Ka-50_3", L"Ka-50 III"}, {"SA342M", L"SA342 Gazelle"}, {"CH-47Fbl1", L"CH-47F Chinook"},
        {"OH58D", L"OH-58D Kiowa"}, {"P-51D-30-NA", L"P-51D Mustang"}, {"SpitfireLFMkIX", L"Spitfire LF Mk IX"},
        {"Bf-109K-4", L"Bf 109 K-4"}, {"FW-190D9", L"Fw 190 D-9"}, {"F-86F Sabre", L"F-86F Sabre"},
        {"MiG-15bis", L"MiG-15bis"}, {"L-39C", L"L-39C"}, {"C-101CC", L"C-101CC"}, {"Mirage-F1CE", L"Mirage F1CE"},
        {"MB-339A", L"MB-339A"}, {"Hercules", L"C-130 Hercules"}, {"FA-18E", L"F/A-18E Super Hornet"},
        {"FA-18F", L"F/A-18F Super Hornet"}, {"EA-18G", L"EA-18G Growler"},
    };
    for (auto& t : table)
        if (_stricmp(raw.c_str(), t.id) == 0) return t.name;
    std::wstring w = Widen(raw);
    for (auto& c : w) if (c == L'_') c = L' ';
    return w;
}

inline IWICImagingFactory* Wic()
{
    IWICImagingFactory* fac = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    return fac;
}

// Load any image as top-down BGRA. Optionally returns the caption stored in the PNG.
inline bool LoadPngBGRA(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h,
                        std::wstring* caption = nullptr)
{
    IWICImagingFactory* fac = Wic();
    if (!fac) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)))
    {
        px.resize((size_t)w * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
        if (ok && caption)
        {
            caption->clear();
            IWICMetadataQueryReader* rd = nullptr;
            if (SUCCEEDED(frame->GetMetadataQueryReader(&rd)) && rd)
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(rd->GetMetadataByName(L"/tEXt/{str=InstantCamCaption}", &v)))
                {
                    if (v.vt == VT_LPSTR && v.pszVal) *caption = Widen(v.pszVal);
                    else if (v.vt == VT_LPWSTR && v.pwszVal) *caption = v.pwszVal;
                }
                PropVariantClear(&v);
                rd->Release();
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    fac->Release();
    return ok;
}

// Save top-down BGRA as PNG, with the caption lines stored as text inside the file.
inline bool SavePng(const std::wstring& path, const uint8_t* bgra, int w, int h, const std::wstring& caption)
{
    IWICImagingFactory* fac = Wic();
    if (!fac) return false;
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    std::wstring tmp = path + L".tmp";
    do
    {
        if (FAILED(fac->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE))) break;
        if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        if (FAILED(frame->SetSize(w, h))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) break;
        if (!caption.empty())
        {
            IWICMetadataQueryWriter* wr = nullptr;
            if (SUCCEEDED(frame->GetMetadataQueryWriter(&wr)) && wr)
            {
                std::string utf8 = Narrow(caption);
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_LPSTR;
                v.pszVal = const_cast<char*>(utf8.c_str());
                wr->SetMetadataByName(L"/tEXt/{str=InstantCamCaption}", &v);
                wr->Release();
            }
        }
        if (FAILED(frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra)))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(enc->Commit())) break;
        ok = true;
    } while (false);
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    fac->Release();
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    else DeleteFileW(tmp.c_str());
    return ok;
}

// ---- paper + font, loaded once per process ----
struct Assets
{
    std::mutex m;
    bool loaded = false;
    std::vector<uint8_t> paper;  // BGRA
    UINT pw = 0, ph = 0;
    bool font = false;
};
inline Assets& GetAssets()
{
    static Assets a;
    return a;
}

inline void LoadAssets(const std::wstring& installDir)
{
    Assets& a = GetAssets();
    std::lock_guard<std::mutex> lk(a.m);
    if (a.loaded) return;
    a.loaded = true;
    LoadPngBGRA(installDir + L"\\paper.png", a.paper, a.pw, a.ph);
    a.font = AddFontResourceExW((installDir + L"\\ShadowsIntoLight.ttf").c_str(), FR_PRIVATE, nullptr) > 0;
}

// Bilinear sample of the paper art into a W x H card.
inline void FillPaper(uint8_t* px, int W, int H)
{
    Assets& a = GetAssets();
    if (a.paper.empty())
    {
        for (int i = 0; i < W * H; ++i) { px[i * 4] = 227; px[i * 4 + 1] = 234; px[i * 4 + 2] = 236; px[i * 4 + 3] = 255; }
        return;
    }
    for (int y = 0; y < H; ++y)
    {
        float fy = std::max(0.f, (y + 0.5f) * a.ph / H - 0.5f);
        int y0 = std::min((int)a.ph - 1, (int)fy), y1 = std::min((int)a.ph - 1, y0 + 1);
        float ty = fy - y0;
        for (int x = 0; x < W; ++x)
        {
            float fx = std::max(0.f, (x + 0.5f) * a.pw / W - 0.5f);
            int x0 = std::min((int)a.pw - 1, (int)fx), x1 = std::min((int)a.pw - 1, x0 + 1);
            float tx = fx - x0;
            const uint8_t* p00 = &a.paper[((size_t)y0 * a.pw + x0) * 4];
            const uint8_t* p10 = &a.paper[((size_t)y0 * a.pw + x1) * 4];
            const uint8_t* p01 = &a.paper[((size_t)y1 * a.pw + x0) * 4];
            const uint8_t* p11 = &a.paper[((size_t)y1 * a.pw + x1) * 4];
            uint8_t* d = px + ((size_t)y * W + x) * 4;
            for (int c = 0; c < 3; ++c)
            {
                float v = (p00[c] * (1 - tx) + p10[c] * tx) * (1 - ty) + (p01[c] * (1 - tx) + p11[c] * tx) * ty;
                d[c] = (uint8_t)std::min(255.f, v + 0.5f);
            }
            d[3] = 255;
        }
    }
}

// Compose a finished print. photo is size x size, RGBA (srcBGRA=false) or BGRA, top-down,
// with rowStride bytes per row. Lines are the handwritten caption (empty lines are skipped).
inline bool Compose(const uint8_t* photo, int size, size_t rowStride, bool srcBGRA, const std::vector<std::wstring>& lines,
                    const std::wstring& installDir, std::vector<uint8_t>& out, int& W, int& H)
{
    LoadAssets(installDir);
    W = CardW(size);
    H = CardH(size);
    const int ox = ImgX(size), oy = ImgY(size);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    uint8_t* px = (uint8_t*)bits;

    FillPaper(px, W, H);
    for (int y = 0; y < size; ++y)
    {
        const uint8_t* s = photo + (size_t)y * rowStride;
        uint8_t* d = px + ((size_t)(y + oy) * W + ox) * 4;
        for (int x = 0; x < size; ++x, s += 4, d += 4)
        {
            if (srcBGRA) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            else { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
            d[3] = 255;
        }
    }

    // handwriting: shrink to fit the widest line
    const wchar_t* face = GetAssets().font ? L"Shadows Into Light" : L"Segoe Print";
    int fh = (int)(size * 0.058);
    int x0 = ox + (int)(size * 0.035);
    int avail = W - x0 - ox;
    auto mkFont = [&](int h) {
        return CreateFontW(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
    };
    HFONT font = mkFont(fh);
    HGDIOBJ oldF = SelectObject(dc, font);
    int widest = 0;
    for (auto& l : lines)
    {
        if (l.empty()) continue;
        SIZE sz{};
        GetTextExtentPoint32W(dc, l.c_str(), (int)l.size(), &sz);
        widest = std::max(widest, (int)sz.cx);
    }
    int lineH = (int)(fh * 1.32);
    if (widest > avail)
    {
        int nh = std::max((int)(fh * 0.62), (int)(fh * (double)avail / widest));
        SelectObject(dc, oldF);
        DeleteObject(font);
        font = mkFont(nh);
        SelectObject(dc, font);
        lineH = (int)(lineH * std::max(0.8, (double)nh / fh));
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(34, 44, 128));
    int y = oy + size + (int)(size * 0.045);
    for (auto& l : lines)
    {
        if (l.empty()) continue;
        RECT r{x0, y, W - ox, y + lineH};
        DrawTextW(dc, l.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += lineH;
    }
    SelectObject(dc, oldF);
    DeleteObject(font);
    GdiFlush();
    for (int i = 0; i < W * H; ++i) px[i * 4 + 3] = 255;  // GDI clears alpha
    out.assign(px, px + (size_t)W * H * 4);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

inline std::wstring JoinLines(const std::vector<std::wstring>& lines)
{
    std::wstring s;
    for (size_t i = 0; i < lines.size(); ++i) s += (i ? L"\n" : L"") + lines[i];
    return s;
}

inline std::vector<std::wstring> SplitLines(const std::wstring& s)
{
    std::vector<std::wstring> r;
    std::wstring cur;
    for (wchar_t c : s)
    {
        if (c == L'\r') continue;
        if (c == L'\n') { r.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    r.push_back(cur);
    return r;
}

}  // namespace card
