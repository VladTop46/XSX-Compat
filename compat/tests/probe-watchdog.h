#pragma once
// Keep a stalled native probe inspectable without requiring an attached debugger.
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

class ProbeWatchdog {
    HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<const char*> stage_{"startup"};
    std::atomic<ULONGLONG> deadline_{GetTickCount64() + 20000};
    std::thread worker_;
public:
    ProbeWatchdog() {
        if (!stop_) throw std::runtime_error("watchdog event");
        try {
            worker_ = std::thread([this] {
                while (WaitForSingleObject(stop_, 250) == WAIT_TIMEOUT) {
                    if (GetTickCount64() < deadline_.load()) continue;
                    std::fprintf(stderr, "[TIMEOUT] stage=%s; writing S:\\logs\\network-timeout.dmp\n", stage_.load());
                    HANDLE file = CreateFileW(L"S:\\logs\\network-timeout.dmp", GENERIC_WRITE,
                        0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    BOOL written = FALSE;
                    if (file != INVALID_HANDLE_VALUE) {
                        written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                            file, MiniDumpWithThreadInfo, nullptr, nullptr, nullptr);
                        CloseHandle(file);
                    }
                    std::fprintf(stderr, "[TIMEOUT] dump_written=%d win32=%lu\n", written, GetLastError());
                    std::fflush(nullptr);
                    ExitProcess(124);
                }
            });
        } catch (...) { CloseHandle(stop_); throw; }
    }
    ~ProbeWatchdog() {
        SetEvent(stop_);
        worker_.join();
        CloseHandle(stop_);
    }
    void stage(const char* name) {
        deadline_ = GetTickCount64() + 20000;
        stage_ = name;
        std::printf("[STAGE] %s\n", name);
    }
};
