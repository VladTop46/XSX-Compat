#pragma once

// Optional bounded snapshots of the unknown Xbox PSO description. The pairs
// below are candidates, NOT a declaration of the Xbox shader/stage layout.
// ReadProcessMemory failures are recorded rather than dereferenced directly.
static bool ReadPsoMemory(const void* address, void* out, size_t size)
{
    SIZE_T copied = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) && copied == size;
}

static bool SavePsoMemory(const char* path, const void* bytes, size_t size)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, (DWORD)size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

static void CapturePso57(const void* desc, REFIID iid, void* caller)
{
    const char* directory = getenv("XSX_DEV_PSO_CAPTURE");
    if (!directory || !*directory || strlen(directory) > MAX_PATH - 80) return;
    static std::atomic<unsigned> captures{0};
    const unsigned number = captures++;
    if (number >= 8) return; // At most 8 * (256 + 15 * 128 KiB).
    if (!CreateDirectoryA(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        XS_LOG("PSO CAPTURE cannot create %s error=%lu", directory, GetLastError());
        return;
    }
    uint64_t window[32]{};
    if (!ReadPsoMemory(desc, window, sizeof window)) {
        XS_LOG("PSO CAPTURE slot57 #%u unreadable 256-byte diagnostic window desc=%p", number, desc);
        return;
    }
    char stem[MAX_PATH], path[MAX_PATH], who[300];
    snprintf(stem, sizeof stem, "%s\\slot57-%02u", directory, number);
    snprintf(path, sizeof path, "%s-desc.bin", stem);
    const bool saved = SavePsoMemory(path, window, sizeof window);
    CallerStr(caller, who, sizeof who);
    char metadata[8192];
    int used = snprintf(metadata, sizeof metadata,
        "slot=57\ncall=%u\nthread=%lu\ncaller=%s\ndesc=%p\n"
        "iid=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n"
        "window_bytes=256\nwindow_saved=%u\n"
        "pairs_are_untyped_candidates=1\n",
        number, GetCurrentThreadId(), who, desc, iid.Data1, iid.Data2, iid.Data3,
        iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5],
        iid.Data4[6], iid.Data4[7], saved);
    std::vector<unsigned char> bytes;
    for (unsigned q = 1; q + 1 < 32; q += 2) {
        const uint64_t pointer = window[q], length = window[q + 1];
        bool read = false, wrote = false;
        if (pointer >= 0x10000 && length && length <= (128u << 10)) {
            bytes.resize((size_t)length);
            read = ReadPsoMemory((void*)(uintptr_t)pointer, bytes.data(), bytes.size());
            if (read) {
                snprintf(path, sizeof path, "%s-pair%02u.bin", stem, (q - 1) / 2);
                wrote = SavePsoMemory(path, bytes.data(), bytes.size());
            }
        }
        used += snprintf(metadata + used, sizeof metadata - used,
            "pair%02u offset=%#x pointer=%#llx size=%#llx read=%u saved=%u\n",
            (q - 1) / 2, q * 8, (unsigned long long)pointer, (unsigned long long)length, read, wrote);
    }
    snprintf(path, sizeof path, "%s.txt", stem);
    SavePsoMemory(path, metadata, (size_t)used);
    XS_LOG("PSO CAPTURE slot57 #%u desc=%p saved=%u path=%s", number, desc, saved, stem);
}
