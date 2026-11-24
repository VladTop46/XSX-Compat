// xg_xs.dll — XG (Xbox Graphics) texture address computer. ЛИНЕЙНАЯ эмуляция (БЕЗ swizzle/tiling Xbox): игра пишет текстуры через CopyIntoSubresource, а PC-ресурсы линейны,
// поэтому данные согласованы только если путь записи идёт через этот объект. Сигнатуры по использованию в игре (callsite 0x1416f7141: XGCreateTextureComputer(const XG_TEXTURE2D_DESC*, ppComputer))
// и DirectXTex (CopyIntoSubresource(base, plane, subresource, pixels, rowPitch, slicePitch)). Раскладка vtable: 0-2 IUnknown, 8 = CopyIntoSubresource (по игре), 9 = CopyFromSubresource (гипотеза);
// слоты 3..7 (GetResourceLayout, GetTexelElementOffsetBytes, …) — лог и E_NOTIMPL, пока не увидим реальные вызовы. XG_RESOURCE_LAYOUT не воспроизводим (раскладка полей неизвестна).
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#define XLOG(...) do { fprintf(stderr, "[XG] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
struct Desc2D { uint32_t Width, Height, MipLevels, ArraySize, Format, SCount, SQuality, Usage, BindFlags, CPUAccess, MiscFlags, ESRAM, TileMode, Pitch; };   // 14*4 = 0x38
static void ElemInfo(uint32_t fmt, uint32_t& bpe, uint32_t& block)   // DXGI-совместимые значения XG_FORMAT
{
    block = 1; bpe = 4;
    if (fmt >= 2 && fmt <= 4) bpe = 16; else if (fmt >= 6 && fmt <= 8) bpe = 12; else if ((fmt >= 9 && fmt <= 14) || (fmt >= 15 && fmt <= 18)) bpe = 8;
    else if (fmt >= 19 && fmt <= 22) bpe = 8; else if ((fmt >= 23 && fmt <= 34) || (fmt >= 35 && fmt <= 38)) bpe = 4; else if (fmt >= 39 && fmt <= 47) bpe = 4;
    else if (fmt >= 48 && fmt <= 59) bpe = (fmt <= 53 ? 2 : 2); else if (fmt >= 60 && fmt <= 65) bpe = 1; else if (fmt == 66) bpe = 1; else if (fmt >= 67 && fmt <= 70) bpe = 4;
    else if (fmt >= 71 && fmt <= 73) { bpe = 8; block = 4; } else if (fmt >= 74 && fmt <= 79) { bpe = 16; block = 4; } else if (fmt >= 80 && fmt <= 84) { bpe = (fmt <= 82 ? 8 : 16); block = 4; }
    else if (fmt >= 85 && fmt <= 89) bpe = 2; else if (fmt >= 90 && fmt <= 99) bpe = 4; else if (fmt >= 94 && fmt <= 99) { bpe = 16; block = 4; }
    if (fmt >= 94 && fmt <= 99) { bpe = 16; block = 4; }
}
struct Obj { void** vt; volatile LONG ref; Desc2D d; uint32_t bpe, block; std::vector<uint64_t> off; std::vector<uint32_t> pitch, rows; uint64_t total; };
static void Build(Obj* o)
{
    uint64_t cur = 0; o->off.clear(); o->pitch.clear(); o->rows.clear();
    for (uint32_t a = 0; a < (o->d.ArraySize ? o->d.ArraySize : 1); a++) for (uint32_t m = 0; m < (o->d.MipLevels ? o->d.MipLevels : 1); m++) {
        uint32_t w = o->d.Width >> m; if (!w) w = 1; uint32_t h = o->d.Height >> m; if (!h) h = 1;
        uint32_t we = (w + o->block - 1) / o->block, he = (h + o->block - 1) / o->block, pitch = (we * o->bpe + 255) & ~255u;
        cur = (cur + 0xFFFF) & ~0xFFFFull; o->off.push_back(cur); o->pitch.push_back(pitch); o->rows.push_back(he); cur += (uint64_t)pitch * he;
    }
    o->total = cur;
}
// Раскладка по игре (callsite 0x145121a84): [0]=AddRef(?), [1]=Release (+8, без аргументов), [2]=GetResourceLayout(XG_RESOURCE_LAYOUT* out) (+0x10; структура 0x6c8 байт копируется memcpy'ем), [8]=CopyIntoSubresource. QueryInterface у интерфейса НЕТ.
// XG_RESOURCE_LAYOUT: точная раскладка неизвестна — заполняем нулями и ПРЕДПОЛОЖИТЕЛЬНО {+0 SizeBytes u64, +8 BaseAlignmentBytes u64, +0x10 MipLevels u32, +0x14 Planes u32}.
static HRESULT __stdcall GetResourceLayout(Obj* o, void* out)
{
    if (!out || IsBadWritePtr(out, 0x6c8)) return E_POINTER; memset(out, 0, 0x6c8);
    *(uint64_t*)out = o->total; *((uint64_t*)out + 1) = 0x10000; *((uint32_t*)out + 4) = o->d.MipLevels ? o->d.MipLevels : 1; *((uint32_t*)out + 5) = 1;
    static int n = 0; if (n++ < 4) XLOG("GetResourceLayout -> size=%#llx (раскладка структуры — ПРЕДПОЛОЖЕНИЕ)", (unsigned long long)o->total); return S_OK;
}
static ULONG __stdcall AddRef(Obj* o) { return InterlockedIncrement(&o->ref); }
static ULONG __stdcall Release(Obj* o) { LONG r = InterlockedDecrement(&o->ref); if (!r) delete o; return r; }
static HRESULT __stdcall Unimpl(Obj*, uintptr_t a, uintptr_t b, uintptr_t c) { XLOG("метод computer НЕ реализован (a=%p b=%p c=%p)", (void*)a, (void*)b, (void*)c); return E_NOTIMPL; }
template <int N> static HRESULT __stdcall Slot(Obj*, uintptr_t a, uintptr_t b, uintptr_t c) { XLOG("computer slot %d НЕ реализован (a=%p b=%p c=%p) -> E_NOTIMPL", N, (void*)a, (void*)b, (void*)c); return E_NOTIMPL; }
static HRESULT __stdcall CopyInto(Obj* o, uint8_t* base, uint32_t plane, uint32_t sub, const uint8_t* src, uint32_t rowPitch, uint32_t slicePitch)
{
    if (sub >= o->off.size()) { XLOG("CopyIntoSubresource: subresource %u вне диапазона (%zu)", sub, o->off.size()); return E_INVALIDARG; }
    uint32_t rowBytes = rowPitch ? rowPitch : o->pitch[sub]; uint32_t copy = rowBytes < o->pitch[sub] ? rowBytes : o->pitch[sub];
    static int n = 0; if (n++ < 8) XLOG("CopyIntoSubresource base=%p plane=%u sub=%u src=%p rowPitch=%u slicePitch=%u -> off=%#llx pitch=%u rows=%u", base, plane, sub, src, rowPitch, slicePitch, (unsigned long long)o->off[sub], o->pitch[sub], o->rows[sub]);
    for (uint32_t r = 0; r < o->rows[sub]; r++) memcpy(base + o->off[sub] + (uint64_t)r * o->pitch[sub], src + (uint64_t)r * rowBytes, copy);
    return S_OK;
}
static HRESULT __stdcall CopyFrom(Obj* o, uint8_t* dst, uint32_t dstPitch, uint32_t dstSlice, const uint8_t* base, uint32_t plane, uint32_t sub)
{
    if (sub >= o->off.size()) return E_INVALIDARG; uint32_t rb = dstPitch ? dstPitch : o->pitch[sub]; uint32_t copy = rb < o->pitch[sub] ? rb : o->pitch[sub];
    for (uint32_t r = 0; r < o->rows[sub]; r++) memcpy(dst + (uint64_t)r * rb, base + o->off[sub] + (uint64_t)r * o->pitch[sub], copy); return S_OK;
}
static void* g_vt[16] = { (void*)AddRef, (void*)Release, (void*)GetResourceLayout, (void*)Slot<3>, (void*)Slot<4>, (void*)Slot<5>, (void*)Slot<6>, (void*)Slot<7>, (void*)CopyInto, (void*)CopyFrom,
                          (void*)Slot<10>, (void*)Slot<11>, (void*)Slot<12>, (void*)Slot<13>, (void*)Slot<14>, (void*)Slot<15> };
extern "C" {
__declspec(dllexport) HRESULT WINAPI XGCreateTextureComputer(const Desc2D* d, void** out)
{
    if (!d || !out) return E_INVALIDARG; auto* o = new Obj(); o->vt = g_vt; o->ref = 1; o->d = *d; ElemInfo(d->Format, o->bpe, o->block); Build(o); *out = o;
    XLOG("XGCreateTextureComputer %ux%u mips=%u arr=%u fmt=%u tile=%u pitch=%u -> LINEAR bpe=%u block=%u total=%#llx", d->Width, d->Height, d->MipLevels, d->ArraySize, d->Format, d->TileMode, d->Pitch, o->bpe, o->block, (unsigned long long)o->total);
    return S_OK;
}
// XGSuggestSwizzleMode(...): сигнатура неизвестна — возвращаем 0 (линейный/«нет предложения») и логируем.
__declspec(dllexport) uintptr_t XGSuggestSwizzleMode(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { XLOG("XGSuggestSwizzleMode(%p,%p,%p,%p) -> 0", (void*)a, (void*)b, (void*)c, (void*)d); return 0; }
}
