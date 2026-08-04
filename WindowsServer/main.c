#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <opus.h>

const CLSID CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const IID IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const IID IID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

#define FrameSamples 960
#define MaxClients 64
#define NetPacketSize 1502

typedef struct {
    struct sockaddr_in Addr;
} Client;

static SOCKET ServerSocket;
static Client Clients[MaxClients];
static int ClientCount = 0;
static CRITICAL_SECTION ClientMutex;
static Client ClientsSnapshot[MaxClients];

static OpusEncoder* Encoder;
static short PacketBuffer[FrameSamples];
static int PacketIndex = 0;
static unsigned char NetPacket[NetPacketSize];
static uint16_t SeqNum = 0;
static int Channels;
static float InvChannels;

DWORD WINAPI ListenerThread(LPVOID Param) {
    char Buffer;
    struct sockaddr_in TempAddr;
    int Len = sizeof(TempAddr);
    while (1) {
        if (recvfrom(ServerSocket, &Buffer, 1, 0, (struct sockaddr*)&TempAddr, &Len) > 0) {
            EnterCriticalSection(&ClientMutex);
            int Found = 0;
            for (int I = 0; I < ClientCount; I++) {
                if (Clients[I].Addr.sin_addr.s_addr == TempAddr.sin_addr.s_addr &&
                    Clients[I].Addr.sin_port == TempAddr.sin_port) {
                    Found = 1;
                    break;
                }
            }
            if (!Found && ClientCount < MaxClients) {
                Clients[ClientCount].Addr = TempAddr;
                ClientCount++;
            }
            LeaveCriticalSection(&ClientMutex);
        }
    }
    return 0;
}

float MixToMono(const float* Samples, UINT32 FrameIndex) {
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
}

void FlushFrame() {
    int EncodedBytes = opus_encode(Encoder, PacketBuffer, FrameSamples, NetPacket + 2, sizeof(NetPacket) - 2);
    if (EncodedBytes > 0) {
        uint16_t NetSeq = htons(SeqNum++);
        memcpy(NetPacket, &NetSeq, 2);
        int TotalBytes = EncodedBytes + 2;

        int SnapshotCount;
        EnterCriticalSection(&ClientMutex);
        SnapshotCount = ClientCount;
        memcpy(ClientsSnapshot, Clients, sizeof(Client) * ClientCount);
        LeaveCriticalSection(&ClientMutex);

        for (int I = 0; I < SnapshotCount; I++) {
            sendto(ServerSocket, (char*)NetPacket, TotalBytes, 0, (struct sockaddr*)&ClientsSnapshot[I].Addr, sizeof(ClientsSnapshot[I].Addr));
        }
    }
    PacketIndex = 0;
}

void PushSample(float Mono) {
    int Val = (int)(Mono * 32767.0f);
    Val = Val > 32767 ? 32767 : (Val < -32768 ? -32768 : Val);
    PacketBuffer[PacketIndex++] = (short)Val;
    if (PacketIndex == FrameSamples) {
        FlushFrame();
    }
}

int main() {
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    WSADATA Wsa;
    WSAStartup(MAKEWORD(2, 2), &Wsa);
    InitializeCriticalSection(&ClientMutex);

    ServerSocket = socket(AF_INET, SOCK_DGRAM, 0);
    int BufSize = 4194304;
    int Tos = 0x10;
    if (setsockopt(ServerSocket, SOL_SOCKET, SO_SNDBUF, (char*)&BufSize, 4) != 0) {
        MessageBoxA(0, "Failed to set socket send buffer size.", "AudioSkid", MB_OK | MB_ICONWARNING);
    }
    setsockopt(ServerSocket, IPPROTO_IP, IP_TOS, (char*)&Tos, 4);

    struct sockaddr_in Addr;
    memset(&Addr, 0, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(11000);
    bind(ServerSocket, (struct sockaddr*)&Addr, sizeof(Addr));

    CreateThread(NULL, 0, ListenerThread, NULL, 0, NULL);

    CoInitialize(0);
    IMMDeviceEnumerator* Enumerator;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, 0, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&Enumerator);

    IMMDevice* Device;
    Enumerator->lpVtbl->GetDefaultAudioEndpoint(Enumerator, eRender, eConsole, &Device);

    IAudioClient* AudioClient;
    Device->lpVtbl->Activate(Device, &IID_IAudioClient, CLSCTX_ALL, 0, (void**)&AudioClient);

    WAVEFORMATEX* Format;
    AudioClient->lpVtbl->GetMixFormat(AudioClient, &Format);

    REFERENCE_TIME DefaultPeriod, MinPeriod;
    AudioClient->lpVtbl->GetDevicePeriod(AudioClient, &DefaultPeriod, &MinPeriod);

    AudioClient->lpVtbl->Initialize(AudioClient, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, MinPeriod, 0, Format, 0);

    HANDLE AudioEvent = CreateEvent(0, 0, 0, 0);
    AudioClient->lpVtbl->SetEventHandle(AudioClient, AudioEvent);

    IAudioCaptureClient* CaptureClient;
    AudioClient->lpVtbl->GetService(AudioClient, &IID_IAudioCaptureClient, (void**)&CaptureClient);
    AudioClient->lpVtbl->Start(AudioClient);

    DWORD TaskIndex = 0;
    HANDLE TaskHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &TaskIndex);
    AvSetMmThreadPriority(TaskHandle, AVRT_PRIORITY_CRITICAL);

    UINT32 PacketLen;
    BYTE* Data;
    UINT32 NumFrames;
    DWORD Flags;
    Channels = Format->nChannels;
    InvChannels = 1.0f / (float)Channels;

    UINT32 DeviceRate = Format->nSamplesPerSec;
    int NeedsResample = (DeviceRate != 48000);
    float ResampleRatio = (float)DeviceRate / 48000.0f;
    float ResamplePos = 0.0f;

    if (NeedsResample && DeviceRate != 44100) {
        char Msg[256];
        sprintf_s(Msg, sizeof(Msg), "Default audio device is running at %u Hz, which isn't supported. Use a 44.1kHz or 48kHz output device.", DeviceRate);
        MessageBoxA(0, Msg, "AudioSkid", MB_OK | MB_ICONERROR);
        return 1;
    }

    int OpusErr = 0;
    Encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &OpusErr);
    if (!Encoder || OpusErr != OPUS_OK) {
        MessageBoxA(0, "Failed to create Opus encoder.", "AudioSkid", MB_OK | MB_ICONERROR);
        return 1;
    }
    opus_encoder_ctl(Encoder, OPUS_SET_BITRATE(160000));
    opus_encoder_ctl(Encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(Encoder, OPUS_SET_COMPLEXITY(10));

    float PrevMono = 0.0f;
    int HavePrev = 0;

    while (1) {
        WaitForSingleObject(AudioEvent, INFINITE);
        while (1) {
            if (FAILED(CaptureClient->lpVtbl->GetNextPacketSize(CaptureClient, &PacketLen)) || PacketLen == 0) break;

            if (SUCCEEDED(CaptureClient->lpVtbl->GetBuffer(CaptureClient, &Data, &NumFrames, &Flags, 0, 0))) {
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
                                HavePrev = 1;
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
                CaptureClient->lpVtbl->ReleaseBuffer(CaptureClient, NumFrames);
            }
        }
    }
    return 0;
}
