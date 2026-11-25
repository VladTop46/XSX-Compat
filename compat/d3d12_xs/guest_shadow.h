#pragma once
// Теневые буферы памяти игры (UMA-модель Xbox: адрес ресурса = адрес памяти игры, память игры — источник истины).
// ShadowFor(start, len) возвращает нативный UPLOAD-буфер, покрывающий [start, start+len) гостевой памяти, и смещение внутри него.
// ShadowSyncAll() копирует память игры в теневые буферы; вызывается перед каждой отправкой списков на очередь.
// Это только чтение хостом: UAV/запись GPU обратно в память игры здесь НЕ реализованы.
struct ShadowRange { uint64_t base, size; ID3D12Resource* res; uint8_t* map; };
static std::map<uint64_t, ShadowRange> g_shadow; static SRWLOCK g_shadowLock = SRWLOCK_INIT;
static uint64_t g_shadowTotal = 0; static std::atomic<uint64_t> g_shadowSyncs{0}, g_shadowBytes{0};
static constexpr uint64_t kShadowCap = 768ull << 20, kShadowAlign = 0x10000;

static void ShadowCopy(void* dst, uint64_t src, uint64_t size)
{
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)src, dst, (SIZE_T)size, &got) || got != size) {
        // частично нечитаемая память: постранично, нечитаемое — нули
        for (uint64_t o = 0; o < size; o += 0x1000) { SIZE_T g2 = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*)(src + o), (char*)dst + o, (SIZE_T)std::min<uint64_t>(0x1000, size - o), &g2) || !g2) memset((char*)dst + o, 0, (size_t)std::min<uint64_t>(0x1000, size - o)); }
    }
}
static ID3D12Resource* ShadowFor(ID3D12Device* dev, uint64_t start, uint64_t len, uint64_t* offset, uint32_t elem = 1)
{
    if (!len) len = 1;
    ExclusiveSrwLock g(g_shadowLock);
    auto it = g_shadow.upper_bound(start);
    if (it != g_shadow.begin()) { auto p = std::prev(it); if (start >= p->second.base && start + len <= p->second.base + p->second.size && (start - p->second.base) % elem == 0) { *offset = start - p->second.base; return p->second.res; } }
    // elem != 1 и смещение от 64К-границы не кратно элементу (структурный буфер с нестепенным stride) — диапазон начинается точно с start
    uint64_t base = start & ~(kShadowAlign - 1), end = (start + len + kShadowAlign - 1) & ~(kShadowAlign - 1);
    if (elem > 1 && (start - base) % elem) { base = start; end = (start + len + 255) & ~255ull; }
    const uint64_t size = end - base;
    if (g_shadowTotal + size > kShadowCap) { static std::atomic<int> n{0}; if (n++ < 5) XS_LOG("SHADOW: лимит %llu МиБ, диапазон %#llx+%#llx не создан", (unsigned long long)(kShadowCap >> 20), (unsigned long long)start, (unsigned long long)len); return nullptr; }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    void* map = nullptr; if (SUCCEEDED(hr)) { D3D12_RANGE none = {}; hr = res->Map(0, &none, &map); }
    if (FAILED(hr) || !map) { XS_LOG("SHADOW: создание %#llx+%#llx hr=0x%08lX", (unsigned long long)base, (unsigned long long)size, (unsigned long)hr); if (res) res->Release(); return nullptr; }
    ShadowCopy(map, base, size);
    g_shadow[base] = { base, size, res, (uint8_t*)map }; g_shadowTotal += size;
    static std::atomic<int> n{0}; if (n++ < 40) XS_LOG("SHADOW: новый диапазон guest %#llx+%#llx -> %p (всего %llu КиБ, диапазонов %zu)", (unsigned long long)base, (unsigned long long)size, (void*)res, (unsigned long long)(g_shadowTotal >> 10), g_shadow.size());
    *offset = start - base; return res;
}
static void ShadowSyncAll()
{
    SharedSrwLock g(g_shadowLock);
    // Большие диапазоны (>4 МиБ, статические пулы данных) копируются редко — полная копия 180+ МиБ на каждую отправку убивает частоту кадров.
    // Это временная политика вместо write-watch/protect (память игры — секции XMem, MEM_WRITE_WATCH недоступен): гарантии актуальности для больших пулов НЕТ.
    const uint64_t n = ++g_shadowSyncs;
    for (auto& e : g_shadow) { if (e.second.size > (4ull << 20) && (n % 256) != 1) continue; ShadowCopy(e.second.map, e.second.base, e.second.size); g_shadowBytes += e.second.size; }
}
// Размер элемента формата консоли (как у VIctory resource::element); 0 — неизвестен или блочный.
static uint32_t ConsoleElementBytes(uint32_t f)
{
    if ((f >= 1 && f <= 4)) return 16; if (f >= 5 && f <= 8) return 12; if (f >= 9 && f <= 22) return 8;
    if ((f >= 23 && f <= 47) || f == 67 || (f >= 87 && f <= 93) || f == 116 || f == 117 || f == 189 || f == 191) return 4;
    if (f >= 118 && f <= 120) return 3;
    if ((f >= 48 && f <= 59) || f == 68 || f == 69 || f == 85 || f == 86 || f == 115) return 2;
    if ((f >= 60 && f <= 65) || f == 190) return 1;
    return 0;
}
