#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "unity.h"

#include <windows.h>

#include <cctype>
#include <cstring>

void Log(const char* fmt, ...);

namespace {
using PPtrDerefFn = void*(__fastcall*)(const void* pptr);
using GetPosFn = float*(__fastcall*)(const void* transform, float* out3);
using FindCamFn = void*(__fastcall*)();
using W2CFn = const float*(__fastcall*)(const void* cam);

PPtrDerefFn PPtrDeref;
GetPosFn GetPos;
FindCamFn FindCam;
W2CFn W2C;
HWND g_gameWnd;

// Object layout (Unity 6000.x): +0x08 instance id; Component +0x20 GameObject*;
// GameObject +0x20 component array {uint32 type; Component* ptr} with Transform first.
// Transform +0x48 children Transform** / +0x58 child count.
constexpr size_t kInstanceId = 0x08, kCompGameObject = 0x20, kGoComponents = 0x20, kGoComponentCount = 0x30;
constexpr size_t kTrChildren = 0x48, kTrChildCount = 0x58;

using GetNameFn = const char*(__fastcall*)(const void* comp);
GetNameFn CompName;

// cached head lookup
int g_headFor = 0;      // speaker instance id the cache belongs to
int g_headId = 0;       // head transform instance id (0 = none found)

void* TransformOf(void* comp) {
    char* go = *(char**)((char*)comp + kCompGameObject);
    if (!go || *(size_t*)(go + kGoComponentCount) == 0) return nullptr;
    return *(void**)(*(char**)(go + kGoComponents) + 8);
}

int HeadScore(const char* n) {
    char l[128];
    size_t i = 0;
    for (; n[i] && i < sizeof l - 1; ++i) l[i] = (char)tolower((unsigned char)n[i]);
    l[i] = 0;
    if (strstr(l, "hair") || strstr(l, "end") || strstr(l, "top") || strstr(l, "acc") || strstr(l, "phone") ||
        strstr(l, "_sec") || strstr(l, "headset") || strstr(l, "headdress"))
        return 0;
    if (!strcmp(l, "head")) return 100;
    if (!strcmp(l, "j_bip_c_head")) return 95;
    size_t L = strlen(l);
    if (L > 4 && !strcmp(l + L - 4, "head") && strchr("_:. ", l[L - 5])) return 80;
    return 0;
}

void FindHead(void* tr, int depth, int* best, void** bestTr, int* visited) {
    if (!tr || depth > 64 || ++*visited > 4000) return;
    const char* n = CompName(tr);
    int sc = (n && *n) ? HeadScore(n) : 0;
    if (sc > 0 && depth < 40) sc -= depth / 4;  // prefer shallower matches on ties
    if (sc > *best) { *best = sc; *bestTr = tr; }
    size_t cnt = *(size_t*)((char*)tr + kTrChildCount);
    void** kids = *(void***)((char*)tr + kTrChildren);
    for (size_t i = 0; i < cnt && i < 512; ++i) FindHead(kids[i], depth + 1, best, bestTr, visited);
}

BOOL CALLBACK FindWnd(HWND h, LPARAM) {
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"UnityWndClass") == 0) { g_gameWnd = h; return FALSE; }
    return TRUE;
}
}  // namespace

void Unity_Init(const UnityAddrs& a) {
    CompName = (GetNameFn)a.componentGetName;
    PPtrDeref = (PPtrDerefFn)a.pptrDeref;
    GetPos = (GetPosFn)a.transformGetPosition;
    FindCam = (FindCamFn)a.findMainCamera;
    W2C = (W2CFn)a.worldToClip;
}

void* Unity_Deref(int id) {
    if (!id || !PPtrDeref) return nullptr;
    __try { return PPtrDeref(&id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

int Unity_InstanceId(const void* obj) {
    if (!obj) return 0;
    __try { return *(const int*)((const char*)obj + kInstanceId); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool ToScreen(const float p[3], float* ox, float* oy) {
    void* cam = FindCam();
    if (!cam) return false;
    const float* m = W2C(cam);  // column-major
    float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (cw <= 1e-5f) return false;
    float nx = cx / cw, ny = cy / cw;
    if (!g_gameWnd || !IsWindow(g_gameWnd)) {
        g_gameWnd = nullptr;
        EnumWindows(FindWnd, 0);
        if (!g_gameWnd) return false;
    }
    RECT rc;
    GetClientRect(g_gameWnd, &rc);
    POINT o{0, 0};
    ClientToScreen(g_gameWnd, &o);
    *ox = o.x + (nx * 0.5f + 0.5f) * (rc.right - rc.left);
    *oy = o.y + (0.5f - ny * 0.5f) * (rc.bottom - rc.top);
    return true;
}

static bool Project(int id, float up, float* ox, float* oy) {
    void* src = Unity_Deref(id);
    if (!src || !GetPos || !FindCam || !W2C) return false;
    void* root = TransformOf(src);
    if (!root) return false;
    if (g_headFor != id) {
        g_headFor = id;
        g_headId = 0;
        int best = 0, visited = 0;
        void* head = nullptr;
        if (CompName) FindHead(root, 0, &best, &head, &visited);
        if (head) g_headId = Unity_InstanceId(head);
        Log("speaker %d: head bone \"%s\" (searched %d transforms)", id, head ? CompName(head) : "<none>", visited);
    }
    float rp[3], rx, ry;
    GetPos(root, rp);
    if (!ToScreen(rp, &rx, &ry)) return false;
    void* head = Unity_Deref(g_headId);
    if (!head) {  // no head bone: fall back to the source position raised by `up`
        rp[1] += up;
        return ToScreen(rp, ox, oy);
    }
    float hp[3], hx, hy;
    GetPos(head, hp);
    hp[1] += up;
    if (!ToScreen(hp, &hx, &hy)) return false;
    // head bone sits at the base of the skull; lift by a fraction of the on-screen body height
    float body = ry - hy;
    *ox = hx;
    *oy = hy - (body > 0 ? body * 0.16f : 0.f);
    return true;
}

// Character id from the model's effect node, a direct child of the root:
// "Eff_Cha_Amechan01" -> "Amechan01", "Eff_ChouTenchan01" -> "ChouTenchan01".
// If you add a character whose id comes out wrong/empty in dmsubs.log, this is the place to adapt.
static bool CharIdRaw(void* src, char* out, size_t n) {
    __try {
        out[0] = 0;
        void* root = TransformOf(src);
        if (!root) return false;
        size_t cnt = *(size_t*)((char*)root + kTrChildCount);
        void** kids = *(void***)((char*)root + kTrChildren);
        for (size_t i = 0; i < cnt && i < 64; ++i) {
            const char* nm = CompName(kids[i]);
            if (!nm || strncmp(nm, "Eff_", 4)) continue;
            const char* id = !strncmp(nm, "Eff_Cha_", 8) ? nm + 8 : nm + 4;
            if (strchr(id, '_')) continue;  // e.g. Eff_Amechan01_Transformation_01
            strncpy_s(out, n, id, _TRUNCATE);
            return true;
        }
        // Other models may not follow that naming: fall back to the root object's own name.
        const char* nm = CompName(root);
        if (!nm) return true;
        strncpy_s(out, n, nm, _TRUNCATE);
        if (char* c = strstr(out, "(Clone)")) *c = 0;
        size_t len = strlen(out);
        while (len && out[len - 1] == ' ') out[--len] = 0;
        for (char* p = out; *p; ++p)  // it becomes a folder name and a key prefix
            if (strchr("\\/:*?\"<>| ", *p)) *p = '_';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string Unity_CharacterId(void* src) {
    static int cachedFor = 0;
    static std::string cached;
    int id = Unity_InstanceId(src);
    if (id && id == cachedFor) return cached;
    char buf[96];
    if (!CompName || !CharIdRaw(src, buf, sizeof buf)) return "";
    cachedFor = id;
    cached = buf;
    return cached;
}

bool Unity_SpeakerScreenPos(int id, float up, float* x, float* y) {
    __try { return Project(id, up, x, y); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
