#include <jni.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <aaudio/AAudio.h>
#include <unistd.h>
#include <atomic>
#include <sched.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <opus.h>

#define BUFFER_MASK 16383
#define BUFFER_SIZE 16384
#define FRAME_SAMPLES 960
#define MAX_PACKET_BYTES 1500
#define FFT_SIZE 1024
#define SPECTRUM_BINS 512

int SockFd = -1;
short RingBuffer[BUFFER_SIZE];
std::atomic<int> Head{ 0 };
std::atomic<int> Tail{ 0 };
std::atomic<bool> CanPlay{ false };
std::atomic<bool> EngineStarted{ false };
std::atomic<bool> SpectrumLock{ false };

float FftReal[FFT_SIZE];
float FftImag[FFT_SIZE];
float SpectrumBins[SPECTRUM_BINS];
float HannWindow[FFT_SIZE];

void InitWindow() {
    for (int I = 0; I < FFT_SIZE; I++) {
        HannWindow[I] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * I / (FFT_SIZE - 1));
    }
}

void ComputeFft() {
    for (int I = 1, J = 0; I < FFT_SIZE; I++) {
        int Bit = FFT_SIZE >> 1;
        for (; J & Bit; Bit >>= 1) J ^= Bit;
        J ^= Bit;
        if (I < J) {
            std::swap(FftReal[I], FftReal[J]);
            std::swap(FftImag[I], FftImag[J]);
        }
    }

    for (int Len = 2; Len <= FFT_SIZE; Len <<= 1) {
        float Angle = -2.0f * (float)M_PI / Len;
        float WReal = cosf(Angle);
        float WImag = sinf(Angle);
        for (int I = 0; I < FFT_SIZE; I += Len) {
            float CurReal = 1.0f, CurImag = 0.0f;
            for (int K = 0; K < Len / 2; K++) {
                float UReal = FftReal[I + K];
                float UImag = FftImag[I + K];
                float VReal = FftReal[I + K + Len / 2] * CurReal - FftImag[I + K + Len / 2] * CurImag;
                float VImag = FftReal[I + K + Len / 2] * CurImag + FftImag[I + K + Len / 2] * CurReal;
                FftReal[I + K] = UReal + VReal;
                FftImag[I + K] = UImag + VImag;
                FftReal[I + K + Len / 2] = UReal - VReal;
                FftImag[I + K + Len / 2] = UImag - VImag;
                float NextReal = CurReal * WReal - CurImag * WImag;
                float NextImag = CurReal * WImag + CurImag * WReal;
                CurReal = NextReal;
                CurImag = NextImag;
            }
        }
    }
}

void UpdateSpectrum() {
    const int CurrentHead = Head.load(std::memory_order_acquire);
    for (int I = 0; I < FFT_SIZE; I++) {
        int Index = (CurrentHead - FFT_SIZE + I) & BUFFER_MASK;
        float Sample = RingBuffer[Index] / 32768.0f;
        FftReal[I] = Sample * HannWindow[I];
        FftImag[I] = 0.0f;
    }

    ComputeFft();

    bool Expected = false;
    if (SpectrumLock.compare_exchange_strong(Expected, true, std::memory_order_acquire)) {
        for (int I = 0; I < SPECTRUM_BINS; I++) {
            float Magnitude = sqrtf(FftReal[I] * FftReal[I] + FftImag[I] * FftImag[I]) / FFT_SIZE;
            float Db = 20.0f * log10f(Magnitude + 1e-9f) + 90.0f;
            SpectrumBins[I] = SpectrumBins[I] * 0.6f + Db * 0.4f;
        }
        SpectrumLock.store(false, std::memory_order_release);
    }
}

aaudio_data_callback_result_t AudioCallback(AAudioStream* Stream, void* UserData, void* AudioData, int32_t NumFrames) {
    if (!CanPlay.load(std::memory_order_acquire)) {
        memset(AudioData, 0, NumFrames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    short* Output = (short*)AudioData;
    const int CurrentHead = Head.load(std::memory_order_acquire);
    const int CurrentTail = Tail.load(std::memory_order_relaxed);

    const int Available = (CurrentHead - CurrentTail) & BUFFER_MASK;

    if (Available < NumFrames) {
        CanPlay.store(false, std::memory_order_release);
        memset(AudioData, 0, NumFrames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    const int Part1 = (CurrentTail + NumFrames) & BUFFER_MASK;
    if (Part1 >= CurrentTail) {
        memcpy(Output, &RingBuffer[CurrentTail], NumFrames * 2);
    } else {
        const int Split = BUFFER_SIZE - CurrentTail;
        memcpy(Output, &RingBuffer[CurrentTail], Split * 2);
        memcpy(Output + Split, &RingBuffer[0], (NumFrames - Split) * 2);
    }

    Tail.store(Part1, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_skid_audio_MainActivity_StartAudioEngine(JNIEnv* Env, jobject, jstring IpStr) {
    bool Expected = false;
    if (!EngineStarted.compare_exchange_strong(Expected, true, std::memory_order_acq_rel)) {
        return;
    }

    InitWindow();

    const char* Ip = Env->GetStringUTFChars(IpStr, 0);

    SockFd = socket(AF_INET, SOCK_DGRAM, 0);
    int BufSize = 4194304;
    int Tos = 0x10;
    setsockopt(SockFd, SOL_SOCKET, SO_RCVBUF, (char*)&BufSize, 4);
    setsockopt(SockFd, IPPROTO_IP, IP_TOS, (char*)&Tos, 4);

    sockaddr_in Addr;
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(11000);
    inet_pton(AF_INET, Ip, &Addr.sin_addr);

    char Ping = 1;
    sendto(SockFd, &Ping, 1, 0, (sockaddr*)&Addr, sizeof(Addr));

    struct sched_param Param;
    Param.sched_priority = sched_get_priority_max(SCHED_RR);
    sched_setscheduler(0, SCHED_RR, &Param);

    AAudioStreamBuilder* Builder;
    AAudio_createStreamBuilder(&Builder);
    AAudioStreamBuilder_setFormat(Builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(Builder, 1);
    AAudioStreamBuilder_setSampleRate(Builder, 48000);
    AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDataCallback(Builder, AudioCallback, nullptr);

    AAudioStream* Stream;
    AAudioStreamBuilder_openStream(Builder, &Stream);
    AAudioStream_setBufferSizeInFrames(Stream, AAudioStream_getFramesPerBurst(Stream) * 4);
    AAudioStream_requestStart(Stream);

    AAudioStreamBuilder_delete(Builder);
    Env->ReleaseStringUTFChars(IpStr, Ip);

    int OpusErr = 0;
    OpusDecoder* Decoder = opus_decoder_create(48000, 1, &OpusErr);

    unsigned char NetBuf[MAX_PACKET_BYTES];
    short PcmFrame[FRAME_SAMPLES];

    while (true) {
        const int Received = recv(SockFd, (char*)NetBuf, MAX_PACKET_BYTES, 0);
        if (Received <= 0) continue;

        const int Decoded = opus_decode(Decoder, NetBuf, Received, PcmFrame, FRAME_SAMPLES, 0);
        if (Decoded != FRAME_SAMPLES) continue;

        const int CurrentHead = Head.load(std::memory_order_relaxed);
        const int CurrentTail = Tail.load(std::memory_order_acquire);

        const int Available = (CurrentHead - CurrentTail) & BUFFER_MASK;
        if (Available > 2400) {
            Tail.store((CurrentHead - 960) & BUFFER_MASK, std::memory_order_release);
        }

        if (!CanPlay.load(std::memory_order_relaxed) && Available >= 960) {
            CanPlay.store(true, std::memory_order_release);
        }

        const int NextHead = (CurrentHead + FRAME_SAMPLES) & BUFFER_MASK;
        if (NextHead >= CurrentHead) {
            memcpy(&RingBuffer[CurrentHead], PcmFrame, FRAME_SAMPLES * 2);
        } else {
            const int Split = BUFFER_SIZE - CurrentHead;
            memcpy(&RingBuffer[CurrentHead], PcmFrame, Split * 2);
            memcpy(&RingBuffer[0], PcmFrame + Split, (FRAME_SAMPLES - Split) * 2);
        }

        Head.store(NextHead, std::memory_order_release);
        UpdateSpectrum();
    }
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_skid_audio_MainActivity_GetSpectrum(JNIEnv* Env, jobject) {
    jfloatArray Result = Env->NewFloatArray(SPECTRUM_BINS);

    bool Expected = false;
    while (!SpectrumLock.compare_exchange_weak(Expected, true, std::memory_order_acquire)) {
        Expected = false;
    }

    Env->SetFloatArrayRegion(Result, 0, SPECTRUM_BINS, SpectrumBins);
    SpectrumLock.store(false, std::memory_order_release);

    return Result;
}
