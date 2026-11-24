#pragma once
// Experimental Xbox CPU descriptor-handle return adapter. See the ABI report.
// Only the proxy's own vtable is changed; the VKD3D heap stays native.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

class XboxDescriptorHeapProxy final : public ID3D12DescriptorHeap {
public:
    explicit XboxDescriptorHeapProxy(ID3D12DescriptorHeap* owned) : real(owned) {
        void** native = *reinterpret_cast<void***>(this);
        std::memcpy(slots, native, sizeof(slots));
        // Xbox +0x39f8 returns the 8-byte CPU handle in RAX. MinGW/VKD3D
        // instead use (this, D3D12_CPU_DESCRIPTOR_HANDLE* result).
        slots[9] = reinterpret_cast<void*>(&XboxCPUStart);
        *reinterpret_cast<void***>(this) = slots;
        std::printf("[HEAP PROXY] proxy=%p real=%p CPU thunk=%p\n",
                    this, real, slots[9]);
    }

    static UINT64 STDMETHODCALLTYPE XboxCPUStart(XboxDescriptorHeapProxy* self) {
        // Call the unmodified PC interface with its real aggregate-return ABI.
        const auto handle = self->real->GetCPUDescriptorHandleForHeapStart();
        HMODULE module = nullptr;
        const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(caller), &module);
        std::printf("[HEAP ABI] CPU this=%p real=%p caller=%p base=%p RVA=0x%llx handle=0x%llx\n",
                    self, self->real, reinterpret_cast<void*>(caller), module,
                    static_cast<unsigned long long>(caller - reinterpret_cast<uintptr_t>(module)),
                    static_cast<unsigned long long>(handle.ptr));
        return handle.ptr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualGUID(iid, __uuidof(IUnknown)) ||
            IsEqualGUID(iid, __uuidof(ID3D12Object)) ||
            IsEqualGUID(iid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualGUID(iid, __uuidof(ID3D12Pageable)) ||
            IsEqualGUID(iid, __uuidof(ID3D12DescriptorHeap))) {
            *out = static_cast<ID3D12DescriptorHeap*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
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
    D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE GetDesc(D3D12_DESCRIPTOR_HEAP_DESC* out) override {
        return real->GetDesc(out);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart(
        D3D12_CPU_DESCRIPTOR_HANDLE* out) override {
        return real->GetCPUDescriptorHandleForHeapStart(out);
    }
    // No Xbox GPU-handle ABI has been established in this experiment.
    D3D12_GPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart(
        D3D12_GPU_DESCRIPTOR_HANDLE* out) override {
        return real->GetGPUDescriptorHandleForHeapStart(out);
    }

private:
    ~XboxDescriptorHeapProxy() { real->Release(); }
    ID3D12DescriptorHeap* real; // Owns exactly the reference returned by creation.
    std::atomic<ULONG> refs{1};
    void* slots[11]{};
};

inline HRESULT WrapXboxDescriptorHeap(void** out) {
    if (!out || !*out) return E_POINTER;
    auto* real = static_cast<ID3D12DescriptorHeap*>(*out);
    auto* proxy = new (std::nothrow) XboxDescriptorHeapProxy(real);
    if (!proxy) {
        real->Release();
        *out = nullptr;
        return E_OUTOFMEMORY;
    }
    *out = static_cast<ID3D12DescriptorHeap*>(proxy);
    return S_OK;
}
