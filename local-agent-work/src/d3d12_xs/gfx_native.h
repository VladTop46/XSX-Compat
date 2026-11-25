#pragma once
// ============================================================================
// gfx_native.h — РЕАЛЬНЫЕ графические PSO из офлайн-кэша переводов (XSX_DEV_NATIVE_GFX=1).
//
// Raw slot57 captures establish an opaque qword0 followed by six (ptr,size)
// records in qwords1..12. Older USEDPSO neighbor-size extraction overread
// several blobs and did not establish their declared lengths.
//   p0 = блок состояния (магия 0x4005d808; размеры секций dword[0x28]=0x11c|secB<<16),
//   p1 = bounded input-layout container, count at0x4c followed by semantic
//        records. Old dumps also included bytes beyond its declared extent.
//   p2 = код PS, p3 = метаданные PS ({2,size-12} записи; хеш PS в конце 1-й записи),
//   p4 = код VS, p5 = метаданные VS. Код начинается с 36-байтного NOP-префикса с
//        хешем программы (тот же формат, что xsx_shader_metadata.nop_hash).
// ГИПОТЕЗА (оценено, логируется): поля блока состояния (blend/depth/raster/RTV),
// depth/raster/write mask; полного image/detile bridge пока нет.
// Input layout читается из p1, поля V# — по публичной раскладке GFX10.
//
// HLSL-абстракция = контракт compat/tests/xsx_gfx_translate.py:
//   Buf<k> ByteAddressBuffer t<k> space1; Tex<k> Texture2D t<k> space2;
//   Smp<j> SamplerState s<j> space1; VSharp cbuffer b0 space2 { uint4 vs[]; }.
// Root signature 1.1 собрана descriptor TABLE'ами (root CBV/SRV параметры в SDK не
// несут регистр/space). Конфликт VS/PS в одной space -> E_NOTIMPL с логом.
// ============================================================================

#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "gfx_buffer_descriptor.h"
#include "gfx_input_layout.h"
#include "gfx_blend.h"
#include "gfx_pso_description.h"

// ---------- кэш переводов ----------
struct GfxBuf { uint32_t off, dwords, reg, space; };
struct GfxTex { uint32_t off, dwords, reg, space; };
struct GfxSmp { uint32_t words[4]; uint32_t reg, space; };
struct GfxVin { std::string sem; uint32_t index, mask, vgpr; };
struct GfxProg {
    bool ok = false; std::string stage, dxil;
    std::vector<GfxBuf> buffers; std::vector<GfxTex> textures; std::vector<GfxSmp> samplers;
    std::vector<GfxVin> vsins; uint32_t vsharp = 0;
};

static bool GfxNativeEnabled()
{
    static bool on = EnvOn("XSX_DEV_NATIVE_GFX"); return on;
}
static std::string GfxEnvPath(const char* name, const char* fallback)
{
    char buf[512]; DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    if (!n) return fallback;
    buf[sizeof buf - 1] = 0; return buf;
}
static bool GfxReadFile(const std::string& path, std::string& out)
{
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    out.clear(); char chunk[65536]; DWORD got;
    while (ReadFile(f, chunk, sizeof chunk, &got, nullptr) && got) out.append(chunk, got);
    CloseHandle(f); return true;
}

static const std::set<std::string>& GfxCacheHashes(const std::string& dir)
{
    // Однократный скан каталога: miss-путь не должен дёргать CreateFileA на каждом из 25k PSO.
    static std::set<std::string> set = [&] {
        std::set<std::string> s;
        WIN32_FIND_DATAA fd = {};
        HANDLE find = FindFirstFileA((dir + "\\*.bind").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::string name = fd.cFileName;
                size_t dot = name.find('.');
                if (dot == 32) s.insert(name.substr(0, 32));
            } while (FindNextFileA(find, &fd));
            FindClose(find);
        }
        return s;
    }();
    return set;
}

static bool GfxLoadProg(const std::string& dir, const std::string& hash, GfxProg& p)
{
    static std::map<std::string, GfxProg> cache; static SRWLOCK lock = SRWLOCK_INIT;
    { SharedSrwLock g(lock); auto it = cache.find(hash); if (it != cache.end()) { p = it->second; return p.ok; } }
    std::string bind, dxil;
    if (!GfxReadFile(dir + "\\" + hash + ".bind", bind) || !GfxReadFile(dir + "\\" + hash + ".dxil", dxil)) { p.ok = false; return false; }
    GfxProg np; np.ok = true; np.dxil = std::move(dxil);
    const char* cur = bind.c_str();
    while (*cur) {
        const char* nl = strchr(cur, '\n');
        const std::string line(cur, nl ? (size_t)(nl - cur) : strlen(cur));
        std::vector<std::string> tok;
        { const char* s = line.c_str(); while (*s) { while (*s && isspace((unsigned char)*s)) s++; if (!*s) break; const char* t = s; while (*s && !isspace((unsigned char)*s)) s++; tok.emplace_back(t, s); } }
        auto num = [&](size_t i) -> uint32_t { return i < tok.size() ? (uint32_t)strtoul(tok[i].c_str(), nullptr, 10) : 0u; };
        if (tok.size() >= 2 && tok[0] == "stage") np.stage = tok[1];
        else if (tok.size() >= 5 && tok[0] == "buffer") np.buffers.push_back({ num(1), num(2), num(3), num(4) });
        else if (tok.size() >= 5 && tok[0] == "texture") np.textures.push_back({ num(1), num(2), num(3), num(4) });
        else if (tok.size() >= 7 && tok[0] == "sampler") { GfxSmp sm; for (int i = 0; i < 4; i++) sm.words[i] = num(1 + i); sm.reg = num(5); sm.space = num(6); np.samplers.push_back(sm); }
        else if (tok.size() >= 4 && tok[0] == "vsharp") np.vsharp = num(3);
        else if (tok.size() >= 5 && tok[0] == "vsin") { GfxVin vin; vin.sem = tok[1]; vin.index = num(2); vin.mask = num(3); vin.vgpr = num(4); np.vsins.push_back(vin); }
        if (!nl) break;
        cur = nl + 1;
    }
    if (np.stage != "VS" && np.stage != "PS") np.ok = false;
    { ExclusiveSrwLock g(lock); cache[hash] = np; }
    p = np; return p.ok;
}

// ---------- хеш-NOP из кода (поля как в xsx_shader_metadata.nop_hash) ----------
static bool GfxCodeHash(const uint8_t* blob, size_t size, std::string& hex)
{
    if (size < 24) return false;
    uint32_t w[6]; for (int i = 0; i < 6; i++) memcpy(&w[i], blob + i * 4, 4);
    if ((w[0] & 0xffff0000u) != 0xd5800000u || (w[1] & ~(0xffu | (0xffu << 9))) != 0x03fe0100u) return false;
    if ((w[3] & 0xffff0000u) != 0xd5800000u || (w[4] & ~(0xffu | (0xffu << 9))) != 0x03fe0100u) return false;
    uint32_t hw[4];
    hw[0] = (w[0] & 0xffff) | ((w[1] & 0xff) << 16) | (((w[1] >> 9) & 0xff) << 24);
    hw[1] = w[2];
    hw[2] = (w[3] & 0xffff) | ((w[4] & 0xff) << 16) | (((w[4] >> 9) & 0xff) << 24);
    hw[3] = w[5];
    char outHex[40];
    for (int i = 0; i < 4; i++) for (int b = 0; b < 4; b++) snprintf(outHex + i * 8 + b * 2, 3, "%02x", (hw[i] >> (b * 8)) & 0xff);
    hex.assign(outHex, 32); return true;
}

// ---------- дамп описания на диск (XSX_PSO_BLOB_DIR) ----------
static void GfxDumpDescription(unsigned seq, const xsx::gfx::PsoDescription& description)
{
    static std::string dir = GfxEnvPath("XSX_PSO_BLOB_DIR", ""); if (dir.empty()) return;
    static const unsigned limit = [] { char v[16]; DWORD n = GetEnvironmentVariableA("XSX_PSO_DUMP_LIMIT", v, sizeof v); return n ? (unsigned)atoi(v) : 8u; }();
    if (seq > limit) return;   // по умолчанию: только первые 8 описаний (25k файлов вешают warmup)
    CreateDirectoryA(dir.c_str(), nullptr);
    { char path[600]; snprintf(path, sizeof path, "%s\\pso%u_desc.bin", dir.c_str(), seq);
      SavePsoMemory(path, &description, sizeof description); }
    const xsx::gfx::PsoSpan spans[]={description.state,description.input,description.ps,
        description.psMetadata,description.vs,description.vsMetadata};
    for (unsigned i=0;i<6;++i) {
        const auto& span=spans[i];
        if (span.address<0x10000 || span.address>>47 || !span.size || span.size>(1u<<20)) continue;
        std::vector<uint8_t> b((size_t)span.size);
        if (!ReadPsoMemory((void*)span.address,b.data(),b.size())) continue;
        char path[600];snprintf(path,sizeof path,"%s\\pso%u_q%u.bin",dir.c_str(),seq,1+2*i);
        SavePsoMemory(path,b.data(),b.size());
    }
}

// ---------- PSO-запись ----------
struct GfxPsoRec {
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* real = nullptr;
    GfxProg ps, vs;
    UINT paramCbvTable = 0, paramSrv1Table = 0, paramSrv2Table = 0;
    UINT bufCount = 0, texCount = 0;         // размеры блоков дескрипторов
    std::vector<std::pair<char, uint32_t>> bufRegs;  // (V/P, reg)
    D3D12_INPUT_LAYOUT_DESC layout = {};
    std::vector<D3D12_INPUT_ELEMENT_DESC> layoutVec;
    std::vector<xsx::gfx::InputElement> decodedLayout;
    DXGI_FORMAT rtv=DXGI_FORMAT_UNKNOWN;
};
static std::map<void*, GfxPsoRec*> g_gfxByWrapped;
static std::map<uintptr_t, GfxPsoRec*> g_gfxByRealPso;
static SRWLOCK g_gfxLock = SRWLOCK_INIT;
static SRWLOCK& GfxBridgeLock() { static SRWLOCK l = SRWLOCK_INIT; return l; }
static std::atomic<unsigned> g_gfxSeq{0};

// Фиксированное состояние: ГИПОТЕЗА (q1 не расшифрован).
struct GfxFixed {
    bool blendEnable = true, depthEnable = false;
    D3D12_BLEND src = D3D12_BLEND_ONE, dst = D3D12_BLEND_INV_SRC_ALPHA;
    DXGI_FORMAT rtv = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE;
    bool frontCCW = false;
};
static GfxFixed GfxFixedFor(const std::string& psHash, const std::string& vsHash)
{
    GfxFixed f;   // premultiplied альфа (Scaleform/blit UI), глубина выключена
    static const std::string rtvEnv = GfxEnvPath("XSX_DEV_GFX_RTV", "28");   // Explicit preview target; validated again at Draw.
    f.rtv = (DXGI_FORMAT)atoi(rtvEnv.c_str());
    (void)psHash; (void)vsHash;   // различий между первыми 4 UI-PSO в модели пока нет
    return f;
}
static void GfxSamplerFromWords(const uint32_t w[4], D3D12_STATIC_SAMPLER_DESC& s)
{
    s = {};
    // ГИПОТЕЗА: минимум полей SQ_S#; linear без mip + addressing по S#1[6:4]
    s.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    UINT addr = (w[1] >> 4) & 7;   // U addressing
    s.AddressU = addr == 1 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : (addr == 2 ? D3D12_TEXTURE_ADDRESS_MODE_BORDER : D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    s.AddressV = s.AddressU; s.AddressW = s.AddressU;
    s.MipLODBias = 0; s.MaxAnisotropy = 1; s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MinLOD = -FLT_MAX; s.MaxLOD = FLT_MAX; s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
}

static ID3D12RootSignature* GfxCreateRootSig(ID3D12Device* dev, const GfxProg& ps, const GfxProg& vs, GfxPsoRec& rec)
{
    UINT bufMaskVS = 0, bufMaskPS = 0;
    for (const auto& b : vs.buffers) bufMaskVS |= 1u << b.reg;
    for (const auto& b : ps.buffers) bufMaskPS |= 1u << b.reg;
    if (bufMaskVS & bufMaskPS) { XS_LOG("GFXNATIVE: VS/PS используют одинаковые t<reg> space1 (маски %x/%x) — нужен другой контракт кэша, отказ", bufMaskVS, bufMaskPS); return nullptr; }
    rec.bufCount = 0;
    for (UINT r = 0; r < 16; r++) if ((bufMaskVS | bufMaskPS) >> r & 1) {
        rec.bufRegs.push_back({ (bufMaskVS >> r & 1) ? 'V' : 'P', r }); rec.bufCount = r + 1;
    }
    rec.texCount = 1;
    for (const auto& t : ps.textures) if (t.reg + 1 > rec.texCount) rec.texCount = t.reg + 1;
    for (const auto& t : vs.textures) if (t.reg + 1 > rec.texCount) rec.texCount = t.reg + 1;
    if (rec.texCount > 32) rec.texCount = 32;

    D3D12_DESCRIPTOR_RANGE1 ranges[3] = {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0 };  // VSharp b0 space2
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, rec.bufCount, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 1 };
    ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, rec.texCount, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 1 + rec.bufCount };
    D3D12_ROOT_PARAMETER1 params[3] = {};
    for (int i = 0; i < 3; i++) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    rec.paramCbvTable = 0; rec.paramSrv1Table = 1; rec.paramSrv2Table = 2;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samp;
    std::set<std::pair<UINT, UINT>> used;
    for (const GfxProg* p : { &ps, &vs }) {
      for (const auto& sm : p->samplers) {
        if (used.count({ sm.reg, sm.space })) continue;
        used.insert({ sm.reg, sm.space });
        D3D12_STATIC_SAMPLER_DESC sd; GfxSamplerFromWords(sm.words, sd);
        sd.ShaderRegister = sm.reg; sd.RegisterSpace = sm.space;
        sd.ShaderVisibility = p == &ps ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_VERTEX;
        samp.push_back(sd);
      }
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd = {}; vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    vd.Desc_1_1.NumParameters = 3; vd.Desc_1_1.pParameters = params;
    vd.Desc_1_1.NumStaticSamplers = (UINT)samp.size(); vd.Desc_1_1.pStaticSamplers = samp.empty() ? nullptr : samp.data();
    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = xsSerializeVersionedRootSignature(&vd, &sig, &err);
    if (FAILED(hr)) { XS_LOG("GFXNATIVE сериализация root sig hr=0x%08lX %s", (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : ""); if (err) err->Release(); return nullptr; }
    ID3D12RootSignature* root = nullptr;
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&root);
    sig->Release(); if (err) err->Release();
    if (FAILED(hr)) { XS_LOG("GFXNATIVE CreateRootSignature hr=0x%08lX (проверьте поддержку root sig 1.1/spaces в VKD3D)", (unsigned long)hr); return nullptr; }
    return root;
}

static HRESULT GfxNativeCreatePso57(Wrap* w, const void* desc, REFIID iid, void** out)
{
    if (!GfxNativeEnabled()) return E_NOTIMPL;
    (void)iid;
    ID3D12Device* dev = (ID3D12Device*)w->real;
    xsx::gfx::PsoDescription raw{},description{};
    if (!ReadPsoMemory(desc,&raw,sizeof raw) || !xsx::gfx::DecodePsoDescription(&raw,sizeof raw,description)) return E_NOTIMPL;
    const unsigned seq = ++g_gfxSeq;
    GfxDumpDescription(seq, description);
    const uint64_t psSz=description.ps.size,psPtr=description.ps.address;
    const uint64_t vsSz=description.vs.size,vsPtr=description.vs.address;
    if (psSz < 40 || psSz > (1u << 20) || vsSz < 40 || vsSz > (1u << 20)) return E_NOTIMPL;
    std::vector<uint8_t> psCode((size_t)psSz), vsCode((size_t)vsSz);
    if (!ReadPsoMemory((void*)psPtr, psCode.data(), psCode.size()) || !ReadPsoMemory((void*)vsPtr, vsCode.data(), vsCode.size())) return E_NOTIMPL;
    std::string psHash, vsHash;
    if (!GfxCodeHash(psCode.data(), psCode.size(), psHash) || !GfxCodeHash(vsCode.data(), vsCode.size(), vsHash)) {
        static std::atomic<unsigned> missN{0};
        if (missN++ < 8) XS_LOG("GFXNATIVE #%u: в p2/p4 нет hash-NOP — структура описания не совпала с доказанной", seq);
        return E_NOTIMPL;
    }
    static const std::string dir = GfxEnvPath("XSX_DEV_SHADER_CACHE", "C:\\shader-cache");
    if (!GfxCacheHashes(dir).count(psHash) || !GfxCacheHashes(dir).count(vsHash)) {
        static std::set<std::string> reported; static SRWLOCK rl = SRWLOCK_INIT;
        const std::string tag = psHash + " " + vsHash; bool fresh; { ExclusiveSrwLock g(rl); fresh = reported.insert(tag).second; }
        if (fresh) XS_LOG("GFXNATIVE #%u: программ нет в кэше %s (xsx_gfx_translate+build-gfx-cache) -> E_NOTIMPL", seq, tag.c_str());
        return E_NOTIMPL;
    }
    GfxProg ps, vs;
    if (!GfxLoadProg(dir, psHash, ps) || !GfxLoadProg(dir, vsHash, vs) || ps.stage != "PS" || vs.stage != "VS") {
        static std::set<std::string> reported; static SRWLOCK rl = SRWLOCK_INIT;
        const std::string tag = psHash + " " + vsHash; bool fresh; { ExclusiveSrwLock g(rl); fresh = reported.insert(tag).second; }
        if (fresh) XS_LOG("GFXNATIVE #%u: программ нет в кэше %s (xsx_gfx_translate+build-gfx-cache) -> E_NOTIMPL", seq, tag.c_str());
        return E_NOTIMPL;
    }
    GfxPsoRec* rec = new GfxPsoRec(); rec->ps = ps; rec->vs = vs;
    rec->root = GfxCreateRootSig(dev, ps, vs, *rec);
    if (!rec->root) { delete rec; return E_NOTIMPL; }
    std::vector<uint8_t> layoutBlob;
    bool layoutOk=description.input.size>=0x50 && description.input.size<=65536;
    if (layoutOk) { layoutBlob.resize((size_t)description.input.size);layoutOk=ReadPsoMemory((void*)description.input.address,layoutBlob.data(),layoutBlob.size()) &&
        xsx::gfx::DecodeInputLayout(layoutBlob.data(),layoutBlob.size(),rec->decodedLayout); }
    if (!layoutOk) { XS_LOG("GFXNATIVE #%u input layout rejected (no guessed formats)",seq);rec->root->Release();delete rec;return E_NOTIMPL; }
    for (const auto& vin:vs.vsins) {
        const xsx::gfx::InputElement* match=nullptr;
        for (const auto& e:rec->decodedLayout) if (e.semantic==vin.sem && e.index==vin.index) match=&e;
        if (!match) { rec->root->Release();delete rec;return E_NOTIMPL; }
    }
    for (const auto& vin:rec->decodedLayout) {
        D3D12_INPUT_ELEMENT_DESC e={};
        e.SemanticName=vin.semantic.c_str();e.SemanticIndex=vin.index;e.Format=(DXGI_FORMAT)vin.format;
        e.InputSlot=vin.slot;e.AlignedByteOffset=vin.offset;
        e.InputSlotClass=(D3D12_INPUT_CLASSIFICATION)vin.classification;e.InstanceDataStepRate=vin.step;
        rec->layoutVec.push_back(e);
    }
    rec->layout.NumElements=(UINT)rec->layoutVec.size();rec->layout.pInputElementDescs=rec->layoutVec.data();
    const GfxFixed fx = GfxFixedFor(psHash, vsHash);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rec->root;
    pd.VS.pShaderBytecode = (void*)vs.dxil.data(); pd.VS.BytecodeLength = vs.dxil.size();
    pd.PS.pShaderBytecode = (void*)ps.dxil.data(); pd.PS.BytecodeLength = ps.dxil.size();
    pd.InputLayout = rec->layout;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = fx.rtv; pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1; pd.SampleMask = ~UINT(0);
    // Offset inferred from the three intact captured state containers. Reject
    // other container shapes; depth/raster/write mask remain UI preview policy.
    uint32_t state[60]{};
    if (!ReadPsoMemory((void*)description.state.address,state,sizeof state) ||
        state[0]!=0x4005d808u || !xsx::gfx::NativeBlend(state[0xd0/4],pd.BlendState.RenderTarget[0])) {
        XS_LOG("GFXNATIVE #%u unsupported fixed-state container bytes=%llu magic=%#x blend=%#x PS=%s VS=%s",seq,
            (unsigned long long)description.state.size,state[0],state[0xd0/4],psHash.c_str(),vsHash.c_str());
        rec->root->Release();delete rec;return E_NOTIMPL;
    }
    rec->rtv=fx.rtv;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = fx.cull;
    pd.RasterizerState.FrontCounterClockwise = fx.frontCCW ? TRUE : FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE; pd.RasterizerState.SlopeScaledDepthBias = 1.0f;
    pd.DepthStencilState.DepthEnable = fx.depthEnable ? TRUE : FALSE;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    HRESULT hr = dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&rec->real);
    if (FAILED(hr)) {
        static std::atomic<unsigned> failN{0};
        if (failN++ < 8) XS_LOG("GFXNATIVE #%u CreateGraphicsPipelineState hr=0x%08lX PS=%s VS=%s rtv=%d (ГИПОТЕЗА-состояние; XSX_DEV_GFX_RTV меняет формат)", seq, (unsigned long)hr, psHash.c_str(), vsHash.c_str(), (int)fx.rtv);
        rec->root->Release(); delete rec; return E_NOTIMPL;
    }
    // A native PSO may be the first child created on this device. Publish only
    // after the wrapper tables (including QI/AddRef/Release) are initialized.
    InitObjVts();
    *out = rec->real;
    hr = WrapOutput(S_OK, out, g_vtPso, 5);
    if (FAILED(hr)) { rec->root->Release(); delete rec; return hr; }
    rec->real->AddRef(); // Cache owns an independent reference, beyond title Release.
    { ExclusiveSrwLock g(g_gfxLock); g_gfxByWrapped[*out] = rec; g_gfxByRealPso[(uintptr_t)rec->real] = rec; }
    XS_LOG("GFXNATIVE #%u НАСТОЯЩИЙ PSO из кэша: PS=%s VS=%s pso=%p bufSlots=%u texSlots=%u layout=%u rtv=%u blend=%#x",
        seq, psHash.c_str(), vsHash.c_str(), (void*)rec->real, rec->bufCount, rec->texCount, (unsigned)rec->layoutVec.size(), (unsigned)rec->rtv, state[0xd0/4]);
    return hr;
}


#include "gfx_native_draw.h"
