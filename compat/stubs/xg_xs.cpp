#include <windows.h>
#include <cstdio>
#include <cstdint>
#define SLOG(n,a,b,c,d,e,f) do { fprintf(stderr, "[STUB xg_xs.dll!%s] args=(%p,%p,%p,%p,%p,%p) -> 0x80004001\n", n, (void*)a,(void*)b,(void*)c,(void*)d,(void*)e,(void*)f); fflush(stderr); } while (0)
extern "C" {
__declspec(dllexport) uintptr_t XGCreateTextureComputer(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XGCreateTextureComputer", a, b, c, d, e, f); return (uintptr_t)0x80004001; }
__declspec(dllexport) uintptr_t XGSuggestSwizzleMode(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { SLOG("XGSuggestSwizzleMode", a, b, c, d, e, f); return (uintptr_t)0x80004001; }
}
