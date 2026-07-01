#include <jni.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <aaudio/AAudio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sched.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <opus.h>

#define BufferMask 16383
#define BufferSize 16384
#define FrameSamples 960
#define MaxPacketBytes 1500
#define JitterStartSamples 3840
#define JitterHighWaterSamples 9600
#define JitterTargetSamples 4800
#define MaxConcealedFrames 25

static int SockFd = -1;
static short RingBuffer[BufferSize];
static atomic_int Head = 0;
static atomic_int Tail = 0;
static atomic_bool CanPlay = false;
static atomic_bool EngineStarted = false;
static atomic_bool EngineRunning = false;
static AAudioStream* ActiveStream = NULL;

aaudio_data_callback_result_t AudioCallback(AAudioStream* Stream, void* UserData, void* AudioData, int32_t NumFrames) {
    if (!atomic_load_explicit(&CanPlay, memory_order_acquire)) {
        memset(AudioData, 0, NumFrames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    short* Output = (short*)AudioData;
    int CurrentHead = atomic_load_explicit(&Head, memory_order_acquire);
    int CurrentTail = atomic_load_explicit(&Tail, memory_order_relaxed);

    int Available = (CurrentHead - CurrentTail) & BufferMask;

    if (Available < NumFrames) {
        atomic_store_explicit(&CanPlay, false, memory_order_release);
        memset(AudioData, 0, NumFrames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    int Part1 = (CurrentTail + NumFrames) & BufferMask;
    if (Part1 >= CurrentTail) {
        memcpy(Output, &RingBuffer[CurrentTail], NumFrames * 2);
    } else {
        int Split = BufferSize - CurrentTail;
        memcpy(Output, &RingBuffer[CurrentTail], Split * 2);
        memcpy(Output + Split, &RingBuffer[0], (NumFrames - Split) * 2);
    }

    atomic_store_explicit(&Tail, Part1, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void PushFrame(const short* PcmFrame) {
    int CurrentHead = atomic_load_explicit(&Head, memory_order_relaxed);
    int CurrentTail = atomic_load_explicit(&Tail, memory_order_acquire);

    int Available = (CurrentHead - CurrentTail) & BufferMask;
    if (Available > JitterHighWaterSamples) {
        atomic_store_explicit(&Tail, (CurrentHead - JitterTargetSamples) & BufferMask, memory_order_release);
    }

    if (!atomic_load_explicit(&CanPlay, memory_order_relaxed) && Available >= JitterStartSamples) {
        atomic_store_explicit(&CanPlay, true, memory_order_release);
    }

    int NextHead = (CurrentHead + FrameSamples) & BufferMask;
    if (NextHead >= CurrentHead) {
        memcpy(&RingBuffer[CurrentHead], PcmFrame, FrameSamples * 2);
    } else {
        int Split = BufferSize - CurrentHead;
        memcpy(&RingBuffer[CurrentHead], PcmFrame, Split * 2);
        memcpy(&RingBuffer[0], PcmFrame + Split, (FrameSamples - Split) * 2);
    }

    atomic_store_explicit(&Head, NextHead, memory_order_release);
}

JNIEXPORT void JNICALL
Java_com_skid_audio_AudioService_StartAudioEngine(JNIEnv* Env, jobject Thiz, jstring IpStr) {
    bool Expected = false;
    if (!atomic_compare_exchange_strong_explicit(&EngineStarted, &Expected, true, memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&EngineRunning, true, memory_order_release);

    const char* Ip = (*Env)->GetStringUTFChars(Env, IpStr, NULL);

    SockFd = socket(AF_INET, SOCK_DGRAM, 0);
    int BufSize = 4194304;
    int Tos = 0x10;
    setsockopt(SockFd, SOL_SOCKET, SO_RCVBUF, (char*)&BufSize, 4);
    setsockopt(SockFd, IPPROTO_IP, IP_TOS, (char*)&Tos, 4);

    struct sockaddr_in Addr;
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(11000);
    inet_pton(AF_INET, Ip, &Addr.sin_addr);

    char Ping = 1;
    sendto(SockFd, &Ping, 1, 0, (struct sockaddr*)&Addr, sizeof(Addr));

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
    AAudioStreamBuilder_setDataCallback(Builder, AudioCallback, NULL);

    AAudioStream* Stream;
    AAudioStreamBuilder_openStream(Builder, &Stream);
    AAudioStream_setBufferSizeInFrames(Stream, AAudioStream_getFramesPerBurst(Stream) * 4);
    AAudioStream_requestStart(Stream);
    ActiveStream = Stream;

    AAudioStreamBuilder_delete(Builder);
    (*Env)->ReleaseStringUTFChars(Env, IpStr, Ip);

    int OpusErr = 0;
    OpusDecoder* Decoder = opus_decoder_create(48000, 1, &OpusErr);

    unsigned char NetBuf[MaxPacketBytes];
    short PcmFrame[FrameSamples];

    bool HaveExpectedSeq = false;
    uint16_t ExpectedSeq = 0;

    while (atomic_load_explicit(&EngineRunning, memory_order_acquire)) {
        int Received = recv(SockFd, (char*)NetBuf, MaxPacketBytes, 0);
        if (!atomic_load_explicit(&EngineRunning, memory_order_acquire)) break;
        if (Received <= 2) continue;

        uint16_t NetSeq;
        memcpy(&NetSeq, NetBuf, 2);
        uint16_t Seq = ntohs(NetSeq);
        const unsigned char* OpusData = NetBuf + 2;
        int OpusLen = Received - 2;

        int MissingFrames = 0;
        if (HaveExpectedSeq) {
            MissingFrames = (uint16_t)(Seq - ExpectedSeq);
            if (MissingFrames > MaxConcealedFrames) {
                MissingFrames = 0;
            }
        }
        HaveExpectedSeq = true;
        ExpectedSeq = Seq + 1;

        for (int M = 0; M < MissingFrames; M++) {
            int Decoded;
            if (M == MissingFrames - 1) {
                Decoded = opus_decode(Decoder, OpusData, OpusLen, PcmFrame, FrameSamples, 1);
            } else {
                Decoded = opus_decode(Decoder, NULL, 0, PcmFrame, FrameSamples, 0);
            }
            if (Decoded == FrameSamples) {
                PushFrame(PcmFrame);
            }
        }

        int Decoded = opus_decode(Decoder, OpusData, OpusLen, PcmFrame, FrameSamples, 0);
        if (Decoded == FrameSamples) {
            PushFrame(PcmFrame);
        }
    }

    if (ActiveStream != NULL) {
        AAudioStream_requestStop(ActiveStream);
        AAudioStream_close(ActiveStream);
        ActiveStream = NULL;
    }
    opus_decoder_destroy(Decoder);
    if (SockFd >= 0) {
        close(SockFd);
        SockFd = -1;
    }
    atomic_store_explicit(&CanPlay, false, memory_order_release);
    atomic_store_explicit(&Head, 0, memory_order_release);
    atomic_store_explicit(&Tail, 0, memory_order_release);
    atomic_store_explicit(&EngineStarted, false, memory_order_release);
}

JNIEXPORT void JNICALL
Java_com_skid_audio_AudioService_StopAudioEngine(JNIEnv* Env, jobject Thiz) {
    atomic_store_explicit(&EngineRunning, false, memory_order_release);
    if (SockFd >= 0) {
        shutdown(SockFd, SHUT_RDWR);
    }
}
