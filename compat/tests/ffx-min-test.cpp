// Проверка ffx_min: PC D3D12 device, градиент 64x36 RGBA32F -> 128x72 через ffxCreateContext/Query/Dispatch.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <ffx_api.h>
#include <ffx_upscale.h>
static int pass = 0, fail = 0;
#define CHECK(c, ...) do { if (c) { pass++; printf("[PASS] "); } else { fail++; printf("[FAIL] "); } printf(__VA_ARGS__); putchar('\n'); } while (0)
#define HR(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("[FAIL] %s hr=0x%08lX\n", #x, (unsigned long)_h); return 1; } } while (0)

static ID3D12Resource* Buf(ID3D12Device* d, D3D12_HEAP_TYPE t, UINT64 sz, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = t;
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, sz, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* r = nullptr; d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_ID3D12Resource, (void**)&r); return r;
}
static ID3D12Resource* Tex(ID3D12Device* d, UINT w, UINT h, D3D12_RESOURCE_FLAGS f, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, f };
    ID3D12Resource* r = nullptr; d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_ID3D12Resource, (void**)&r); return r;
}
static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b }; l->ResourceBarrier(1, &br);
}

int main()
{
    HMODULE ffx = LoadLibraryW(L"amd_fidelityfx_loader_xbox.dll");
    CHECK(ffx, "load ffx loader (err=%lu)", GetLastError()); if (!ffx) return 2;
    auto create = (PfnFfxCreateContext)GetProcAddress(ffx, "ffxCreateContext");
    auto destroy = (PfnFfxDestroyContext)GetProcAddress(ffx, "ffxDestroyContext");
    auto query = (PfnFfxQuery)GetProcAddress(ffx, "ffxQuery");
    auto dispatch = (PfnFfxDispatch)GetProcAddress(ffx, "ffxDispatch");
    CHECK(create && destroy && query && dispatch, "exports resolved");

    HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    auto cdev = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d, "D3D12CreateDevice");
    ID3D12Device* dev = nullptr; HR(cdev(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12Device, (void**)&dev));

    // --- queries ---
    int32_t phases = 0; ffxQueryDescUpscaleGetJitterPhaseCount qp{}; qp.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    qp.renderWidth = 64; qp.displayWidth = 128; qp.pOutPhaseCount = &phases;
    CHECK(query(nullptr, &qp.header) == 0 && phases == 32, "jitter phase count 64->128 = %d (ожидается 32)", phases);
    float jx = 9, jy = 9; ffxQueryDescUpscaleGetJitterOffset qj{}; qj.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    qj.index = 0; qj.phaseCount = phases; qj.pOutX = &jx; qj.pOutY = &jy;
    CHECK(query(nullptr, &qj.header) == 0 && fabsf(jx - 0.0f) < 1e-6f && fabsf(jy - (1.0f/3 - 0.5f)) < 1e-5f, "jitter[0] = (%.4f, %.4f) (Halton 2,3: 0.0, -0.1667)", jx, jy);
    ffxQueryDescHeader bad{}; bad.type = 0xdeadbeef; CHECK(query(nullptr, &bad) == 2, "unknown query -> UNKNOWN_DESCTYPE");

    // --- context ---
    ffxCreateContextDescUpscale cd{}; cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.maxRenderSize = {64, 36}; cd.maxUpscaleSize = {128, 72}; cd.flags = 0x29;
    ffxContext ctx = nullptr; CHECK(create(&ctx, &cd.header, nullptr) == 0 && ctx, "ffxCreateContext");

    // --- ресурсы ---
    const UINT W = 64, H = 36, UW = 128, UH = 72;
    ID3D12Resource* src = Tex(dev, W, H, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* out = Tex(dev, UW, UH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UINT srcPitch = (W * 16 + 255) & ~255u, outPitch = (UW * 16 + 255) & ~255u;
    ID3D12Resource* up = Buf(dev, D3D12_HEAP_TYPE_UPLOAD, UINT64(srcPitch) * H, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* rb = Buf(dev, D3D12_HEAP_TYPE_READBACK, UINT64(outPitch) * UH, D3D12_RESOURCE_STATE_COPY_DEST);
    uint8_t* p = nullptr; up->Map(0, nullptr, (void**)&p);
    for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) { float* px = (float*)(p + y * srcPitch) + 4 * x; px[0] = x / float(W - 1); px[1] = y / float(H - 1); px[2] = 0.5f; px[3] = 1; }
    up->Unmap(0, nullptr);

    ID3D12CommandQueue* q; ID3D12CommandAllocator* a; ID3D12GraphicsCommandList* l; ID3D12Fence* f;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&q)); HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&a));
    HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_ID3D12GraphicsCommandList, (void**)&l)); HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&f));

    D3D12_TEXTURE_COPY_LOCATION s = {}, d = {};
    s.pResource = up; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = { 0, { DXGI_FORMAT_R32G32B32A32_FLOAT, W, H, 1, srcPitch } };
    d.pResource = src; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
    l->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    Barrier(l, src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    for (int pass_i = 0; pass_i < 2; pass_i++) {   // 0: jitter=0 (проверка значений), 1: jitter=(0.5,0) (проверка сдвига)
        ffxDispatchDescUpscale dd{}; dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE; dd.commandList = l;
        dd.color.resource = src; dd.output.resource = out; dd.renderSize = {W, H}; dd.upscaleSize = {UW, UH};
        dd.jitterOffset = { pass_i ? 0.5f : 0.0f, 0.0f };
        CHECK(dispatch(&ctx, &dd.header) == 0, "ffxDispatch #%d recorded", pass_i);
        Barrier(l, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION o = {}, r = {};
        o.pResource = out; o.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        r.pResource = rb; r.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; r.PlacedFootprint = { 0, { DXGI_FORMAT_R32G32B32A32_FLOAT, UW, UH, 1, outPitch } };
        l->CopyTextureRegion(&r, 0, 0, 0, &o, nullptr);
        HR(l->Close()); ID3D12CommandList* ls[] = { l }; q->ExecuteCommandLists(1, ls);
        q->Signal(f, pass_i + 1); HANDLE ev = CreateEventW(nullptr, 0, 0, nullptr); f->SetEventOnCompletion(pass_i + 1, ev);
        CHECK(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "GPU done #%d", pass_i); CloseHandle(ev);

        uint8_t* m = nullptr; rb->Map(0, nullptr, (void**)&m);
        double maxErr = 0; int n = 0;
        for (UINT y = 2; y < UH - 2; y++) for (UINT x = 2; x < UW - 2; x++) {     // без краёв (clamp)
            const float* px = (const float*)(m + y * outPitch) + 4 * x;
            double sx = ((x + 0.5) / UW * W + (pass_i ? 0.5 : 0.0)) - 0.5, sy = ((y + 0.5) / UH * H) - 0.5;   // ожидаемая позиция в исходнике (bilinear)
            double ex = sx / (W - 1), ey = sy / (H - 1);
            maxErr = fmax(maxErr, fmax(fabs(px[0] - ex), fabs(px[1] - ey))); n++;
        }
        const float* c = (const float*)(m + 36 * outPitch) + 4 * 64;
        printf("[INFO] pass %d: sample out(64,36) = (%.4f %.4f %.4f %.4f), max |err| vs analytic bilinear = %.5f over %d px\n", pass_i, c[0], c[1], c[2], c[3], maxErr, n);
        CHECK(maxErr < 0.01, "pass %d values match bilinear (err %.5f < 0.01)", pass_i, maxErr);
        rb->Unmap(0, nullptr);
        if (pass_i == 0) { HR(a->Reset()); HR(l->Reset(a, nullptr)); Barrier(l, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    }
    CHECK(destroy(&ctx, nullptr) == 0 && !ctx, "ffxDestroyContext");
    printf("\nSUMMARY pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}
