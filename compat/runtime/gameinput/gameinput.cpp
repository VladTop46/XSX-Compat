// GameInput.dll — shim: IGameInput (IID 11BE2A7E-4254-445A-9C09-FFC40F006918, 22 слота vtable по GDK GameInput.h),
// «нет устройств»: чтения -> GAMEINPUT_E_READING_NOT_FOUND, колбэки регистрируются, но не вызываются.
// Реальной обработки ввода нет. Все вызовы логируются.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#define GLOG(...) do { fprintf(stderr, "[GameInput] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
static const HRESULT E_READING_NOT_FOUND = (HRESULT)0x838A0003, E_DEVICE_NOT_FOUND = (HRESULT)0x838A0002;
static const GUID IID_IGameInput_ = {0x11BE2A7E, 0x4254, 0x445A, {0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18}};
static uint64_t g_token = 0x1000;
typedef void* P;

struct Obj { void** vt; volatile LONG ref; };
static HRESULT STDMETHODCALLTYPE QI(Obj* o, REFIID r, void** out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(r, IID_IGameInput_) || IsEqualGUID(r, GUID{0,0,0,{0xC0,0,0,0,0,0,0,0x46}})) { *out = o; InterlockedIncrement(&o->ref); return S_OK; }
    *out = nullptr; GLOG("QueryInterface {%08lX-%04X-%04X-...} -> E_NOINTERFACE", r.Data1, r.Data2, r.Data3); return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE AddRef(Obj* o) { return InterlockedIncrement(&o->ref); }
static ULONG STDMETHODCALLTYPE Release(Obj* o) { return InterlockedDecrement(&o->ref); }   // синглтон: не освобождаем
static uint64_t STDMETHODCALLTYPE GetCurrentTimestamp(Obj*) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return uint64_t(c.QuadPart) * 1000000ULL / uint64_t(f.QuadPart); }
static HRESULT STDMETHODCALLTYPE GetCurrentReading(Obj*, uint32_t kind, P, P* r) { if (r) *r = nullptr; return E_READING_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE GetNextReading(Obj*, P, uint32_t, P, P* r) { if (r) *r = nullptr; return E_READING_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE GetPreviousReading(Obj*, P, uint32_t, P, P* r) { if (r) *r = nullptr; return E_READING_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE GetTemporalReading(Obj*, uint64_t, P, P* r) { if (r) *r = nullptr; return E_READING_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE RegisterReadingCallback(Obj*, P dev, uint32_t kind, float thr, P ctx, P cb, uint64_t* tok)
    { if (tok) *tok = ++g_token; GLOG("RegisterReadingCallback kind=0x%x cb=%p -> token (never invoked)", kind, cb); return S_OK; }
static HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(Obj*, P dev, uint32_t kind, uint32_t status, uint32_t enumKind, P ctx, P cb, uint64_t* tok)
    { if (tok) *tok = ++g_token; GLOG("RegisterDeviceCallback kind=0x%x status=0x%x enum=%u cb=%p -> token (no devices to enumerate)", kind, status, enumKind, cb); return S_OK; }
static HRESULT STDMETHODCALLTYPE RegisterGuideButtonCallback(Obj*, P, P, P cb, uint64_t* tok) { if (tok) *tok = ++g_token; GLOG("RegisterGuideButtonCallback cb=%p", cb); return S_OK; }
static HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(Obj*, P, P, P cb, uint64_t* tok) { if (tok) *tok = ++g_token; GLOG("RegisterKeyboardLayoutCallback cb=%p", cb); return S_OK; }
static void STDMETHODCALLTYPE StopCallback(Obj*, uint64_t t) { GLOG("StopCallback %llx", (unsigned long long)t); }
static bool STDMETHODCALLTYPE UnregisterCallback(Obj*, uint64_t t, uint64_t) { GLOG("UnregisterCallback %llx", (unsigned long long)t); return true; }
static HRESULT STDMETHODCALLTYPE CreateDispatcher(Obj*, P* d) { if (d) *d = nullptr; GLOG("CreateDispatcher -> E_NOTIMPL"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE CreateAggregateDevice(Obj*, uint32_t, P* d) { if (d) *d = nullptr; return E_DEVICE_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE FindDeviceFrom(Obj*, P, P* d) { if (d) *d = nullptr; return E_DEVICE_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(Obj*, uint16_t, uint16_t, uint8_t, uint8_t) { return S_OK; }
static void STDMETHODCALLTYPE SetFocusPolicy(Obj*, uint32_t p) { GLOG("SetFocusPolicy 0x%x", p); }

static void* g_vt[22] = { (void*)QI, (void*)AddRef, (void*)Release, (void*)GetCurrentTimestamp, (void*)GetCurrentReading, (void*)GetNextReading,
    (void*)GetPreviousReading, (void*)GetTemporalReading, (void*)RegisterReadingCallback, (void*)RegisterDeviceCallback, (void*)RegisterGuideButtonCallback,
    (void*)RegisterKeyboardLayoutCallback, (void*)StopCallback, (void*)UnregisterCallback, (void*)CreateDispatcher, (void*)CreateAggregateDevice,
    (void*)FindDeviceFrom, (void*)FindDeviceFrom, (void*)FindDeviceFrom, (void*)FindDeviceFrom, (void*)EnableOemDeviceSupport, (void*)SetFocusPolicy };
static Obj g_obj = { g_vt, 1 };

extern "C" __declspec(dllexport) HRESULT WINAPI GameInputCreate(void** gameInput)
{
    GLOG("GameInputCreate out=%p -> S_OK (no devices)", gameInput);
    if (!gameInput) return E_POINTER;
    InterlockedIncrement(&g_obj.ref); *gameInput = &g_obj; return S_OK;
}
