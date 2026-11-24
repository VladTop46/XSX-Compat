// XCRDAPI.dll — реализация Xbox DStorage (DStorageGetFactory) поверх обычных Win32-файлов.
// Раскладки vtable ВЫВЕДЕНЫ по сэмплу DirectStorageWin32 (Microsoft ATG, переписанный Xbox DStorage API):
//   Factory(IID 62959d11… у игры): 3 CreateQueue, 4 OpenFile, 5 CreateStatusArray, 6 SetCpuAffinity, 7 SetDebugFlags, 8 SetStagingBufferSize
//   File:   3 Close, 4 GetHandle              Status: 3 IsComplete, 4 GetHResult
//   Queue:  3 EnqueueRequest, 4 EnqueueStatus, 5 EnqueueSignal, 6 Submit, 7 CancelRequestsWithTag, 8 Close,
//           9 GetErrorEvent, 10 RetrieveErrorRecord, 11 Query
// IID возвращаемых объектов у Xbox свои (не равны wrapper-овским); принимаем любой riid и логируем.
// Подтверждено только совпадением слота 8 фабрики с вызовом игры. Реализовано: file->memory read, memory->memory copy,
// status/fence/error event. НЕ реализовано: ZlibDecompress, Bcpack, swizzle, physical pages (запрос отклоняется с ошибкой).
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include "inflate.h"
#include "third_party/miniz/miniz.h"

#define DLOG(...) do { fprintf(stderr, "[XCRDAPI] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
static const GUID IID_Factory = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
static const GUID kIUnknown = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const HRESULT E_DS_UNSUPPORTED = E_NOTIMPL;

// --- структуры (по dstorage_win32.h) ---
struct Options { uint64_t zlib : 1, bcpack : 4, swizzle : 16, destPhys : 1, srcType : 1, srcPhys : 1, rsv : 40; };
// Раскладка DSTORAGE_REQUEST у Xbox-игры (по дампу EnqueueRequest, все поля 8-байтовые): {options, Destination, +0x10 (=0, вероятно PageOffset), DestinationSize,
//  File*(или Source*), FileOffset, SourceSize, IntermediateSize, CancellationTag, Name*}. Отличается от wrapper-сэмпла (там поля 32-битные).
struct Request {
    union { uint64_t asU64; Options o; } options;
    void* destination; uint64_t destPageOffset; uint64_t destSize;
    union { void* file; void* source; }; uint64_t fileOffset;
    uint64_t sourceSize, intermediateSize, cancelTag; const char* name;
};
struct QueueDesc { uint64_t sourceType; uint16_t capacity; int8_t priority; const char* name; };
struct QueueInfo { QueueDesc desc; uint16_t emptySlots, untilAutoSubmit; };
struct ErrorRecord {
    uint32_t failureCount;
    struct { HRESULT hr; uint64_t options; void* dest; uint32_t destSize; WCHAR filename[MAX_PATH]; uint64_t fileOffset; void* source; uint32_t sourceSize; uint64_t tag; char name[64]; } first;
};

template <class T> struct Com {   // базовый COM-объект: vt[0..2] = QI/AddRef/Release
    void** vt; volatile LONG ref; GUID iid;
};

// ---------- File ----------
struct File { void** vt; volatile LONG ref; GUID iid; HANDLE h; std::wstring path; };
#include <set>
static std::set<void*> g_files; static std::mutex g_filesM;
static bool IsFile(const void* p) { std::lock_guard<std::mutex> l(g_filesM); return g_files.count((void*)p) != 0; }
static HRESULT STDMETHODCALLTYPE QI_generic(void* self, REFIID r, void** out)
{
    auto* o = (File*)self; if (!out) return E_POINTER;
    if (IsEqualGUID(r, o->iid) || IsEqualGUID(r, kIUnknown)) { *out = self; InterlockedIncrement(&o->ref); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE AddRef_g(void* self) { return InterlockedIncrement(&((File*)self)->ref); }
static void FileClose(File* f) { if (f->h != INVALID_HANDLE_VALUE) { CloseHandle(f->h); f->h = INVALID_HANDLE_VALUE; } }
static ULONG STDMETHODCALLTYPE File_Release(File* f) { LONG r = InterlockedDecrement(&f->ref); if (!r) { { std::lock_guard<std::mutex> l(g_filesM); g_files.erase(f); } FileClose(f); delete f; } return r; }
static void STDMETHODCALLTYPE File_Close(File* f) { FileClose(f); }
static HANDLE STDMETHODCALLTYPE File_GetHandle(File* f) { return f->h; }
static void* g_fileVt[5] = { (void*)QI_generic, (void*)AddRef_g, (void*)File_Release, (void*)File_Close, (void*)File_GetHandle };

// ---------- Status array ----------
struct Status { void** vt; volatile LONG ref; GUID iid; std::vector<HRESULT> hr; std::vector<char> done; std::vector<uint64_t> gen; std::mutex m; };
static ULONG STDMETHODCALLTYPE Status_Release(Status* s) { LONG r = InterlockedDecrement(&s->ref); if (!r) delete s; return r; }
static bool STDMETHODCALLTYPE Status_IsComplete(Status* s, uint32_t i) {
    if (getenv("XSX_DS_LOG")) { static std::atomic<long> k{0}; if (k++ < 200) { std::lock_guard<std::mutex> g(s->m); DLOG("IsComplete status=%p idx=%u -> %d", (void*)s, i, i < s->done.size() ? (int)s->done[i] : -1); } } std::lock_guard<std::mutex> g(s->m); return i < s->done.size() && s->done[i]; }
static HRESULT STDMETHODCALLTYPE Status_GetHResult(Status* s, uint32_t i) { std::lock_guard<std::mutex> g(s->m); return i < s->hr.size() ? s->hr[i] : E_INVALIDARG; }
template <int K, int N> static uintptr_t __stdcall Obj_Unknown(void* self, uintptr_t a, uintptr_t b, uintptr_t c)
{ static std::atomic<int> n[2][32]; if (n[K][N]++ < 4) DLOG("%s slot %d НЕ реализован (this=%p a=%p b=%p c=%p) -> 0", K ? "queue" : "status", N, self, (void*)a, (void*)b, (void*)c); return 0; }
// Status: наблюдалось, что игра вызывает слот 9 (+0x48) с (fn, ctx) — похоже на регистрацию колбэка завершения; пока лог-заглушка (HRESULT 0).
static void* g_statusVt[16] = { (void*)QI_generic, (void*)AddRef_g, (void*)Status_Release, (void*)Status_IsComplete, (void*)Status_GetHResult,
    (void*)Obj_Unknown<0,5>, (void*)Obj_Unknown<0,6>, (void*)Obj_Unknown<0,7>, (void*)Obj_Unknown<0,8>, (void*)Obj_Unknown<0,9>, (void*)Obj_Unknown<0,10>, (void*)Obj_Unknown<0,11>,
    (void*)Obj_Unknown<0,12>, (void*)Obj_Unknown<0,13>, (void*)Obj_Unknown<0,14>, (void*)Obj_Unknown<0,15> };

// ---------- Queue ----------
struct Entry { int kind; Request req; std::string name; File* file; Status* st; uint32_t idx; ID3D12Fence* fence; uint64_t val; };  // 0 request, 1 status, 2 signal
struct Queue {
    void** vt; volatile LONG ref; GUID iid;
    QueueDesc desc{}; std::string name;
    std::mutex m; std::condition_variable cv; std::deque<Entry> pending, submitted;
    std::thread th; std::vector<std::thread> pool; int outstanding = 0; bool stop = false; HANDLE errEvent; HRESULT firstErr = S_OK; uint32_t failCount = 0; ErrorRecord rec{};
    Queue() { errEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
};
static HRESULT ReadSource(const Entry& e, std::vector<uint8_t>& tmp)       // источник -> буфер (файл или память)
{
    const Request& r = e.req; tmp.resize(r.sourceSize);
    if (r.options.o.srcType == 1) { memcpy(tmp.data(), r.source, r.sourceSize); return S_OK; }
    File* f = e.file; if (!f || f->h == INVALID_HANDLE_VALUE) return E_HANDLE;
    uint64_t left = r.sourceSize, done = 0;
    while (left) {
        OVERLAPPED ov = {}; uint64_t off = r.fileOffset + done; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0; BOOL ok = ReadFile(f->h, tmp.data() + done, left, &got, &ov); DWORD err = ok ? 0 : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) { ok = GetOverlappedResult(f->h, &ov, &got, TRUE); err = ok ? 0 : GetLastError(); }
        CloseHandle(ov.hEvent);
        if (!ok && err != ERROR_HANDLE_EOF) return HRESULT_FROM_WIN32(err);
        if (!got) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        done += got; left -= got;
    }
    return S_OK;
}
static HRESULT DoRequest(Queue* q, Entry& e)
{
    const Request& r = e.req;
    if (r.options.o.bcpack || r.options.o.swizzle || r.options.o.destPhys || r.options.o.srcPhys) {
        DLOG("request '%s' UNSUPPORTED options=0x%llx (bcpack=%d swz=%d dstPhys=%d srcPhys=%d)", e.name.c_str(), (unsigned long long)r.options.asU64,
             (int)r.options.o.bcpack, (int)r.options.o.swizzle, (int)r.options.o.destPhys, (int)r.options.o.srcPhys);
        return E_DS_UNSUPPORTED;
    }
    if (r.destPageOffset) { static std::atomic<int> k{0}; if (k++ < 8) DLOG("request '%s' destPageOffset=0x%llx НЕ учитывается (dest=%p size=%llu)", e.name.c_str(), (unsigned long long)r.destPageOffset, r.destination, (unsigned long long)r.destSize); }
    { static bool on = [] { char v[16]; return GetEnvironmentVariableA("XSX_DS_LOG", v, sizeof v) > 0; }(); static std::atomic<long> k{0};
      if (on && k++ < 20000) DLOG("REQ file=%ls off=0x%llx src=%llu dst=%llu inter=%llu opt=0x%llx dest=%p", e.file ? e.file->path.c_str() : L"(mem)", (unsigned long long)r.fileOffset, (unsigned long long)r.sourceSize, (unsigned long long)r.destSize, (unsigned long long)r.intermediateSize, (unsigned long long)r.options.asU64, r.destination); }
    std::vector<uint8_t> tmp; HRESULT hr = ReadSource(e, tmp); if (FAILED(hr)) return hr;
    if (r.options.o.zlib) {
        size_t n = r.destSize; int rc = 0;
        { size_t got = tinfl_decompress_mem_to_mem(r.destination, r.destSize, tmp.data(), tmp.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
          if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) { n = r.destSize; rc = infl::inflate_zlib(tmp.data(), tmp.size(), (uint8_t*)r.destination, &n); }   // запасной путь
          else n = got; }
        if (rc) { DLOG("request '%s' zlib inflate rc=%d (wrote %zu of %u)", e.name.c_str(), rc, n, r.destSize); return rc == 1 ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
        if (n != r.destSize) DLOG("request '%s' zlib: inflated %zu bytes, DestinationSize=%u (несовпадение)", e.name.c_str(), n, r.destSize);
        // Диагностика: XSX_DS_DUMP=N — сохранить первые N zlib-запросов (сжатый источник и наш результат) в T:\dsdump_NNN.{src,dst}
        { static long limit = [] { char v[16]; return GetEnvironmentVariableA("XSX_DS_DUMP", v, sizeof v) ? atol(v) : 0L; }(); static std::atomic<long> k{0};
          if (limit) { long i = k++; if (i < limit) { char path[64]; DWORD wr;
              snprintf(path, sizeof path, "T:\\dsdump_%03ld.src", i); HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); if (h != INVALID_HANDLE_VALUE) { WriteFile(h, tmp.data(), (DWORD)tmp.size(), &wr, 0); CloseHandle(h); }
              snprintf(path, sizeof path, "T:\\dsdump_%03ld.dst", i); h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); if (h != INVALID_HANDLE_VALUE) { WriteFile(h, r.destination, (DWORD)n, &wr, 0); CloseHandle(h); }
              DLOG("DSDUMP %03ld '%s' file=%ls off=0x%llx src=%u dst=%u inflated=%zu opt=0x%llx f10=0x%llx", i, e.name.c_str(), e.file ? e.file->path.c_str() : L"(mem)", (unsigned long long)r.fileOffset, r.sourceSize, r.destSize, n, (unsigned long long)r.options.asU64, (unsigned long long)r.destPageOffset); } } }
        return S_OK;
    }
    uint32_t n = r.sourceSize < r.destSize ? r.sourceSize : r.destSize; memcpy(r.destination, tmp.data(), n); return S_OK;
}
// Пул: запросы (kind 0) выполняются параллельно; status/signal (kind 1/2) — только когда все ранее выданные запросы завершены
// (семантика DStorage: status/signal отражают завершение всех предшествующих запросов очереди).
static void Worker(Queue* q)
{
    for (;;) {
        Entry e;
        { std::unique_lock<std::mutex> l(q->m);
          q->cv.wait(l, [&] { return q->stop || (!q->submitted.empty() && (q->submitted.front().kind == 0 || q->outstanding == 0)); });
          if (q->stop && q->submitted.empty()) return;
          e = std::move(q->submitted.front()); q->submitted.pop_front(); if (e.kind == 0) q->outstanding++; }
        if (e.kind == 0) {
            HRESULT hr = DoRequest(q, e);
            if (e.file) File_Release(e.file);
            { std::lock_guard<std::mutex> l(q->m);
              if (FAILED(hr)) {
                if (q->firstErr == S_OK) q->firstErr = hr;
                if (!q->failCount++) {
                    q->rec = {}; q->rec.first.hr = hr; q->rec.first.options = e.req.options.asU64; q->rec.first.dest = e.req.destination; q->rec.first.destSize = e.req.destSize;
                    q->rec.first.fileOffset = e.req.fileOffset; q->rec.first.sourceSize = e.req.sourceSize; q->rec.first.tag = e.req.cancelTag; strncpy(q->rec.first.name, e.name.c_str(), 63);
                    SetEvent(q->errEvent);
                }
              }
              q->outstanding--; }
            if (FAILED(hr)) DLOG("request '%s' FAILED hr=0x%08lX offset=0x%llx size=%u", e.name.c_str(), (unsigned long)hr, (unsigned long long)e.req.fileOffset, e.req.sourceSize);
            q->cv.notify_all();
        } else if (e.kind == 1) {
            std::lock_guard<std::mutex> g(e.st->m); HRESULT hr; { std::lock_guard<std::mutex> l(q->m); hr = q->firstErr; q->firstErr = S_OK; }
            if (e.idx < e.st->hr.size() && (e.idx >= e.st->gen.size() || e.st->gen[e.idx] == e.val)) { e.st->hr[e.idx] = hr; e.st->done[e.idx] = 1; }
            Status_Release(e.st); q->cv.notify_all();
        } else if (e.kind == 2) { if (e.fence) { e.fence->Signal(e.val); e.fence->Release(); } q->cv.notify_all(); }
    }
}
static ULONG STDMETHODCALLTYPE Queue_Release(Queue* q)
{
    LONG r = InterlockedDecrement(&q->ref);
    if (!r) { { std::lock_guard<std::mutex> l(q->m); q->stop = true; } q->cv.notify_all(); if (q->th.joinable()) q->th.join(); for (auto& t : q->pool) if (t.joinable()) t.join(); CloseHandle(q->errEvent); delete q; }
    return r;
}
static void QueueSubmit(Queue* q) { { std::lock_guard<std::mutex> l(q->m); while (!q->pending.empty()) { q->submitted.push_back(std::move(q->pending.front())); q->pending.pop_front(); } } q->cv.notify_all(); }
static void STDMETHODCALLTYPE Q_EnqueueRequest(Queue* q, const Request* r)
{
    Entry e{}; e.kind = 0; e.req = *r; e.name = r->name && !IsBadReadPtr(r->name, 1) ? r->name : "";
    static std::atomic<int> dumped{0}; if (dumped++ < 6) { const uint64_t* q = (const uint64_t*)r; DLOG("EnqueueRequest RAW: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx", (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7], (unsigned long long)q[8], (unsigned long long)q[9]); }
    if (r->options.o.srcType == 0 && IsFile(r->file)) { e.file = (File*)r->file; InterlockedIncrement(&e.file->ref); }
    else if (r->options.o.srcType == 0) { DLOG("EnqueueRequest: поле file=%p не наш объект (раскладка запроса Xbox отличается от сэмпла)", r->file); e.req.sourceSize = 0; e.req.destSize = 0; e.req.options.o.srcType = 1; e.req.source = nullptr; }
    { static bool on = [] { char v[16]; return GetEnvironmentVariableA("XSX_DS_LOG", v, sizeof v) > 0; }(); static std::atomic<long> k{0};
      if (on && k++ < 20000) DLOG("ENQ q='%s'(%p) file=%ls off=0x%llx src=%llu dst=%llu opt=0x%llx pending=%zu", q->name.c_str(), (void*)q, e.file ? e.file->path.c_str() : L"(mem/неизв.)", (unsigned long long)r->fileOffset, (unsigned long long)r->sourceSize, (unsigned long long)r->destSize, (unsigned long long)r->options.asU64, q->pending.size()); }
    bool kick; { std::lock_guard<std::mutex> l(q->m); q->pending.push_back(std::move(e)); kick = q->pending.size() >= q->desc.capacity / 2u; }
    if (kick) QueueSubmit(q);
}
static bool DsLogOn() { static bool on = [] { char v[16]; return GetEnvironmentVariableA("XSX_DS_LOG", v, sizeof v) > 0; }(); return on; }
static void STDMETHODCALLTYPE Q_EnqueueStatus(Queue* q, Status* s, uint32_t i)
{ if (DsLogOn()) { static std::atomic<long> k{0}; if (k++ < 3000) DLOG("ENQ-STATUS q=%p status=%p idx=%u", (void*)q, (void*)s, i); }
  // Семантика DStorage: постановка status-записи переводит слот в «не завершён» до момента её обработки очередью.
  // Игра переиспользует один слот (idx 0) для каждой пачки и сразу опрашивает IsComplete — без сброса она видела «готово»
  // от прошлой пачки и разбирала ещё не записанные буферы (подмена файлов: setup2.xml, *.zmt → ассерты 0x1dac734/0x9dfc18).
  // Слот отражает ТОЛЬКО последнюю поставленную status-запись: у каждой записи своё поколение; завершение более ранней записи
  // (если игра поставила в тот же слот новую пачку, не дождавшись) слот не трогает.
  uint64_t gen = 0;
  if (s) { std::lock_guard<std::mutex> g(s->m); if (i < s->done.size()) { if (s->gen.size() < s->done.size()) s->gen.resize(s->done.size()); gen = ++s->gen[i]; s->done[i] = 0; s->hr[i] = S_OK; } }
  Entry e{}; e.kind = 1; e.st = s; e.idx = i; e.val = gen; InterlockedIncrement(&s->ref); std::lock_guard<std::mutex> l(q->m); q->pending.push_back(std::move(e)); }
static void STDMETHODCALLTYPE Q_EnqueueSignal(Queue* q, ID3D12Fence* f, uint64_t v)
{ if (DsLogOn()) { static std::atomic<long> k{0}; if (k++ < 3000) DLOG("ENQ-SIGNAL q=%p fence=%p value=%llu", (void*)q, (void*)f, (unsigned long long)v); }
  Entry e{}; e.kind = 2; e.fence = f; e.val = v; if (f) f->AddRef(); std::lock_guard<std::mutex> l(q->m); q->pending.push_back(std::move(e)); }
static void STDMETHODCALLTYPE Q_Submit(Queue* q)
{
    static bool on = [] { char v[16]; return GetEnvironmentVariableA("XSX_DS_LOG", v, sizeof v) > 0; }(); static std::atomic<long> k{0};
    if (on && k++ < 5000) { size_t n; { std::lock_guard<std::mutex> l(q->m); n = q->pending.size(); } DLOG("SUBMIT q='%s'(%p) pending=%zu", q->name.c_str(), (void*)q, n); }
    QueueSubmit(q);
}
static void STDMETHODCALLTYPE Q_Cancel(Queue* q, uint64_t mask, uint64_t value)
{ std::lock_guard<std::mutex> l(q->m); for (auto& e : q->pending) if (e.kind == 0 && (e.req.cancelTag & mask) == value) { e.req.sourceSize = 0; e.req.destSize = 0; e.req.options.o.srcType = 1; } }
static void STDMETHODCALLTYPE Q_Close(Queue* q) { QueueSubmit(q); }
static HANDLE STDMETHODCALLTYPE Q_GetErrorEvent(Queue* q) { return q->errEvent; }
static void STDMETHODCALLTYPE Q_Retrieve(Queue* q, ErrorRecord* out) { std::lock_guard<std::mutex> l(q->m); if (out) { *out = q->rec; out->failureCount = q->failCount; } q->failCount = 0; ResetEvent(q->errEvent); }
static void STDMETHODCALLTYPE Q_Query(Queue* q, QueueInfo* out) { if (out) { out->desc = q->desc; std::lock_guard<std::mutex> l(q->m); out->emptySlots = (uint16_t)(q->desc.capacity > q->pending.size() ? q->desc.capacity - q->pending.size() : 0); out->untilAutoSubmit = (uint16_t)(q->desc.capacity / 2u); } }
static void* g_queueVt[24] = { (void*)QI_generic, (void*)AddRef_g, (void*)Queue_Release, (void*)Q_EnqueueRequest, (void*)Q_EnqueueStatus, (void*)Q_EnqueueSignal,
                               (void*)Q_Submit, (void*)Q_Cancel, (void*)Q_Close, (void*)Q_GetErrorEvent, (void*)Q_Retrieve, (void*)Q_Query,
    (void*)Obj_Unknown<1,12>, (void*)Obj_Unknown<1,13>, (void*)Obj_Unknown<1,14>, (void*)Obj_Unknown<1,15>, (void*)Obj_Unknown<1,16>, (void*)Obj_Unknown<1,17>, (void*)Obj_Unknown<1,18>, (void*)Obj_Unknown<1,19>, (void*)Obj_Unknown<1,20>, (void*)Obj_Unknown<1,21>, (void*)Obj_Unknown<1,22>, (void*)Obj_Unknown<1,23> };

// ---------- Factory ----------
struct Factory { void** vt; volatile LONG ref; GUID iid; };
static HRESULT STDMETHODCALLTYPE F_CreateQueue(Factory*, const QueueDesc* d, REFIID riid, void** ppv)
{
    if (!d || !ppv) return E_INVALIDARG;
    auto* q = new Queue(); q->vt = g_queueVt; q->ref = 1; q->iid = riid; q->desc = *d; q->name = d->name ? d->name : ""; q->desc.name = nullptr;
    if (!q->desc.capacity) q->desc.capacity = 0x80;
    q->th = std::thread(Worker, q);
    { char v[16]; long n = GetEnvironmentVariableA("XSX_DS_THREADS", v, sizeof v) ? atol(v) : 0; if (n <= 0) { n = (long)std::thread::hardware_concurrency() / 2; if (n < 2) n = 2; if (n > 8) n = 8; }
      for (long i = 1; i < n; i++) q->pool.emplace_back(Worker, q); }
    *ppv = q;
    DLOG("CreateQueue name='%s' cap=%u prio=%d srcType=%llu riid={%08lX-..} -> %p", q->name.c_str(), q->desc.capacity, q->desc.priority, (unsigned long long)q->desc.sourceType, riid.Data1, (void*)q);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE F_OpenFile(Factory*, const wchar_t* path, REFIID riid, void** ppv)
{
    if (!path || !ppv) return E_INVALIDARG; *ppv = nullptr;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); DLOG("OpenFile %ls -> FAILED 0x%08lX", path, (unsigned long)hr); return hr; }
    auto* f = new File(); f->vt = g_fileVt; f->ref = 1; f->iid = riid; f->h = h; f->path = path; *ppv = f; { std::lock_guard<std::mutex> l(g_filesM); g_files.insert(f); }
    LARGE_INTEGER sz = {}; GetFileSizeEx(h, &sz);
    DLOG("OpenFile %ls size=%lld riid={%08lX-%04X-%04X-..} -> %p", path, sz.QuadPart, riid.Data1, riid.Data2, riid.Data3, (void*)f);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE F_CreateStatusArray(Factory*, uint32_t cap, const char* name, REFIID riid, void** ppv)
{
    if (!ppv) return E_INVALIDARG; auto* s = new Status(); s->vt = g_statusVt; s->ref = 1; s->iid = riid; s->hr.assign(cap, S_OK); s->done.assign(cap, 0); *ppv = s;
    DLOG("CreateStatusArray cap=%u name='%s' -> %p", cap, name ? name : "", (void*)s); return S_OK;
}
static HRESULT STDMETHODCALLTYPE F_SetCpuAffinity(Factory*, uint64_t a) { DLOG("SetCpuAffinity 0x%llx (ignored)", (unsigned long long)a); return S_OK; }
static void STDMETHODCALLTYPE F_SetDebugFlags(Factory*, uint32_t f) { DLOG("SetDebugFlags 0x%x", f); }
static HRESULT STDMETHODCALLTYPE F_SetStagingBufferSize(Factory*, uint32_t s) { DLOG("SetStagingBufferSize 0x%x", s); return S_OK; }
static ULONG STDMETHODCALLTYPE F_Release(Factory* f) { return InterlockedDecrement(&f->ref); }   // синглтон
template <int N> static uintptr_t __stdcall F_Unknown(Factory*, uintptr_t a, uintptr_t b, uintptr_t c) { DLOG("factory slot %d НЕ реализован (a=%p b=%p c=%p) -> 0", N, (void*)a, (void*)b, (void*)c); return 0; }
// Раскладка Xbox-фабрики: ПО exe наблюдаются слоты 4 (OpenFile), 8 (SetStagingBufferSize), 10 (CreateQueue(desc{u64 srcType; u16 capacity @+8; i8 priority @+0xa; char* name @+0x10}, riid, ppv)).
// Остальные слоты — гипотеза по wrapper-сэмплу (3=CreateQueue-дубликат, 5=CreateStatusArray, 6=SetCpuAffinity, 7=SetDebugFlags) или лог-заглушки.
static void* g_facVt[16] = { (void*)QI_generic, (void*)AddRef_g, (void*)F_Release, (void*)F_CreateQueue, (void*)F_OpenFile, (void*)F_CreateStatusArray,
                             (void*)F_SetCpuAffinity, (void*)F_SetDebugFlags, (void*)F_SetStagingBufferSize, (void*)F_Unknown<9>, (void*)F_CreateQueue,
                             (void*)F_Unknown<11>, (void*)F_Unknown<12>, (void*)F_Unknown<13>, (void*)F_Unknown<14>, (void*)F_Unknown<15> };
static Factory g_fac = { g_facVt, 1, IID_Factory };

extern "C" __declspec(dllexport) HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv)
{
    DLOG("DStorageGetFactory riid={%08lX-%04X-%04X-...} out=%p", riid.Data1, riid.Data2, riid.Data3, ppv);
    if (!ppv) return E_POINTER;
    if (!IsEqualGUID(riid, IID_Factory) && !IsEqualGUID(riid, kIUnknown)) { *ppv = nullptr; return E_NOINTERFACE; }
    InterlockedIncrement(&g_fac.ref); *ppv = &g_fac; return S_OK;
}
