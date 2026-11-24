// Exercise the game's original libHttpClient and XCurl. All URLs are loopback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <XGameRuntimeInit.h>
#include <httpClient/httpClient.h>
#include <httpClient/httpProvider.h>
#include <httpClient/mock.h>
#include <XCurl.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../runtime/network/xnetworking_loopback.h"
#include "xcurl-runtime-trace.h"
#include "probe-watchdog.h"

unsigned checks = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)
template<class T> T symbol(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("missing export ") + name);
    T result;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

#define HC_FUNCTIONS(X) \
    X(HCInitialize) X(HCIsInitialized) X(HCCleanupAsync) \
    X(HCHttpCallCreate) X(HCHttpCallCloseHandle) X(HCHttpCallDuplicateHandle) \
    X(HCHttpCallRequestSetUrl) X(HCHttpCallRequestGetUrl) \
    X(HCHttpCallRequestSetHeader) X(HCHttpCallRequestGetHeader) \
    X(HCHttpCallRequestSetRequestBodyBytes) X(HCHttpCallRequestGetRequestBodyBytes) \
    X(HCHttpCallRequestSetRetryAllowed) X(HCHttpCallRequestSetTimeout) \
    X(HCHttpCallRequestSetTimeoutWindow) X(HCHttpCallPerformAsync) \
    X(HCHttpCallResponseGetStatusCode) X(HCHttpCallResponseGetNetworkErrorCode) \
    X(HCHttpCallResponseGetResponseBodyBytesSize) X(HCHttpCallResponseGetResponseBodyBytes) \
    X(HCHttpCallResponseGetHeader) X(HCMockCallCreate) X(HCMockCallCloseHandle) \
    X(HCMockAddMock) X(HCMockClearMocks) X(HCMockResponseSetStatusCode) \
    X(HCMockResponseSetResponseBodyBytes) X(HCMockResponseSetHeader) \
    X(HCMockResponseSetNetworkErrorCode)
struct HttpClient {
    HMODULE module;
#define MEMBER(name) decltype(&::name) name;
    HC_FUNCTIONS(MEMBER)
#undef MEMBER
    HttpClient() : module(LoadLibraryW(L"libHttpClient.GDK.dll")) {
        CHECK(module != nullptr);
#define LOAD(name) name = symbol<decltype(name)>(module, #name);
        HC_FUNCTIONS(LOAD)
#undef LOAD
    }
};

XTaskQueueHandle task_queue = nullptr;
decltype(&::XAsyncGetStatus) async_status = nullptr;
struct Completion {
    XAsyncBlock block{};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<unsigned> callbacks{0};
    Completion() {
        if (!event) throw std::runtime_error("CreateEventW");
        block.queue = task_queue;
        block.context = this;
        block.callback = [](XAsyncBlock* async) {
            auto* self = static_cast<Completion*>(async->context);
            ++self->callbacks;
            SetEvent(self->event);
        };
    }
    ~Completion() { CloseHandle(event); }
    HRESULT wait() {
        CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
        const HRESULT status = async_status(&block, true); // Wait for callback return too.
        CHECK(callbacks == 1);
        CHECK(status != E_PENDING);
        return status;
    }
};

const uint8_t request_body[] = {'a', 0, 'b', 0xff};
const uint8_t response_body[] = {'x', 0, 's', 'x', 0x80};

class LoopbackServer {
    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
public:
    unsigned short port = 0;
    std::atomic<unsigned> good_requests{0}, errors{0};
    LoopbackServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("loopback socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener_, 8)) {
            closesocket(listener_);
            throw std::runtime_error("loopback bind/listen");
        }
        int length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length)) {
            closesocket(listener_);
            throw std::runtime_error("getsockname");
        }
        port = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        closesocket(listener_);
    }
    std::string url(const char* path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
private:
    void serve() noexcept {
        while (!stop_) {
            fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable);
            timeval timeout{0, 100000};
            int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == 0) continue;
            if (ready == SOCKET_ERROR) { ++errors; return; }
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) { ++errors; continue; }
            DWORD milliseconds = 2000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&milliseconds), sizeof(milliseconds));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&milliseconds), sizeof(milliseconds));
            try { respond(client); } catch (...) { ++errors; }
            closesocket(client);
        }
    }
    void respond(SOCKET client) {
        std::string request;
        size_t header_end = std::string::npos, needed = 0;
        while (request.size() < 65536) {
            char data[2048]; int count = recv(client, data, sizeof(data), 0);
            if (count <= 0) throw std::runtime_error("request recv");
            request.append(data, count);
            header_end = request.find("\r\n\r\n");
            if (header_end == std::string::npos) continue;
            auto lower = request.substr(0, header_end);
            for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            size_t field = lower.find("\r\ncontent-length:");
            needed = field == std::string::npos ? 0 : std::stoul(lower.substr(field + 17));
            if (needed > 65536) throw std::runtime_error("request size");
            if (request.size() >= header_end + 4 + needed) break;
        }
        if (header_end == std::string::npos || request.size() < header_end + 4 + needed)
            throw std::runtime_error("incomplete request");
        auto headers = request.substr(0, header_end);
        for (auto& c : headers) c = char(std::tolower(static_cast<unsigned char>(c)));
        bool good = request.rfind("POST /hc ", 0) == 0 && needed == sizeof(request_body) &&
            request.compare(header_end + 4, needed, reinterpret_cast<const char*>(request_body), sizeof(request_body)) == 0 &&
            headers.find("x-xsx-probe: request") != std::string::npos;
        good = good || request.rfind("GET /curl ", 0) == 0;
        if (good) ++good_requests; else ++errors;
        std::string response = std::string("HTTP/1.1 ") + (good ? "200 OK" : "400 Bad Request") +
            "\r\nContent-Length: " + std::to_string(sizeof(response_body)) +
            "\r\nX-XSX-Probe: response\r\nConnection: close\r\n\r\n";
        response.append(reinterpret_cast<const char*>(response_body), sizeof(response_body));
        for (size_t sent = 0; sent < response.size();) {
            int count = send(client, response.data() + sent, int(response.size() - sent), 0);
            if (count <= 0) throw std::runtime_error("response send");
            sent += size_t(count);
        }
    }
};

void http_call(HttpClient& hc, const std::string& url, bool mock, HRESULT error = S_OK) {
    HCMockCallHandle fixture = nullptr;
    if (mock) {
        CHECK(hc.HCMockCallCreate(&fixture) == S_OK && fixture);
        CHECK(hc.HCMockResponseSetStatusCode(fixture, FAILED(error) ? 0 : 503) == S_OK);
        CHECK(hc.HCMockResponseSetNetworkErrorCode(fixture, error, FAILED(error) ? ERROR_WINHTTP_CANNOT_CONNECT : 0) == S_OK);
        CHECK(hc.HCMockResponseSetHeader(fixture, "X-XSX-Probe", "response") == S_OK);
        CHECK(hc.HCMockResponseSetResponseBodyBytes(fixture, response_body, sizeof(response_body)) == S_OK);
        CHECK(hc.HCMockAddMock(fixture, "POST", url.c_str(), request_body, sizeof(request_body)) == S_OK);
    }
    HCCallHandle call = nullptr;
    CHECK(hc.HCHttpCallCreate(&call) == S_OK && call);
    CHECK(hc.HCHttpCallRequestSetUrl(call, "POST", url.c_str()) == S_OK);
    CHECK(hc.HCHttpCallRequestSetHeader(call, "X-XSX-Probe", "request", false) == S_OK);
    CHECK(hc.HCHttpCallRequestSetRequestBodyBytes(call, request_body, sizeof(request_body)) == S_OK);
    CHECK(hc.HCHttpCallRequestSetRetryAllowed(call, false) == S_OK);
    CHECK(hc.HCHttpCallRequestSetTimeout(call, 2) == S_OK);
    CHECK(hc.HCHttpCallRequestSetTimeoutWindow(call, 2) == S_OK);
    const char *method = nullptr, *stored_url = nullptr, *header = nullptr;
    CHECK(hc.HCHttpCallRequestGetUrl(call, &method, &stored_url) == S_OK &&
          method && stored_url && std::strcmp(method, "POST") == 0 && stored_url == url);
    CHECK(hc.HCHttpCallRequestGetHeader(call, "X-XSX-Probe", &header) == S_OK && header && std::strcmp(header, "request") == 0);
    const uint8_t* body = nullptr; uint32_t body_size = 0;
    CHECK(hc.HCHttpCallRequestGetRequestBodyBytes(call, &body, &body_size) == S_OK &&
          body_size == sizeof(request_body) && std::memcmp(body, request_body, body_size) == 0);
    HCCallHandle duplicate = hc.HCHttpCallDuplicateHandle(call);
    CHECK(duplicate != nullptr);
    CHECK(hc.HCHttpCallCloseHandle(call) == S_OK); // Duplicate must keep the request alive.
    call = duplicate;
    auto completion = std::make_unique<Completion>();
    HRESULT performed = hc.HCHttpCallPerformAsync(call, &completion->block);
    std::printf("[HTTP] PerformAsync mock=%d expected_error=0x%08lx hr=0x%08lx\n", mock, static_cast<unsigned long>(error), static_cast<unsigned long>(performed));
    CHECK(performed == S_OK);
    HRESULT async_result = completion->wait();
    std::printf("[HTTP] Async result=0x%08lx\n", static_cast<unsigned long>(async_result));
    HRESULT network = E_UNEXPECTED; uint32_t platform = 0, status = 999;
    CHECK(hc.HCHttpCallResponseGetNetworkErrorCode(call, &network, &platform) == S_OK && network == error);
    CHECK(hc.HCHttpCallResponseGetStatusCode(call, &status) == S_OK);
    if (SUCCEEDED(error)) {
        CHECK(status == (mock ? 503u : 200u)); // HTTP error status is distinct from transport failure.
        size_t required = 0, used = 0; uint8_t output[16]{};
        CHECK(hc.HCHttpCallResponseGetResponseBodyBytesSize(call, &required) == S_OK && required == sizeof(response_body));
        CHECK(hc.HCHttpCallResponseGetResponseBodyBytes(call, sizeof(output), output, &used) == S_OK &&
              used == required && std::memcmp(output, response_body, used) == 0);
        CHECK(hc.HCHttpCallResponseGetHeader(call, "X-XSX-Probe", &header) == S_OK && header && std::strcmp(header, "response") == 0);
    } else CHECK(platform == ERROR_WINHTTP_CANNOT_CONNECT);
    CHECK(hc.HCHttpCallCloseHandle(call) == S_OK);
    if (fixture) {
        CHECK(hc.HCMockClearMocks() == S_OK);
        CHECK(hc.HCMockCallCloseHandle(fixture) == S_OK);
    }
    std::printf("[PASS] HTTP %s callbacks=1 network=0x%08lx status=%u\n", mock ? "mock" : "loopback", static_cast<unsigned long>(network), status);
}

size_t curl_write(char* data, size_t size, size_t count, void* context) {
    if (size && count > SIZE_MAX / size) return 0;
    try { static_cast<std::string*>(context)->append(data, size * count); }
    catch (...) { return 0; }
    return size * count;
}

char* curl_duplicate(const char* text) {
    size_t size = std::strlen(text) + 1;
    auto* copy = static_cast<char*>(std::malloc(size));
    if (copy) std::memcpy(copy, text, size);
    return copy;
}

void networking_provider_checks(HMODULE runtime) {
    auto create = symbol<decltype(&::XTaskQueueCreate)>(runtime, "XTaskQueueCreate");
    auto dispatch = symbol<decltype(&::XTaskQueueDispatch)>(runtime, "XTaskQueueDispatch");
    auto terminate = symbol<decltype(&::XTaskQueueTerminate)>(runtime, "XTaskQueueTerminate");
    auto close = symbol<decltype(&::XTaskQueueCloseHandle)>(runtime, "XTaskQueueCloseHandle");
    XTaskQueueHandle queue = nullptr;
    CHECK(create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK);
    struct State {
        unsigned calls = 0;
        XNetworkingConnectivityHint hint{};
    } state;
    const auto callback = +[](void* context, const XNetworkingConnectivityHint* hint) {
        auto& observed = *static_cast<State*>(context);
        ++observed.calls; observed.hint = *hint;
    };
    auto& service = xsx::network::service;
    XTaskQueueRegistrationToken token{};
    CHECK(service.Register(queue, &state, callback, &token) == S_OK && token.token != 0);
    CHECK(state.calls == 0); // Completion must use the selected manual queue.
    CHECK(dispatch(queue, XTaskQueuePort::Completion, 100));
    CHECK(state.calls == 1 && state.hint.networkInitialized &&
          state.hint.connectivityLevel == XNetworkingConnectivityLevelHint::LocalAccess);
    CHECK(service.Unregister(token, true));
    CHECK(!service.Unregister(token, true));
    CHECK(service.Register(queue, &state, callback, &token) == S_OK);
    CHECK(service.Unregister(token, true));
    CHECK(dispatch(queue, XTaskQueuePort::Completion, 100));
    CHECK(state.calls == 1); // Unregister suppresses the still-queued callback.
    std::printf("[XNETWORKING] Queued callback suppressed\n");
    // Termination enqueues its own markers on BOTH manual ports. A synchronous
    // wait on their dispatcher thread deadlocks even after user callbacks drain.
    std::atomic<bool> terminated{false};
    CHECK(terminate(queue, false, &terminated, +[](void* context) {
        static_cast<std::atomic<bool>*>(context)->store(true);
    }) == S_OK);
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (!terminated && GetTickCount64() < deadline) {
        dispatch(queue, XTaskQueuePort::Work, 0);
        dispatch(queue, XTaskQueuePort::Completion, 1);
    }
    CHECK(terminated);
    std::printf("[XNETWORKING] Manual queue terminated\n");
    CHECK(FAILED(service.Register(queue, &state, callback, &token)) && token.token == 0);
    close(queue);
    CHECK(service.live() == 0);
    CHECK(xsx::network::local_http(L"http://127.0.0.1:1/path"));
    CHECK(!xsx::network::local_http(L"https://127.0.0.1/path"));
    CHECK(!xsx::network::local_http(L"http://127.0.0.1.example/path"));
    CHECK(!xsx::network::local_http(L"http://user@127.0.0.1/path"));
    CHECK(!xsx::network::local_http(L"http://example.com/path"));
    std::printf("[PASS] XNetworking initial_callback=1 queued_cancel=1 live=0\n");

    struct Internet {
        HINTERNET handle;
        ~Internet() { if (handle) WinHttpCloseHandle(handle); }
    } session{WinHttpOpen(L"XSXLoopbackProbe", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0)};
    CHECK(session.handle != nullptr);
    Internet connection{WinHttpConnect(session.handle, L"127.0.0.1", 9, 0)};
    CHECK(connection.handle != nullptr);
    Internet plain{WinHttpOpenRequest(connection.handle, L"GET", L"/", nullptr, nullptr, nullptr, 0)};
    Internet secure{WinHttpOpenRequest(connection.handle, L"GET", L"/", nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)};
    CHECK(plain.handle && secure.handle);
    XNetworkingSecurityInformation info{};
    CHECK(service.Verify(plain.handle, &info) == S_OK);
    CHECK(service.Verify(secure.handle, &info) == E_NOTIMPL);
    CHECK(FAILED(service.Verify(session.handle, &info))); // Not a request handle.
    CHECK(FAILED(service.Verify(nullptr, &info)));
    info.thumbprintCount = 1;
    CHECK(service.Verify(plain.handle, &info) == E_NOTIMPL);
    // Creating WinHTTP handles performs no request or DNS lookup.
    Internet external{WinHttpConnect(session.handle, L"example.invalid", 80, 0)};
    CHECK(external.handle != nullptr);
    Internet external_request{WinHttpOpenRequest(external.handle, L"GET", L"/", nullptr, nullptr, nullptr, 0)};
    CHECK(external_request.handle != nullptr);
    info = {};
    CHECK(service.Verify(external_request.handle, &info) == E_NOTIMPL);
    std::printf("[PASS] XNetworking verify_plain_loopback=1 https_rejected=1 external_rejected=1\n");
}

void curl_call(const std::string& url, ProbeWatchdog& watchdog) {
    HMODULE module = LoadLibraryW(L"XCurl.dll");
    CHECK(module != nullptr);
    xcurl_trace::Imports trace(module);
#define LOAD(name) auto name = symbol<decltype(&::name)>(module, #name);
    LOAD(curl_global_init_mem) LOAD(curl_global_cleanup) LOAD(curl_easy_init)
    LOAD(curl_easy_cleanup) LOAD(curl_easy_setopt) LOAD(curl_easy_perform) LOAD(curl_easy_getinfo) LOAD(curl_easy_strerror)
#undef LOAD
    CURLcode initialized = curl_global_init_mem(CURL_GLOBAL_DEFAULT, &std::malloc, &std::free,
                                               &std::realloc, &curl_duplicate, &std::calloc);
    std::printf("[XCURL] GlobalInit explicit_allocators=1 result=%d message=%s\n", int(initialized), curl_easy_strerror(initialized));
    CHECK(initialized == CURLE_OK);
    std::string body; // Keep callback storage alive through easy/global cleanup.
    struct Cleanup {
        CURL* call = nullptr;
        decltype(curl_easy_cleanup) close;
        decltype(curl_global_cleanup) shutdown;
        ProbeWatchdog& watchdog;
        ~Cleanup() {
            watchdog.stage("XCurl easy cleanup");
            if (call) close(call);
            watchdog.stage("XCurl global cleanup");
            shutdown();
        }
    } cleanup{nullptr, curl_easy_cleanup, curl_global_cleanup, watchdog};
    CURL* call = cleanup.call = curl_easy_init(); CHECK(call != nullptr);
    CHECK(curl_easy_setopt(call, CURLOPT_URL, url.c_str()) == CURLE_OK);
    // XCurl has a smaller option set than libcurl. Do not require proxy support
    // for a numeric-loopback request; the process-level timeout remains bounded.
    CURLcode timeout = curl_easy_setopt(call, CURLOPT_TIMEOUT, 3L);
    std::printf("[XCURL] CURLOPT_TIMEOUT result=%d\n", int(timeout));
    CHECK(timeout == CURLE_OK || timeout == CURLE_NOT_BUILT_IN);
    CHECK(curl_easy_setopt(call, CURLOPT_WRITEFUNCTION, &curl_write) == CURLE_OK);
    CHECK(curl_easy_setopt(call, CURLOPT_WRITEDATA, &body) == CURLE_OK);
    watchdog.stage("XCurl perform");
    CURLcode result = curl_easy_perform(call);
    std::printf("[XCURL] Perform result=%d\n", int(result));
    CHECK(result == CURLE_OK);
    long status = 0;
    CHECK(curl_easy_getinfo(call, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200);
    CHECK(body.size() == sizeof(response_body) && std::memcmp(body.data(), response_body, body.size()) == 0);
    std::printf("[PASS] XCurl loopback bytes=%zu status=%ld\n", body.size(), status);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        ProbeWatchdog watchdog;
        watchdog.stage("runtime initialization");
        const bool offline = argc == 2 && std::strcmp(argv[1], "--offline") == 0;
        CHECK(argc == 1 || offline);
        WSADATA data{}; CHECK(WSAStartup(MAKEWORD(2,2), &data) == 0);
        HMODULE runtime = LoadLibraryW(L"xgameruntime.thunks.dll");
        CHECK(runtime != nullptr);
        auto initialize = symbol<decltype(&::XGameRuntimeInitialize)>(runtime, "XGameRuntimeInitialize");
        auto uninitialize = symbol<decltype(&::XGameRuntimeUninitialize)>(runtime, "XGameRuntimeUninitialize");
        auto create_queue = symbol<decltype(&::XTaskQueueCreate)>(runtime, "XTaskQueueCreate");
        auto terminate_queue = symbol<decltype(&::XTaskQueueTerminate)>(runtime, "XTaskQueueTerminate");
        auto close_queue = symbol<decltype(&::XTaskQueueCloseHandle)>(runtime, "XTaskQueueCloseHandle");
        async_status = symbol<decltype(async_status)>(runtime, "XAsyncGetStatus");
        xsx::network::initialize(runtime);
        HRESULT runtime_result = initialize();
        std::printf("[RUNTIME] Initialize hr=0x%08lx\n", static_cast<unsigned long>(runtime_result));
        CHECK(runtime_result == S_OK);
        watchdog.stage("XNetworking queue regression");
        networking_provider_checks(runtime);
        CHECK(create_queue(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &task_queue) == S_OK);
        std::unique_ptr<LoopbackServer> server;
        if (!offline) server = std::make_unique<LoopbackServer>();
        HttpClient hc;
        watchdog.stage("HC initialization and mocks");
        HRESULT initialized = hc.HCInitialize(nullptr);
        std::printf("[HTTP] HCInitialize hr=0x%08lx\n", static_cast<unsigned long>(initialized));
        CHECK(initialized == S_OK && hc.HCIsInitialized());
        const std::string base = server ? server->url("") : "http://127.0.0.1:9";
        http_call(hc, base + "/mock-status", true);
        http_call(hc, base + "/mock-network", true, HRESULT_FROM_WIN32(ERROR_WINHTTP_CANNOT_CONNECT));
        bool curl_ok = true;
        bool server_ok = true;
        if (server) {
            watchdog.stage("HC loopback POST");
            http_call(hc, server->url("/hc"), false);
            watchdog.stage("XCurl initialization");
            try { curl_call(server->url("/curl"), watchdog); }
            catch (const std::exception& error) {
                curl_ok = false;
                std::fprintf(stderr, "[FAIL] XCurl %s\n", error.what());
            }
            const unsigned requests = server->good_requests;
            std::printf("[HTTP SERVER] good_requests=%u errors=%u\n", requests, server->errors.load());
            // Verification can fail after the server has received the request.
            // Preserve cleanup and report the transport failure independently.
            server_ok = server->errors == 0 &&
                (curl_ok ? requests == 2 : (requests >= 1 && requests <= 2));
        }
        watchdog.stage("HC cleanup");
        auto cleanup = std::make_unique<Completion>();
        CHECK(hc.HCCleanupAsync(&cleanup->block) == S_OK);
        CHECK(cleanup->wait() == S_OK);
        CHECK(!hc.HCIsInitialized());
        watchdog.stage("queue and runtime cleanup");
        CHECK(terminate_queue(task_queue, true, nullptr, nullptr) == S_OK);
        CHECK(xsx::network::service.live() == 0);
        close_queue(task_queue);
        uninitialize();
        server.reset(); WSACleanup();
        CHECK(server_ok);
        std::printf("[%s] network probe checks=%u offline=%d cleanup_callbacks=1\n", curl_ok ? "PASS" : "FAIL", checks, offline);
        return curl_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] network %s win32=%lu wsa=%d\n", error.what(), GetLastError(), WSAGetLastError());
        return 1;
    }
}
