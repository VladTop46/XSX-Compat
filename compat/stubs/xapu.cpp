#include <windows.h>
#include <cstdio>
#include <cstdint>
#define SLOG(n,a,b,c,d,e,f) do { fprintf(stderr, "[STUB xapu.dll!%s] args=(%p,%p,%p,%p,%p,%p) -> 0\n", n, (void*)a,(void*)b,(void*)c,(void*)d,(void*)e,(void*)f); fflush(stderr); } while (0)
extern "C" {
__declspec(dllexport) uintptr_t XApuConnect(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XApuConnect", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XApuDequeueResult(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XApuDequeueResult", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XApuDisconnect(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XApuDisconnect", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XApuEnqueueCommand(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XApuEnqueueCommand", a, b, c, d, e, f); return (uintptr_t)0; }
__declspec(dllexport) uintptr_t XApuGetResultCount(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XApuGetResultCount", a, b, c, d, e, f); return (uintptr_t)0; }
}
