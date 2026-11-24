// Тест XCRDAPI (Xbox DStorage поверх Win32): file->memory, status array, ошибки, error event. Использует G:\appdata0.rpf.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
static int pass = 0, fail = 0;
#define CHECK(c, ...) do { if (c) { pass++; printf("[PASS] "); } else { fail++; printf("[FAIL] "); } printf(__VA_ARGS__); putchar('\n'); } while (0)
struct Options { uint64_t b; };
struct Request { uint64_t options; void* dest; uint64_t pageOff; uint64_t destSize; void* file; uint64_t fileOffset; uint64_t sourceSize, interSize, tag; const char* name; };
struct QDesc { uint64_t srcType; uint16_t cap; int8_t prio; const char* name; };
struct ErrRec { uint32_t count; HRESULT hr; };
typedef HRESULT (WINAPI* GetFactoryFn)(REFIID, void**);
struct VT { void** v; };
#define CALL(o, idx, T, ...) ((T)((*(void***)(o))[idx]))(o, ##__VA_ARGS__)
int main(int argc, char** argv)
{
    HMODULE m = LoadLibraryW(L"XCRDAPI.dll"); auto get = (GetFactoryFn)GetProcAddress(m, "DStorageGetFactory");
    CHECK(get, "export DStorageGetFactory");
    GUID iid = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
    void* fac = nullptr; CHECK(SUCCEEDED(get(iid, &fac)) && fac, "factory");
    GUID bad = {1,2,3,{4,5,6,7,8,9,10,11}}; void* x = (void*)1; CHECK(get(bad, &x) == E_NOINTERFACE && !x, "wrong IID -> E_NOINTERFACE");
    CHECK(SUCCEEDED(CALL(fac, 8, HRESULT(WINAPI*)(void*, uint32_t), 0x2000000)), "SetStagingBufferSize");
    void* file = nullptr; GUID anyIid = {0xAAAA, 1, 2, {3,3,3,3,3,3,3,3}};
    HRESULT hr = CALL(fac, 4, HRESULT(WINAPI*)(void*, const wchar_t*, REFIID, void**), L"G:\\appdata0.rpf", anyIid, &file);
    CHECK(SUCCEEDED(hr) && file, "OpenFile appdata0.rpf hr=0x%08lX", (unsigned long)hr);
    void* nofile = nullptr; hr = CALL(fac, 4, HRESULT(WINAPI*)(void*, const wchar_t*, REFIID, void**), L"G:\\does-not-exist.rpf", anyIid, &nofile);
    CHECK(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !nofile, "OpenFile missing -> 0x%08lX", (unsigned long)hr);
    QDesc qd = { 0, 128, 0, "test" }; void* q = nullptr;
    CHECK(SUCCEEDED(CALL(fac, 3, HRESULT(WINAPI*)(void*, const QDesc*, REFIID, void**), &qd, anyIid, &q)) && q, "CreateQueue");
    void* st = nullptr; CHECK(SUCCEEDED(CALL(fac, 5, HRESULT(WINAPI*)(void*, uint32_t, const char*, REFIID, void**), 4, "st", anyIid, &st)) && st, "CreateStatusArray");

    // эталон: прямое чтение
    HANDLE h = CreateFileW(L"G:\\appdata0.rpf", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD fsz = GetFileSize(h, nullptr); uint8_t ref[256] = {}; DWORD got = 0; ReadFile(h, ref, 256, &got, nullptr); CloseHandle(h);
    printf("[INFO] appdata0.rpf size=%lu first 16 bytes:", fsz); for (int i = 0; i < 16; i++) printf(" %02x", ref[i]); puts("");

    uint8_t buf[256] = {}, buf2[64] = {};
    Request r = {}; r.dest = buf; r.destSize = 256; r.file = file; r.fileOffset = 0; r.sourceSize = 256; r.name = "read0";
    CALL(q, 3, void(WINAPI*)(void*, const Request*), &r);
    CALL(q, 4, void(WINAPI*)(void*, void*, uint32_t), st, 0);                     // статус 0: после успешного чтения
    Request r2 = r; r2.dest = buf2; r2.destSize = 64; r2.fileOffset = fsz + 4096; r2.sourceSize = 64; r2.name = "past-eof";   // ошибка
    CALL(q, 3, void(WINAPI*)(void*, const Request*), &r2);
    CALL(q, 4, void(WINAPI*)(void*, void*, uint32_t), st, 1);
    Request r3 = {}; r3.options = 1ull << 21; /* ZlibDecompress бит 0 =1? см. ниже */ r3.options = 1; r3.dest = buf2; r3.destSize = 64; r3.file = file; r3.sourceSize = 16; r3.name = "zlib";
    CALL(q, 3, void(WINAPI*)(void*, const Request*), &r3);
    CALL(q, 4, void(WINAPI*)(void*, void*, uint32_t), st, 2);
    CALL(q, 6, void(WINAPI*)(void*)), (void)0;                                    // Submit
    typedef bool (WINAPI* IsCompleteFn)(void*, uint32_t); typedef HRESULT (WINAPI* GetHrFn)(void*, uint32_t);
    for (int i = 0; i < 300 && !CALL(st, 3, IsCompleteFn, 2); i++) Sleep(10);
    CHECK(CALL(st, 3, IsCompleteFn, 0) && CALL(st, 3, IsCompleteFn, 1) && CALL(st, 3, IsCompleteFn, 2), "all statuses complete");
    CHECK(CALL(st, 4, GetHrFn, 0) == S_OK && memcmp(buf, ref, got) == 0, "status0 OK, data == direct ReadFile (%lu bytes)", got);
    HRESULT h1 = CALL(st, 4, GetHrFn, 1), h2 = CALL(st, 4, GetHrFn, 2);
    CHECK(FAILED(h1), "status1 (read past EOF) failed hr=0x%08lX", (unsigned long)h1);
    CHECK(FAILED(h2), "status2 (zlib на не-zlib данных) -> ошибка hr=0x%08lX", (unsigned long)h2);
    HANDLE ev = CALL(q, 9, HANDLE(WINAPI*)(void*)); CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "error event signalled");
    ErrRec rec = {}; CALL(q, 10, void(WINAPI*)(void*, ErrRec*), &rec); CHECK(rec.count >= 1, "RetrieveErrorRecord failureCount=%u hr=0x%08lX", rec.count, (unsigned long)rec.hr);
    // --- zlib: наборы zt_*.z/.raw (сгенерированы эталонным zlib на хосте) ---
    { FILE* lf = fopen("G:\\zt_list.txt", "rb"); char nm[128];
      while (lf && fscanf(lf, "%127s", nm) == 1) {
        char zp[200], rp[200]; snprintf(zp, 200, "G:\\zt_%s.z", nm); snprintf(rp, 200, "G:\\zt_%s.raw", nm);
        FILE* rf = fopen(rp, "rb"); fseek(rf, 0, SEEK_END); long rsz = ftell(rf); fseek(rf, 0, SEEK_SET); std::vector<uint8_t> raw(rsz); if (rsz) fread(raw.data(), 1, rsz, rf); fclose(rf);
        FILE* zf = fopen(zp, "rb"); fseek(zf, 0, SEEK_END); long zsz = ftell(zf); fclose(zf);
        wchar_t wz[200]; MultiByteToWideChar(CP_ACP, 0, zp, -1, wz, 200); void* zfile = nullptr; CALL(fac, 4, HRESULT(WINAPI*)(void*, const wchar_t*, REFIID, void**), wz, anyIid, &zfile);
        std::vector<uint8_t> out(rsz + 16, 0xEE); Request zr = {}; zr.options = 1; zr.dest = out.data(); zr.destSize = (uint32_t)rsz; zr.file = zfile; zr.sourceSize = (uint32_t)zsz; zr.name = "z";
        void* st2 = nullptr; CALL(fac, 5, HRESULT(WINAPI*)(void*, uint32_t, const char*, REFIID, void**), 1, "z", anyIid, &st2);
        CALL(q, 3, void(WINAPI*)(void*, const Request*), &zr); CALL(q, 4, void(WINAPI*)(void*, void*, uint32_t), st2, 0); CALL(q, 6, void(WINAPI*)(void*));
        typedef bool (WINAPI* IsC)(void*, uint32_t); typedef HRESULT (WINAPI* GH)(void*, uint32_t);
        for (int i = 0; i < 500 && !CALL(st2, 3, IsC, 0); i++) Sleep(10);
        bool same = CALL(st2, 4, GH, 0) == S_OK && memcmp(out.data(), raw.data(), rsz) == 0 && out[rsz] == 0xEE;
        CHECK(same, "zlib %-14s compressed=%ld raw=%ld hr=0x%08lX", nm, zsz, rsz, (unsigned long)CALL(st2, 4, GH, 0));
        CALL(st2, 2, ULONG(WINAPI*)(void*)); if (zfile) CALL(zfile, 2, ULONG(WINAPI*)(void*)); }
      if (lf) fclose(lf); }
    CALL(q, 2, ULONG(WINAPI*)(void*)); CALL(st, 2, ULONG(WINAPI*)(void*)); CALL(file, 2, ULONG(WINAPI*)(void*));
    printf("\nSUMMARY pass=%d fail=%d\n", pass, fail); return fail ? 1 : 0;
}
