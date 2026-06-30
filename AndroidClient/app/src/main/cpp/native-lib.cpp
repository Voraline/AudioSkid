#include <jni.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <aaudio/AAudio.h>
#include <unistd.h>
#include <atomic>
#include <sched.h>
#include <cstring>
#include <opus.h>

#define BUFFER_MASK 16383
#define BUFFER_SIZE 16384
#define FRAME_SAMPLES 960
#define MAX_PACKET_BYTES 1500

int SockFd = -1;
short RingBuffer[BUFFER_SIZE];
std::atomic<int> Head{ 0 };
std::atomic<int> Tail{ 0 };
std::atomic<bool> CanPlay{ false };

aaudio_data_callback_result_t AudioCallback(AAudioStream* Stream, void* UserData, void* AudioData, int32_t NumFrames) {
    if (!CanPlay.load(std::memory_order_acquire)) {
        memset(AudioData, 0, NumFrames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    short* Output = (short*)AudioData;
    int CurrentHead = Head.load(std::memory_order_acquire);
    int CurrentTail = Tail.load(std::memory_order_relaxed);

    int Available = (CurrentHead - CurrentTail) & BUFFER_MASK;

    if (Available < NumFrames) {
        CanPlay.store(false, std::memory_order_release);
        memset(AudioData, 0, NumFrames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    int Part1 = (CurrentTail + NumFrames) & BUFFER_MASK;
    if (Part1 >= CurrentTail) {
        memcpy(Output, &RingBuffer[CurrentTail], NumFrames * 2);
    } else {
        int Split = BUFFER_SIZE - CurrentTail;
        memcpy(Output, &RingBuffer[CurrentTail], Split * 2);
        memcpy((char*)Output + Split * 2, &RingBuffer[0], (NumFrames - Split) * 2);
    }

    Tail.store(Part1, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_skid_audio_MainActivity_startAudioEngine(JNIEnv* Env, jobject, jstring IpStr) {
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
    Param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    sched_setscheduler(0, SCHED_FIFO, &Param);

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
        int Received = recv(SockFd, (char*)NetBuf, MAX_PACKET_BYTES, 0);
        if (Received <= 0) continue;

        int Decoded = opus_decode(Decoder, NetBuf, Received, PcmFrame, FRAME_SAMPLES, 0);
        if (Decoded != FRAME_SAMPLES) continue;

        int CurrentHead = Head.load(std::memory_order_relaxed);
        int CurrentTail = Tail.load(std::memory_order_acquire);

        int Available = (CurrentHead - CurrentTail) & BUFFER_MASK;
        if (Available > 2400) {
            Tail.store((CurrentHead - 960) & BUFFER_MASK, std::memory_order_release);
        }

        if (!CanPlay.load(std::memory_order_relaxed) && Available >= 960) {
            CanPlay.store(true, std::memory_order_release);
        }

        int NextHead = (CurrentHead + FRAME_SAMPLES) & BUFFER_MASK;
        if (NextHead >= CurrentHead) {
            memcpy(&RingBuffer[CurrentHead], PcmFrame, FRAME_SAMPLES * 2);
        } else {
            int Split = BUFFER_SIZE - CurrentHead;
            memcpy(&RingBuffer[CurrentHead], PcmFrame, Split * 2);
            memcpy(&RingBuffer[0], (char*)PcmFrame + Split * 2, (FRAME_SAMPLES - Split) * 2);
        }

        Head.store(NextHead, std::memory_order_release);
    }
}
