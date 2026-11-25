#pragma once
// ============================================================================
// gfx_snapshot.h — снимки present-поверхности для сравнения пикселей.
// XSX_PRESENT_SNAPSHOT=N — каждый N-й кадр; XSX_PRESENT_SNAPSHOT_DIR=<каталог> (*.ppm).
// Только чтение GPU: CopyTextureRegion в readback-буфер, ничего не перекрашивается.
// Выключено по умолчанию (N<=0).
// ============================================================================

static ID3D12Resource* g_snapBuf = nullptr;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_snapFp = {};
static UINT g_snapW = 0, g_snapH = 0, g_snapRowPitch = 0;
static bool g_snapPending = false;

static int GfxSnapEvery()
{
    static int every = [] { char v[16]; DWORD n = GetEnvironmentVariableA("XSX_PRESENT_SNAPSHOT", v, sizeof v); return n ? atoi(v) : 0; }();
    return every;
}
static bool GfxSnapRecord(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState)
{
    const int every = GfxSnapEvery();
    if (every <= 0) return false;
    const D3D12_RESOURCE_DESC d = target->GetDesc();
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        static bool warned = false;
        if (!warned) { warned = true; XS_LOG("SNAPSHOT: формат цели %d пока не поддерживается (ожидаем RGBA8/BGRA8)", (int)d.Format); }
        return false;
    }
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) return false;
    if (!g_snapBuf || g_snapW != (UINT)d.Width || g_snapH != d.Height) {
        if (g_snapBuf) { g_snapBuf->Release(); g_snapBuf = nullptr; }
        UINT numRows = 0; UINT64 rowPitch = 0, total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &g_snapFp, &numRows, &rowPitch, &total);
        g_snapW = (UINT)d.Width; g_snapH = (UINT)d.Height; g_snapRowPitch = (UINT)rowPitch;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = total; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_snapBuf);
        dev->Release();
        if (FAILED(hr) || !g_snapBuf) { g_snapBuf = nullptr; XS_LOG("SNAPSHOT: нет readback-буфера (%u Б) hr=0x%08lX", total, (unsigned long)hr); return false; }
    } else dev->Release();
    using xsx::presentation::Transition;
    Transition(list, target, targetState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = g_snapBuf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = g_snapFp;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = target; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE, targetState);
    return true;
}
// Вызывать ПОСЛЕ Signal(fence, fenceValue); ждёт completion только для снятого кадра.
static void GfxSnapSave(UINT64 fenceValue, ID3D12Fence* fence, HANDLE ev, ID3D12Resource* target)
{
    if (!g_snapPending) return;
    g_snapPending = false;
    if (fence->GetCompletedValue() < fenceValue) {
        if (FAILED(fence->SetEventOnCompletion(fenceValue, ev)) || WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) {
            XS_LOG("SNAPSHOT: fence=%llu не дождались — кадр пропущен", (unsigned long long)fenceValue); return;
        }
    }
    D3D12_RANGE r = { 0, (SIZE_T)g_snapFp.Footprint.RowPitch * g_snapH }; void* p = nullptr;
    if (FAILED(g_snapBuf->Map(0, &r, &p)) || !p) { XS_LOG("SNAPSHOT: Map readback не удался"); return; }
    char dir[512]; if (!GetEnvironmentVariableA("XSX_PRESENT_SNAPSHOT_DIR", dir, sizeof dir)) strcpy(dir, ".");
    const D3D12_RESOURCE_DESC d = target->GetDesc();
    char path[700]; snprintf(path, sizeof path, "%s\\snap%llu_%llux%u_fmt%d.ppm", dir, (unsigned long long)fenceValue,
        (unsigned long long)d.Width, d.Height, (int)d.Format);
    CreateDirectoryA(dir, nullptr);
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", g_snapW, g_snapH);
        const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        std::vector<unsigned char> row((size_t)g_snapW * 3);
        for (UINT y = 0; y < g_snapH; y++) {
            const unsigned char* s = (const unsigned char*)p + g_snapFp.Offset + (SIZE_T)y * g_snapRowPitch;
            for (UINT x = 0; x < g_snapW; x++) {
                const unsigned char b = s[x * 4 + 0], g = s[x * 4 + 1], rr = s[x * 4 + 2];
                row[x * 3 + 0] = bgra ? b : rr; row[x * 3 + 1] = g; row[x * 3 + 2] = bgra ? rr : b;
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        XS_LOG("SNAPSHOT fence=%llu файл=%s формат=%d pitch=%u", (unsigned long long)fenceValue, path, (int)d.Format, g_snapRowPitch);
    } else XS_LOG("SNAPSHOT: не удалось открыть %s", path);
    g_snapBuf->Unmap(0, nullptr);
}
