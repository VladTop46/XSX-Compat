// GameInput.dll — shim IGameInput (IID 11BE2A7E-4254-445A-9C09-FFC40F006918, 22 vtable slots, GDK
// GameInput v0) with ONE virtual Xbox gamepad driven by the keyboard of the game window
// (GetAsyncKeyState; Wine reports state while our window has focus). XSX_PAD=0 -> "no devices".
// Mapping: Enter/Space=A, Esc/Backspace=B, Q=X, E=Y, arrows=D-pad, WASD=left stick,
// IJKL=right stick, Z/C=LB/RB, 1/3=LT/RT, Tab=View, M/F1=Menu, F/G=L3/R3.
// Gamepad readings also answer UiNavigation queries. Every first-seen call shape is logged.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <cstdlib>
#define GLOG(...) do { fprintf(stderr, "[GameInput] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
static const HRESULT E_READING_NOT_FOUND = (HRESULT)0x838A0003, E_DEVICE_NOT_FOUND = (HRESULT)0x838A0002;
static const GUID IID_IGameInput_ = {0x11BE2A7E, 0x4254, 0x445A, {0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18}};
static const GUID IID_IUnknown_ = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
enum : uint32_t { KindControllerAxis = 0x2, KindControllerButton = 0x4, KindKeyboard = 0x10, KindMouse = 0x20,
                  KindGamepad = 0x40000, KindUiNavigation = 0x1000000 };
static const uint32_t kPadKinds = KindGamepad | KindUiNavigation;
static std::atomic<uint64_t> g_token{0x1000}, g_seq{1};
typedef void* P;
static bool PadOn() { static bool on = [] { char v[4]; return !(GetEnvironmentVariableA("XSX_PAD", v, sizeof v) > 0 && v[0] == '0'); }(); return on; }
static uint64_t NowUs() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return uint64_t(c.QuadPart) * 1000000ULL / uint64_t(f.QuadPart); }

// ---- gamepad state from the keyboard ----
struct PadState { uint32_t buttons; float lt, rt, lx, ly, rx, ry; };
static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
// XSX_PAD_SCRIPT="40:4,50:20" -> at 40 s after GameInputCreate hold buttons 0x4 (A) for 400 ms, at 50 s 0x20 (Y)...
static uint64_t g_t0;
static uint32_t ScriptButtons()
{
    static struct Ev { double t; uint32_t b; } ev[32]; static int n = -1;
    if (n < 0) { n = 0; char v[512]; if (GetEnvironmentVariableA("XSX_PAD_SCRIPT", v, sizeof v)) {
        for (char* p = v; *p && n < 32;) { double t = strtod(p, &p); if (*p != ':') break; uint32_t b = strtoul(p + 1, &p, 16); ev[n++] = {t, b}; if (*p == ',') p++; }
        GLOG("pad script: %d events", n); } }
    const double now = (NowUs() - g_t0) / 1e6; uint32_t b = 0;
    for (int i = 0; i < n; i++) if (now >= ev[i].t && now < ev[i].t + 0.4) b |= ev[i].b;
    return b;
}
static PadState ReadPad()
{
    PadState s{}; s.buttons |= ScriptButtons();
    struct { int vk; uint32_t bit; } map[] = {
        {VK_RETURN, 0x4}, {VK_SPACE, 0x4}, {VK_ESCAPE, 0x8}, {VK_BACK, 0x8}, {'Q', 0x10}, {'E', 0x20},
        {VK_UP, 0x40}, {VK_DOWN, 0x80}, {VK_LEFT, 0x100}, {VK_RIGHT, 0x200}, {'Z', 0x400}, {'C', 0x800},
        {'F', 0x1000}, {'G', 0x2000}, {VK_TAB, 0x2}, {'M', 0x1}, {VK_F1, 0x1} };
    for (auto& m : map) if (Key(m.vk)) s.buttons |= m.bit;
    s.lx = (Key('D') ? 1.f : 0.f) - (Key('A') ? 1.f : 0.f); s.ly = (Key('W') ? 1.f : 0.f) - (Key('S') ? 1.f : 0.f);
    s.rx = (Key('L') ? 1.f : 0.f) - (Key('J') ? 1.f : 0.f); s.ry = (Key('I') ? 1.f : 0.f) - (Key('K') ? 1.f : 0.f);
    s.lt = Key('1') ? 1.f : 0.f; s.rt = Key('3') ? 1.f : 0.f;
    static uint32_t last = 0; if (s.buttons != last) { GLOG("pad buttons 0x%x lx=%.0f ly=%.0f", s.buttons, s.lx, s.ly); last = s.buttons; }
    return s;
}
// GameInputUiNavigationButtons: Menu 1, View 2, Accept 4, Cancel 8, Up 0x10, Down 0x20, Left 0x40, Right 0x80,
// Context1..4 0x100.., PageUp.. 0x1000.., PageLeft.. ; derived from the pad.
static uint32_t UiNav(const PadState& s)
{
    uint32_t b = 0, p = s.buttons;
    if (p & 0x1) b |= 0x1; if (p & 0x2) b |= 0x2; if (p & 0x4) b |= 0x4; if (p & 0x8) b |= 0x8;
    if ((p & 0x40) || s.ly > .5f) b |= 0x10; if ((p & 0x80) || s.ly < -.5f) b |= 0x20;
    if ((p & 0x100) || s.lx < -.5f) b |= 0x40; if ((p & 0x200) || s.lx > .5f) b |= 0x80;
    if (p & 0x10) b |= 0x100; if (p & 0x20) b |= 0x200; if (p & 0x400) b |= 0x4000; if (p & 0x800) b |= 0x8000;
    return b;
}

struct Obj { void** vt; std::atomic<long> ref; };
static ULONG STDMETHODCALLTYPE AddRef(Obj* o) { return ++o->ref; }

// ---- IGameInputDevice (singleton virtual pad) ----
struct DeviceInfo {   // GameInputDeviceInfo v0 prefix; tail zeroed (pointers NULL, counts 0)
    uint32_t infoSize; uint16_t vendorId, productId, revisionNumber; uint8_t interfaceNumber, collectionNumber;
    uint16_t usagePage, usageId; uint16_t hw[4], fw[4]; uint8_t deviceId[32], deviceRootId[32];
    uint32_t deviceFamily, capabilities, supportedInput, supportedRumbleMotors;
    uint8_t tail[512];
};
static DeviceInfo g_info;
static Obj g_dev;
static HRESULT STDMETHODCALLTYPE DevQI(Obj* o, REFIID r, void** out) { if (!out) return E_POINTER; *out = o; ++o->ref; GLOG("device QI {%08lX}", r.Data1); return S_OK; }
static ULONG STDMETHODCALLTYPE DevRelease(Obj* o) { return --o->ref; }
static const DeviceInfo* STDMETHODCALLTYPE DevGetInfo(Obj*) { static int n; if (n++ < 4) GLOG("device GetDeviceInfo"); return &g_info; }
static uint32_t STDMETHODCALLTYPE DevGetStatus(Obj*) { return 0x1 /*Connected*/ | 0x2 /*InputEnabled*/ | 0x80 /*... remain conservative*/ * 0; }
static void STDMETHODCALLTYPE DevGetBattery(Obj*, void* b) { if (b) memset(b, 0, 24); }
static HRESULT STDMETHODCALLTYPE DevNotImpl(Obj*, P, P, P, P) { static int n; if (n++ < 8) GLOG("device method E_NOTIMPL"); return E_NOTIMPL; }
static bool STDMETHODCALLTYPE DevFalse(Obj*, P) { return false; }
static void STDMETHODCALLTYPE DevVoid(Obj*, P, P) {}
static void* g_devVt[22] = { (void*)DevQI, (void*)AddRef, (void*)DevRelease, (void*)DevGetInfo, (void*)DevGetStatus, (void*)DevGetBattery,
    (void*)DevNotImpl, (void*)DevFalse, (void*)DevVoid, (void*)DevVoid, (void*)DevVoid, (void*)DevVoid, (void*)DevVoid, (void*)DevVoid,
    (void*)DevNotImpl, (void*)DevNotImpl, (void*)DevNotImpl, (void*)DevNotImpl, (void*)DevNotImpl, (void*)DevNotImpl, (void*)DevNotImpl, (void*)DevVoid };

// ---- IGameInputReading ----
struct Reading { void** vt; std::atomic<long> ref; uint32_t kind; uint64_t seq, ts; PadState pad; };
static HRESULT STDMETHODCALLTYPE RdQI(Reading* o, REFIID, void** out) { if (!out) return E_POINTER; *out = o; ++o->ref; return S_OK; }
static ULONG STDMETHODCALLTYPE RdAddRef(Reading* o) { return ++o->ref; }
static ULONG STDMETHODCALLTYPE RdRelease(Reading* o) { long r = --o->ref; if (!r) delete o; return r; }
static uint32_t STDMETHODCALLTYPE RdKind(Reading* o) { return o->kind; }
static uint64_t STDMETHODCALLTYPE RdSeq(Reading* o, uint32_t) { return o->seq; }
static uint64_t STDMETHODCALLTYPE RdTs(Reading* o) { return o->ts; }
static void STDMETHODCALLTYPE RdDevice(Reading*, Obj** d) { if (d) { ++g_dev.ref; *d = &g_dev; } }
static bool STDMETHODCALLTYPE RdRaw(Reading*, P* r) { if (r) *r = nullptr; return false; }
static uint32_t STDMETHODCALLTYPE RdZeroCount(Reading*) { return 0; }
static uint32_t STDMETHODCALLTYPE RdZeroArr(Reading*, uint32_t, P) { return 0; }
static bool STDMETHODCALLTYPE RdFalseP(Reading*, P) { return false; }
static bool STDMETHODCALLTYPE RdGamepad(Reading* o, void* out)
{   // GameInputGamepadState { uint32 buttons; float lt, rt, lx, ly, rx, ry; }
    if (!out) return false; memcpy(out, &o->pad, sizeof o->pad); return true;
}
static bool STDMETHODCALLTYPE RdUiNav(Reading* o, void* out)
{   // GameInputUiNavigationState { uint32 buttons; }
    if (!out) return false; uint32_t b = UiNav(o->pad); memcpy(out, &b, 4); return true;
}
static void* g_rdVt[] = { (void*)RdQI, (void*)RdAddRef, (void*)RdRelease, (void*)RdKind, (void*)RdSeq, (void*)RdTs, (void*)RdDevice, (void*)RdRaw,
    (void*)RdZeroCount, (void*)RdZeroArr, (void*)RdZeroCount, (void*)RdZeroArr, (void*)RdZeroCount, (void*)RdZeroArr,
    (void*)RdZeroCount, (void*)RdZeroArr, (void*)RdFalseP, (void*)RdZeroCount, (void*)RdZeroArr, (void*)RdFalseP,
    (void*)RdFalseP, (void*)RdFalseP, (void*)RdGamepad, (void*)RdFalseP, (void*)RdUiNav };
static Reading* NewReading(uint32_t kind)
{
    auto* r = new Reading{ g_rdVt, {1}, kind & kPadKinds ? (kind & kPadKinds) : KindGamepad, g_seq++, NowUs(), ReadPad() };
    return r;
}

// ---- IGameInput ----
static HRESULT STDMETHODCALLTYPE QI(Obj* o, REFIID r, void** out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(r, IID_IGameInput_) || IsEqualGUID(r, IID_IUnknown_)) { *out = o; ++o->ref; return S_OK; }
    *out = nullptr; GLOG("QueryInterface {%08lX-%04X-%04X-...} -> E_NOINTERFACE", r.Data1, r.Data2, r.Data3); return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Release(Obj* o) { return --o->ref; }
static uint64_t STDMETHODCALLTYPE GetCurrentTimestamp(Obj*) { return NowUs(); }
static void LogShape(const char* what, uint32_t kind, P dev)
{
    static std::atomic<int> n{0}; if (n++ < 24) GLOG("%s kind=0x%x device=%p", what, kind, dev);
}
static HRESULT STDMETHODCALLTYPE GetCurrentReading(Obj*, uint32_t kind, P dev, P* r)
{
    LogShape("GetCurrentReading", kind, dev);
    if (r) *r = nullptr;
    if (!PadOn() || !(kind & kPadKinds) || (dev && dev != &g_dev)) return E_READING_NOT_FOUND;
    *r = NewReading(kind); return S_OK;
}
static HRESULT STDMETHODCALLTYPE GetNextReading(Obj*, P ref, uint32_t kind, P dev, P* r)
{   // Only "current" state is modelled: a newer reading exists when the state changed is not tracked.
    LogShape("GetNextReading", kind, dev); if (r) *r = nullptr; return E_READING_NOT_FOUND;
}
static HRESULT STDMETHODCALLTYPE GetPreviousReading(Obj*, P, uint32_t kind, P dev, P* r) { LogShape("GetPreviousReading", kind, dev); if (r) *r = nullptr; return E_READING_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE GetTemporalReading(Obj*, uint64_t, P dev, P* r) { LogShape("GetTemporalReading", 0, dev); if (r) *r = nullptr; return E_READING_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE RegisterReadingCallback(Obj*, P dev, uint32_t kind, float thr, P ctx, P cb, uint64_t* tok)
    { if (tok) *tok = ++g_token; GLOG("RegisterReadingCallback kind=0x%x cb=%p -> token (polling only, never invoked)", kind, cb); return S_OK; }
typedef void (CALLBACK* DeviceCb)(uint64_t token, void* ctx, Obj* device, uint64_t ts, uint32_t cur, uint32_t prev);
static HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(Obj*, P dev, uint32_t kind, uint32_t status, uint32_t enumKind, P ctx, P cb, uint64_t* tok)
{
    const uint64_t t = ++g_token; if (tok) *tok = t;
    const bool match = PadOn() && (kind & kPadKinds) && (status & 0x1);
    GLOG("RegisterDeviceCallback kind=0x%x status=0x%x enum=%u cb=%p -> token %llx%s", kind, status, enumKind, cb, (unsigned long long)t, match ? " (virtual pad announced)" : "");
    // Blocking (2) and async (1) enumeration both announce existing devices; we do it synchronously.
    if (match && enumKind && cb) ((DeviceCb)cb)(t, ctx, &g_dev, NowUs(), 0x1 | 0x2, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE RegisterGuideButtonCallback(Obj*, P, P, P cb, uint64_t* tok) { if (tok) *tok = ++g_token; GLOG("RegisterGuideButtonCallback cb=%p", cb); return S_OK; }
static HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(Obj*, P, P, P cb, uint64_t* tok) { if (tok) *tok = ++g_token; GLOG("RegisterKeyboardLayoutCallback cb=%p", cb); return S_OK; }
static void STDMETHODCALLTYPE StopCallback(Obj*, uint64_t t) { GLOG("StopCallback %llx", (unsigned long long)t); }
static bool STDMETHODCALLTYPE UnregisterCallback(Obj*, uint64_t t, uint64_t) { GLOG("UnregisterCallback %llx", (unsigned long long)t); return true; }
static HRESULT STDMETHODCALLTYPE CreateDispatcher(Obj*, P* d) { if (d) *d = nullptr; GLOG("CreateDispatcher -> E_NOTIMPL"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE CreateAggregateDevice(Obj*, uint32_t, P* d) { if (d) *d = nullptr; return E_DEVICE_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE FindDeviceFrom(Obj*, P, P* d) { GLOG("FindDeviceFrom*"); if (d) *d = nullptr; return E_DEVICE_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(Obj*, uint16_t, uint16_t, uint8_t, uint8_t) { return S_OK; }
static void STDMETHODCALLTYPE SetFocusPolicy(Obj*, uint32_t p) { GLOG("SetFocusPolicy 0x%x", p); }

static void* g_vt[22] = { (void*)QI, (void*)AddRef, (void*)Release, (void*)GetCurrentTimestamp, (void*)GetCurrentReading, (void*)GetNextReading,
    (void*)GetPreviousReading, (void*)GetTemporalReading, (void*)RegisterReadingCallback, (void*)RegisterDeviceCallback, (void*)RegisterGuideButtonCallback,
    (void*)RegisterKeyboardLayoutCallback, (void*)StopCallback, (void*)UnregisterCallback, (void*)CreateDispatcher, (void*)CreateAggregateDevice,
    (void*)FindDeviceFrom, (void*)FindDeviceFrom, (void*)FindDeviceFrom, (void*)FindDeviceFrom, (void*)EnableOemDeviceSupport, (void*)SetFocusPolicy };
static Obj g_obj;

extern "C" __declspec(dllexport) HRESULT WINAPI GameInputCreate(void** gameInput)
{
    static bool init = [] {
        g_t0 = NowUs(); g_obj.vt = g_vt; g_obj.ref = 1; g_dev.vt = g_devVt; g_dev.ref = 1;
        memset(&g_info, 0, sizeof g_info); g_info.infoSize = sizeof g_info - sizeof g_info.tail + 128;
        g_info.vendorId = 0x045E; g_info.productId = 0x0B12; g_info.revisionNumber = 1; g_info.usagePage = 1; g_info.usageId = 5;
        g_info.deviceFamily = 1 /*XboxOne*/; g_info.supportedInput = kPadKinds | KindControllerAxis | KindControllerButton;
        g_info.deviceId[0] = 0x5A; g_info.deviceRootId[0] = 0x5A; return true; }();
    (void)init;
    GLOG("GameInputCreate out=%p -> S_OK (virtual keyboard pad %s)", gameInput, PadOn() ? "ON" : "off");
    if (!gameInput) return E_POINTER;
    ++g_obj.ref; *gameInput = &g_obj; return S_OK;
}
