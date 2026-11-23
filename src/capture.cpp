#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

void Log(const char* fmt, ...);
std::wstring Utf8ToWide(const std::string& s);  // also maps '/' to '\\' 
double DmNow();

namespace {

constexpr int kRate = 48000;
constexpr size_t kRing = (size_t)kRate * 120;  // 2 minutes of mono audio
constexpr double kPre = 0.15, kPost = 0.5;

std::wstring g_dir;
std::mutex g_mx;
std::vector<int16_t> g_ring(kRing);
int64_t g_total = 0;       // samples written since start (ring index = total % kRing)
double g_t0 = -1;          // DmNow() time of sample 0
struct Req { std::string rel; double start, len; };
std::vector<Req> g_reqs;

class Completion : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
   public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    IAudioClient* client = nullptr;
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *p = (IActivateAudioInterfaceCompletionHandler*)this;
        } else if (r == __uuidof(IAgileObject)) {
            *p = (IAgileObject*)this;
        } else {
            *p = nullptr;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT ahr = E_FAIL;
        IUnknown* unk = nullptr;
        hr = op->GetActivateResult(&ahr, &unk);
        if (SUCCEEDED(hr)) hr = ahr;
        if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        if (unk) unk->Release();
        SetEvent(done);
        return S_OK;
    }
};

void WriteWav(const std::wstring& path, const std::vector<int16_t>& s) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return;
    uint32_t data = (uint32_t)(s.size() * 2), riff = 36 + data, fmtLen = 16, rate = kRate, byteRate = kRate * 2;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

void ServiceRequests() {
    std::vector<std::pair<Req, std::vector<int16_t>>> ready;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_t0 < 0) return;
        double capturedUntil = g_t0 + (double)g_total / kRate;
        for (size_t i = 0; i < g_reqs.size();) {
            Req& r = g_reqs[i];
            double a = r.start - kPre, b = r.start + r.len + kPost;
            if (capturedUntil >= b || DmNow() > b + 3.0) {  // +3s: stream went silent, take what we have
                int64_t ia = (int64_t)((a - g_t0) * kRate), ib = (int64_t)((b - g_t0) * kRate);
                ia = std::max<int64_t>(ia, std::max<int64_t>(0, g_total - (int64_t)kRing));
                std::vector<int16_t> s;
                for (int64_t k = ia; k < ib; ++k) s.push_back(k < g_total ? g_ring[k % kRing] : 0);
                ready.push_back({r, std::move(s)});
                g_reqs.erase(g_reqs.begin() + i);
            } else {
                ++i;
            }
        }
    }
    for (auto& [r, s] : ready) {
        std::wstring path = g_dir + Utf8ToWide(r.rel) + L".wav";
        CreateDirectoryW(g_dir.c_str(), nullptr);
        size_t slash = path.find_last_of(L'\\');
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
        int peak = 0;
        for (auto v : s) peak = std::max(peak, std::abs((int)v));
        if (peak < 200) {
            Log("capture %s: silent (is game audio muted?) - not saved", r.rel.c_str());
            continue;
        }
        WriteWav(path, s);
        Log("captured %s (%.2fs)", r.rel.c_str(), s.size() / (double)kRate);
    }
}

DWORD WINAPI CaptureThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof ap;
    pv.blob.pBlobData = (BYTE*)&ap;
    auto* cb = new Completion();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, cb, &op);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(cb->done, 10000);
        hr = cb->hr;
    }
    if (FAILED(hr) || !cb->client) { Log("capture: activation failed 0x%08X", (unsigned)hr); return 0; }
    IAudioClient* ac = cb->client;
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 2, kRate, kRate * 4, 4, 16, 0};
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                        2000000, 0, &wf, nullptr);
    IAudioCaptureClient* cc = nullptr;
    if (SUCCEEDED(hr)) hr = ac->SetEventHandle(ev);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc);
    if (SUCCEEDED(hr)) hr = ac->Start();
    if (FAILED(hr)) { Log("capture: init failed 0x%08X", (unsigned)hr); return 0; }
    Log("capture: recording game audio");
    for (;;) {
        WaitForSingleObject(ev, 100);
        BYTE* data;
        UINT32 frames;
        DWORD flags;
        UINT64 qpcPos;
        UINT32 next = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&next)) && next &&
               SUCCEEDED(cc->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos))) {
            double t = qpcPos / 1e7;  // 100ns QPC units -> seconds, same clock as DmNow()
            {
                std::lock_guard<std::mutex> lk(g_mx);
                if (g_t0 < 0) g_t0 = t;
                // Loopback delivers nothing while the game is silent: pad the gap so index == time.
                int64_t expect = (int64_t)((t - g_t0) * kRate);
                if (expect > g_total + kRate / 50) {
                    int64_t pad = std::min<int64_t>(expect - g_total, (int64_t)kRing);
                    for (int64_t k = 0; k < pad; ++k) g_ring[(g_total + k) % kRing] = 0;
                    g_total = expect;
                }
                auto* in = (const int16_t*)data;
                bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
                for (UINT32 i = 0; i < frames; ++i)
                    g_ring[(g_total + i) % kRing] = silent ? 0 : (int16_t)(((int)in[2 * i] + in[2 * i + 1]) / 2);
                g_total += frames;
            }
            cc->ReleaseBuffer(frames);
        }
        ServiceRequests();
    }
}

}  // namespace

void Capture_Start(const std::wstring& clipDir) {
    g_dir = clipDir;
    CloseHandle(CreateThread(nullptr, 0, CaptureThread, nullptr, 0, nullptr));
}

void Capture_Request(const std::string& relPath, double start, double len) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& r : g_reqs)
        if (r.rel == relPath) return;
    g_reqs.push_back({relPath, start, len});
}
