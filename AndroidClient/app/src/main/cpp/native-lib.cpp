#include <jni.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <aaudio/AAudio.h>
#include <unistd.h>
#include <atomic>
#include <sched.h>
#include <cstring>
#include <cstdint>
#include <opus.h>

#define BUFFER_MASK 16383
#define BUFFER_SIZE 16384
#define FRAME_SAMPLES 960
#define MAX_PACKET_BYTES 1500
#define JITTER_START_SAMPLES 3840
#define JITTER_HIGH_WATER_SAMPLES 9600
#define JITTER_TARGET_SAMPLES 4800
#define MAX_CONCEALED_FRAMES 25

int SockFd = -1;
short RingBuffer[BUFFER_SIZE];
std::atomic<int> Head{ 0 };
std::atomic<int> Tail{ 0 };
std::atomic<bool> CanPlay{ false };
std::atomic<bool> EngineStarted{ false };
std::atomic<bool> EngineRunning{ false };
AAudioStream* ActiveStream = nullptr;

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

void PushFrame(const short* PcmFrame) {
    const int CurrentHead = Head.load(std::memory_order_relaxed);
    const int CurrentTail = Tail.load(std::memory_order_acquire);

    const int Available = (CurrentHead - CurrentTail) & BUFFER_MASK;
    if (Available > JITTER_HIGH_WATER_SAMPLES) {
        Tail.store((CurrentHead - JITTER_TARGET_SAMPLES) & BUFFER_MASK, std::memory_order_release);
    }

    if (!CanPlay.load(std::memory_order_relaxed) && Available >= JITTER_START_SAMPLES) {
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
}

extern "C" JNIEXPORT void JNICALL
Java_com_skid_audio_AudioService_StartAudioEngine(JNIEnv* Env, jobject, jstring IpStr) {
    bool Expected = false;
    if (!EngineStarted.compare_exchange_strong(Expected, true, std::memory_order_acq_rel)) {
        return;
    }
    EngineRunning.store(true, std::memory_order_release);

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
    ActiveStream = Stream;

    AAudioStreamBuilder_delete(Builder);
    Env->ReleaseStringUTFChars(IpStr, Ip);

    int OpusErr = 0;
    OpusDecoder* Decoder = opus_decoder_create(48000, 1, &OpusErr);

    unsigned char NetBuf[MAX_PACKET_BYTES];
    short PcmFrame[FRAME_SAMPLES];

    bool HaveExpectedSeq = false;
    uint16_t ExpectedSeq = 0;

    while (EngineRunning.load(std::memory_order_acquire)) {
        const int Received = recv(SockFd, (char*)NetBuf, MAX_PACKET_BYTES, 0);
        if (!EngineRunning.load(std::memory_order_acquire)) break;
        if (Received <= 2) continue;

        uint16_t NetSeq;
        memcpy(&NetSeq, NetBuf, 2);
        const uint16_t Seq = ntohs(NetSeq);
        const unsigned char* OpusData = NetBuf + 2;
        const int OpusLen = Received - 2;

        int MissingFrames = 0;
        if (HaveExpectedSeq) {
            MissingFrames = (uint16_t)(Seq - ExpectedSeq);
            if (MissingFrames > MAX_CONCEALED_FRAMES) {
                MissingFrames = 0;
            }
        }
        HaveExpectedSeq = true;
        ExpectedSeq = Seq + 1;

        for (int M = 0; M < MissingFrames; M++) {
            int Decoded;
            if (M == MissingFrames - 1) {
                Decoded = opus_decode(Decoder, OpusData, OpusLen, PcmFrame, FRAME_SAMPLES, 1);
            } else {
                Decoded = opus_decode(Decoder, nullptr, 0, PcmFrame, FRAME_SAMPLES, 0);
            }
            if (Decoded == FRAME_SAMPLES) {
                PushFrame(PcmFrame);
            }
        }

        const int Decoded = opus_decode(Decoder, OpusData, OpusLen, PcmFrame, FRAME_SAMPLES, 0);
        if (Decoded == FRAME_SAMPLES) {
            PushFrame(PcmFrame);
        }
    }

    if (ActiveStream != nullptr) {
        AAudioStream_requestStop(ActiveStream);
        AAudioStream_close(ActiveStream);
        ActiveStream = nullptr;
    }
    opus_decoder_destroy(Decoder);
    if (SockFd >= 0) {
        close(SockFd);
        SockFd = -1;
    }
    CanPlay.store(false, std::memory_order_release);
    Head.store(0, std::memory_order_release);
    Tail.store(0, std::memory_order_release);
    EngineStarted.store(false, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_com_skid_audio_AudioService_StopAudioEngine(JNIEnv*, jobject) {
    EngineRunning.store(false, std::memory_order_release);
    if (SockFd >= 0) {
        shutdown(SockFd, SHUT_RDWR);
    }
}
