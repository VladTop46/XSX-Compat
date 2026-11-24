#include <windows.h>
#include <cstdio>
#include <cstdint>

using Queue = void*;
using Callback = void (WINAPI*)(void*, bool);

static void WINAPI callback(void* ctx, bool canceled)
{
    auto* counter = static_cast<int*>(ctx);

    printf("[CALLBACK] canceled=%d thread=%lu\n",
           canceled, GetCurrentThreadId());

    if (!canceled)
        ++*counter;
}

int main()
{
    HMODULE mod = LoadLibraryW(L"xgameruntime.thunks.dll");
    if (!mod) return 1;

    auto init = reinterpret_cast<HRESULT(WINAPI*)()>(
        GetProcAddress(mod, "XGameRuntimeInitialize"));

    auto uninit = reinterpret_cast<void(WINAPI*)()>(
        GetProcAddress(mod, "XGameRuntimeUninitialize"));

    auto create = reinterpret_cast<HRESULT(WINAPI*)(
        uint32_t, uint32_t, Queue*)>(
        GetProcAddress(mod, "XTaskQueueCreate"));

    auto submit = reinterpret_cast<HRESULT(WINAPI*)(
        Queue, uint32_t, void*, Callback)>(
        GetProcAddress(mod, "XTaskQueueSubmitCallback"));

    auto dispatch = reinterpret_cast<bool(WINAPI*)(
        Queue, uint32_t, uint32_t)>(
        GetProcAddress(mod, "XTaskQueueDispatch"));

    auto close = reinterpret_cast<void(WINAPI*)(Queue)>(
        GetProcAddress(mod, "XTaskQueueCloseHandle"));

    if (!init || !uninit || !create ||
        !submit || !dispatch || !close)
        return 2;

    if (FAILED(init())) return 3;

    Queue queue = nullptr;

    HRESULT hr = create(0, 0, &queue);

    printf("[CREATE] hr=0x%08lX queue=%p\n",
           (unsigned long)hr, queue);

    if (FAILED(hr) || !queue) {
        uninit();
        return 4;
    }

    int counter = 0;
    bool ok = true;

    printf("[MAIN] thread=%lu\n", GetCurrentThreadId());

    for (int i = 0; i < 10; ++i) {
        hr = submit(queue, 0, &counter, callback);
        if (FAILED(hr)) {
            ok = false;
            break;
        }
    }

    printf("[BEFORE DISPATCH] counter=%d\n", counter);
    ok &= counter == 0;

    int dispatched = 0;

    while (dispatched < 10 && dispatch(queue, 0, 100))
        ++dispatched;

    printf("[AFTER DISPATCH] counter=%d\n", counter);
    printf("[DISPATCHED] %d\n", dispatched);

    ok &= counter == 10 && dispatched == 10;

    bool empty = !dispatch(queue, 0, 0);
    printf("[EMPTY QUEUE] %s\n", empty ? "PASS" : "FAIL");
    ok &= empty;

    close(queue);
    uninit();

    printf("[RESULT] %s\n", ok ? "PASS" : "FAIL");

    FreeLibrary(mod);
    return ok ? 0 : 5;
}
