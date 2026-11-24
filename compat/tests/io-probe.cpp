// Проверка файлового ввода-вывода в стиле игры: NO_BUFFERING (sync), NO_BUFFERING|OVERLAPPED (event), IOCP. Сравнение с буферизованным чтением.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
static int fails = 0;
static void Probe(const wchar_t* path, unsigned long long off, DWORD len)
{
    wprintf(L"== %ls off=0x%llx len=0x%lx\n", path, off, len);
    HANDLE hb = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hb == INVALID_HANDLE_VALUE) { wprintf(L"  open buffered FAILED %lu\n", GetLastError()); fails++; return; }
    LARGE_INTEGER sz; GetFileSizeEx(hb, &sz); printf("  size=%lld\n", sz.QuadPart);
    std::vector<char> ref(len), buf((size_t)len + 4096);
    char* al = (char*)(((uintptr_t)buf.data() + 4095) & ~4095ull);
    OVERLAPPED ov = {}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32); DWORD got = 0;
    BOOL ok = ReadFile(hb, ref.data(), len, &got, &ov); printf("  buffered(overlap-offset) ok=%d got=%lu err=%lu\n", ok, got, ok ? 0 : GetLastError());
    CloseHandle(hb);
    struct { const char* n; DWORD flags; } modes[] = { {"NO_BUFFERING sync", FILE_FLAG_NO_BUFFERING}, {"NO_BUFFERING|OVERLAPPED+event", FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED}, {"NO_BUFFERING|OVERLAPPED+IOCP", FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED} };
    for (int i = 0; i < 3; i++) {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, modes[i].flags, nullptr);
        if (h == INVALID_HANDLE_VALUE) { printf("  [%s] open FAILED %lu\n", modes[i].n, GetLastError()); fails++; continue; }
        memset(al, 0xCC, len); OVERLAPPED o = {}; o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)(off >> 32); got = 0; DWORD err = 0; BOOL r;
        if (i == 0) { LARGE_INTEGER p; p.QuadPart = off; SetFilePointerEx(h, p, nullptr, FILE_BEGIN); r = ReadFile(h, al, len, &got, nullptr); err = r ? 0 : GetLastError(); }
        else if (i == 1) { o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); r = ReadFile(h, al, len, &got, &o); err = r ? 0 : GetLastError();
            if (!r && err == ERROR_IO_PENDING) { DWORD w = WaitForSingleObject(o.hEvent, 5000); r = GetOverlappedResult(h, &o, &got, FALSE); err = r ? 0 : GetLastError(); if (w != WAIT_OBJECT_0) err = 0xDEAD0000 | w; } CloseHandle(o.hEvent); }
        else { HANDLE port = CreateIoCompletionPort(h, nullptr, 7, 0); r = ReadFile(h, al, len, &got, &o); err = r ? 0 : GetLastError();
            if (!r && err == ERROR_IO_PENDING) { DWORD n; ULONG_PTR key; OVERLAPPED* po; BOOL g = GetQueuedCompletionStatus(port, &n, &key, &po, 5000); got = n; err = g ? 0 : GetLastError(); r = g; }
            else if (r) { DWORD n; ULONG_PTR key; OVERLAPPED* po; GetQueuedCompletionStatus(port, &n, &key, &po, 1000); }
            CloseHandle(port); }
        bool same = (got == len) && !memcmp(al, ref.data(), len);
        printf("  [%-30s] ok=%d got=%lu err=0x%lx data=%s\n", modes[i].n, (int)r, got, err, same ? "MATCH" : "MISMATCH"); fails += !same;
        CloseHandle(h);
    }
}
int main()
{
    Probe(L"G:\\common.rpf", 0, 0x1000);
    Probe(L"G:\\common.rpf", 0x200000, 0x10000);
    Probe(L"G:\\movies.rpf", 0, 0x1000);
    Probe(L"G:\\audio.rpf", 0, 0x4000);
    Probe(L"G:\\audio.rpf", 20ull * 1024 * 1024 * 1024, 0x1000);    // далеко внутри большого файла (может быть не докачано)
    printf("\nSUMMARY fails=%d\n", fails); return fails ? 1 : 0;
}
