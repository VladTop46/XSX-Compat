#pragma once
// Included after the native PSO decoder. Recording owns immutable descriptors
// and uploads. Submission takes another owner until a real queue fence passes.
// Unknown guest descriptors are diagnosed and skipped, never sent to VKD3D.
static std::atomic<uint64_t> g_gfxLiveBytes{0};
struct GfxRefresh { ID3D12Resource* buffer; uint64_t guest,size; const char* what; uint64_t hash; };
// FNV-1a over 8-byte words (tail bytewise): cheap change detection without reading back UPLOAD (write-combined) memory.
static uint64_t GfxHash(const uint8_t* p,size_t n) { uint64_t h=1469598103934665603ull; size_t i=0;
    for (;i+8<=n;i+=8) { uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ull; }
    for (;i<n;++i) h=(h^p[i])*1099511628211ull; return h; }
struct GfxDrawStorage {
    std::vector<IUnknown*> objects;
    std::vector<GfxRefresh> refresh; // Guest ranges re-read at submit (GPU reads at execution on Xbox).
    bool submitted=false;
    uint64_t bytes=0;
    ~GfxDrawStorage() { for (auto* p:objects) p->Release();g_gfxLiveBytes-=bytes; }
    void own(IUnknown* p) { objects.push_back(p); }
};
using GfxStorage=std::shared_ptr<GfxDrawStorage>;
struct GfxRtv {
    ID3D12Resource* resource=nullptr;
    IUnknown* exposed=nullptr;
    uint64_t native=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    ~GfxRtv() { if (exposed) exposed->Release(); }
};
static std::map<uint64_t,std::shared_ptr<GfxRtv>> g_gfxRtvs;
// Typed SRV creation registry. Guest d3d12core writes opaque tagged records
// (observed dword0=0x63, rest zero) into shader tables; the same tag sits in
// the guest CPU slot passed to the typed CreateShaderResourceView, where the
// real native descriptor now lives. Resolution is "latest preceding record
// with the same tag" — an explicit heuristic until pixels prove otherwise.
struct GfxSrvRec { uint64_t guest=0,native=0; uint32_t tag=0; ID3D12Resource* exposed=nullptr; unsigned seq=0;
                   D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; bool hasDesc=false; };
static std::vector<GfxSrvRec> g_gfxSrvs; static unsigned g_gfxSrvSeq=0;
static bool GfxSrvBridgeEnabled()
{
    static bool on = [] { char v[4]; return !(GetEnvironmentVariableA("XSX_DEV_NATIVE_SRV", v, sizeof v) > 0 && v[0]=='0'); }();
    return on;
}
static void GfxNativeRememberSrv(ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,uint64_t guest,uint64_t native)
{
    if (!guest || !native) return;
    uint32_t tag=0; ReadPsoMemory((void*)guest,&tag,sizeof tag);
    GfxSrvRec rec; rec.guest=guest; rec.native=native; rec.tag=tag; rec.exposed=resource;
    if (desc) { rec.desc=*desc; rec.hasDesc=true; }
    if (resource) resource->AddRef();
    std::vector<GfxSrvRec> evicted;
    { ExclusiveSrwLock held(GfxBridgeLock());
      rec.seq=++g_gfxSrvSeq; g_gfxSrvs.push_back(rec);
      if (g_gfxSrvs.size()>4096) { evicted.assign(g_gfxSrvs.begin(),g_gfxSrvs.begin()+1024);
        g_gfxSrvs.erase(g_gfxSrvs.begin(),g_gfxSrvs.begin()+1024); } }
    for (auto& e:evicted) if (e.exposed) e.exposed->Release();
    static std::atomic<unsigned> logged{0};
    if (logged++<16) XS_LOG("GFXNATIVE SRV reg seq=%u tag=%#x guest=%#llx native=%#llx dim=%u fmt=%u",rec.seq,tag,
        (unsigned long long)guest,(unsigned long long)native,desc?(unsigned)desc->ViewDimension:0u,desc?(unsigned)desc->Format:0u);
}
static std::map<Wrap*,std::shared_ptr<GfxRtv>> g_gfxTargets;
static std::map<Wrap*,std::vector<GfxStorage>> g_gfxRecorded;
struct GfxFlight {
    ID3D12Fence* fence=nullptr;
    std::vector<GfxStorage> draws;
    ~GfxFlight() { if (fence) fence->Release(); }
};
static std::vector<std::shared_ptr<GfxFlight>> g_gfxFlights;
static void GfxCollectCompleted() // Under GfxBridgeLock.
{
    for (auto it=g_gfxFlights.begin();it!=g_gfxFlights.end();) {
        const UINT64 done=(*it)->fence?(*it)->fence->GetCompletedValue():0;
        if (done!=UINT64_MAX && done>=1) it=g_gfxFlights.erase(it);else ++it;
    }
}
static void GfxNativeOnReset(Wrap* w)
{
    ExclusiveSrwLock held(GfxBridgeLock());
    GfxCollectCompleted();g_gfxRecorded.erase(w);g_gfxTargets.erase(w);
}
static bool GfxRefreshOn() { static bool on=[]{ char v[4]; return !(GetEnvironmentVariableA("XSX_DEV_GFX_REFRESH",v,sizeof v)>0 && v[0]=='0'); }(); return on; }
// Xbox GPU reads guest memory when the list executes, not when Draw is
// recorded. Re-copy each guest range once, at its first submission; replays
// keep the first-submission contents (the GPU may still read them).
static void GfxRefreshAtSubmit(const std::vector<GfxStorage>& draws)
{
    static std::atomic<unsigned> logged{0},changedTotal{0};
    std::vector<uint8_t> tmp;
    for (const auto& st:draws) {
        if (!st || st->submitted) continue;
        st->submitted=true;
        if (!GfxRefreshOn()) continue;
        for (const auto& r:st->refresh) {
            tmp.resize((size_t)r.size);
            if (!ReadPsoMemory((void*)r.guest,tmp.data(),tmp.size())) continue;
            if (GfxHash(tmp.data(),tmp.size())==r.hash) continue;   // unchanged: no mapping, no copy
            void* mapped=nullptr;D3D12_RANGE none={};
            if (FAILED(r.buffer->Map(0,&none,&mapped))) continue;
            const size_t diff=tmp.size();   // changed (exact byte count not computed: would read WC memory)
            memcpy(mapped,tmp.data(),tmp.size());
            D3D12_RANGE written={0,(SIZE_T)tmp.size()};r.buffer->Unmap(0,&written);
            if (diff) { ++changedTotal; if (logged++<24) XS_LOG("GFXNATIVE REFRESH %s guest=%#llx bytes=%llu range_bytes=%zu changed (draw-time snapshot was stale)",r.what,(unsigned long long)r.guest,(unsigned long long)r.size,diff); }
        }
    }
    static std::atomic<unsigned> calls{0};
    if (!(++calls%2000)) XS_LOG("GFXNATIVE REFRESH summary submits=%u stale_ranges=%u",calls.load(),changedTotal.load());
}
static HRESULT GfxNativeSubmit(Wrap* w,UINT n,ID3D12CommandList* const* lists)
{
    static SRWLOCK serial=SRWLOCK_INIT;
    std::unique_ptr<ProfScope> lockProf(new ProfScope(PROF_SUB_LOCK));
    ExclusiveSrwLock submit(serial);
    lockProf.reset();
    auto flight=std::make_shared<GfxFlight>();
    {
        ExclusiveSrwLock held(GfxBridgeLock());GfxCollectCompleted();
        for (UINT i=0;i<n;++i) {
            auto it=g_gfxRecorded.find((Wrap*)lists[i]);
            if (it!=g_gfxRecorded.end()) flight->draws.insert(flight->draws.end(),it->second.begin(),it->second.end());
        }
    }
    if (flight->draws.empty()) return SubmitListsWithWrites(w,n,lists);
    { PROF_SCOPE(PROF_SUB_REFRESH); GfxRefreshAtSubmit(flight->draws); }
    auto* q=(ID3D12CommandQueue*)w->real;
    ID3D12Device* dev=nullptr;
    HRESULT hr;
    { PROF_SCOPE(PROF_SUB_FENCE); hr=q->GetDevice(__uuidof(ID3D12Device),(void**)&dev);
      if (SUCCEEDED(hr)) { hr=dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),(void**)&flight->fence);dev->Release(); } }
    if (FAILED(hr)) return hr; // Nothing submitted, local owners can be dropped.
    { PROF_SCOPE(PROF_SUB_EXEC); hr=SubmitListsWithWrites(w,n,lists); }
    HRESULT signal; { PROF_SCOPE(PROF_SUB_SIGNAL); signal=q->Signal(flight->fence,1); }
    if (FAILED(signal)) {
        // Completion unknown: retain permanently, rather than recycling memory.
        flight->fence->Release();flight->fence=nullptr;
        XS_LOG("GFXNATIVE submission fence failed hr=%08lX; allocations retained",(unsigned long)signal);
    }
    { ExclusiveSrwLock held(GfxBridgeLock());g_gfxFlights.push_back(flight); }
    return FAILED(hr)?hr:signal;
}
static void GfxNativeRememberRtv(ID3D12Resource* resource,const D3D12_RENDER_TARGET_VIEW_DESC* desc,uint64_t guest,uint64_t native)
{
    if (!resource) return;
    auto rec=std::make_shared<GfxRtv>();resource->AddRef();rec->exposed=resource;
    rec->resource=(ID3D12Resource*)Unwrap(resource);rec->native=native;
    rec->format=desc?desc->Format:rec->resource->GetDesc().Format;
    std::vector<std::shared_ptr<GfxRtv>> old;
    { ExclusiveSrwLock held(GfxBridgeLock());
      auto a=g_gfxRtvs.find(guest);if (a!=g_gfxRtvs.end()) old.push_back(a->second);
      auto b=g_gfxRtvs.find(native);if (b!=g_gfxRtvs.end()) old.push_back(b->second);
      g_gfxRtvs[guest]=rec;g_gfxRtvs[native]=rec; }
    // Any final title allocation callback runs after the registry lock drops.
}
static ID3D12Resource* GfxSnapshotBuffer(ID3D12Device* dev,uint64_t guest,uint64_t size,GfxDrawStorage& storage,const char* refreshWhat=nullptr)
{
    if (!guest || !size || size>(8ull<<20) || guest>UINT64_MAX-size) return nullptr;
    const uint64_t rounded=(size+255)&~255ull;
    if (g_gfxLiveBytes+rounded>(256ull<<20)) return nullptr;
    D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd={};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width=rounded;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;
    rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer=nullptr;
    HRESULT hr=dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,__uuidof(ID3D12Resource),(void**)&buffer);
    if (FAILED(hr)) return nullptr;
    void* mapped=nullptr;D3D12_RANGE none={};hr=buffer->Map(0,&none,&mapped);
    if (FAILED(hr)) { buffer->Release();return nullptr; }
    // Read into cached memory first: the hash must not be computed from the write-combined mapping.
    static thread_local std::vector<uint8_t> tmp; tmp.assign((size_t)rounded,0);
    const bool read=ReadPsoMemory((void*)guest,tmp.data(),(size_t)size);
    memcpy(mapped,tmp.data(),(size_t)rounded);
    D3D12_RANGE written={0,(SIZE_T)rounded};buffer->Unmap(0,&written);
    if (!read) { buffer->Release();return nullptr; }
    storage.own(buffer);storage.bytes+=rounded;g_gfxLiveBytes+=rounded;
    if (refreshWhat) storage.refresh.push_back({buffer,guest,size,refreshWhat,GfxHash(tmp.data(),(size_t)size)});
    return buffer;
}
static bool GfxReadVsharp(uint64_t address,uint32_t words[8],uint64_t& guest,uint64_t& bytes,uint32_t& stride,uint32_t& records)
{
    if (!ReadPsoMemory((void*)address,words,32)) return false;
    if (words[0]==kViewTag) {
        ViewRec view={};
        { SharedSrwLock held(g_viewLock);
          if (words[1]>=g_views.size()) return false;
          view=g_views[words[1]]; }
        if (view.kind || words[2]!=view.gen || (view.B[0]>>32)!=D3D12_SRV_DIMENSION_BUFFER) return false;
        stride=(uint32_t)(view.B[3]>>32);records=(uint32_t)view.B[3];
        const uint32_t elem=stride?stride:ConsoleElementBytes((uint32_t)view.B[0]);
        if (!elem || !records || view.B[2]>(UINT64_MAX-view.B[5])/elem) return false;
        guest=view.B[5]+view.B[2]*elem;bytes=(uint64_t)records*elem;
        return true;
    }
    xsx::gfx::BufferDescriptor descriptor={};
    if (!xsx::gfx::DecodeBufferDescriptor(words,descriptor) || !xsx::gfx::LinearBufferBytes(descriptor,bytes)) return false;
    guest=descriptor.address;stride=descriptor.stride;records=descriptor.records;
    return true;
}
static void GfxDescribeTexture(Wrap* w,uint64_t table,const GfxTex& texture)
{
    // T# is an IMAGE descriptor, never V#. Decode only the public GFX10
    // identity fields here; creating/uploading the image needs separate work.
    uint32_t words[8]={};const bool read=table && ReadPsoMemory((void*)(table+texture.off),words,sizeof words);
    static std::atomic<unsigned> logged{0};if (logged++>=16) return;
    const uint64_t address=table+texture.off;
    GfxViewTrace("texture-use",w,__builtin_return_address(0),address,XlatCpu(address),nullptr,nullptr,nullptr,0,texture.off,texture.reg);
    const uint64_t base=((uint64_t)(words[1]&0xff)<<32|words[0])<<8;
    const uint32_t width=(((words[2]&0x3fff)<<2)|(words[1]>>30))+1;
    const uint32_t height=((words[2]>>14)&0xffff)+1;
    XS_LOG("GFXNATIVE T# off=%u readable=%u words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x base=%#llx size=%ux%u type=%u swizzle=%u format=%u (image bridge pending)",
        texture.off,read,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],
        (unsigned long long)base,width,height,words[3]>>28,(words[3]>>20)&31,(words[1]>>20)&511);
}
// ---- Black-frame diagnostics (read-only unless XSX_DEV_GFX_MARK=1) ----------
// Proves whether native draws target the resource PresentX later shows and
// whether the forwarded PC slots 21/22 really carry viewport/scissor data.
struct GfxVpState { D3D12_VIEWPORT vp{}; D3D12_RECT sc{}; UINT nvp=0,nsc=0; bool vpSet=false,scSet=false; };
static std::map<Wrap*,GfxVpState> g_gfxVp; // Under GfxBridgeLock.
static std::map<ID3D12Resource*,unsigned> g_gfxDrawsByTarget; // Under GfxBridgeLock; reset on present.
static void* g_gfxOrigRs21=nullptr; static void* g_gfxOrigRs22=nullptr;
static bool GfxMarkOn() { static bool on=[]{ char v[4]; return GetEnvironmentVariableA("XSX_DEV_GFX_MARK",v,sizeof v)>0 && v[0]=='1'; }(); return on; }
static void GfxDumpFloats(const char* what,uint64_t guest,uint64_t bytes)
{
    uint32_t d[32]={};const size_t n=(size_t)std::min<uint64_t>(bytes,sizeof d)/4;
    if (!n || !ReadPsoMemory((void*)guest,d,n*4)) { XS_LOG("GFXNATIVE %s guest=%#llx unreadable",what,(unsigned long long)guest);return; }
    for (size_t row=0;row<n;row+=8) {
        char line[512];int p=snprintf(line,sizeof line,"GFXNATIVE %s +%zu:",what,row*4);
        for (size_t i=row;i<n && i<row+8;++i) { float f;memcpy(&f,&d[i],4);p+=snprintf(line+p,sizeof line-p," %08x(%g)",d[i],f); }
        XS_LOG("%s",line);
    }
}
// Xbox D3D_PRIMITIVE_TOPOLOGY 0x11 = RECTLIST (AMD): three vertices of an
// axis-aligned rectangle; the hardware derives the fourth corner.
static const UINT kXboxRectList=0x11;
static std::map<Wrap*,UINT> g_gfxTopo; // Under GfxBridgeLock.
static void* g_gfxOrigTopo20=nullptr;
static void __stdcall Gfx_IASetTopology20(Wrap* w,UINT topo)
{
    if (IsXboxList(w)) { ExclusiveSrwLock held(GfxBridgeLock());g_gfxTopo[w]=topo; }
    ((void(__stdcall*)(Wrap*,UINT))g_gfxOrigTopo20)(w,topo);
}
static bool GfxIsFloatFormat(DXGI_FORMAT f)
{
    return f==DXGI_FORMAT_R32G32B32A32_FLOAT||f==DXGI_FORMAT_R32G32B32_FLOAT||f==DXGI_FORMAT_R32G32_FLOAT||f==DXGI_FORMAT_R32_FLOAT;
}
// Expands each rect triple (a,b,c) into triangles a,b,c + a,c,d where the
// right-angle corner r is found from POSITION and d = (sum of others) - r.
// Float attributes are extrapolated linearly; packed attributes take the
// corner opposite in the parallelogram per byte (clamped), which is exact for
// constant-colour rectangles and linear UNORM data.
static ID3D12Resource* GfxRectListBuffer(ID3D12Device* dev,const D3D12_VERTEX_BUFFER_VIEW& guest,const GfxPsoRec& rec,UINT slot,UINT start,UINT vertices,GfxDrawStorage& storage)
{
    const UINT stride=guest.StrideInBytes;
    if (!stride || (uint64_t)(start+vertices)*stride>guest.SizeInBytes) return nullptr;
    std::vector<uint8_t> in((size_t)vertices*stride),out((size_t)vertices*2*stride);
    if (!ReadPsoMemory((void*)(guest.BufferLocation+(uint64_t)start*stride),in.data(),in.size())) return nullptr;
    UINT posOff=UINT_MAX;
    for (const auto& e:rec.layoutVec) if (e.InputSlot==slot && e.SemanticName && !_stricmp(e.SemanticName,"POSITION") && GfxIsFloatFormat(e.Format)) posOff=e.AlignedByteOffset;
    auto pos=[&](const uint8_t* v,int c) { float f=0;memcpy(&f,v+posOff+4*c,4);return f; };
    for (UINT t=0;t<vertices/3;++t) {
        const uint8_t* v[3]={&in[(3*t)*stride],&in[(3*t+1)*stride],&in[(3*t+2)*stride]};
        int r=1; // default: middle vertex is the corner
        if (posOff!=UINT_MAX) for (int i=0;i<3;++i) {
            const uint8_t* a=v[(i+1)%3];const uint8_t* b=v[(i+2)%3];
            const bool sx=pos(v[i],0)==pos(a,0)||pos(v[i],0)==pos(b,0), sy=pos(v[i],1)==pos(a,1)||pos(v[i],1)==pos(b,1);
            if (sx && sy) { r=i;break; }
        }
        const uint8_t* R=v[r];const uint8_t* A=v[(r+1)%3];const uint8_t* B=v[(r+2)%3];
        std::vector<uint8_t> d(R,R+stride);
        for (const auto& e:rec.layoutVec) {
            if (e.InputSlot!=slot) continue;
            const UINT off=e.AlignedByteOffset;
            if (GfxIsFloatFormat(e.Format)) {
                const UINT n=e.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?4:e.Format==DXGI_FORMAT_R32G32B32_FLOAT?3:e.Format==DXGI_FORMAT_R32G32_FLOAT?2:1;
                for (UINT c=0;c<n && off+4*c+4<=stride;++c) { float fa,fb,fr;memcpy(&fa,A+off+4*c,4);memcpy(&fb,B+off+4*c,4);memcpy(&fr,R+off+4*c,4);
                    const float fd=fa+fb-fr;memcpy(&d[off+4*c],&fd,4); }
            } else for (UINT c=0;c<4 && off+c<stride;++c) { const int x=(int)A[off+c]+(int)B[off+c]-(int)R[off+c];d[off+c]=(uint8_t)std::min(255,std::max(0,x)); }
        }
        uint8_t* o=&out[(size_t)6*t*stride];
        memcpy(o,v[0],stride);memcpy(o+stride,v[1],stride);memcpy(o+2*stride,v[2],stride);
        memcpy(o+3*stride,A,stride);memcpy(o+4*stride,d.data(),stride);memcpy(o+5*stride,B,stride);
    }
    const uint64_t rounded=(out.size()+255)&~255ull;
    if (g_gfxLiveBytes+rounded>(256ull<<20)) return nullptr;
    D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd={};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width=rounded;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer=nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,__uuidof(ID3D12Resource),(void**)&buffer))) return nullptr;
    void* mapped=nullptr;D3D12_RANGE none={};
    if (FAILED(buffer->Map(0,&none,&mapped))) { buffer->Release();return nullptr; }
    memset(mapped,0,(size_t)rounded);memcpy(mapped,out.data(),out.size());
    D3D12_RANGE written={0,(SIZE_T)rounded};buffer->Unmap(0,&written);
    storage.own(buffer);storage.bytes+=rounded;g_gfxLiveBytes+=rounded;
    static std::atomic<unsigned> logged{0};
    if (logged++<8) XS_LOG("GFXNATIVE RECTLIST expanded slot=%u vertices=%u->%u start=%u stride=%u pos_off=%d",slot,vertices,vertices*2,start,stride,(int)posOff);
    return buffer;
}
static void __stdcall Gfx_RSSetViewports21(Wrap* w,UINT n,const D3D12_VIEWPORT* vp)
{
    if (IsXboxList(w)) {
        D3D12_VIEWPORT v{};const bool ok=n && vp && ReadPsoMemory(vp,&v,sizeof v);
        { ExclusiveSrwLock held(GfxBridgeLock());auto& s=g_gfxVp[w];s.nvp=n;if (ok) { s.vp=v;s.vpSet=true; } }
        static std::atomic<unsigned> logged{0};
        if (logged++<16) XS_LOG("GFXNATIVE VIEWPORT21 n=%u ptr=%p read=%u x=%g y=%g w=%g h=%g z=%g..%g",n,(void*)vp,ok,v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth);
    }
    ((void(__stdcall*)(Wrap*,UINT,const D3D12_VIEWPORT*))g_gfxOrigRs21)(w,n,vp);
}
static void __stdcall Gfx_RSSetScissors22(Wrap* w,UINT n,const D3D12_RECT* rc)
{
    if (IsXboxList(w)) {
        D3D12_RECT r{};const bool ok=n && rc && ReadPsoMemory(rc,&r,sizeof r);
        { ExclusiveSrwLock held(GfxBridgeLock());auto& s=g_gfxVp[w];s.nsc=n;if (ok) { s.sc=r;s.scSet=true; } }
        static std::atomic<unsigned> logged{0};
        if (logged++<16) XS_LOG("GFXNATIVE SCISSOR22 n=%u ptr=%p read=%u rect=%ld,%ld-%ld,%ld",n,(void*)rc,ok,(long)r.left,(long)r.top,(long)r.right,(long)r.bottom);
    }
    ((void(__stdcall*)(Wrap*,UINT,const D3D12_RECT*))g_gfxOrigRs22)(w,n,rc);
}
static bool GfxEnvOff(const char* name) { char v[4]={}; return GetEnvironmentVariableA(name,v,sizeof v)>0 && v[0]=='0'; }
static void GfxNativeOnPresent(ID3D12Resource* source,unsigned frame)
{
    static const bool off=GfxEnvOff("XSX_DEV_GFX_PRESENTDIAG"); if (off) return;
    std::map<ID3D12Resource*,unsigned> draws;
    { ExclusiveSrwLock held(GfxBridgeLock());draws.swap(g_gfxDrawsByTarget); }
    static std::atomic<unsigned> logged{0};
    if (draws.empty() && logged>=24) return;
    if (logged++>=24 && frame%500) return;
    unsigned toSource=0,other=0;std::string others;
    for (auto& d:draws) {
        if (d.first==source) toSource+=d.second;
        else { other+=d.second;char b[96];const auto dd=d.first->GetDesc();
               snprintf(b,sizeof b,"%s%p:%llux%u/f%u:%u",others.empty()?"":",",(void*)d.first,(unsigned long long)dd.Width,dd.Height,(unsigned)dd.Format,d.second);
               if (others.size()<600) others+=b; }
    }
    XS_LOG("GFXNATIVE PRESENTSRC frame=%u source=%p draws_to_source=%u draws_elsewhere=%u targets=[%s]",frame,(void*)source,toSource,other,others.c_str());
}
static GfxDrawResult GfxNativeOnDrawBind(Wrap* w,bool indexed,UINT vertices,UINT start,UINT* outVertices,UINT* outStart)
{
    PROF_SCOPE(PROF_DRAWBIND);
    XListState st;
    { SharedSrwLock held(g_xstateLock);auto it=g_xstate.find(w);if (it==g_xstate.end()) return GfxDrawResult::Unrecognized;st=it->second; }
    GfxPsoRec* rec=nullptr;
    { SharedSrwLock held(g_gfxLock);auto it=g_gfxByWrapped.find((void*)st.pso);if (it!=g_gfxByWrapped.end()) rec=it->second; }
    if (!rec) return GfxDrawResult::Unrecognized;
    ExclusiveSrwLock held(GfxBridgeLock());
    static std::atomic<unsigned> attempts{0},ready{0},skipped{0};
    const unsigned attempt=++attempts;const bool verbose=attempt<=24;
    auto* list=(ID3D12GraphicsCommandList*)w->real;
    xsx::presentation::Ref<ID3D12Device> device;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device),(void**)device.Put()))) return GfxDrawResult::Incomplete;
    auto storage=std::make_shared<GfxDrawStorage>();
    std::string missing;
    auto fail=[&](const char* text) { if (!missing.empty()) missing+=",";missing+=text; };
    auto target=g_gfxTargets.find(w);
    if (target==g_gfxTargets.end() || !target->second) fail("render-target");
    else if (target->second->format!=rec->rtv) fail("target-format");
    else { target->second->exposed->AddRef();storage->own(target->second->exposed); }

    std::map<UINT,D3D12_VERTEX_BUFFER_VIEW> nativeViews;
    if (!st.vertexValid) fail("vertex-snapshot");
    UINT topo=4; { auto t=g_gfxTopo.find(w); if (t!=g_gfxTopo.end()) topo=t->second; }
    static const bool rectOff=GfxEnvOff("XSX_DEV_GFX_RECTLIST");
    const bool rect=topo==kXboxRectList && !rectOff;
    if (rect && (indexed || vertices%3 || !vertices || vertices>3u*4096)) fail("rectlist-unsupported");
    else if (!rectOff && topo!=kXboxRectList && topo!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) fail("topology");
    for (const auto& element:rec->layoutVec) {
        if (nativeViews.count(element.InputSlot)) continue;
        auto it=st.vertexViews.find(element.InputSlot);
        if (it==st.vertexViews.end()) { fail("vertex-slot");continue; }
        const auto& guest=it->second;
        ID3D12Resource* buffer=nullptr;
        if (rect) buffer=GfxRectListBuffer(device.Get(),guest,*rec,element.InputSlot,start,vertices,*storage);
        else buffer=GfxSnapshotBuffer(device.Get(),guest.BufferLocation,guest.SizeInBytes,*storage,"vb");
        if (!buffer || !guest.StrideInBytes || element.AlignedByteOffset>=guest.StrideInBytes) { fail("vertex-data");continue; }
        auto view=guest;view.BufferLocation=buffer->GetGPUVirtualAddress();
        if (rect) view.SizeInBytes=(UINT)(2ull*vertices*guest.StrideInBytes);
        nativeViews[element.InputSlot]=view;
        if (verbose) XS_LOG("GFXNATIVE VB slot=%u guest=%#llx bytes=%u stride=%u copied=1",element.InputSlot,(unsigned long long)guest.BufferLocation,guest.SizeInBytes,guest.StrideInBytes);
        if (verbose) GfxDumpFloats("VBDATA",guest.BufferLocation,std::min<uint64_t>(guest.SizeInBytes,4u*guest.StrideInBytes));
    }
    D3D12_INDEX_BUFFER_VIEW nativeIndex={};
    if (indexed) {
        const auto& guest=st.indexView;
        if (!st.indexValid || !st.indexPresent || (guest.Format!=DXGI_FORMAT_R16_UINT && guest.Format!=DXGI_FORMAT_R32_UINT)) fail("index-view");
        else {
            auto* buffer=GfxSnapshotBuffer(device.Get(),guest.BufferLocation,guest.SizeInBytes,*storage,"ib");
            if (!buffer) fail("index-data");else { nativeIndex=guest;nativeIndex.BufferLocation=buffer->GetGPUVirtualAddress(); }
        }
    }
    uint64_t table=0;
    auto root=st.slots.find({36,0});
    if (root!=st.slots.end()) { table=GpuHandleToGuest(root->second[1]);if (!table) table=root->second[1]; }
    const UINT count=1+rec->bufCount+rec->texCount;
    ID3D12DescriptorHeap* heap=nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=count;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd,__uuidof(ID3D12DescriptorHeap),(void**)&heap))) fail("descriptor-heap");
    if (!heap) return GfxDrawResult::Incomplete;
    storage->own(heap);
    const UINT increment=device->GetDescriptorHandleIncrementSize(hd.Type);
    const auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const auto gpu=heap->GetGPUDescriptorHandleForHeapStart();
    auto slot=[&](UINT n) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr+(SIZE_T)n*increment}; };
    D3D12_SHADER_RESOURCE_VIEW_DESC nullBuffer={};nullBuffer.Format=DXGI_FORMAT_R32_TYPELESS;
    nullBuffer.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;nullBuffer.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullBuffer.Buffer.NumElements=1;nullBuffer.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
    for (UINT n=0;n<rec->bufCount;++n) device->CreateShaderResourceView(nullptr,&nullBuffer,slot(1+n));
    D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture={};nullTexture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    nullTexture.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;nullTexture.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullTexture.Texture2D.MipLevels=1;
    for (UINT n=0;n<rec->texCount;++n) device->CreateShaderResourceView(nullptr,&nullTexture,slot(1+rec->bufCount+n));
    uint32_t vsharp[64]={};
    for (const GfxProg* program:{&rec->vs,&rec->ps}) {
        for (const auto& binding:program->buffers) {
            uint32_t words[8]={},stride=0,records=0;uint64_t guest=0,bytes=0;
            const bool parsed=table && GfxReadVsharp(table+binding.off,words,guest,bytes,stride,records);
            if (verbose) XS_LOG("GFXNATIVE V# raw off=%u reg=%u words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x parsed=%u",
                binding.off,binding.reg,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],parsed);
            if (!table || binding.reg>=rec->bufCount || binding.reg>=16 ||
                !parsed || (guest&3) || (bytes&3)) {
                fail("buffer-descriptor");continue;
            }
            auto* buffer=GfxSnapshotBuffer(device.Get(),guest,bytes,*storage,"vsharp");
            if (!buffer) { fail("buffer-data");continue; }
            auto srv=nullBuffer;srv.Buffer.NumElements=(UINT)(bytes/4);
            device->CreateShaderResourceView(buffer,&srv,slot(1+binding.reg));
            vsharp[4*binding.reg]=stride;vsharp[4*binding.reg+1]=records;
            if (verbose) XS_LOG("GFXNATIVE V# off=%u reg=%u guest=%#llx stride=%u records=%u bytes=%llu copied=1",binding.off,binding.reg,(unsigned long long)guest,stride,records,(unsigned long long)bytes);
            if (verbose) GfxDumpFloats("VSHARPDATA",guest,std::min<uint64_t>(bytes,128));
        }
        for (const auto& texture:program->textures) {
            if (texture.reg>=rec->texCount) { fail("texture-slot"); continue; }
            uint32_t tag=0;
            const bool readable=table && ReadPsoMemory((void*)(table+texture.off),&tag,sizeof tag);
            const GfxSrvRec* chosen=nullptr;
            if (readable && tag && tag!=kViewTag)
                for (auto it=g_gfxSrvs.rbegin();it!=g_gfxSrvs.rend();++it)
                    if (it->tag==tag) { chosen=&*it; break; }
            if (chosen) {
                if (!GfxSrvBridgeEnabled()) { if (verbose) GfxDescribeTexture(w,table,texture); fail("image-bridge-bridged-off"); continue; }
                static std::atomic<unsigned> logged2{0};
                if (verbose || logged2++<16) XS_LOG("GFXNATIVE T# off=%u reg=%u tag=%#x from-srv-seq=%u guest=%#llx native=%#llx exposed=%p hasDesc=%u (pre-bind)",
                    texture.off,texture.reg,tag,chosen->seq,(unsigned long long)chosen->guest,(unsigned long long)chosen->native,(void*)chosen->exposed,(unsigned)chosen->hasDesc);
                if (chosen->exposed && chosen->hasDesc) {
                    // Rebuild the SRV from the typed creation description instead
                    // of copying the title heap slot bytes; same resource, no reliance
                    // on the opaque slot content.
                    device->CreateShaderResourceView((ID3D12Resource*)Unwrap(chosen->exposed),&chosen->desc,slot(1+rec->bufCount+texture.reg));
                } else {
                    const D3D12_CPU_DESCRIPTOR_HANDLE src={(SIZE_T)chosen->native};
                    device->CopyDescriptorsSimple(1,slot(1+rec->bufCount+texture.reg),src,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
                if (chosen->exposed) { chosen->exposed->AddRef(); storage->own(chosen->exposed); }
                if (verbose && chosen->exposed) {
                    auto* nr=(ID3D12Resource*)Unwrap(chosen->exposed);const auto rd=nr->GetDesc();
                    bool isRt=false;for (auto& r:g_gfxRtvs) if (r.second && r.second->resource==nr) { isRt=true;break; }
                    XS_LOG("GFXNATIVE T# resource native=%p %llux%u fmt=%u dim=%u mips=%u flags=%#x is_known_rt=%u srvfmt=%u srvdim=%u",
                        (void*)nr,(unsigned long long)rd.Width,rd.Height,(unsigned)rd.Format,(unsigned)rd.Dimension,(unsigned)rd.MipLevels,(unsigned)rd.Flags,isRt,
                        (unsigned)chosen->desc.Format,(unsigned)chosen->desc.ViewDimension);
                }
                static std::atomic<unsigned> logged{0};
                if (verbose || logged++<16) XS_LOG("GFXNATIVE T# off=%u reg=%u tag=%#x resolved=srv-seq=%u guest=%#llx native=%#llx (heuristic: latest tag match)",
                    texture.off,texture.reg,tag,chosen->seq,(unsigned long long)chosen->guest,(unsigned long long)chosen->native);
            } else {
                if (verbose) GfxDescribeTexture(w,table,texture);
                fail("image-bridge");
            }
        }
    }
    auto* constants=GfxSnapshotBuffer(device.Get(),(uint64_t)vsharp,sizeof vsharp,*storage);
    if (!constants) fail("constant-upload");
    else { D3D12_CONSTANT_BUFFER_VIEW_DESC cb={constants->GetGPUVirtualAddress(),256};device->CreateConstantBufferView(&cb,slot(0)); }
    if (!missing.empty()) {
        const unsigned n=++skipped;
        if (verbose || !(n%2000)) XS_LOG("GFXNATIVE DRAW skipped n=%u reason=%s indexed=%u live_bytes=%llu",n,missing.c_str(),indexed,(unsigned long long)g_gfxLiveBytes.load());
        return GfxDrawResult::Incomplete;
    }
    list->SetPipelineState(rec->real);list->SetGraphicsRootSignature(rec->root);
    const D3D12_CPU_DESCRIPTOR_HANDLE nativeTarget={(SIZE_T)target->second->native};
    list->OMSetRenderTargets(1,&nativeTarget,FALSE,nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (rect) { *outVertices=vertices*2;*outStart=0; }
    for (const auto& item:nativeViews) list->IASetVertexBuffers(item.first,1,&item.second);
    if (indexed) list->IASetIndexBuffer(&nativeIndex);
    list->SetDescriptorHeaps(1,&heap);
    list->SetGraphicsRootDescriptorTable(rec->paramCbvTable,gpu);
    if (rec->bufCount) list->SetGraphicsRootDescriptorTable(rec->paramSrv1Table,gpu);
    list->SetGraphicsRootDescriptorTable(rec->paramSrv2Table,gpu);
    g_gfxRecorded[w].push_back(storage);
    const unsigned n=++ready;
    ID3D12Resource* targetRes=target->second->resource;
    ++g_gfxDrawsByTarget[targetRes];
    if (GfxMarkOn() && n<=4096) {
        // Debug marker: 256x256 magenta at the top-left of the draw target.
        const float magenta[4]={1.f,0.f,1.f,1.f};const D3D12_RECT mark={0,0,256,256};
        list->ClearRenderTargetView(nativeTarget,magenta,1,&mark);
    }
    if (n<=16 || !(n%1000)) {
        const auto td=targetRes->GetDesc();GfxVpState vs{};
        auto v=g_gfxVp.find(w);if (v!=g_gfxVp.end()) vs=v->second;
        XS_LOG("GFXNATIVE DRAW ready n=%u indexed=%u descriptors=%u immutable=1 live_bytes=%llu target=%p %llux%u fmt=%u rtvfmt=%u vp(%u,%u)=%g,%g %gx%g sc(%u,%u)=%ld,%ld-%ld,%ld",
            n,indexed,count,(unsigned long long)g_gfxLiveBytes.load(),(void*)targetRes,(unsigned long long)td.Width,td.Height,(unsigned)td.Format,(unsigned)target->second->format,
            vs.nvp,vs.vpSet,vs.vp.TopLeftX,vs.vp.TopLeftY,vs.vp.Width,vs.vp.Height,vs.nsc,vs.scSet,(long)vs.sc.left,(long)vs.sc.top,(long)vs.sc.right,(long)vs.sc.bottom);
    }
    return GfxDrawResult::Ready;
}
static bool GfxNativeOnSetPso(Wrap* w,ID3D12PipelineState* p)
{
    SharedSrwLock held(g_gfxLock);
    return g_gfxByWrapped.count(p)!=0 && IsXboxList(w);
}
static void __stdcall Gfx_OMSetRT50(Wrap* w,uintptr_t firstArg,uintptr_t secondArg,uintptr_t thirdArg,uintptr_t fourthArg,UINT nr,const D3D12_RECT* rc)
{
    if (!IsXboxList(w)) { List_ClearUAVFloat(w,D3D12_GPU_DESCRIPTOR_HANDLE{firstArg},D3D12_CPU_DESCRIPTOR_HANDLE{secondArg},(ID3D12Resource*)thirdArg,(const FLOAT*)fourthArg,nr,rc);return; }
    const UINT n=(UINT)firstArg;const auto* rtv=(const D3D12_CPU_DESCRIPTOR_HANDLE*)secondArg;
    const BOOL single=(BOOL)thirdArg;const auto* dsv=(const D3D12_CPU_DESCRIPTOR_HANDLE*)fourthArg;
    ExclusiveSrwLock held(GfxBridgeLock());g_gfxTargets.erase(w);
    D3D12_CPU_DESCRIPTOR_HANDLE first={};
    const bool readable=n==1 && rtv && ReadPsoMemory(rtv,&first,sizeof first);
    auto it=readable?g_gfxRtvs.find(first.ptr):g_gfxRtvs.end();
    D3D12_CPU_DESCRIPTOR_HANDLE depth={};
    const bool noDepth=!dsv || (ReadPsoMemory(dsv,&depth,sizeof depth) && !depth.ptr);
    // A bound DSV is accepted and deliberately not forwarded: the native UI
    // PSOs are created with DepthEnable=FALSE, DepthFunc=ALWAYS and
    // DSVFormat=UNKNOWN, so OMSetRenderTargets(…,FALSE,nullptr) is valid
    // D3D12 for them. Depth semantics against title memory remain unproven.
    static bool acceptDsv = [] { char v[4]; return !(GetEnvironmentVariableA("XSX_DEV_NATIVE_DSV", v, sizeof v) > 0 && v[0]=='0'); }();
    const bool ready=it!=g_gfxRtvs.end() && (noDepth || acceptDsv);
    if (ready) g_gfxTargets[w]=it->second;
    static std::atomic<unsigned> calls{0};const unsigned call=++calls;
    if (call<=16 || (!ready && !(call%2000))) {
        GfxViewTrace("rt-use",w,__builtin_return_address(0),first.ptr,XlatCpu(first.ptr),nullptr,nullptr,nullptr,0,ready,noDepth);
        uint32_t words[8]={};if (readable) ReadPsoMemory((void*)first.ptr,words,sizeof words);
        XS_LOG("GFXNATIVE RT50 n=%u single=%u handle=%#llx known=%u depth-bound=%u words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",n,single,(unsigned long long)first.ptr,ready,!noDepth,
            words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7]);
    }
}
static void __stdcall Gfx_Slot51(Wrap* w,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d)
{
    if (!IsXboxList(w)) { ListDiscardResource51(w,(ID3D12Resource*)a,(const D3D12_DISCARD_REGION*)b);return; }
    static std::atomic<unsigned> calls{0};if (calls++<8) XS_LOG("GFXNATIVE depth clear51 skipped: a=%p b=%p c=%p d=%p",(void*)a,(void*)b,(void*)c,(void*)d);
}
static void __stdcall Gfx_Slot52(Wrap* w,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d)
{
    if (!IsXboxList(w)) { List_Slot52(w,(void*)a,b,c,d);return; }
    ExclusiveSrwLock held(GfxBridgeLock());auto it=g_gfxRtvs.find(a);float color[4]={};
    if (it!=g_gfxRtvs.end() && !c && !d && ReadPsoMemory((void*)b,color,sizeof color)) {
        const D3D12_CPU_DESCRIPTOR_HANDLE target={(SIZE_T)it->second->native};
        ((ID3D12GraphicsCommandList*)w->real)->ClearRenderTargetView(target,color,0,nullptr);
        auto storage=std::make_shared<GfxDrawStorage>();it->second->exposed->AddRef();storage->own(it->second->exposed);g_gfxRecorded[w].push_back(storage);
        return;
    }
    static std::atomic<unsigned> calls{0};if (calls++<8) XS_LOG("GFXNATIVE color clear52 skipped: unresolved handle=%p c=%p d=%p",(void*)a,(void*)c,(void*)d);
}
static void GfxNativeInstallListHooks()
{
    if (!GfxNativeEnabled()) return;
    g_vtList[50]=(void*)Gfx_OMSetRT50;g_vtList[51]=(void*)Gfx_Slot51;g_vtList[52]=(void*)Gfx_Slot52;
    char hv[4]={};const DWORD hn=GetEnvironmentVariableA("XSX_DEV_GFX_HOOKS",hv,sizeof hv); // bisect: 0=none,1=topology only,2=all (default)
    const int hooks=hn?hv[0]-'0':2;
    if (hooks>=1 && !g_gfxOrigTopo20) { g_gfxOrigTopo20=g_vtList[20];g_vtList[20]=(void*)Gfx_IASetTopology20; }
    if (hooks>=2 && !g_gfxOrigRs21) { g_gfxOrigRs21=g_vtList[21];g_vtList[21]=(void*)Gfx_RSSetViewports21; }
    if (hooks>=2 && !g_gfxOrigRs22) { g_gfxOrigRs22=g_vtList[22];g_vtList[22]=(void*)Gfx_RSSetScissors22; }
    XS_LOG("GFXNATIVE diag hooks level=%d",hooks);
    XS_LOG("GFXNATIVE list50/51/52 installed; unknown guest handles skipped; per-draw immutable uploads/descriptors");
}
