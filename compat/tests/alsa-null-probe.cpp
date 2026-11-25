// Native dependency check for the sandbox's playback-only null PCM.
#include <alsa/asoundlib.h>
#include <cstdio>
#include <cstdlib>
#include <array>

struct Pcm {
    snd_pcm_t* p = nullptr;
    ~Pcm() { if (p) snd_pcm_close(p); }
};
static bool ok(int result, const char* call)
{
    if (result >= 0) return true;
    std::fprintf(stderr,"[FAIL] ALSA %s: %s\n",call,snd_strerror(result));
    return false;
}
int main(int argc, char** argv)
{
    if (argc != 2 || setenv("ALSA_CONFIG_PATH",argv[1],1)) return 2;
    Pcm playback, capture;
    if (!ok(snd_pcm_open(&playback.p,"default",SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK),"open playback")) return 1;
    // asym returns the actual playback slave. Require null, not hardware.
    if (snd_pcm_type(playback.p) != SND_PCM_TYPE_NULL) {
        std::fprintf(stderr,"[FAIL] ALSA expected NULL PCM, got %s\n",snd_pcm_type_name(snd_pcm_type(playback.p))); return 1;
    }
    if (!ok(snd_pcm_set_params(playback.p,SND_PCM_FORMAT_FLOAT_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,48000,0,20000),"configure float stereo 48kHz")) return 1;
    std::array<float,512> silence{};
    unsigned written = 0;
    for (unsigned i=0;i<8;++i) {
        const auto n=snd_pcm_writei(playback.p,silence.data(),256);
        if (n != 256) { std::fprintf(stderr,"[FAIL] ALSA write returned %ld\n",n); return 1; }
        written += (unsigned)n;
    }
    if (!ok(snd_pcm_drain(playback.p),"drain")) return 1;
    const int captureResult=snd_pcm_open(&capture.p,"default",SND_PCM_STREAM_CAPTURE,SND_PCM_NONBLOCK);
    if (captureResult >= 0) { std::fprintf(stderr,"[FAIL] ALSA capture unexpectedly available\n"); return 1; }
    std::printf("[PASS] ALSA null playback frames=%u channels=2 rate=48000 capture_rejected=1\n",written);
    return 0;
}
