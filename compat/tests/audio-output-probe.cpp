// Real Wine MMDevice/WASAPI/spatial output, using the isolated ALSA null PCM.
// Only zero samples are submitted. No game DLL, graphics device or network.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <initializer_list>

static unsigned checks = 0;
static void check(bool value, const char* call)
{
    ++checks;
    if (!value) { std::printf("[FAIL] AUDIO %s\n",call); throw std::runtime_error(call); }
}
static void hr(HRESULT value, const char* call)
{
    ++checks;
    if (FAILED(value)) {
        std::printf("[FAIL] AUDIO %s hr=0x%08lx\n",call,(unsigned long)value);
        throw std::runtime_error(call);
    }
}
template<class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if(p) p->Release(); }
    T* operator->() const { return p; }
};
struct Event {
    HANDLE h = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    ~Event() { if(h) CloseHandle(h); }
};
struct Format {
    WAVEFORMATEX* p = nullptr;
    ~Format() { CoTaskMemFree(p); }
};
template<class T> struct Stop {
    T* p;
    ~Stop() { p->Stop(); }
};
static void wasapi(IMMDevice* device)
{
    Event event; check(event.h != nullptr,"create WASAPI event");
    Com<IAudioClient> client;
    hr(device->Activate(__uuidof(IAudioClient),CLSCTX_INPROC_SERVER,nullptr,(void**)&client.p),"activate IAudioClient");
    check(client.p != nullptr,"IAudioClient output");
    Format format; hr(client->GetMixFormat(&format.p),"GetMixFormat");
    check(format.p && format.p->nChannels && format.p->nSamplesPerSec && format.p->nBlockAlign,"mix format fields");
    std::printf("[AUDIO] WASAPI channels=%u rate=%lu bits=%u\n",format.p->nChannels,(unsigned long)format.p->nSamplesPerSec,format.p->wBitsPerSample);
    hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,1000000,0,format.p,nullptr),"WASAPI Initialize");
    hr(client->SetEventHandle(event.h),"WASAPI SetEventHandle");
    UINT32 capacity=0; hr(client->GetBufferSize(&capacity),"GetBufferSize"); check(capacity>0,"nonempty WASAPI buffer");
    Com<IAudioRenderClient> render;
    hr(client->GetService(__uuidof(IAudioRenderClient),(void**)&render.p),"IAudioRenderClient");
    Com<IAudioClock> clock;
    hr(client->GetService(__uuidof(IAudioClock),(void**)&clock.p),"IAudioClock");
    check(render.p && clock.p,"render/clock service outputs");
    UINT64 frequency=0; hr(clock->GetFrequency(&frequency),"clock frequency"); check(frequency>0,"nonzero clock frequency");
    BYTE* data=nullptr; hr(render->GetBuffer(capacity,&data),"initial render buffer");
    check(data != nullptr,"initial buffer output");
    std::memset(data,0,(size_t)capacity*format.p->nBlockAlign);
    hr(render->ReleaseBuffer(capacity,0),"initial ReleaseBuffer");
    hr(client->Start(),"WASAPI Start"); Stop<IAudioClient> stop{client.p};
    UINT64 first=0,last=0,total=capacity; hr(clock->GetPosition(&first,nullptr),"initial clock position"); last=first;
    unsigned callbacks=0;
    for(unsigned i=0;i<12;++i) {
        check(WaitForSingleObject(event.h,1500)==WAIT_OBJECT_0,"WASAPI event delivery"); ++callbacks;
        UINT32 padding=0; hr(client->GetCurrentPadding(&padding),"GetCurrentPadding"); check(padding<=capacity,"padding within capacity");
        const UINT32 available=capacity-padding;
        if(available) {
            hr(render->GetBuffer(available,&data),"render buffer"); check(data != nullptr,"render buffer output");
            std::memset(data,0,(size_t)available*format.p->nBlockAlign);
            hr(render->ReleaseBuffer(available,0),"ReleaseBuffer"); total+=available;
        }
        UINT64 now=0; hr(clock->GetPosition(&now,nullptr),"clock position"); check(now>=last,"monotonic clock"); last=now;
    }
    check(last>first && total>capacity,"clock advances and queued frames are consumed");
    hr(client->Stop(),"WASAPI Stop"); hr(client->Reset(),"WASAPI Reset");
    UINT32 padding=~0u; hr(client->GetCurrentPadding(&padding),"padding after Reset"); check(padding==0,"Reset clears padding");
    std::printf("[PASS] WASAPI callbacks=%u submitted_frames=%llu clock_delta=%llu frequency=%llu\n",callbacks,(unsigned long long)total,(unsigned long long)(last-first),(unsigned long long)frequency);
}
static void spatial(IMMDevice* device)
{
    Event event; check(event.h != nullptr,"create spatial event");
    Com<ISpatialAudioClient> client;
    hr(device->Activate(__uuidof(ISpatialAudioClient),CLSCTX_INPROC_SERVER,nullptr,(void**)&client.p),"activate ISpatialAudioClient");
    check(client.p != nullptr,"ISpatialAudioClient output");
    AudioObjectType mask=AudioObjectType_None;
    // The title chooses its own static mask. Wine can render static objects
    // while this advisory query is E_NOTIMPL; activation/buffers below prove
    // the requested objects work, without claiming native-mask support.
    const HRESULT maskResult=client->GetNativeStaticObjectTypeMask(&mask);
    if(maskResult==E_NOTIMPL)
        std::printf("[AUDIO] spatial native_static_mask=unsupported hr=0x%08lx required=0\n",(unsigned long)maskResult);
    else {
        hr(maskResult,"spatial static object mask");
        std::printf("[AUDIO] spatial native_static_mask=%#x required=0\n",(unsigned)mask);
    }
    const auto stereo=(AudioObjectType)(AudioObjectType_FrontLeft|AudioObjectType_FrontRight);
    Com<IAudioFormatEnumerator> formats;
    hr(client->GetSupportedAudioObjectFormatEnumerator(&formats.p),"spatial format enumerator");
    check(formats.p != nullptr,"spatial format enumerator output");
    UINT32 count=0; hr(formats->GetCount(&count),"spatial format count"); check(count>0,"spatial formats exist");
    WAVEFORMATEX* format=nullptr;
    hr(formats->GetFormat(0,&format),"spatial GetFormat");
    check(format && format->nChannels==1 && format->nBlockAlign>0,"mono spatial object format");
    hr(client->IsAudioObjectFormatSupported(format),"spatial format support");
    std::printf("[AUDIO] spatial formats=%u rate=%lu bits=%u requested_static_mask=%#x\n",count,(unsigned long)format->nSamplesPerSec,format->wBitsPerSample,(unsigned)stereo);
    SpatialAudioObjectRenderStreamActivationParams params{};
    params.ObjectFormat=format; params.StaticObjectTypeMask=stereo;
    params.Category=AudioCategory_GameEffects; params.EventHandle=event.h;
    PROPVARIANT activation{}; activation.vt=VT_BLOB;
    activation.blob.cbSize=sizeof params; activation.blob.pBlobData=(BYTE*)&params;
    Com<ISpatialAudioObjectRenderStream> stream;
    hr(client->ActivateSpatialAudioStream(&activation,__uuidof(ISpatialAudioObjectRenderStream),(void**)&stream.p),"ActivateSpatialAudioStream");
    check(stream.p != nullptr,"spatial stream output");
    Com<ISpatialAudioObject> left,right;
    hr(stream->ActivateSpatialAudioObject(AudioObjectType_FrontLeft,&left.p),"activate front-left");
    hr(stream->ActivateSpatialAudioObject(AudioObjectType_FrontRight,&right.p),"activate front-right");
    check(left.p && right.p,"spatial static object outputs");
    hr(stream->Start(),"spatial Start"); Stop<ISpatialAudioObjectRenderStream> stop{stream.p};
    UINT64 submitted=0; unsigned callbacks=0,nonempty=0;
    for(unsigned i=0;i<12;++i) {
        check(WaitForSingleObject(event.h,1500)==WAIT_OBJECT_0,"spatial event delivery"); ++callbacks;
        UINT32 dynamic=0,frames=0;
        hr(stream->BeginUpdatingAudioObjects(&dynamic,&frames),"BeginUpdatingAudioObjects");
        struct EndUpdate { ISpatialAudioObjectRenderStream* stream; bool active=true; ~EndUpdate(){if(active)stream->EndUpdatingAudioObjects();} } end{stream.p};
        for(auto* object:{left.p,right.p}) {
            BYTE* data=nullptr; UINT32 bytes=0;
            hr(object->GetBuffer(&data,&bytes),"spatial GetBuffer");
            check((UINT64)bytes==(UINT64)frames*format->nBlockAlign,"spatial buffer byte count");
            check(!bytes || data,"spatial buffer output");
            if(bytes)std::memset(data,0,bytes);
        }
        const HRESULT ended=stream->EndUpdatingAudioObjects(); end.active=false;
        hr(ended,"EndUpdatingAudioObjects");
        if(frames)++nonempty;
        submitted+=frames;
    }
    check(nonempty>=2 && submitted>0,"repeated spatial buffers");
    hr(stream->Stop(),"spatial Stop");
    std::printf("[PASS] spatial callbacks=%u nonempty_updates=%u submitted_frames=%llu static_objects=2\n",callbacks,nonempty,(unsigned long long)submitted);
    // GetFormat points into the live enumerator/client; it is not ours to free.
}
int main()
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    bool initialized=false; int result=1;
    try {
        hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx"); initialized=true;
        Com<IMMDeviceEnumerator> enumerator;
        hr(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),(void**)&enumerator.p),"MMDeviceEnumerator");
        check(enumerator.p != nullptr,"MMDeviceEnumerator output");
        Com<IMMDeviceCollection> endpoints;
        hr(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&endpoints.p),"EnumAudioEndpoints");
        check(endpoints.p != nullptr,"render collection output");
        UINT count=0; hr(endpoints->GetCount(&count),"render endpoint count"); check(count>0,"active playback endpoint exists");
        Com<IMMDeviceCollection> capture;
        hr(enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&capture.p),"Enum capture endpoints");
        check(capture.p != nullptr,"capture collection output");
        UINT captureCount=~0u; hr(capture->GetCount(&captureCount),"capture endpoint count");
        check(captureCount==0,"null profile exposes no capture endpoint");
        Com<IMMDevice> device;
        hr(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device.p),"default console render endpoint");
        check(device.p != nullptr,"default endpoint output");
        std::printf("[AUDIO] active_render_endpoints=%u active_capture_endpoints=%u default_console=1\n",count,captureCount);
        wasapi(device.p); spatial(device.p);
        std::printf("[PASS] audio output checks=%u scope=real-WASAPI-and-spatial-null-playback\n",checks); result=0;
    } catch(const std::exception& error) {
        std::printf("[FAIL] audio output checks=%u stage=%s\n",checks,error.what());
    }
    if(initialized)CoUninitialize();
    return result;
}
