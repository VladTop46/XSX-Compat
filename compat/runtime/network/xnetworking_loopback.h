#pragma once
// Narrow provider for the exact XNetworking interface queried by XCurl.
// The GUID and slots are established by its native wrapper disassembly and SDK
// declarations. This advertises local access only; Internet policy is unsupported.
#include <windows.h>
#include <unknwn.h>
#include <winhttp.h>
#include <XNetworking.h>
#include <XAsyncProvider.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace xsx::network {
inline constexpr GUID service_id{0x37e56907,0x2f10,0x41e8,{0xb7,0x2f,0x36,0xed,0xb1,0x85,0x33,0x1a}};
inline constexpr GUID interface_id{0xbf2346b2,0x39af,0x4658,{0xb5,0xea,0x44,0x71,0x3c,0x7e,0x83,0xb3}};
inline constexpr GUID unknown_id{0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
inline bool equal(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

inline decltype(&::XAsyncBegin) begin;
inline decltype(&::XAsyncSchedule) schedule;
inline decltype(&::XAsyncComplete) complete;
inline decltype(&::XAsyncGetResultSize) result_size;
inline decltype(&::XAsyncGetResult) result;
inline decltype(&::XTaskQueueSubmitCallback) submit;
inline decltype(&::XTaskQueueGetCurrentProcessTaskQueue) process_queue;
inline decltype(&::XTaskQueueDuplicateHandle) duplicate_queue;
inline decltype(&::XTaskQueueCloseHandle) close_queue;
inline const char security_identity[] = "XSXLoopbackSecurity";

template<class T> inline void bind(T& pointer, HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(name);
    static_assert(sizeof(pointer) == sizeof(address));
    std::memcpy(&pointer, &address, sizeof(pointer));
}
inline void initialize(HMODULE runtime) {
    bind(begin, runtime, "XAsyncBegin"); bind(schedule, runtime, "XAsyncSchedule");
    bind(complete, runtime, "XAsyncComplete"); bind(result_size, runtime, "XAsyncGetResultSize");
    bind(result, runtime, "XAsyncGetResult");
    bind(submit, runtime, "XTaskQueueSubmitCallback");
    bind(process_queue, runtime, "XTaskQueueGetCurrentProcessTaskQueue");
    bind(duplicate_queue, runtime, "XTaskQueueDuplicateHandle");
    bind(close_queue, runtime, "XTaskQueueCloseHandle");
}

inline bool local_http(const wchar_t* url) {
    if (!url) return false;
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTP ||
        parts.dwUserNameLength || parts.dwPasswordLength) return false;
    constexpr wchar_t loopback[] = L"127.0.0.1";
    return parts.dwHostNameLength == 9 && std::wmemcmp(parts.lpszHostName, loopback, 9) == 0;
}

inline HRESULT verify_plain_loopback(void* request, const XNetworkingSecurityInformation* info) {
    if (!request || !info) return E_INVALIDARG;
    if (info->enabledHttpSecurityProtocolFlags || info->thumbprintCount || info->thumbprints)
        return E_NOTIMPL;
    // XCurl also invokes Verify for plaintext HTTP. Accept only a real WinHTTP
    // request whose current URL is numeric loopback HTTP; no TLS flags change.
    DWORD bytes = 0;
    if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return E_INVALIDARG;
    if (bytes < sizeof(wchar_t) || bytes > 65536) return E_INVALIDARG;
    try {
        std::vector<wchar_t> url(bytes / sizeof(wchar_t) + 1, 0);
        if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &bytes))
            return HRESULT_FROM_WIN32(GetLastError());
        return local_http(url.data()) ? S_OK : E_NOTIMPL;
    } catch (...) { return E_OUTOFMEMORY; }
}

inline HRESULT CALLBACK security_provider(XAsyncOp op, const XAsyncProviderData* data) {
    std::printf("[XNETWORKING] Security provider op=%u\n", static_cast<unsigned>(op));
    switch (op) {
    case XAsyncOp::Begin: return schedule(data->async, 0);
    case XAsyncOp::DoWork:
        complete(data->async, S_OK, sizeof(XNetworkingSecurityInformation)); return S_OK;
    case XAsyncOp::GetResult: {
        if (data->bufferSize < sizeof(XNetworkingSecurityInformation)) return E_INVALIDARG;
        XNetworkingSecurityInformation info{}; // Plain HTTP on numeric loopback only.
        std::memcpy(data->buffer, &info, sizeof(info)); return S_OK;
    }
    case XAsyncOp::Cancel: complete(data->async, E_ABORT, 0); return S_OK;
    case XAsyncOp::Cleanup: return S_OK;
    }
    return E_NOTIMPL;
}

struct Interface : IUnknown {
#define RESERVED(n) virtual HRESULT WINAPI Unknown##n() = 0;
    RESERVED(3) RESERVED(4) RESERVED(5) RESERVED(6)
    RESERVED(7) RESERVED(8) RESERVED(9) RESERVED(10)
#undef RESERVED
    virtual HRESULT WINAPI Security(const wchar_t*, XAsyncBlock*) = 0;                 // 11
    virtual HRESULT WINAPI SecuritySize(XAsyncBlock*, size_t*) = 0;                    // 12
    virtual HRESULT WINAPI SecurityResult(XAsyncBlock*, size_t, size_t*, uint8_t*, XNetworkingSecurityInformation**) = 0; // 13
    virtual HRESULT WINAPI Verify(void*, const XNetworkingSecurityInformation*) = 0;   // 14
    virtual HRESULT WINAPI Hint(XNetworkingConnectivityHint*) = 0;                     // 15
    virtual HRESULT WINAPI Register(XTaskQueueHandle, void*, XNetworkingConnectivityHintChangedCallback*, XTaskQueueRegistrationToken*) = 0; // 16
    virtual bool WINAPI Unregister(XTaskQueueRegistrationToken, bool) = 0;             // 17
};

class Loopback final : public Interface {
    std::atomic<ULONG> references_{1}; // Permanent factory reference.
    struct Registration {
        std::mutex mutex;
        std::condition_variable finished;
        void* context = nullptr;
        XNetworkingConnectivityHintChangedCallback* callback = nullptr;
        bool removed = false, running = false;
        DWORD thread = 0;
    };
    std::mutex mutex_;
    uint64_t next_ = 1;
    std::map<uint64_t, std::shared_ptr<Registration>> registrations_;
    static XNetworkingConnectivityHint local_hint() {
        XNetworkingConnectivityHint hint{};
        hint.connectivityLevel = XNetworkingConnectivityLevelHint::LocalAccess;
        hint.connectivityCost = XNetworkingConnectivityCostHint::Unknown;
        hint.ianaInterfaceType = 24; // softwareLoopback.
        hint.networkInitialized = true;
        return hint;
    }
    static void CALLBACK initial_hint(void* context, bool canceled) {
        std::unique_ptr<std::shared_ptr<Registration>> holder(
            static_cast<std::shared_ptr<Registration>*>(context));
        auto registration = *holder;
        {
            std::lock_guard<std::mutex> lock(registration->mutex);
            if (canceled || registration->removed) return;
            registration->running = true;
            registration->thread = GetCurrentThreadId();
        }
        const auto hint = local_hint();
        std::printf("[XNETWORKING] Initial callback initialized=1 level=%u\n",
                    static_cast<unsigned>(hint.connectivityLevel));
        registration->callback(registration->context, &hint);
        {
            std::lock_guard<std::mutex> lock(registration->mutex);
            registration->running = false;
            registration->thread = 0;
        }
        registration->finished.notify_all();
    }
public:
    HRESULT WINAPI QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!equal(iid, interface_id) && !equal(iid, unknown_id)) return E_NOINTERFACE;
        *output = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return ++references_; }
    ULONG WINAPI Release() override { return --references_; }
#define RESERVED(n) HRESULT WINAPI Unknown##n() override { return E_NOTIMPL; }
    RESERVED(3) RESERVED(4) RESERVED(5) RESERVED(6)
    RESERVED(7) RESERVED(8) RESERVED(9) RESERVED(10)
#undef RESERVED
    HRESULT WINAPI Security(const wchar_t* url, XAsyncBlock* async) override {
        std::printf("[XNETWORKING] Security local_http=%d\n", local_http(url));
        if (!async || !url) return E_INVALIDARG;
        if (!local_http(url)) return E_NOTIMPL;
        return begin(async, nullptr, security_identity, "LoopbackSecurity", &security_provider);
    }
    HRESULT WINAPI SecuritySize(XAsyncBlock* async, size_t* bytes) override {
        HRESULT hr = result_size(async, bytes);
        std::printf("[XNETWORKING] SecuritySize hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT WINAPI SecurityResult(XAsyncBlock* async, size_t bytes, size_t* used,
                                  uint8_t* buffer, XNetworkingSecurityInformation** info) override {
        if (!info) return E_POINTER;
        *info = nullptr;
        const HRESULT hr = result(async, security_identity, bytes, buffer, used);
        if (SUCCEEDED(hr)) *info = reinterpret_cast<XNetworkingSecurityInformation*>(buffer);
        return hr;
    }
    HRESULT WINAPI Verify(void* request, const XNetworkingSecurityInformation* info) override {
        const HRESULT hr = verify_plain_loopback(request, info);
        std::printf("[XNETWORKING] Verify plain_loopback=%d hr=0x%08lx\n",
                    hr == S_OK, static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT WINAPI Hint(XNetworkingConnectivityHint* hint) override {
        if (!hint) return E_POINTER;
        *hint = local_hint();
        std::printf("[XNETWORKING] Hint initialized=1 level=%u\n", static_cast<unsigned>(hint->connectivityLevel));
        return S_OK;
    }
    HRESULT WINAPI Register(XTaskQueueHandle queue, void* context,
                            XNetworkingConnectivityHintChangedCallback* callback,
                            XTaskQueueRegistrationToken* token) override {
        if (!token) return E_POINTER;
        token->token = 0;
        if (!callback) return E_INVALIDARG;
        XTaskQueueHandle owned_queue = nullptr;
        HRESULT hr = queue ? duplicate_queue(queue, &owned_queue) :
            (process_queue(&owned_queue) ? S_OK : E_NO_TASK_QUEUE);
        if (FAILED(hr)) return hr;
        std::shared_ptr<Registration> registration;
        std::shared_ptr<Registration>* pending = nullptr;
        uint64_t value = 0;
        try {
            registration = std::make_shared<Registration>();
            registration->context = context;
            registration->callback = callback;
            pending = new std::shared_ptr<Registration>(registration);
            std::lock_guard<std::mutex> lock(mutex_);
            if (next_ == 0) throw std::bad_alloc();
            value = next_++;
            registrations_.emplace(value, registration);
            token->token = value;
        } catch (...) {
            delete pending; close_queue(owned_queue); return E_OUTOFMEMORY;
        }
        // The API requires an initial notification even if connectivity is stable.
        // Submission may invoke immediately; publish the token before submitting.
        hr = submit(owned_queue, XTaskQueuePort::Completion, pending, &initial_hint);
        close_queue(owned_queue);
        if (FAILED(hr)) {
            delete pending;
            Unregister(*token, true);
            token->token = 0;
        }
        std::printf("[XNETWORKING] Register token=%llu hr=0x%08lx\n",
                    static_cast<unsigned long long>(value), static_cast<unsigned long>(hr));
        return hr;
    }
    bool WINAPI Unregister(XTaskQueueRegistrationToken token, bool wait) override {
        std::shared_ptr<Registration> registration;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = registrations_.find(token.token);
            if (found == registrations_.end()) return false;
            registration = found->second;
            registrations_.erase(found);
        }
        std::unique_lock<std::mutex> lock(registration->mutex);
        registration->removed = true;
        // A callback can unregister itself without waiting for its own return.
        if (wait && registration->thread != GetCurrentThreadId())
            registration->finished.wait(lock, [&] { return !registration->running; });
        std::printf("[XNETWORKING] Unregister token=%llu wait=%d\n",
                    static_cast<unsigned long long>(token.token), wait);
        return true;
    }
    size_t live() {
        std::lock_guard<std::mutex> lock(mutex_);
        return registrations_.size();
    }
};

inline Loopback service;
inline HRESULT acquire(const GUID* cls, const GUID* iid, void** output) {
    if (!cls || !iid || !output || !equal(*cls, service_id)) return E_NOINTERFACE;
    return service.QueryInterface(*iid, output);
}
} // namespace xsx::network
