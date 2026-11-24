#include <windows.h>
#include <d3d12.h>
#include <cstdio>

using DeserializeFn = HRESULT (WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**
);

#define CHECK(expr) do {                                  \
    HRESULT hr_ = (expr);                                 \
    printf("[%s] %s: 0x%08lX\n",                          \
           SUCCEEDED(hr_) ? "PASS" : "FAIL",              \
           #expr, (unsigned long)hr_);                    \
    fflush(stdout);                                       \
    if (FAILED(hr_)) ExitProcess(1);                      \
} while (0)

int main()
{
    HMODULE mod = LoadLibraryW(L"d3d12_xs.dll");

    if (!mod) {
        printf("[FAIL] LoadLibrary: %lu\n",
               GetLastError());
        return 1;
    }

    auto deserialize = reinterpret_cast<DeserializeFn>(
        GetProcAddress(
            mod,
            "D3D12CreateRootSignatureDeserializer"));

    if (!deserialize)
        return 2;

    printf("[PASS] Proxy loaded\n");

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    param.Descriptor.ShaderRegister = 3;
    param.Descriptor.RegisterSpace = 2;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &param;

    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;

    CHECK(D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &blob,
        &error));

    printf("[SERIALIZED] %zu bytes\n",
           blob->GetBufferSize());

    ID3D12RootSignatureDeserializer* result = nullptr;

    CHECK(deserialize(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        IID_PPV_ARGS(&result)));

    const auto* decoded = result->GetRootSignatureDesc();

    printf("[PARAMETERS] %u\n", decoded->NumParameters);

    bool ok = decoded->NumParameters == 1;

    if (ok) {
        const auto& p = decoded->pParameters[0];

        printf("[PARAMETER TYPE] %u\n", p.ParameterType);
        printf("[SHADER REGISTER] %u\n",
               p.Descriptor.ShaderRegister);
        printf("[REGISTER SPACE] %u\n",
               p.Descriptor.RegisterSpace);

        ok = p.ParameterType ==
                 D3D12_ROOT_PARAMETER_TYPE_UAV &&
             p.Descriptor.ShaderRegister == 3 &&
             p.Descriptor.RegisterSpace == 2;
    }

    result->Release();
    blob->Release();

    if (error)
        error->Release();

    FreeLibrary(mod);

    printf("[RESULT] %s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 3;
}
