#pragma once
// AMD CB_BLENDn_CONTROL bitfields, not a claim about an Xbox PSO blob offset.
// Hardware reference: GPUOpen PAL gfx9_plus_merged_registers.h, CB_BLEND0_CONTROL.
// Only directly representable, non-dual-source D3D12 factors are accepted.
#include <cstdint>

namespace xsx::gfx {
struct BlendControl {
    unsigned colorSource, colorDestination, colorOperation;
    unsigned alphaSource, alphaDestination, alphaOperation;
    bool enabled, separateAlpha, ropDisabled;
};
inline bool DecodeBlendControl(uint32_t word, BlendControl& out)
{
    if (word & 0x0000e000u) return false; // Reserved bits.
    out = {word & 31u, (word >> 8) & 31u, (word >> 5) & 7u,
           (word >> 16) & 31u, (word >> 24) & 31u, (word >> 21) & 7u,
           bool(word & (1u << 30)), bool(word & (1u << 29)), bool(word & (1u << 31))};
    if (!out.separateAlpha) {
        out.alphaSource = out.colorSource; out.alphaDestination = out.colorDestination;
        out.alphaOperation = out.colorOperation;
    }
    return true;
}
#ifdef _WIN32
// Include d3d12.h before this header when using the native mapping.
inline bool NativeBlendFactor(unsigned f, D3D12_BLEND& out)
{
    switch (f) {
    case 0: out=D3D12_BLEND_ZERO; break;
    case 1: out=D3D12_BLEND_ONE; break;
    case 2: out=D3D12_BLEND_SRC_COLOR; break;
    case 3: out=D3D12_BLEND_INV_SRC_COLOR; break;
    case 4: out=D3D12_BLEND_SRC_ALPHA; break;
    case 5: out=D3D12_BLEND_INV_SRC_ALPHA; break;
    case 6: out=D3D12_BLEND_DEST_ALPHA; break;
    case 7: out=D3D12_BLEND_INV_DEST_ALPHA; break;
    case 8: out=D3D12_BLEND_DEST_COLOR; break;
    case 9: out=D3D12_BLEND_INV_DEST_COLOR; break;
    case 10: out=D3D12_BLEND_SRC_ALPHA_SAT; break;
    default: return false;
    }
    return true;
}
inline bool NativeBlendOperation(unsigned f, D3D12_BLEND_OP& out)
{
    switch (f) {
    case 0: out=D3D12_BLEND_OP_ADD; break;
    case 1: out=D3D12_BLEND_OP_SUBTRACT; break;
    case 2: out=D3D12_BLEND_OP_MIN; break;
    case 3: out=D3D12_BLEND_OP_MAX; break;
    case 4: out=D3D12_BLEND_OP_REV_SUBTRACT; break;
    default: return false;
    }
    return true;
}
inline bool NativeBlend(uint32_t word, D3D12_RENDER_TARGET_BLEND_DESC& out)
{
    BlendControl b{};
    D3D12_RENDER_TARGET_BLEND_DESC result{};
    if (!DecodeBlendControl(word,b) || !b.ropDisabled ||
        !NativeBlendFactor(b.colorSource,result.SrcBlend) ||
        !NativeBlendFactor(b.colorDestination,result.DestBlend) ||
        !NativeBlendFactor(b.alphaSource,result.SrcBlendAlpha) ||
        !NativeBlendFactor(b.alphaDestination,result.DestBlendAlpha) ||
        !NativeBlendOperation(b.colorOperation,result.BlendOp) ||
        !NativeBlendOperation(b.alphaOperation,result.BlendOpAlpha)) return false;
    // D3D12 forbids COLOR factors in the alpha equation: expand the equivalent.
    auto alpha = [](D3D12_BLEND f) {
        switch(f) {
        case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
        case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
        case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
        case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
        case D3D12_BLEND_SRC_ALPHA_SAT: return D3D12_BLEND_ONE;
        default: return f;
        }
    };
    result.SrcBlendAlpha=alpha(result.SrcBlendAlpha);
    result.DestBlendAlpha=alpha(result.DestBlendAlpha);
    result.BlendEnable=b.enabled; result.LogicOp=D3D12_LOGIC_OP_NOOP;
    result.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL; // Explicit probe contract, not decoded.
    out=result;
    return true;
}
#endif
}
