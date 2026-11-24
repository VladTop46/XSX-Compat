#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>

#include <XGameRuntimeInit.h>
#include <XTaskQueue.h>
#include <XAsync.h>
#include <XAsyncProvider.h>

static decltype(&XAsyncBegin) pBegin;
static decltype(&XAsyncSchedule) pSchedule;
static decltype(&XAsyncComplete) pComplete;
static decltype(&XAsyncGetStatus) pStatus;
static decltype(&XAsyncGetResult) pResult;
static decltype(&XAsyncGetResultSize) pResultSize;

struct State {
    uint64_t input = 20;
    uint64_t output = 0;
    uint64_t received = 0;

    DWORD worker = 0;
    DWORD completion = 0;

    HRESULT resultHr = E_FAIL;

    bool done = false;
    std::atomic<bool> cleaned{false};
};

static HRESULT CALLBACK provider(
    XAsyncOp op,
    const XAsyncProviderData* data)
{
    auto& s = *static_cast<State*>(data->context);

    switch (op) {
    case XAsyncOp::Begin:
        printf("[BEGIN] thread=%lu\n",
               GetCurrentThreadId());
        return pSchedule(data->async, 50);

    case XAsyncOp::DoWork:
        s.worker = GetCurrentThreadId();

        printf("[WORK] thread=%lu\n", s.worker);

        s.output = 1;

        for (uint64_t i = 2; i <= s.input; ++i)
            s.output *= i;

        pComplete(data->async, S_OK,
                  sizeof(s.output));

        return S_OK;

    case XAsyncOp::GetResult:
        printf("[GET RESULT] size=%zu\n",
               data->bufferSize);

        memcpy(data->buffer, &s.output,
               sizeof(s.output));

        return S_OK;

    case XAsyncOp::Cancel:
        printf("[CANCEL]\n");
        return S_OK;

    case XAsyncOp::Cleanup:
        printf("[CLEANUP]\n");
        s.cleaned = true;
        return S_OK;
    }

    return E_NOTIMPL;
}

static void CALLBACK completed(XAsyncBlock* async)
{
    auto& s = *static_cast<State*>(async->context);

    s.completion = GetCurrentThreadId();

    printf("[COMPLETION] thread=%lu\n", s.completion);

    HRESULT hr = pStatus(async, false);

    printf("[STATUS] 0x%08lX\n",
           (unsigned long)hr);

    size_t required = 0;
    size_t used = 0;

    if (SUCCEEDED(hr))
        hr = pResultSize(async, &required);

    printf("[RESULT SIZE] %zu\n", required);

    if (SUCCEEDED(hr))
        hr = pResult(async, nullptr,
                     sizeof(s.received),
                     &s.received, &used);

    printf("[RESULT HR] 0x%08lX\n",
           (unsigned long)hr);

    printf("[BYTES USED] %zu\n", used);
    printf("[VALUE] %llu\n",
           (unsigned long long)s.received);

    s.resultHr = hr;
    s.done = true;
}

int main()
{
    HMODULE mod = LoadLibraryW(
        L"xgameruntime.thunks.dll");

    if (!mod) return 1;

#define LOAD(var, name)                                      \
    auto var = reinterpret_cast<decltype(&name)>(            \
        GetProcAddress(mod, #name));                         \
    if (!var) {                                              \
        printf("[MISSING] %s\n", #name);                      \
        return 2;                                            \
    }

#define BIND(var, name)                                      \
    var = reinterpret_cast<decltype(&name)>(                 \
        GetProcAddress(mod, #name));                         \
    if (!var) {                                              \
        printf("[MISSING] %s\n", #name);                      \
        return 2;                                            \
    }

    LOAD(init, XGameRuntimeInitialize);
    LOAD(uninit, XGameRuntimeUninitialize);
    LOAD(create, XTaskQueueCreate);
    LOAD(dispatch, XTaskQueueDispatch);
    LOAD(terminate, XTaskQueueTerminate);
    LOAD(close, XTaskQueueCloseHandle);

    BIND(pBegin, XAsyncBegin);
    BIND(pSchedule, XAsyncSchedule);
    BIND(pComplete, XAsyncComplete);
    BIND(pStatus, XAsyncGetStatus);
    BIND(pResult, XAsyncGetResult);
    BIND(pResultSize, XAsyncGetResultSize);

    if (FAILED(init())) return 3;

    XTaskQueueHandle queue = nullptr;

    HRESULT hr = create(
        XTaskQueueDispatchMode::ThreadPool,
        XTaskQueueDispatchMode::Manual,
        &queue);

    if (FAILED(hr)) {
        uninit();
        return 4;
    }

    State state;
    XAsyncBlock async{};

    async.queue = queue;
    async.context = &state;
    async.callback = completed;

    DWORD mainThread = GetCurrentThreadId();

    printf("[MAIN] thread=%lu\n", mainThread);
    printf("[ASYNC BLOCK SIZE] %zu\n",
           sizeof(XAsyncBlock));

    hr = pBegin(
        &async,
        &state,
        nullptr,
        "FactorialTest",
        provider);

    printf("[ASYNC BEGIN] 0x%08lX\n",
           (unsigned long)hr);

    if (FAILED(hr)) return 5;

    ULONGLONG start = GetTickCount64();

    while (!state.done) {
        dispatch(
            queue,
            XTaskQueuePort::Completion,
            100);

        if (GetTickCount64() - start > 10000) {
            printf("[FAIL] timeout\n");
            fflush(stdout);
            ExitProcess(6);
        }
    }

    printf("[TERMINATE] begin\n");
    fflush(stdout);

    hr = terminate(queue, false, nullptr, nullptr);

    printf("[TERMINATE] hr=0x%08lX\n",
           (unsigned long)hr);

    if (FAILED(hr))
        ExitProcess(8);

    while (dispatch(queue,
                    XTaskQueuePort::Completion,
                    INFINITE))
    {}

    printf("[TERMINATE] completed\n");

    close(queue);
    printf("[CLOSE] OK\n");
    fflush(stdout);

    bool ok =
        SUCCEEDED(state.resultHr) &&
        state.received == 2432902008176640000ULL &&
        state.worker != mainThread &&
        state.worker != 0 &&
        state.completion == mainThread &&
        state.cleaned.load();

    printf("[RESULT] %s\n", ok ? "PASS" : "FAIL");

    uninit();
    FreeLibrary(mod);

    return ok ? 0 : 7;
}
