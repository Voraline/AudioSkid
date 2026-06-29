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

int sock_fd = -1;
short ring_buffer[BUFFER_SIZE];
std::atomic<int> head{ 0 };
std::atomic<int> tail{ 0 };
std::atomic<bool> can_play{ false };

aaudio_data_callback_result_t AudioCallback(AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames) {
    if (!can_play.load(std::memory_order_acquire)) {
        memset(audio_data, 0, num_frames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    short* output = (short*)audio_data;
    int current_head = head.load(std::memory_order_acquire);
    int current_tail = tail.load(std::memory_order_relaxed);
    
    int available = (current_head - current_tail) & BUFFER_MASK;

    if (available < num_frames) {
        can_play.store(false, std::memory_order_release);
        memset(audio_data, 0, num_frames * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    int part1 = (current_tail + num_frames) & BUFFER_MASK;
    if (part1 >= current_tail) {
        memcpy(output, &ring_buffer[current_tail], num_frames * 2);
    } else {
        int split = BUFFER_SIZE - current_tail;
        memcpy(output, &ring_buffer[current_tail], split * 2);
        memcpy((char*)output + split * 2, &ring_buffer[0], (num_frames - split) * 2);
    }

    tail.store(part1, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_skid_audio_MainActivity_startAudioEngine(JNIEnv* env, jobject, jstring ip_str) {
    const char* ip = env->GetStringUTFChars(ip_str, 0);
    
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int buf_size = 4194304;
    int tos = 0x10;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, 4);
    setsockopt(sock_fd, IPPROTO_IP, IP_TOS, (char*)&tos, 4);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(11000);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    char ping = 1;
    sendto(sock_fd, &ping, 1, 0, (sockaddr*)&addr, sizeof(addr));

    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    sched_setscheduler(0, SCHED_FIFO, &param);

    AAudioStreamBuilder* builder;
    AAudio_createStreamBuilder(&builder);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDataCallback(builder, AudioCallback, nullptr);

    AAudioStream* stream;
    AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStream_setBufferSizeInFrames(stream, AAudioStream_getFramesPerBurst(stream) * 4);
    AAudioStream_requestStart(stream);

    AAudioStreamBuilder_delete(builder);
    env->ReleaseStringUTFChars(ip_str, ip);

    int opus_err = 0;
    OpusDecoder* decoder = opus_decoder_create(48000, 1, &opus_err);

    unsigned char net_buf[MAX_PACKET_BYTES];
    short pcm_frame[FRAME_SAMPLES];

    while (true) {
        int received = recv(sock_fd, (char*)net_buf, MAX_PACKET_BYTES, 0);
        if (received <= 0) continue;

        int decoded = opus_decode(decoder, net_buf, received, pcm_frame, FRAME_SAMPLES, 0);
        if (decoded != FRAME_SAMPLES) continue;

        int current_head = head.load(std::memory_order_relaxed);
        int current_tail = tail.load(std::memory_order_acquire);

        int available = (current_head - current_tail) & BUFFER_MASK;
        if (available > 2400) {
            tail.store((current_head - 960) & BUFFER_MASK, std::memory_order_release);
        }

        if (!can_play.load(std::memory_order_relaxed) && available >= 960) {
            can_play.store(true, std::memory_order_release);
        }

        int next_head = (current_head + FRAME_SAMPLES) & BUFFER_MASK;
        if (next_head >= current_head) {
            memcpy(&ring_buffer[current_head], pcm_frame, FRAME_SAMPLES * 2);
        } else {
            int split = BUFFER_SIZE - current_head;
            memcpy(&ring_buffer[current_head], pcm_frame, split * 2);
            memcpy(&ring_buffer[0], (char*)pcm_frame + split * 2, (FRAME_SAMPLES - split) * 2);
        }

        head.store(next_head, std::memory_order_release);
    }
}
