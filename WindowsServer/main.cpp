#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <thread>
#include <vector>
#include <mutex>
#include <opus.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

#define FRAME_SAMPLES 960

SOCKET server_socket;
struct Client {
    sockaddr_in addr;
};
std::vector<Client> clients;
std::mutex client_mutex;

void ListenerThread() {
    char buffer;
    sockaddr_in temp_addr;
    int len = sizeof(temp_addr);
    while (true) {
        if (recvfrom(server_socket, &buffer, 1, 0, (sockaddr*)&temp_addr, &len) > 0) {
            std::lock_guard<std::mutex> lock(client_mutex);
            bool found = false;
            for (auto& c : clients) {
                if (c.addr.sin_addr.s_addr == temp_addr.sin_addr.s_addr && c.addr.sin_port == temp_addr.sin_port) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                clients.push_back({ temp_addr });
            }
        }
    }
}

int main() {
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    
    server_socket = socket(AF_INET, SOCK_DGRAM, 0);
    int buf_size = 4194304;
    int tos = 0x10;
    setsockopt(server_socket, SOL_SOCKET, SO_SNDBUF, (char*)&buf_size, 4);
    setsockopt(server_socket, IPPROTO_IP, IP_TOS, (char*)&tos, 4);
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(11000);
    bind(server_socket, (sockaddr*)&addr, sizeof(addr));
    
    std::thread(ListenerThread).detach();
    
    CoInitialize(0);
    IMMDeviceEnumerator* enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    
    IMMDevice* device;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    
    IAudioClient* audio_client;
    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&audio_client);
    
    WAVEFORMATEX* format;
    audio_client->GetMixFormat(&format);
    
    REFERENCE_TIME duration;
    audio_client->GetDevicePeriod(&duration, 0);
    
    audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, format, 0);
    
    HANDLE audio_event = CreateEvent(0, 0, 0, 0);
    audio_client->SetEventHandle(audio_event);
    
    IAudioCaptureClient* capture_client;
    audio_client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client);
    audio_client->Start();
    
    DWORD task_index = 0;
    HANDLE task_handle = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
    AvSetMmThreadPriority(task_handle, AVRT_PRIORITY_CRITICAL);
    
    UINT32 packet_len;
    BYTE* data;
    UINT32 num_frames;
    DWORD flags;
    short packet_buffer[FRAME_SAMPLES];
    int packet_index = 0;
    int channels = format->nChannels;

    if (format->nSamplesPerSec != 48000) {
        // Opus only accepts 8/12/16/24/48 kHz; this build doesn't resample.
        MessageBoxA(0, "Default audio device is not running at 48kHz. AudioSkid needs a 48kHz output device.", "AudioSkid", MB_OK | MB_ICONERROR);
        return 1;
    }

    int opus_err = 0;
    OpusEncoder* encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &opus_err);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(28000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));

    unsigned char opus_packet[1500];

    while (true) {
        WaitForSingleObject(audio_event, INFINITE);
        while (true) {
            if (FAILED(capture_client->GetNextPacketSize(&packet_len)) || packet_len == 0) break;
            
            if (SUCCEEDED(capture_client->GetBuffer(&data, &num_frames, &flags, 0, 0))) {
                if (num_frames) {
                    float* samples = (float*)data;
                    for (UINT32 i = 0; i < num_frames; i++) {
                        float sum = 0;
                        for (int k = 0; k < channels; k++) {
                            sum += samples[i * channels + k];
                        }
                        
                        int val = (int)((sum / channels) * 32767.0f);
                        if (val > 32767) val = 32767;
                        if (val < -32768) val = -32768;
                        
                        packet_buffer[packet_index++] = (short)val;
                        
                        if (packet_index == FRAME_SAMPLES) {
                            int encoded_bytes = opus_encode(encoder, packet_buffer, FRAME_SAMPLES, opus_packet, sizeof(opus_packet));
                            if (encoded_bytes > 0) {
                                std::lock_guard<std::mutex> lock(client_mutex);
                                for (auto& c : clients) {
                                    sendto(server_socket, (char*)opus_packet, encoded_bytes, 0, (sockaddr*)&c.addr, sizeof(c.addr));
                                }
                            }
                            packet_index = 0;
                        }
                    }
                }
                capture_client->ReleaseBuffer(num_frames);
            }
        }
    }
    return 0;
}
