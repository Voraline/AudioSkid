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

SOCKET ServerSocket;
struct Client {
    sockaddr_in Addr;
};
std::vector<Client> Clients;
std::mutex ClientMutex;
std::vector<Client> ClientsSnapshot;

void ListenerThread() {
    char Buffer;
    sockaddr_in TempAddr;
    int Len = sizeof(TempAddr);
    while (true) {
        if (recvfrom(ServerSocket, &Buffer, 1, 0, (sockaddr*)&TempAddr, &Len) > 0) {
            std::lock_guard<std::mutex> Lock(ClientMutex);
            bool Found = false;
            for (auto& C : Clients) {
                if (C.Addr.sin_addr.s_addr == TempAddr.sin_addr.s_addr && C.Addr.sin_port == TempAddr.sin_port) {
                    Found = true;
                    break;
                }
            }
            if (!Found) {
                Clients.push_back({ TempAddr });
            }
        }
    }
}

int main() {
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    WSADATA Wsa;
    WSAStartup(MAKEWORD(2, 2), &Wsa);

    ServerSocket = socket(AF_INET, SOCK_DGRAM, 0);
    int BufSize = 4194304;
    int Tos = 0x10;
    setsockopt(ServerSocket, SOL_SOCKET, SO_SNDBUF, (char*)&BufSize, 4);
    setsockopt(ServerSocket, IPPROTO_IP, IP_TOS, (char*)&Tos, 4);

    sockaddr_in Addr{};
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(11000);
    bind(ServerSocket, (sockaddr*)&Addr, sizeof(Addr));

    std::thread(ListenerThread).detach();

    CoInitialize(0);
    IMMDeviceEnumerator* Enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&Enumerator);

    IMMDevice* Device;
    Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device);

    IAudioClient* AudioClient;
    Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&AudioClient);

    WAVEFORMATEX* Format;
    AudioClient->GetMixFormat(&Format);

    REFERENCE_TIME Duration;
    AudioClient->GetDevicePeriod(&Duration, 0);

    AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, Duration, 0, Format, 0);

    HANDLE AudioEvent = CreateEvent(0, 0, 0, 0);
    AudioClient->SetEventHandle(AudioEvent);

    IAudioCaptureClient* CaptureClient;
    AudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&CaptureClient);
    AudioClient->Start();

    DWORD TaskIndex = 0;
    HANDLE TaskHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &TaskIndex);
    AvSetMmThreadPriority(TaskHandle, AVRT_PRIORITY_CRITICAL);

    UINT32 PacketLen;
    BYTE* Data;
    UINT32 NumFrames;
    DWORD Flags;
    short PacketBuffer[FRAME_SAMPLES];
    int PacketIndex = 0;
    const int Channels = Format->nChannels;
    const float InvChannels = 1.0f / (float)Channels;

    UINT32 DeviceRate = Format->nSamplesPerSec;
    bool NeedsResample = (DeviceRate != 48000);
    float ResampleRatio = (float)DeviceRate / 48000.0f;
    float ResamplePos = 0.0f;

    if (NeedsResample && DeviceRate != 44100) {
        char Msg[256];
        sprintf_s(Msg, "Default audio device is running at %u Hz, which isn't supported. Use a 44.1kHz or 48kHz output device.", DeviceRate);
        MessageBoxA(0, Msg, "AudioSkid", MB_OK | MB_ICONERROR);
        return 1;
    }

    int OpusErr = 0;
    OpusEncoder* Encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &OpusErr);
    opus_encoder_ctl(Encoder, OPUS_SET_BITRATE(48000));
    opus_encoder_ctl(Encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(Encoder, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(Encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(Encoder, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(Encoder, OPUS_SET_DTX(1));

    unsigned char OpusPacket[1500];

    auto MixToMono = [Channels, InvChannels](const float* Samples, UINT32 FrameIndex) -> float {
        const float* Base = Samples + FrameIndex * Channels;
        if (Channels == 2) {
            return (Base[0] + Base[1]) * 0.5f;
        }
        if (Channels == 1) {
            return Base[0];
        }
        float Sum = 0;
        for (int K = 0; K < Channels; K++) {
            Sum += Base[K];
        }
        return Sum * InvChannels;
    };

    auto FlushFrame = [&]() {
        int EncodedBytes = opus_encode(Encoder, PacketBuffer, FRAME_SAMPLES, OpusPacket, sizeof(OpusPacket));
        if (EncodedBytes > 0) {
            ClientsSnapshot.clear();
            {
                std::lock_guard<std::mutex> Lock(ClientMutex);
                ClientsSnapshot = Clients;
            }
            for (auto& C : ClientsSnapshot) {
                sendto(ServerSocket, (char*)OpusPacket, EncodedBytes, 0, (sockaddr*)&C.Addr, sizeof(C.Addr));
            }
        }
        PacketIndex = 0;
    };

    auto PushSample = [&](float Mono) {
        int Val = (int)(Mono * 32767.0f);
        Val = Val > 32767 ? 32767 : (Val < -32768 ? -32768 : Val);
        PacketBuffer[PacketIndex++] = (short)Val;
        if (PacketIndex == FRAME_SAMPLES) {
            FlushFrame();
        }
    };

    float PrevMono = 0.0f;
    bool HavePrev = false;

    while (true) {
        WaitForSingleObject(AudioEvent, INFINITE);
        while (true) {
            if (FAILED(CaptureClient->GetNextPacketSize(&PacketLen)) || PacketLen == 0) break;

            if (SUCCEEDED(CaptureClient->GetBuffer(&Data, &NumFrames, &Flags, 0, 0))) {
                if (NumFrames) {
                    const float* Samples = (const float*)Data;

                    if (!NeedsResample) {
                        for (UINT32 I = 0; I < NumFrames; I++) {
                            PushSample(MixToMono(Samples, I));
                        }
                    } else {
                        for (UINT32 I = 0; I < NumFrames; I++) {
                            float CurMono = MixToMono(Samples, I);
                            if (!HavePrev) {
                                PrevMono = CurMono;
                                HavePrev = true;
                                continue;
                            }

                            while (ResamplePos < 1.0f) {
                                float Interpolated = PrevMono + (CurMono - PrevMono) * ResamplePos;
                                PushSample(Interpolated);
                                ResamplePos += ResampleRatio;
                            }
                            ResamplePos -= 1.0f;
                            PrevMono = CurMono;
                        }
                    }
                }
                CaptureClient->ReleaseBuffer(NumFrames);
            }
        }
    }
    return 0;
}
