#pragma once
// Opt-in descriptor provenance. Read-only snapshots; no descriptor decoding,
// COM calls, resource ownership or binding changes. Raw native bytes remain
// opaque. The file avoids stdout interleaving with Wine/VKD3D diagnostics.
static void GfxViewTrace(const char* kind,void* object,void* caller,
    uint64_t guest,uint64_t native,void* exposed,void* real,
    const void* description,size_t descriptionBytes,uint64_t a=0,uint64_t b=0,
    const void* extra=nullptr,size_t extraBytes=0)
{
    struct PreserveError { DWORD value=GetLastError();~PreserveError() { SetLastError(value); } } savedError;
    static const std::string directory=[] {
        char path[512]={};const DWORD n=GetEnvironmentVariableA("XSX_DEV_VIEW_TRACE_DIR",path,sizeof path);
        return n && n<sizeof path?std::string(path):std::string();
    }();
    if (directory.empty()) return;
    // Initialization and a complete record write share one lock. No bridge,
    // resource or view-registry lock is acquired by this function.
    static SRWLOCK lock=SRWLOCK_INIT;ExclusiveSrwLock held(lock);
    static HANDLE file=INVALID_HANDLE_VALUE;static bool initialized=false;
    static unsigned count=0,lanes[3]={};
    const unsigned lane=(!std::strcmp(kind,"texture-use") || !std::strcmp(kind,"rt-use"))?2:
        (!std::strcmp(kind,"rtv-create")?1:0);
    const unsigned limits[3]={4096,512,128};
    if (!initialized) {
        initialized=true;
        if (CreateDirectoryA(directory.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS) {
            const std::string path=directory+"\\descriptors.tsv";
            file=CreateFileA(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        }
        XS_LOG("VIEWTRACE path=%s\\descriptors.tsv active=%u limits=producer4096/rtv512/use128 snapshot_bytes=64 desc_cap=128 extra_cap=64 (diagnostic only)",directory.c_str(),file!=INVALID_HANDLE_VALUE);
    }
    if (file==INVALID_HANDLE_VALUE || lanes[lane]>=limits[lane]) return;
    ++lanes[lane];
    const unsigned sequence=++count;
    unsigned char guestBytes[64]={},nativeBytes[64]={},desc[128]={},tail[64]={};
    auto read=[](const void* address,void* out,size_t bytes) {
        SIZE_T got=0;return address && bytes && ReadProcessMemory(GetCurrentProcess(),address,out,bytes,&got) && got==bytes;
    };
    const bool guestRead=read((void*)(uintptr_t)guest,guestBytes,sizeof guestBytes);
    const bool nativeRead=read((void*)(uintptr_t)native,nativeBytes,sizeof nativeBytes);
    if (descriptionBytes>sizeof desc) descriptionBytes=sizeof desc;
    if (extraBytes>sizeof tail) extraBytes=sizeof tail;
    const bool descRead=read(description,desc,descriptionBytes),extraRead=read(extra,tail,extraBytes);
    char guestHex[129],nativeHex[129],descHex[257],extraHex[129];
    auto hex=[](const unsigned char* bytes,size_t size,char* out) {
        const char digits[]="0123456789abcdef";
        for (size_t i=0;i<size;++i) { out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15]; }
        out[2*size]=0;
    };
    hex(guestBytes,sizeof guestBytes,guestHex);hex(nativeBytes,sizeof nativeBytes,nativeHex);
    hex(desc,descriptionBytes,descHex);hex(tail,extraBytes,extraHex);
    char record[2048];
    const int size=snprintf(record,sizeof record,
        "v1\tseq=%u\ttid=%lu\tkind=%s\tobject=%p\tcaller=%p\tguest=%#llx\tnative=%#llx\texposed=%p\treal=%p\ta=%#llx\tb=%#llx\tguest_read=%u\tnative_read=%u\tguest64=%s\tnative64=%s\tdesc_bytes=%zu\tdesc_read=%u\tdesc=%s\textra_bytes=%zu\textra_read=%u\textra=%s\n",
        sequence,GetCurrentThreadId(),kind,object,caller,(unsigned long long)guest,(unsigned long long)native,
        exposed,real,(unsigned long long)a,(unsigned long long)b,guestRead,nativeRead,guestHex,nativeHex,
        descriptionBytes,descRead,descHex,extraBytes,extraRead,extraHex);
    DWORD written=0;
    if (size<=0 || (size_t)size>=sizeof record || !WriteFile(file,record,(DWORD)size,&written,nullptr) || written!=(DWORD)size) {
        CloseHandle(file);file=INVALID_HANDLE_VALUE;XS_LOG("VIEWTRACE write failed; capture disabled");
    } else if (lanes[lane]==limits[lane]) XS_LOG("VIEWTRACE lane=%u limit=%u reached; later lane events not captured",lane,limits[lane]);
}
