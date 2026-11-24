#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
int main()
{
    LoadLibraryW(L"d3d12.dll"); HMODULE core = nullptr; auto cd = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice");
    ID3D12Device* d = nullptr; cd(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12Device, (void**)&d); core = GetModuleHandleW(L"d3d12core.dll"); printf("core=%p\n", core);
    auto P = [&](const char* n, void* o) { if (o) printf("%-22s vtable core+%#llx\n", n, (unsigned long long)(*(char**)o - (char*)core)); else printf("%-22s (null)\n", n); };
    P("device", d);
    ID3D12Fence* f; d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&f); P("fence", f);
    ID3D12CommandAllocator* a; d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&a); P("allocator", a);
    ID3D12GraphicsCommandList* l; d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_ID3D12GraphicsCommandList, (void**)&l); P("cmdlist", l);
    D3D12_COMMAND_QUEUE_DESC qd = {}; ID3D12CommandQueue* q; d->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&q); P("queue", q);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 }; ID3D12DescriptorHeap* h; d->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, (void**)&h); P("descheap", h);
    D3D12_ROOT_SIGNATURE_DESC rd = {}; ID3DBlob* sig; ID3DBlob* er; auto ser = (HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**))GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &sig, &er); ID3D12RootSignature* rs; d->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_ID3D12RootSignature, (void**)&rs); P("rootsig", rs);
    D3D12_HEAP_DESC hp = {}; hp.SizeInBytes = 65536; hp.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; hp.Alignment = 65536; hp.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES; ID3D12Heap* hh; d->CreateHeap(&hp, IID_ID3D12Heap, (void**)&hh); P("heap", hh);
    D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0 }; ID3D12QueryHeap* qhp; d->CreateQueryHeap(&qh, IID_ID3D12QueryHeap, (void**)&qhp); P("queryheap", qhp);
    D3D12_INDIRECT_ARGUMENT_DESC ia = { D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH }; D3D12_COMMAND_SIGNATURE_DESC cs = { 12, 1, &ia, 0 }; ID3D12CommandSignature* csg; d->CreateCommandSignature(&cs, nullptr, IID_ID3D12CommandSignature, (void**)&csg); P("cmdsignature", csg);
    D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* r; d->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, (void**)&r); P("resource", r);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = rs; static const unsigned char dummy[1] = {0}; (void)dummy; P("(pso: нужен шейдер, пропущен)", nullptr);
    return 0;
}
