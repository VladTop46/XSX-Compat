#include <windows.h>
#include <cstdio>
#include <cstdint>
#define SLOG(n,a,b,c,d,e,f) do { fprintf(stderr, "[STUB chad.dll!%s] args=(%p,%p,%p,%p,%p,%p) -> 0\n", n, (void*)a,(void*)b,(void*)c,(void*)d,(void*)e,(void*)f); fflush(stderr); } while (0)
extern "C" {
__declspec(dllexport) uintptr_t XDspActivate(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XDspActivate", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XDspConnectWithMaximumStreamLimit(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XDspConnectWithMaximumStreamLimit", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XDspDeactivate(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XDspDeactivate", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XDspDisconnect(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XDspDisconnect", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XDspSubmitCommand(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XDspSubmitCommand", a, b, c, d, e, f); return (uintptr_t)0; }
}
