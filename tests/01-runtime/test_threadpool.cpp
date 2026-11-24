#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <set>

using Queue = void*;
using Callback = void (WINAPI*)(void*);
using TaskCallback = void (WINAPI*)(void*, bool);

constexpr int TASKS = 32;

struct State {
    std::atomic<int> completed{0};
    std::atomic<int> canceled{0};
    std::atomic<int> active{0};
    std::atomic<int> peak{0};

    std::mutex lock;
    std::set<DWORD> threads;

    HANDLE done = nullptr;
};

static void WINAPI callback(void* ctx, bool canceled)
{
    auto& s = *static_cast<State*>(ctx);

    if (canceled) {
        ++s.canceled;
    } else {
        int active = ++s.active;
        int peak = s.peak.load();

        while (active > peak &&
               !s.peak.compare_exchange_weak(peak, active))
        {}

        {
            std::lock_guard<std::mutex> guard(s.lock);
            s.threads.insert(GetCurrentThreadId());
        }

        Sleep(50);

        --s.active;
    }

    if (++s.completed == TASKS)
        SetEvent(s.done);
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
        Queue, uint32_t, void*, TaskCallback)>(
        GetProcAddress(mod, "XTaskQueueSubmitCallback"));

    auto terminate = reinterpret_cast<void(WINAPI*)(
        Queue, bool, void*, Callback)>(
        GetProcAddress(mod, "XTaskQueueTerminate"));

    auto close = reinterpret_cast<void(WINAPI*)(Queue)>(
        GetProcAddress(mod, "XTaskQueueCloseHandle"));

    if (!init || !uninit || !create ||
        !submit || !terminate || !close)
        return 2;

    if (FAILED(init())) return 3;

    DWORD mainThread = GetCurrentThreadId();
    printf("[MAIN] thread=%lu\n", mainThread);

    bool allOk = true;

    for (uint32_t mode = 1; mode <= 2; ++mode) {
        printf("\n[MODE] %s\n",
               mode == 1 ? "ThreadPool" :
                           "SerializedThreadPool");

        Queue queue = nullptr;
        HRESULT hr = create(mode, mode, &queue);

        if (FAILED(hr) || !queue) {
            printf("[FAIL] create: 0x%08lX\n",
                   (unsigned long)hr);
            allOk = false;
            continue;
        }

        State s;
        s.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (!s.done) {
            terminate(queue, true, nullptr, nullptr);
            close(queue);
            allOk = false;
            continue;
        }

        ULONGLONG start = GetTickCount64();

        bool submitted = true;

        for (int i = 0; i < TASKS; ++i) {
            hr = submit(queue, 0, &s, callback);

            if (FAILED(hr)) {
                printf("[FAIL] submit: 0x%08lX\n",
                       (unsigned long)hr);
                submitted = false;
                break;
            }
        }

        DWORD result = submitted
            ? WaitForSingleObject(s.done, 15000)
            : WAIT_FAILED;

        ULONGLONG elapsed = GetTickCount64() - start;

        terminate(queue, true, nullptr, nullptr);
        close(queue);

        printf("[COMPLETED] %d/%d\n",
               s.completed.load(), TASKS);
        printf("[CANCELED] %d\n", s.canceled.load());
        printf("[THREADS] %zu\n", s.threads.size());
        printf("[PEAK PARALLELISM] %d\n", s.peak.load());
        printf("[ELAPSED] %llu ms\n", elapsed);

        printf("[THREAD IDS]");
        for (DWORD id : s.threads)
            printf(" %lu", id);
        printf("\n");

        bool ok = submitted &&
                  result == WAIT_OBJECT_0 &&
                  s.completed == TASKS &&
                  s.canceled == 0 &&
                  s.threads.count(mainThread) == 0 &&
                  s.peak >= 1;

        if (mode == 2)
            ok &= s.peak == 1;

        printf("[RESULT] %s\n", ok ? "PASS" : "FAIL");

        CloseHandle(s.done);
        allOk &= ok;
    }

    uninit();
    FreeLibrary(mod);

    return allOk ? 0 : 4;
}
