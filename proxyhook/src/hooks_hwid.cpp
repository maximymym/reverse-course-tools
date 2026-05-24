// hooks_hwid.cpp — per-process HWID spoof поверх реальных API-вызовов.
//
// Покрытие:
//   Registry : NtQueryValueKey (MachineGuid, HwProfileGuid, ProductId,
//              BIOSVendor, BIOSVersion, SystemManufacturer, SystemProductName)
//   Network  : GetAdaptersInfo, GetAdaptersAddresses (MAC override)
//   Storage  : DeviceIoControl + IOCTL_STORAGE_QUERY_PROPERTY (disk serial)
//   Volume   : GetVolumeInformationW/A (volume serial DWORD)
//   SMBIOS   : GetSystemFirmwareTable (RSMB — serial, UUID, vendor)
//
// Принцип:
//   - Вызываем оригинал → получаем legit buffer → патчим in-place → возвращаем.
//     Format/layout сохраняется, подменяются только строки/байты
//     идентификаторов. Это совместимо со любым consumer, что проверяет
//     структуру буфера (size, type fields).
//   - No-op если g_state().hwid.enabled == false.

#include "hooks.h"
#include "hwid_gen.h"

#include <WinSock2.h>
#include <Windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <winternl.h>
#include <winioctl.h>
#include <MinHook.h>
#include <cstring>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ntdll.lib")

// KEY_VALUE_INFORMATION_CLASS и связанные struct не экспортированы из winternl.h.
// Объявляем сами — соответствуют ntddk.h. Важно: значения enum должны совпадать
// с реальным ntdll ABI (KeyValueBasicInformation = 0, KeyValueFullInformation = 1,
// KeyValuePartialInformation = 2, KeyValueFullInformationAlign64 = 3).
#ifndef KEY_VALUE_INFORMATION_CLASS_DEFINED
#define KEY_VALUE_INFORMATION_CLASS_DEFINED
typedef enum _KEY_VALUE_INFORMATION_CLASS
{
    KeyValueBasicInformation = 0,
    KeyValueFullInformation,
    KeyValuePartialInformation,
    KeyValueFullInformationAlign64,
    KeyValuePartialInformationAlign64
} KEY_VALUE_INFORMATION_CLASS;
#endif

#ifndef KEY_VALUE_PARTIAL_INFORMATION_DEFINED
#define KEY_VALUE_PARTIAL_INFORMATION_DEFINED
typedef struct _KEY_VALUE_PARTIAL_INFORMATION_S
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION_S;
#define KEY_VALUE_PARTIAL_INFORMATION KEY_VALUE_PARTIAL_INFORMATION_S
#endif

#ifndef KEY_VALUE_FULL_INFORMATION_DEFINED
#define KEY_VALUE_FULL_INFORMATION_DEFINED
typedef struct _KEY_VALUE_FULL_INFORMATION_S
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataOffset;
    ULONG DataLength;
    ULONG NameLength;
    WCHAR Name[1];
} KEY_VALUE_FULL_INFORMATION_S;
#define KEY_VALUE_FULL_INFORMATION KEY_VALUE_FULL_INFORMATION_S
#endif

namespace proxyhook
{

// Serializes in-place SMBIOS/storage/registry patching. Valve ACE может дёргать
// GetSystemFirmwareTable('RSMB', ...) параллельно с нашим self-check thread'ом,
// а обе стороны патчат один и тот же legit buffer. Без lock — race на in-place
// string replace → intermittent crash в loading screen / match start.
static std::mutex g_patchMutex;

// ──────────────────────────── Helpers ────────────────────────────

static bool HwidOn()
{
    return g_state().hwid.enabled;
}

static const HwidFakeValues& Fake()
{
    return g_state().hwid.values;
}

// UNICODE_STRING → lowercased std::wstring
static std::wstring UsToLower( PUNICODE_STRING us )
{
    if ( !us || !us->Buffer || us->Length == 0 )
        return {};
    std::wstring s( us->Buffer, us->Length / sizeof( WCHAR ) );
    for ( auto& c : s )
        c = (wchar_t)towlower( c );
    return s;
}

// Сравнение name с множеством возможных ключей (lowercase).
static bool WsEqualsAny( const std::wstring& w, std::initializer_list<const wchar_t*> names )
{
    for ( auto n : names )
        if ( w == n )
            return true;
    return false;
}

// UTF-8 → UTF-16
static std::wstring Utf8ToWide( const std::string& s )
{
    if ( s.empty() ) return {};
    int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
    std::wstring w( n, 0 );
    MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n );
    return w;
}

// ──────────────────────────── Registry hook ────────────────────────────
//
// Подменяем output NtQueryValueKey для известных HWID value-ключей. Key NAME
// недоступен в этом API (только handle) — идентифицируем по ValueName +
// эвристика (подходящий тип + разумный размер).

typedef NTSTATUS (NTAPI* fn_NtQueryValueKey_t)(
    HANDLE                        KeyHandle,
    PUNICODE_STRING               ValueName,
    KEY_VALUE_INFORMATION_CLASS   KeyValueInformationClass,
    PVOID                         KeyValueInformation,
    ULONG                         Length,
    PULONG                        ResultLength
);

static fn_NtQueryValueKey_t o_NtQueryValueKey = nullptr;

// Проверить что KeyValueInformationClass одна из поддерживаемых с полем Data.
// Возвращает указатель на Data и размер через out-параметры. Если structure не
// содержит данных (KeyValueBasicInformation) — returns false.
static bool GetKvData( KEY_VALUE_INFORMATION_CLASS cls, PVOID info, ULONG Length,
    uint8_t** dataOut, ULONG** dataLenOut, DWORD* typeOut )
{
    if ( !info || Length == 0 )
        return false;

    switch ( cls )
    {
    case KeyValuePartialInformation:
    {
        // typedef struct { DWORD TitleIndex; DWORD Type; DWORD DataLength; UCHAR Data[1]; }
        auto p = (KEY_VALUE_PARTIAL_INFORMATION*)info;
        if ( Length < sizeof( KEY_VALUE_PARTIAL_INFORMATION ) )
            return false;
        *dataOut    = p->Data;
        *dataLenOut = &p->DataLength;
        *typeOut    = p->Type;
        return true;
    }
    case KeyValueFullInformation:
    case KeyValueFullInformationAlign64:
    {
        auto p = (KEY_VALUE_FULL_INFORMATION*)info;
        if ( Length < sizeof( KEY_VALUE_FULL_INFORMATION ) )
            return false;
        *dataOut    = (uint8_t*)info + p->DataOffset;
        *dataLenOut = &p->DataLength;
        *typeOut    = p->Type;
        return true;
    }
    default:
        return false;
    }
}

// Подмена STRING (REG_SZ) на newText. Если буфер короче — обрезаем.
// newText — UTF-16 (REG_SZ хранит wchar_t). Null-termination сохраняется
// в пределах DataLength (совместимо с rt Windows ведёт себя).
static void ReplaceWideString( uint8_t* data, ULONG* dataLen, ULONG bufRemaining,
    const std::wstring& newText )
{
    // Сколько байт влезет (с null-term если возможно)
    ULONG desiredBytes = (ULONG)( ( newText.size() + 1 ) * sizeof( WCHAR ) );
    ULONG writeBytes = desiredBytes;
    if ( writeBytes > bufRemaining )
        writeBytes = bufRemaining;

    memset( data, 0, writeBytes );
    size_t charsToCopy = ( writeBytes / sizeof( WCHAR ) );
    if ( charsToCopy == 0 ) return;
    size_t copy = ( newText.size() < charsToCopy - 1 ) ? newText.size() : ( charsToCopy - 1 );
    memcpy( data, newText.data(), copy * sizeof( WCHAR ) );
    *dataLen = writeBytes;
}

// Подмена ANSI string (REG_SZ но некоторые consumer ждут ANSI — редкий случай).
static void ReplaceAnsiString( uint8_t* data, ULONG* dataLen, ULONG bufRemaining,
    const std::string& newText )
{
    ULONG desiredBytes = (ULONG)( newText.size() + 1 );
    ULONG writeBytes = desiredBytes;
    if ( writeBytes > bufRemaining )
        writeBytes = bufRemaining;

    memset( data, 0, writeBytes );
    size_t copy = ( newText.size() < writeBytes - 1 ) ? newText.size() : ( writeBytes - 1 );
    memcpy( data, newText.data(), copy );
    *dataLen = writeBytes;
}

// Подставляет spoof-value при совпадении ValueName. Возвращает true если подменил.
static bool MaybeSpoofRegValue( PUNICODE_STRING valueName, KEY_VALUE_INFORMATION_CLASS cls,
    PVOID info, ULONG Length )
{
    std::wstring name = UsToLower( valueName );
    if ( name.empty() )
        return false;

    uint8_t* data = nullptr;
    ULONG*   dataLen = nullptr;
    DWORD    type = 0;
    if ( !GetKvData( cls, info, Length, &data, &dataLen, &type ) )
        return false;

    // bufRemaining = from data start до конца Length
    ULONG bufRemaining = Length - ( ULONG )( ( uint8_t* )data - ( uint8_t* )info );

    const HwidFakeValues& fv = Fake();

    // MachineGuid — REG_SZ, WITHOUT braces, lowercase.
    if ( WsEqualsAny( name, { L"machineguid" } ) && type == REG_SZ )
    {
        std::wstring w = Utf8ToWide( fv.machineGuid );
        ReplaceWideString( data, dataLen, bufRemaining, w );
        return true;
    }

    // HwProfileGuid — REG_SZ, WITH braces.
    if ( WsEqualsAny( name, { L"hwprofileguid" } ) && type == REG_SZ )
    {
        std::wstring w = Utf8ToWide( fv.hwProfileGuid );
        ReplaceWideString( data, dataLen, bufRemaining, w );
        return true;
    }

    // ProductId
    if ( WsEqualsAny( name, { L"productid", L"digitalproductid" } ) && type == REG_SZ )
    {
        std::wstring w = Utf8ToWide( fv.productId );
        ReplaceWideString( data, dataLen, bufRemaining, w );
        return true;
    }

    // BIOS / System Info (HKLM\HARDWARE\DESCRIPTION\System\BIOS\*)
    if ( WsEqualsAny( name, { L"biosvendor" } ) && type == REG_SZ )
    {
        std::wstring w = Utf8ToWide( fv.biosVendor );
        ReplaceWideString( data, dataLen, bufRemaining, w );
        return true;
    }
    if ( WsEqualsAny( name, { L"biosversion", L"biosreleasedate" } ) && type == REG_SZ )
    {
        std::wstring w = Utf8ToWide( fv.biosVersion );
        ReplaceWideString( data, dataLen, bufRemaining, w );
        return true;
    }
    if ( WsEqualsAny( name, { L"systemmanufacturer" } ) && type == REG_SZ )
    {
        std::wstring w = L"ASUSTeK COMPUTER INC.";
        ReplaceWideString( data, dataLen, bufRemaining, w );
        return true;
    }
    if ( WsEqualsAny( name, { L"systemproductname" } ) && type == REG_SZ )
    {
        // Use first 8 chars of disk serial as "model code"
        std::string model = "PRIME " + fv.diskSerial.substr( 0, 4 );
        ReplaceWideString( data, dataLen, bufRemaining, Utf8ToWide( model ) );
        return true;
    }

    // NetworkAddress (в некоторых Network\{guid}\* параметрах MAC в REG_SZ hex).
    if ( WsEqualsAny( name, { L"networkaddress" } ) && type == REG_SZ )
    {
        // MAC hex uppercase no separator, e.g. "02AABBCCDDEE"
        char buf[16];
        snprintf( buf, sizeof( buf ), "%02X%02X%02X%02X%02X%02X",
            fv.mac[0], fv.mac[1], fv.mac[2], fv.mac[3], fv.mac[4], fv.mac[5] );
        ReplaceWideString( data, dataLen, bufRemaining, Utf8ToWide( buf ) );
        return true;
    }

    return false;
}

static NTSTATUS NTAPI h_NtQueryValueKey(
    HANDLE KeyHandle, PUNICODE_STRING ValueName,
    KEY_VALUE_INFORMATION_CLASS KvCls,
    PVOID Info, ULONG Length, PULONG ResultLength )
{
    NTSTATUS st = o_NtQueryValueKey( KeyHandle, ValueName, KvCls, Info, Length, ResultLength );
    if ( !NT_SUCCESS( st ) || !HwidOn() || !Info || Length == 0 )
        return st;

    std::lock_guard<std::mutex> lk( g_patchMutex );
    if ( MaybeSpoofRegValue( ValueName, KvCls, Info, Length ) )
    {
        // Первые N матчей логируем
        static std::atomic<int> hits{ 0 };
        if ( hits.fetch_add( 1, std::memory_order_relaxed ) < 30 )
        {
            std::wstring name = UsToLower( ValueName );
            char buf[256];
            WideCharToMultiByte( CP_UTF8, 0, name.c_str(), -1, buf, sizeof( buf ), nullptr, nullptr );
            DbgLog( "hwid: reg spoofed '%s'", buf );
        }
    }

    return st;
}

// ──────────────────────────── Network hooks ────────────────────────────

typedef DWORD (WINAPI* fn_GetAdaptersInfo_t)( PIP_ADAPTER_INFO, PULONG );
typedef ULONG (WINAPI* fn_GetAdaptersAddresses_t)( ULONG, ULONG, PVOID,
    PIP_ADAPTER_ADDRESSES, PULONG );

static fn_GetAdaptersInfo_t       o_GetAdaptersInfo = nullptr;
static fn_GetAdaptersAddresses_t  o_GetAdaptersAddresses = nullptr;

// Подсчёт интерфейсов — каждому новому адаптеру даём slightly-modified MAC
// (base^index_byte), чтобы у разных адаптеров были разные MAC (как в реальности).
// Steam чаще смотрит первый non-loopback; главное — не допустить, чтобы все
// адаптеры были одинаковыми.
static void PatchMac( uint8_t* dst, size_t dstLen, int index )
{
    if ( dstLen < 6 ) return;
    const HwidFakeValues& fv = Fake();
    memcpy( dst, fv.mac.data(), 6 );
    // Index-variation последнего октета (index 0 = unchanged)
    dst[5] ^= (uint8_t)( index & 0xFF );
    // Сохраняем locally-administered bit
    dst[0] &= 0xFE;
    dst[0] |= 0x02;
}

static DWORD WINAPI h_GetAdaptersInfo( PIP_ADAPTER_INFO pOut, PULONG pLen )
{
    DWORD r = o_GetAdaptersInfo( pOut, pLen );
    if ( r != NO_ERROR || !HwidOn() || !pOut )
        return r;

    int idx = 0;
    for ( PIP_ADAPTER_INFO p = pOut; p; p = p->Next, idx++ )
    {
        if ( p->AddressLength >= 6 )
            PatchMac( (uint8_t*)p->Address, p->AddressLength, idx );
    }
    return r;
}

static ULONG WINAPI h_GetAdaptersAddresses( ULONG family, ULONG flags, PVOID reserved,
    PIP_ADAPTER_ADDRESSES pOut, PULONG pLen )
{
    ULONG r = o_GetAdaptersAddresses( family, flags, reserved, pOut, pLen );
    if ( r != ERROR_SUCCESS || !HwidOn() || !pOut )
        return r;

    int idx = 0;
    for ( PIP_ADAPTER_ADDRESSES p = pOut; p; p = p->Next, idx++ )
    {
        if ( p->PhysicalAddressLength >= 6 )
            PatchMac( p->PhysicalAddress, p->PhysicalAddressLength, idx );
    }
    return r;
}

// ──────────────────────────── Storage hook (IOCTL) ────────────────────────────

typedef BOOL (WINAPI* fn_DeviceIoControl_t)(
    HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED );

static fn_DeviceIoControl_t o_DeviceIoControl = nullptr;

// IOCTL_STORAGE_QUERY_PROPERTY output: STORAGE_DEVICE_DESCRIPTOR.
//   typedef struct {
//     DWORD Version, Size;
//     BYTE  DeviceType, DeviceTypeModifier, RemovableMedia, CommandQueueing;
//     DWORD VendorIdOffset, ProductIdOffset, ProductRevisionOffset, SerialNumberOffset;
//     STORAGE_BUS_TYPE BusType;
//     DWORD RawPropertiesLength;
//     BYTE  RawDeviceProperties[1];
//   } STORAGE_DEVICE_DESCRIPTOR;

static void PatchStorageSerial( uint8_t* out, DWORD bytes )
{
    if ( bytes < sizeof( STORAGE_DEVICE_DESCRIPTOR ) )
        return;

    auto d = (STORAGE_DEVICE_DESCRIPTOR*)out;
    if ( d->SerialNumberOffset == 0 || d->SerialNumberOffset >= bytes )
        return;

    const HwidFakeValues& fv = Fake();
    // Сколько у нас места до конца buffer (до первого следующего offset либо Size или bytes)
    DWORD avail = bytes - d->SerialNumberOffset;
    char* dst = (char*)out + d->SerialNumberOffset;

    // Находим длину текущего null-terminated serial чтобы не превысить
    DWORD curLen = 0;
    for ( DWORD i = 0; i < avail; i++ )
    {
        if ( dst[i] == 0 ) { curLen = i; break; }
    }
    if ( curLen == 0 ) curLen = avail - 1;

    DWORD write = (DWORD)fv.diskSerial.size();
    if ( write > curLen ) write = curLen;
    memset( dst, 0, curLen );
    memcpy( dst, fv.diskSerial.data(), write );
    // Terminating NUL только если есть место (curLen < avail). Иначе curLen+1
    // попадает за границу STORAGE_DEVICE_DESCRIPTOR'а и OOB-пишет.
    if ( curLen < avail )
        dst[curLen] = 0;
}

static BOOL WINAPI h_DeviceIoControl( HANDLE h, DWORD code, LPVOID in, DWORD inLen,
    LPVOID out, DWORD outLen, LPDWORD bytesRet, LPOVERLAPPED ov )
{
    BOOL r = o_DeviceIoControl( h, code, in, inLen, out, outLen, bytesRet, ov );
    if ( !r || !HwidOn() || !out )
        return r;

    // IOCTL_STORAGE_QUERY_PROPERTY = 0x2D1400
    if ( code == IOCTL_STORAGE_QUERY_PROPERTY && in && inLen >= sizeof( STORAGE_PROPERTY_QUERY ) )
    {
        auto q = (STORAGE_PROPERTY_QUERY*)in;
        if ( q->PropertyId == StorageDeviceProperty && q->QueryType == PropertyStandardQuery )
        {
            DWORD br = bytesRet ? *bytesRet : outLen;
            std::lock_guard<std::mutex> lk( g_patchMutex );
            PatchStorageSerial( (uint8_t*)out, br );
        }
    }

    return r;
}

// ──────────────────────────── Volume serial hooks ────────────────────────────

typedef BOOL (WINAPI* fn_GetVolumeInformationW_t)(
    LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, DWORD );
typedef BOOL (WINAPI* fn_GetVolumeInformationA_t)(
    LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD );
typedef BOOL (WINAPI* fn_GetVolumeInformationByHandleW_t)(
    HANDLE, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, DWORD );

static fn_GetVolumeInformationW_t           o_GetVolumeInformationW = nullptr;
static fn_GetVolumeInformationA_t           o_GetVolumeInformationA = nullptr;
static fn_GetVolumeInformationByHandleW_t   o_GetVolumeInformationByHandleW = nullptr;

static BOOL WINAPI h_GetVolumeInformationW( LPCWSTR root, LPWSTR volName, DWORD volBufLen,
    LPDWORD volSer, LPDWORD maxComp, LPDWORD flags, LPWSTR fsName, DWORD fsBufLen )
{
    BOOL r = o_GetVolumeInformationW( root, volName, volBufLen, volSer, maxComp, flags, fsName, fsBufLen );
    if ( r && HwidOn() && volSer )
        *volSer = Fake().volumeSerial;
    return r;
}

static BOOL WINAPI h_GetVolumeInformationA( LPCSTR root, LPSTR volName, DWORD volBufLen,
    LPDWORD volSer, LPDWORD maxComp, LPDWORD flags, LPSTR fsName, DWORD fsBufLen )
{
    BOOL r = o_GetVolumeInformationA( root, volName, volBufLen, volSer, maxComp, flags, fsName, fsBufLen );
    if ( r && HwidOn() && volSer )
        *volSer = Fake().volumeSerial;
    return r;
}

static BOOL WINAPI h_GetVolumeInformationByHandleW( HANDLE h, LPWSTR volName, DWORD volBufLen,
    LPDWORD volSer, LPDWORD maxComp, LPDWORD flags, LPWSTR fsName, DWORD fsBufLen )
{
    BOOL r = o_GetVolumeInformationByHandleW( h, volName, volBufLen, volSer, maxComp, flags, fsName, fsBufLen );
    if ( r && HwidOn() && volSer )
        *volSer = Fake().volumeSerial;
    return r;
}

// ──────────────────────────── SMBIOS hook ────────────────────────────
//
// SMBIOS layout (RSMB):
//   RawSMBIOSData header:
//     BYTE  Used20CallingMethod;
//     BYTE  SMBIOSMajorVersion;
//     BYTE  SMBIOSMinorVersion;
//     BYTE  DmiRevision;
//     DWORD Length;
//     BYTE  SMBIOSTableData[Length];
//
// Внутри SMBIOSTableData лежат структуры:
//   header: { BYTE Type; BYTE Length; WORD Handle; }  ← Length — только formatted part,
//           после него идут string table (null-terminated), double-null = конец.
//
// Type 0 (BIOS):     offsets {Vendor=0x04, Version=0x05, ReleaseDate=0x08} — номера строк (1-based)
// Type 1 (System):   offsets {Manufacturer=0x04, ProductName=0x05, Version=0x06, SerialNumber=0x07,
//                              UUID raw@0x08 (16 bytes), WakeUp@0x18, ...}
// Type 2 (Baseboard): offsets {Manufacturer=0x04, Product=0x05, Version=0x06, SerialNumber=0x07, ...}
//
// Strings хранятся как concatenated null-terminated sequence после formatted part.
// 1-based index: первая строка = idx 1. idx=0 = «not set».

typedef UINT (WINAPI* fn_GetSystemFirmwareTable_t)( DWORD, DWORD, PVOID, DWORD );
typedef UINT (WINAPI* fn_EnumSystemFirmwareTables_t)( DWORD, PVOID, DWORD );

static fn_GetSystemFirmwareTable_t      o_GetSystemFirmwareTable = nullptr;
static fn_EnumSystemFirmwareTables_t    o_EnumSystemFirmwareTables = nullptr;

#pragma pack(push, 1)
struct RawSmbiosHeader
{
    BYTE  Used20CallingMethod;
    BYTE  SMBIOSMajorVersion;
    BYTE  SMBIOSMinorVersion;
    BYTE  DmiRevision;
    DWORD Length;
    // BYTE  SMBIOSTableData[Length];
};

struct SmbiosStructHeader
{
    BYTE  Type;
    BYTE  Length;
    WORD  Handle;
};
#pragma pack(pop)

// Найти границы string-table после formatted-part. Возвращает указатель на
// начало string table и размер до double-null (inclusive). Если end >= bufEnd
// — table malformed, возвращаем nullptr.
static bool FindStringTable( uint8_t* formattedEnd, uint8_t* bufEnd,
    uint8_t** strStart, uint8_t** strEnd )
{
    if ( formattedEnd >= bufEnd )
        return false;
    *strStart = formattedEnd;
    uint8_t* p = formattedEnd;
    // Особый случай: если formattedEnd[0] == 0 && [1] == 0 — пустая table (2 nulls).
    while ( p < bufEnd - 1 )
    {
        if ( p[0] == 0 && p[1] == 0 )
        {
            *strEnd = p + 2;
            return true;
        }
        p++;
    }
    return false;
}

// Замена строки с index strIdx (1-based) на newStr внутри string table,
// ТОЛЬКО in-place: длина оригинальной строки сохраняется. Если fake длиннее —
// truncated до foundLen. Если короче — правая часть padded пробелами.
//
// Важно: blob size НЕ меняется → integrity checks в Dota/Valve не триггерятся.
// Return всегда 0 (no delta).
static int ReplaceSmbiosString( uint8_t* strStart, uint8_t* strEnd, uint8_t* /*bufEnd*/,
    int strIdx, const std::string& newStr )
{
    if ( strIdx <= 0 || !strStart || !strEnd )
        return 0;

    // Ищем N-ю null-terminated строку
    uint8_t* p = strStart;
    int curIdx = 1;
    uint8_t* foundStart = nullptr;
    size_t foundLen = 0;
    while ( p < strEnd )
    {
        uint8_t* sEnd = p;
        while ( sEnd < strEnd && *sEnd != 0 ) sEnd++;
        if ( sEnd == p )
            break; // empty string = конец table
        if ( curIdx == strIdx )
        {
            foundStart = p;
            foundLen = sEnd - p;
            break;
        }
        p = sEnd + 1;
        curIdx++;
    }

    if ( !foundStart || foundLen == 0 )
        return 0;

    // Копируем fake, truncate или pad spaces
    size_t copyLen = ( newStr.size() < foundLen ) ? newStr.size() : foundLen;
    memcpy( foundStart, newStr.data(), copyLen );
    if ( copyLen < foundLen )
        memset( foundStart + copyLen, ' ', foundLen - copyLen );

    return 0;  // blob size не меняется
}

// Патчим один struct (возвращает delta к общему size).
// structStart указывает на начало header'а. Обновляем string table и (для Type 1)
// raw UUID по offset 0x08 внутри formatted part.
static int PatchSmbiosStruct( uint8_t* structStart, uint8_t* bufEnd )
{
    if ( structStart + sizeof( SmbiosStructHeader ) > bufEnd )
        return 0;
    auto hdr = (SmbiosStructHeader*)structStart;
    if ( hdr->Length < sizeof( SmbiosStructHeader ) )
        return 0;
    uint8_t* formattedEnd = structStart + hdr->Length;
    if ( formattedEnd > bufEnd )
        return 0;

    uint8_t* strStart = nullptr;
    uint8_t* strEnd   = nullptr;
    if ( !FindStringTable( formattedEnd, bufEnd, &strStart, &strEnd ) )
        return 0;

    const HwidFakeValues& fv = Fake();
    int delta = 0;

    switch ( hdr->Type )
    {
    case 0: // BIOS
    {
        // Vendor = structStart[0x04], Version = [0x05], ReleaseDate = [0x08]
        delta += ReplaceSmbiosString( strStart, strEnd, bufEnd,
            structStart[0x04], fv.biosVendor );
        // Пересчёт после delta — strEnd сдвинулся
        strEnd += delta;
        delta += ReplaceSmbiosString( strStart, strEnd, bufEnd,
            structStart[0x05], fv.biosVersion );
        break;
    }
    case 1: // System
    {
        // Manufacturer=0x04, Product=0x05, Version=0x06, Serial=0x07, UUID raw @0x08 (16B)
        if ( hdr->Length >= 0x18 )
        {
            // UUID
            memcpy( structStart + 0x08, fv.smbiosUuid.data(), 16 );
        }
        delta += ReplaceSmbiosString( strStart, strEnd, bufEnd,
            structStart[0x04], "ASUSTeK COMPUTER INC." );
        strEnd += delta;
        delta += ReplaceSmbiosString( strStart, strEnd, bufEnd,
            structStart[0x07], fv.systemSerial );
        break;
    }
    case 2: // Baseboard
    {
        // Manufacturer=0x04, Product=0x05, Version=0x06, Serial=0x07
        delta += ReplaceSmbiosString( strStart, strEnd, bufEnd,
            structStart[0x04], "ASUSTeK COMPUTER INC." );
        strEnd += delta;
        delta += ReplaceSmbiosString( strStart, strEnd, bufEnd,
            structStart[0x07], fv.systemSerial );
        break;
    }
    default:
        break;
    }

    return delta;
}

static UINT WINAPI h_GetSystemFirmwareTable( DWORD provSig, DWORD tblId,
    PVOID buf, DWORD bufSize )
{
    UINT got = o_GetSystemFirmwareTable( provSig, tblId, buf, bufSize );
    if ( !HwidOn() || !buf || bufSize == 0 || got == 0 || got > bufSize )
        return got;

    // RSMB = 'RSMB' в provSig (little-endian: 'R','S','M','B')
    if ( provSig != 'RSMB' )
        return got;

    std::lock_guard<std::mutex> lk( g_patchMutex );

    uint8_t* blob = (uint8_t*)buf;
    uint8_t* bufEnd = blob + got;
    if ( got < sizeof( RawSmbiosHeader ) )
        return got;

    auto rawHdr = (RawSmbiosHeader*)blob;
    if ( rawHdr->Length == 0 || rawHdr->Length > got - sizeof( RawSmbiosHeader ) )
        return got;

    uint8_t* tbl      = blob + sizeof( RawSmbiosHeader );
    uint8_t* tblEnd   = tbl + rawHdr->Length;
    if ( tblEnd > bufEnd )
        tblEnd = bufEnd;

    int totalDelta = 0;
    uint8_t* p = tbl;
    int safety = 0;
    while ( p + sizeof( SmbiosStructHeader ) <= tblEnd && safety++ < 1024 )
    {
        auto hdr = (SmbiosStructHeader*)p;
        if ( hdr->Length < sizeof( SmbiosStructHeader ) )
            break;
        // End-of-table marker: Type 127
        if ( hdr->Type == 127 )
            break;

        int delta = PatchSmbiosStruct( p, tblEnd );
        if ( delta != 0 )
        {
            tblEnd += delta;
            totalDelta += delta;
        }

        // Найти конец текущей struct (formatted + string table + double-null)
        uint8_t* formattedEnd = p + hdr->Length;
        if ( formattedEnd >= tblEnd ) break;
        uint8_t* sStart = nullptr;
        uint8_t* sEnd = nullptr;
        if ( !FindStringTable( formattedEnd, tblEnd, &sStart, &sEnd ) )
            break;
        p = sEnd;
    }

    if ( totalDelta != 0 )
    {
        rawHdr->Length = (DWORD)( (int)rawHdr->Length + totalDelta );
        // Returned size: header + table data
        got = (UINT)( sizeof( RawSmbiosHeader ) + rawHdr->Length );
        DbgLog( "hwid: smbios patched (delta=%d, new_len=%u)", totalDelta, rawHdr->Length );
    }
    else
    {
        DbgLog( "hwid: smbios patched in-place (no length change)" );
    }

    return got;
}

// ──────────────────────────── Install ────────────────────────────

static bool HookMod( LPCSTR mod, LPCSTR fn, LPVOID detour, LPVOID* pOrig )
{
    HMODULE h = GetModuleHandleA( mod );
    if ( !h )
        h = LoadLibraryA( mod );
    if ( !h )
    {
        DbgLog( "hwid hook: module %s not loaded", mod );
        return false;
    }
    void* target = GetProcAddress( h, fn );
    if ( !target )
    {
        DbgLog( "hwid hook: GetProcAddress %s!%s failed", mod, fn );
        return false;
    }
    MH_STATUS s = MH_CreateHook( target, detour, pOrig );
    if ( s != MH_OK )
    {
        DbgLog( "hwid hook: CreateHook %s!%s: %d", mod, fn, s );
        return false;
    }
    s = MH_EnableHook( target );
    if ( s != MH_OK )
    {
        DbgLog( "hwid hook: EnableHook %s!%s: %d", mod, fn, s );
        return false;
    }
    return true;
}

bool InstallHwidHooks()
{
    if ( !g_state().hwid.enabled )
    {
        DbgLog( "hwid hooks: disabled (no seed)" );
        return true; // success, just no-op
    }

    const auto& fv = g_state().hwid.values;
    DbgLog( "hwid hooks: seed='%s' mac=%s vol=%08X disk=%s guid=%s",
        g_state().hwid.seed.c_str(),
        fv.macStr.c_str(), fv.volumeSerial,
        fv.diskSerial.c_str(), fv.machineGuid.c_str() );

    bool allOk = true;

    // Registry (ntdll!NtQueryValueKey). Required — без него главные идентификаторы
    // (MachineGuid) не spoof'ятся.
    allOk &= HookMod( "ntdll.dll", "NtQueryValueKey",
        (LPVOID)h_NtQueryValueKey, (LPVOID*)&o_NtQueryValueKey );

    // Network. Iphlpapi обычно loaded в user-mode процессах которые делают net.
    (void)HookMod( "iphlpapi.dll", "GetAdaptersInfo",
        (LPVOID)h_GetAdaptersInfo, (LPVOID*)&o_GetAdaptersInfo );
    (void)HookMod( "iphlpapi.dll", "GetAdaptersAddresses",
        (LPVOID)h_GetAdaptersAddresses, (LPVOID*)&o_GetAdaptersAddresses );

    // Storage (kernel32!DeviceIoControl)
    (void)HookMod( "kernel32.dll", "DeviceIoControl",
        (LPVOID)h_DeviceIoControl, (LPVOID*)&o_DeviceIoControl );
    // Некоторые приложения получают DeviceIoControl через kernelbase.dll.
    // Если hook на kernel32 уже стоит — kernelbase резолвит в тот же trampoline.

    // Volume
    (void)HookMod( "kernel32.dll", "GetVolumeInformationW",
        (LPVOID)h_GetVolumeInformationW, (LPVOID*)&o_GetVolumeInformationW );
    (void)HookMod( "kernel32.dll", "GetVolumeInformationA",
        (LPVOID)h_GetVolumeInformationA, (LPVOID*)&o_GetVolumeInformationA );
    (void)HookMod( "kernel32.dll", "GetVolumeInformationByHandleW",
        (LPVOID)h_GetVolumeInformationByHandleW, (LPVOID*)&o_GetVolumeInformationByHandleW );

    // SMBIOS
    (void)HookMod( "kernel32.dll", "GetSystemFirmwareTable",
        (LPVOID)h_GetSystemFirmwareTable, (LPVOID*)&o_GetSystemFirmwareTable );

    DbgLog( "InstallHwidHooks: registry=%s network=%s storage=%s volume=%s smbios=%s",
        o_NtQueryValueKey ? "OK" : "FAIL",
        ( o_GetAdaptersInfo && o_GetAdaptersAddresses ) ? "OK" : "partial",
        o_DeviceIoControl ? "OK" : "FAIL",
        o_GetVolumeInformationW ? "OK" : "FAIL",
        o_GetSystemFirmwareTable ? "OK" : "FAIL" );

    return allOk;
}

} // namespace proxyhook
