/*
 * WinDivert.h — vendored from github.com/basil00/WinDivert/releases (LGPL/MIT
 * dual-licensed). This is the minimal subset of the public API needed by
 * proxydivert. Full headers shipped with WinDivert 2.2.2 binary release.
 *
 * Real header has many more constants/structs — this only declares what we
 * actually call. CI step in package.sh replaces this stub with the real header
 * from the WinDivert release zip.
 */
#ifndef WINDIVERT_H
#define WINDIVERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <Windows.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <stdint.h>

// ── Layers ──
typedef enum
{
    WINDIVERT_LAYER_NETWORK         = 0,
    WINDIVERT_LAYER_NETWORK_FORWARD = 1,
    WINDIVERT_LAYER_FLOW            = 2,
    WINDIVERT_LAYER_SOCKET          = 3,
    WINDIVERT_LAYER_REFLECT         = 4,
} WINDIVERT_LAYER;

// ── Flags ──
#define WINDIVERT_FLAG_SNIFF        0x0001
#define WINDIVERT_FLAG_DROP         0x0002
#define WINDIVERT_FLAG_RECV_ONLY    0x0004
#define WINDIVERT_FLAG_SEND_ONLY    0x0008
#define WINDIVERT_FLAG_NO_INSTALL   0x0010
#define WINDIVERT_FLAG_FRAGMENTS    0x0020

// ── Params ──
typedef enum
{
    WINDIVERT_PARAM_QUEUE_LENGTH    = 0,
    WINDIVERT_PARAM_QUEUE_TIME      = 1,
    WINDIVERT_PARAM_QUEUE_SIZE      = 2,
    WINDIVERT_PARAM_VERSION_MAJOR   = 3,
    WINDIVERT_PARAM_VERSION_MINOR   = 4,
} WINDIVERT_PARAM;

#define WINDIVERT_QUEUE_LENGTH_MAX  16384
#define WINDIVERT_QUEUE_TIME_MAX    16000
#define WINDIVERT_QUEUE_SIZE_MAX    (33554432)

// ── Address ──
typedef struct
{
    INT64  Timestamp;
    UINT32 Layer:8;
    UINT32 Event:8;
    UINT32 Sniffed:1;
    UINT32 Outbound:1;
    UINT32 Loopback:1;
    UINT32 Impostor:1;
    UINT32 IPv6:1;
    UINT32 IPChecksum:1;
    UINT32 TCPChecksum:1;
    UINT32 UDPChecksum:1;
    UINT32 Reserved1:8;
    UINT32 Reserved2;
    union
    {
        struct
        {
            UINT32 IfIdx;
            UINT32 SubIfIdx;
        } Network;
        struct
        {
            UINT64 EndpointId;
            UINT64 ParentEndpointId;
            UINT32 ProcessId;
            UINT32 LocalAddr[4];
            UINT32 RemoteAddr[4];
            UINT16 LocalPort;
            UINT16 RemotePort;
            UINT8  Protocol;
        } Flow;
        struct
        {
            UINT64 EndpointId;
            UINT64 ParentEndpointId;
            UINT32 ProcessId;
            UINT32 LocalAddr[4];
            UINT32 RemoteAddr[4];
            UINT16 LocalPort;
            UINT16 RemotePort;
            UINT8  Protocol;
        } Socket;
        struct
        {
            INT64  Timestamp;
            UINT32 ProcessId;
            UINT32 Layer;
            UINT64 Flags;
            INT16  Priority;
        } Reflect;
        UINT8 Reserved3[64];
    };
} WINDIVERT_ADDRESS, *PWINDIVERT_ADDRESS;

// ── IPv4 / IPv6 / TCP / UDP headers ──
typedef struct
{
    UINT8  HdrLength:4;
    UINT8  Version:4;
    UINT8  TOS;
    UINT16 Length;
    UINT16 Id;
    UINT16 FragOff0;
    UINT8  TTL;
    UINT8  Protocol;
    UINT16 Checksum;
    UINT32 SrcAddr;
    UINT32 DstAddr;
} WINDIVERT_IPHDR, *PWINDIVERT_IPHDR;

typedef struct
{
    UINT32 TrafficClass0:4;
    UINT32 Version:4;
    UINT32 FlowLabel0:4;
    UINT32 TrafficClass1:4;
    UINT32 FlowLabel1[2];
    UINT16 Length;
    UINT8  NextHdr;
    UINT8  HopLimit;
    UINT32 SrcAddr[4];
    UINT32 DstAddr[4];
} WINDIVERT_IPV6HDR, *PWINDIVERT_IPV6HDR;

typedef struct
{
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT32 SeqNum;
    UINT32 AckNum;
    UINT16 Reserved1:4;
    UINT16 HdrLength:4;
    UINT16 Fin:1;
    UINT16 Syn:1;
    UINT16 Rst:1;
    UINT16 Psh:1;
    UINT16 Ack:1;
    UINT16 Urg:1;
    UINT16 Reserved2:2;
    UINT16 Window;
    UINT16 Checksum;
    UINT16 UrgPtr;
} WINDIVERT_TCPHDR, *PWINDIVERT_TCPHDR;

typedef struct
{
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT16 Length;
    UINT16 Checksum;
} WINDIVERT_UDPHDR, *PWINDIVERT_UDPHDR;

// ── Functions exported by WinDivert.dll ──
HANDLE WINAPI WinDivertOpen(const char* filter, WINDIVERT_LAYER layer,
    INT16 priority, UINT64 flags);

BOOL WINAPI WinDivertRecv(HANDLE handle, void* pPacket, UINT packetLen,
    UINT* pRecvLen, WINDIVERT_ADDRESS* pAddr);

BOOL WINAPI WinDivertRecvEx(HANDLE handle, void* pPacket, UINT packetLen,
    UINT* pRecvLen, UINT64 flags, WINDIVERT_ADDRESS* pAddr, UINT* pAddrLen,
    LPOVERLAPPED lpOverlapped);

BOOL WINAPI WinDivertSend(HANDLE handle, const void* pPacket, UINT packetLen,
    UINT* pSendLen, const WINDIVERT_ADDRESS* pAddr);

BOOL WINAPI WinDivertSendEx(HANDLE handle, const void* pPacket, UINT packetLen,
    UINT* pSendLen, UINT64 flags, const WINDIVERT_ADDRESS* pAddr,
    UINT addrLen, LPOVERLAPPED lpOverlapped);

BOOL WINAPI WinDivertShutdown(HANDLE handle, UINT how);
BOOL WINAPI WinDivertClose(HANDLE handle);

BOOL WINAPI WinDivertSetParam(HANDLE handle, WINDIVERT_PARAM param, UINT64 value);
BOOL WINAPI WinDivertGetParam(HANDLE handle, WINDIVERT_PARAM param, UINT64* pValue);

BOOL WINAPI WinDivertHelperParsePacket(const void* pPacket, UINT packetLen,
    PWINDIVERT_IPHDR* ppIpHdr, PWINDIVERT_IPV6HDR* ppIpv6Hdr,
    UINT8* pProtocol, void** ppIcmpHdr, void** ppIcmpv6Hdr,
    PWINDIVERT_TCPHDR* ppTcpHdr, PWINDIVERT_UDPHDR* ppUdpHdr,
    void** ppData, UINT* pDataLen, void** ppNext, UINT* pNextLen);

BOOL WINAPI WinDivertHelperCalcChecksums(void* pPacket, UINT packetLen,
    WINDIVERT_ADDRESS* pAddr, UINT64 flags);

#define WINDIVERT_SHUTDOWN_RECV  0x1
#define WINDIVERT_SHUTDOWN_SEND  0x2
#define WINDIVERT_SHUTDOWN_BOTH  0x3

#ifdef __cplusplus
}
#endif

#endif // WINDIVERT_H
