#pragma once
// Experimental Xbox ID3D12Resource COM proxy (diagnostic only).
// Native VKD3D resources retain their original vtables.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

class XboxResourceProxy final : public ID3D12Resource {
public:
    explicit XboxResourceProxy(ID3D12Resource* owned)
    : real(owned) {
        // The inherited COM vtable has slots 0..14. Clone the proxy's
        // own table (NOT VKD3D's) so only this proxy has an Xbox slot 9.
        void** native = *reinterpret_cast<void***>(this);
        std::memcpy(slots, native, sizeof(slots));
        slots[9] = reinterpret_cast<void*>(&XboxSlot9);
        *reinterpret_cast<void***>(this) = slots;
        std::printf("[PROXY] created proxy=%p real=%p slot9=%p\n",
                    this, real, slots[9]);
    }

    // Caution: for a texture, GPU VA is normally zero. The purpose of
    // this hook is to test whether the call targets THIS resource.
    static UINT64 STDMETHODCALLTYPE XboxSlot9(XboxResourceProxy* self) {
        UINT64 va = self->real->GetGPUVirtualAddress();
        std::printf("[PROXY] Xbox slot9 called: proxy=%p gpuVA=0x%llx\n",
                    self, static_cast<unsigned long long>(va));
        std::fflush(stdout);
        return va;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualGUID(iid, __uuidof(IUnknown)) ||
            IsEqualGUID(iid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(iid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(iid, __uuidof(ID3D12Pageable)) ||
            IsEqualGUID(iid, __uuidof(ID3D12Resource))) {
            *out = static_cast<ID3D12Resource*>(this);
        AddRef();
        return S_OK;
            }
            return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT* n, void* data) override {
        return real->GetPrivateData(id, n, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT n, const void* data) override {
        return real->SetPrivateData(id, n, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown* data) override {
        return real->SetPrivateDataInterface(id, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return real->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override {
        return real->GetDevice(iid, out);
    }
    HRESULT STDMETHODCALLTYPE Map(UINT sub, const D3D12_RANGE* range, void** data) override {
        return real->Map(sub, range, data);
    }
    void STDMETHODCALLTYPE Unmap(UINT sub, const D3D12_RANGE* range) override {
        real->Unmap(sub, range);
    }
    D3D12_RESOURCE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC* out) override {
        return real->GetDesc(out);
    }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        return real->GetGPUVirtualAddress();
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT sub, const D3D12_BOX* box,
                                                 const void* data, UINT row, UINT depth) override {
                                                     return real->WriteToSubresource(sub, box, data, row, depth);
                                                 }
                                                 HRESULT STDMETHODCALLTYPE ReadFromSubresource(void* data, UINT row, UINT depth,
                                                                                               UINT sub, const D3D12_BOX* box) override {
                                                                                                   return real->ReadFromSubresource(data, row, depth, sub, box);
                                                                                               }
                                                                                               HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* props,
                                                                                                                                           D3D12_HEAP_FLAGS* flags) override {
                                                                                                                                               return real->GetHeapProperties(props, flags);
                                                                                                                                           }

private:
    ~XboxResourceProxy() { real->Release(); }
    ID3D12Resource* real; // Takes ownership of ONE existing COM reference.
    std::atomic<ULONG> refs{1};
    void* slots[15]{};
};

inline HRESULT WrapXboxResource(void** out) {
    if (!out || !*out) return E_POINTER;
    auto* real = static_cast<ID3D12Resource*>(*out);
    auto* proxy = new (std::nothrow) XboxResourceProxy(real);
    if (!proxy) {
        real->Release();
        *out = nullptr;
        return E_OUTOFMEMORY;
    }
    *out = static_cast<ID3D12Resource*>(proxy);
    return S_OK;
}
