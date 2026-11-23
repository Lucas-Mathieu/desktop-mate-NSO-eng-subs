// Click-through subtitle window drawn with GDI+ and UpdateLayeredWindow.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "overlay.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "subtitles.h"
#include "unity.h"

#pragma comment(lib, "gdiplus.lib")

void Log(const char* fmt, ...);
double DmNow();

namespace {

struct Settings {
    std::wstring font = L"Segoe UI";
    float size = 26.f;          // px
    int maxWidth = 760;         // px, before wrapping
    int bgAlpha = 150;          // 0 = outline only
    bool showJapanese = false;  // second, smaller line with the original
    bool showUnknown = false;   // show "[clip name]" for untranslated clips (for authoring)
    float linger = 0.35f;       // seconds after the clip ends
    float headUp = 0.0f;        // world units added above the speaker transform
    int gapPx = 12;             // between anchor and subtitle bottom
    bool follow = true;         // track the character; false = fixed bottom-centre
    float smoothing = 0.35f;    // seconds; how softly the subtitle follows (0 = rigid)
    float deadZone = 18.f;      // px of head movement ignored entirely
    int fixedBottomMargin = 120;
} S;

std::wstring g_dir;

// ---- state shared between Unity main thread and overlay thread ----
std::mutex g_mx;
struct Active {
    void* src = nullptr;
    int srcId = 0;
    std::string clip;
    std::shared_ptr<const Line> line;
    double start = 0, len = 0;
    bool on = false;
} g_act;
struct Anchor {
    bool valid = false;
    float x = 0, y = 0;  // screen px
} g_anchor;

// ---- settings ---------------------------------------------------------------
void LoadSettings() {
    std::wstring ini = g_dir + L"dmsubs.ini";
    wchar_t b[256];
    auto str = [&](const wchar_t* k, const std::wstring& d) {
        GetPrivateProfileStringW(L"display", k, d.c_str(), b, 256, ini.c_str());
        return std::wstring(b);
    };
    auto num = [&](const wchar_t* k, double d) {
        wchar_t def[32];
        swprintf_s(def, L"%g", d);
        return _wtof(str(k, def).c_str());
    };
    S.font = str(L"font", S.font);
    S.size = (float)num(L"font_size", S.size);
    S.maxWidth = (int)num(L"max_width", S.maxWidth);
    S.bgAlpha = std::clamp((int)num(L"background_alpha", S.bgAlpha), 0, 255);
    S.showJapanese = num(L"show_japanese", S.showJapanese) != 0;
    S.showUnknown = num(L"show_untranslated_clip_names", S.showUnknown) != 0;
    S.linger = (float)num(L"linger_seconds", S.linger);
    S.headUp = (float)num(L"head_offset_world", S.headUp);
    S.gapPx = (int)num(L"gap_px", S.gapPx);
    S.follow = num(L"follow_character", S.follow) != 0;
    S.smoothing = std::max(0.f, (float)num(L"follow_smoothing", S.smoothing));
    S.deadZone = std::max(0.f, (float)num(L"follow_dead_zone_px", S.deadZone));
    S.fixedBottomMargin = (int)num(L"fixed_bottom_margin", S.fixedBottomMargin);
}

// ---- rendering --------------------------------------------------------------
HWND g_wnd;
std::wstring g_shownText;
POINT g_shownPos{-99999, -99999};
SIZE g_shownSize{};

void Hide() {
    if (!g_shownText.empty()) {
        ShowWindow(g_wnd, SW_HIDE);
        g_shownText.clear();
    }
}

void Render(const std::wstring& en, const std::wstring& jp, float ax, float ay, bool anchored) {
    using namespace Gdiplus;
    std::wstring key = en + L"\x1f" + jp;
    const int pad = 14;
    FontFamily fam(S.font.c_str());
    FontFamily* family = fam.IsAvailable() ? &fam : FontFamily::GenericSansSerif()->Clone();
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentNear);

    GraphicsPath pEn, pJp;
    RectF layout(0, 0, (REAL)S.maxWidth, 4000);
    pEn.AddString(en.c_str(), -1, family, FontStyleBold, S.size, layout, &fmt);
    RectF bEn;
    pEn.GetBounds(&bEn);
    RectF bJp(0, 0, 0, 0);
    if (!jp.empty()) {
        RectF l2(0, bEn.GetBottom() + S.size * 0.25f, (REAL)S.maxWidth, 4000);
        pJp.AddString(jp.c_str(), -1, family, FontStyleRegular, S.size * 0.62f, l2, &fmt);
        pJp.GetBounds(&bJp);
    }
    RectF all = bEn;
    if (!jp.empty()) RectF::Union(all, bEn, bJp);
    int w = (int)std::ceil(all.Width) + pad * 2, h = (int)std::ceil(all.Height) + pad * 2;
    if (w <= pad * 2 || h <= pad * 2) { Hide(); return; }

    // window position: centred above the anchor, clamped to the anchor's monitor
    POINT want;
    POINT ref{(LONG)ax, (LONG)ay};
    HMONITOR mon = MonitorFromPoint(ref, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    if (anchored) {
        want = {(LONG)(ax - w / 2.f), (LONG)(ay - S.gapPx - h)};
    } else {
        want = {(wa.left + wa.right) / 2 - w / 2, wa.bottom - S.fixedBottomMargin - h};
    }
    want.x = std::clamp(want.x, wa.left, std::max(wa.left, wa.right - w));
    want.y = std::clamp(want.y, wa.top, std::max(wa.top, wa.bottom - h));

    if (key == g_shownText && g_shownSize.cx == w && g_shownSize.cy == h) {
        if (want.x != g_shownPos.x || want.y != g_shownPos.y) {
            SetWindowPos(g_wnd, HWND_TOPMOST, want.x, want.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            g_shownPos = want;
        }
        return;
    }

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        Matrix m;
        m.Translate(pad - all.X, pad - all.Y);
        pEn.Transform(&m);
        pJp.Transform(&m);
        if (S.bgAlpha > 0) {
            GraphicsPath box;
            float r = std::min(12.f, h / 2.f), bw = (float)w - 1, bh = (float)h - 1;
            box.AddArc(0.f, 0.f, r * 2, r * 2, 180, 90);
            box.AddArc(bw - r * 2, 0.f, r * 2, r * 2, 270, 90);
            box.AddArc(bw - r * 2, bh - r * 2, r * 2, r * 2, 0, 90);
            box.AddArc(0.f, bh - r * 2, r * 2, r * 2, 90, 90);
            box.CloseFigure();
            SolidBrush bg(Color((BYTE)S.bgAlpha, 12, 12, 18));
            g.FillPath(&bg, &box);
        }
        Pen outline(Color(230, 0, 0, 0), std::max(2.f, S.size * 0.16f));
        outline.SetLineJoin(LineJoinRound);
        SolidBrush white(Color(255, 255, 255, 255));
        SolidBrush grey(Color(255, 215, 215, 225));
        g.DrawPath(&outline, &pEn);
        g.FillPath(&white, &pEn);
        if (!jp.empty()) {
            Pen o2(Color(200, 0, 0, 0), std::max(1.5f, S.size * 0.1f));
            o2.SetLineJoin(LineJoinRound);
            g.DrawPath(&o2, &pJp);
            g.FillPath(&grey, &pJp);
        }
    }
    POINT src{0, 0};
    SIZE sz{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(g_wnd, screen, &want, &sz, mem, &src, 0, &bf, ULW_ALPHA))
        Log("UpdateLayeredWindow failed (%lu)", GetLastError());
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (family != &fam) delete family;
    g_shownText = key;
    g_shownPos = want;
    g_shownSize = sz;
}

void Tick() {
    Active a;
    Anchor an;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        a = g_act;
        an = g_anchor;
    }
    if (!a.on) { Hide(); return; }
    double el = DmNow() - a.start;
    if (el < 0) { Hide(); return; }
    if (el > a.len + S.linger) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_act.on && g_act.start == a.start) g_act.on = false;
        Hide();
        return;
    }
    std::wstring en, jp;
    if (a.line) {
        if (!a.line->cues.empty()) {
            for (auto& c : a.line->cues)
                if (c.t <= el) en = c.text;
        } else {
            en = a.line->en;
        }
        if (S.showJapanese) jp = a.line->jp;
    } else if (S.showUnknown) {
        en = L"[" + std::wstring(a.clip.begin(), a.clip.end()) + L"]";
    }
    if (en.empty() && jp.empty()) { Hide(); return; }

    // Smooth the anchor: ignore small sway (dead zone), ease towards bigger moves.
    static double smoothedFor = -1, lastT = 0;
    static float goalX, goalY, sx, sy;
    double now = DmNow();
    if (an.valid) {
        if (smoothedFor != a.start) {  // new line: start exactly on the head
            smoothedFor = a.start;
            goalX = sx = an.x;
            goalY = sy = an.y;
        } else {
            if (std::fabs(an.x - goalX) > S.deadZone || std::fabs(an.y - goalY) > S.deadZone) {
                // follow, but stay dead-zone distance behind so we don't re-trigger on the way back
                float dx = an.x - goalX, dy = an.y - goalY, d = std::sqrt(dx * dx + dy * dy);
                float keep = d > 0 ? (d - S.deadZone * 0.5f) / d : 0.f;
                goalX += dx * keep;
                goalY += dy * keep;
            }
            float k = S.smoothing > 0 ? (float)(1.0 - std::exp(-(now - lastT) / S.smoothing)) : 1.f;
            sx += (goalX - sx) * k;
            sy += (goalY - sy) * k;
        }
    }
    lastT = now;
    Render(en, jp, sx, sy, S.follow && an.valid);
}

std::atomic<bool> g_recreate{false};

HWND g_unityWnd;

BOOL CALLBACK FindUnity(HWND h, LPARAM) {
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClassNameW(h, cls, 64) &&
        wcscmp(cls, L"UnityWndClass") == 0) {
        g_unityWnd = h;
        return FALSE;
    }
    return TRUE;
}

// The game's window plugin (LibUniWinC) attaches to the first window of the
// process, or to that window's owner. Owning our window by Unity's window makes
// it resolve to the game window even if it enumerates ours first.
HWND MakeWindow() {
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           L"dmsubs_overlay", L"dmsubs", WS_POPUP, 0, 0, 1, 1, g_unityWnd, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI OverlayThread(void*) {
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gsi, nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dmsubs_overlay";
    RegisterClassW(&wc);
    // Wait for Unity's window and for the game to finish styling it (transparent = layered).
    double t0 = DmNow();
    while (!g_unityWnd || (!(GetWindowLongW(g_unityWnd, GWL_EXSTYLE) & WS_EX_LAYERED) && DmNow() - t0 < 20)) {
        if (!g_unityWnd) EnumWindows(FindUnity, 0);
        Sleep(200);
    }
    Sleep(500);
    g_wnd = MakeWindow();
    Log("overlay window created (owner %p)", g_unityWnd);
    double lastPoll = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_recreate.exchange(false)) {
            g_unityWnd = nullptr;
            EnumWindows(FindUnity, 0);
            HWND old = g_wnd;
            g_wnd = MakeWindow();
            DestroyWindow(old);
            g_shownText.clear();
            Log("overlay window recreated");
        }
        double now = DmNow();
        if (now - lastPoll > 1.0) {
            lastPoll = now;
            Subtitles_Poll();
        }
        Tick();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
}

}  // namespace

HWND Overlay_Window() { return g_wnd; }
void Overlay_RecreateWindow() { g_recreate = true; }

void Overlay_Start(const std::wstring& dir) {
    g_dir = dir;
    LoadSettings();
    CloseHandle(CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr));
}

void Overlay_OnClipStart(void* src, const std::string& clip, std::shared_ptr<const Line> line, float len,
                         double delay) {
    std::lock_guard<std::mutex> lk(g_mx);
    // Short untranslated clips (SFX, breaths) must not replace a line that is still showing.
    if (!line && g_act.on && g_act.line && DmNow() < g_act.start + g_act.len) return;
    g_act.src = src;
    g_act.srcId = Unity_InstanceId(src);
    g_act.clip = clip;
    g_act.line = line;
    g_act.start = DmNow() + delay;
    g_act.len = len;
    g_act.on = true;
}

bool Overlay_OnSourceStop(void* src) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_act.on && g_act.src == src) {
        g_act.on = false;
        return true;
    }
    return false;
}

void Overlay_OnFrame() {
    int id;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_act.on || !S.follow) return;
        id = g_act.srcId;
    }
    float x = 0, y = 0;
    bool ok = Unity_SpeakerScreenPos(id, S.headUp, &x, &y);
    std::lock_guard<std::mutex> lk(g_mx);
    g_anchor.valid = ok;
    if (ok) { g_anchor.x = x; g_anchor.y = y; }
}
