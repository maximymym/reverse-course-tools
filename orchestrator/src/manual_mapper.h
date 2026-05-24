#pragma once
// =====================================================================
// PE Manual Mapper (DotaFarm port of deploy/stub/src/mapper.h).
// Differences vs the EFT mapper:
//   - usermode only: OpenProcess + VirtualAllocEx + RPM/WPM (no kernel driver).
//     Dota2 has no kernel AC; usermode is sufficient.
//   - trigger = CreateRemoteThread on the mapped DllMain (proven path), no
//     IAT hook tricks. Allocated shellcode page lives inside the target,
//     calls RtlAddFunctionTable + DllMain + RET.
// =====================================================================

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#ifndef _NTDEF_
typedef long NTSTATUS;
#endif

namespace manual_mapper {

// ── Logging shim — replaced by Log() if caller wires it; default = stdout.
inline void DefaultLog( const char* fmt, ... )
{
	char buf[1024];
	va_list ap; va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	fputs( buf, stdout );
	OutputDebugStringA( buf );
}
inline void ( *LogFn )( const char* fmt, ... ) = DefaultLog;
inline void SetLogger( void ( *fn )( const char*, ... ) ) { LogFn = fn; }

// ── Remote memory primitives (usermode) ───────────────────────────────

inline bool RpmRead( HANDLE hProc, uint64_t addr, void* out, size_t size )
{
	SIZE_T got = 0;
	BOOL ok = ReadProcessMemory( hProc, reinterpret_cast<LPCVOID>( addr ),
		out, size, &got );
	return ok && got == size;
}

inline bool WpmWrite( HANDLE hProc, uint64_t addr, const void* data, size_t size )
{
	SIZE_T put = 0;
	BOOL ok = WriteProcessMemory( hProc, reinterpret_cast<LPVOID>( addr ),
		data, size, &put );
	return ok && put == size;
}

// =====================================================================
//  Structures (identical layout to mapper::*)
// =====================================================================

struct SectionInfo {
	char name[9]{};
	uint32_t virtualAddress = 0;
	uint32_t virtualSize = 0;
	uint32_t rawSize = 0;
	uint32_t rawOffset = 0;
	std::vector<uint8_t> data;
};

struct ImportEntry {
	std::string name;
	bool isOrdinal = false;
	uint16_t ordinal = 0;
	uint32_t iatRva = 0;
};

struct ImportBlock {
	std::string dll;
	std::vector<ImportEntry> entries;
};

struct RelocEntry {
	uint32_t rva;
	uint8_t type;
};

struct SectionHdr {
	uint32_t virtualAddress = 0;
	uint32_t virtualSize = 0;
	uint32_t characteristics = 0;
};

struct PeInfo {
	uint64_t imageBase = 0;
	uint32_t sizeOfImage = 0;
	uint32_t entryPointRva = 0;
	uint32_t headerSize = 0;
	std::vector<SectionInfo> sections;
	std::vector<ImportBlock> imports;
	std::vector<RelocEntry> relocs;
	uint32_t pdataRva = 0;
	uint32_t pdataSize = 0;
	uint32_t pdataEntryCount = 0;
	uint32_t tlsDirRva = 0;
	uint32_t loadCfgRva = 0;
	uint32_t loadCfgSize = 0;
	std::vector<SectionHdr> sectionHdrs;
};

struct ExportEntry {
	uint64_t address = 0;
	std::string forwarder;
	bool isForwarder = false;
};

struct ExportTable {
	std::map<std::string, ExportEntry> byName;
	uint32_t ordinalBase = 0;
	std::vector<uint8_t> funcTableRaw;
	uint32_t numFunctions = 0;
	uint64_t dllBase = 0;
	uint32_t expRva = 0;
	uint32_t expSize = 0;
};

// =====================================================================
//  PE Parsing (local file)
// =====================================================================

static uint32_t RvaToFileOffset( const std::vector<SectionInfo>& secs, uint32_t rva )
{
	for ( auto& s : secs )
	{
		uint32_t secEnd = s.virtualAddress + ( ( s.virtualSize > s.rawSize ) ? s.virtualSize : s.rawSize );
		if ( rva >= s.virtualAddress && rva < secEnd )
			return s.rawOffset + ( rva - s.virtualAddress );
	}
	return 0;
}

inline bool ParsePE( const uint8_t* data, size_t size, PeInfo& out )
{
	if ( size < sizeof( IMAGE_DOS_HEADER ) ) return false;
	auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( data );
	if ( dos->e_magic != IMAGE_DOS_SIGNATURE ) return false;

	size_t ntOff = dos->e_lfanew;
	if ( ntOff + sizeof( IMAGE_NT_HEADERS64 ) > size ) return false;
	auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>( data + ntOff );
	if ( nt->Signature != IMAGE_NT_SIGNATURE ) return false;
	if ( nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ) return false;

	out.imageBase = nt->OptionalHeader.ImageBase;
	out.sizeOfImage = nt->OptionalHeader.SizeOfImage;
	out.entryPointRva = nt->OptionalHeader.AddressOfEntryPoint;
	out.headerSize = nt->OptionalHeader.SizeOfHeaders;

	auto* secHdr = IMAGE_FIRST_SECTION( nt );
	for ( WORD i = 0; i < nt->FileHeader.NumberOfSections; i++ )
	{
		SectionInfo sec{};
		memcpy( sec.name, secHdr[i].Name, 8 );
		sec.name[8] = 0;
		sec.virtualAddress = secHdr[i].VirtualAddress;
		sec.virtualSize = secHdr[i].Misc.VirtualSize;
		sec.rawSize = secHdr[i].SizeOfRawData;
		sec.rawOffset = secHdr[i].PointerToRawData;
		if ( sec.rawSize > 0 && sec.rawOffset + sec.rawSize <= size )
			sec.data.assign( data + sec.rawOffset, data + sec.rawOffset + sec.rawSize );
		out.sections.push_back( std::move( sec ) );

		SectionHdr sh{};
		sh.virtualAddress = secHdr[i].VirtualAddress;
		sh.virtualSize = secHdr[i].Misc.VirtualSize;
		sh.characteristics = secHdr[i].Characteristics;
		out.sectionHdrs.push_back( sh );

		char cleanName[9]{};
		memcpy( cleanName, secHdr[i].Name, 8 );
		if ( strcmp( cleanName, ".pdata" ) == 0 )
		{
			out.pdataRva = secHdr[i].VirtualAddress;
			out.pdataSize = secHdr[i].SizeOfRawData;
			out.pdataEntryCount = secHdr[i].SizeOfRawData / 12;
		}
	}

	// TLS + LoadConfig directories
	auto& tlsDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
	if ( tlsDir.VirtualAddress && tlsDir.Size )
		out.tlsDirRva = tlsDir.VirtualAddress;

	auto& lcDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
	if ( lcDir.VirtualAddress && lcDir.Size )
	{
		out.loadCfgRva = lcDir.VirtualAddress;
		out.loadCfgSize = lcDir.Size;
	}

	// Imports
	auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if ( impDir.VirtualAddress && impDir.Size )
	{
		uint32_t impOff = RvaToFileOffset( out.sections, impDir.VirtualAddress );
		while ( impOff && impOff + sizeof( IMAGE_IMPORT_DESCRIPTOR ) <= size )
		{
			auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>( data + impOff );
			if ( !desc->Name ) break;

			uint32_t nameOff = RvaToFileOffset( out.sections, desc->Name );
			if ( !nameOff || nameOff >= size ) break;

			ImportBlock block;
			block.dll = reinterpret_cast<const char*>( data + nameOff );

			uint32_t oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
			uint32_t iatRva = desc->FirstThunk;
			uint32_t thunkOff = RvaToFileOffset( out.sections, oftRva );

			if ( thunkOff )
			{
				int idx = 0;
				while ( thunkOff + ( idx + 1 ) * 8 <= size )
				{
					uint64_t thunkVal;
					memcpy( &thunkVal, data + thunkOff + idx * 8, 8 );
					if ( thunkVal == 0 ) break;

					ImportEntry entry;
					entry.iatRva = iatRva + idx * 8;
					if ( thunkVal & IMAGE_ORDINAL_FLAG64 )
					{
						entry.isOrdinal = true;
						entry.ordinal = static_cast<uint16_t>( thunkVal & 0xFFFF );
					}
					else
					{
						entry.isOrdinal = false;
						uint32_t hintOff = RvaToFileOffset( out.sections, static_cast<uint32_t>( thunkVal ) );
						if ( hintOff && hintOff + 3 < size )
							entry.name = reinterpret_cast<const char*>( data + hintOff + 2 );
					}
					block.entries.push_back( std::move( entry ) );
					idx++;
				}
			}
			out.imports.push_back( std::move( block ) );
			impOff += sizeof( IMAGE_IMPORT_DESCRIPTOR );
		}
	}

	// Relocations
	auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	if ( relocDir.VirtualAddress && relocDir.Size )
	{
		uint32_t relocOff = RvaToFileOffset( out.sections, relocDir.VirtualAddress );
		uint32_t relocEnd = relocOff + relocDir.Size;

		while ( relocOff && relocOff + 8 <= relocEnd && relocOff + 8 <= size )
		{
			uint32_t blockVa, blockSize;
			memcpy( &blockVa, data + relocOff, 4 );
			memcpy( &blockSize, data + relocOff + 4, 4 );
			if ( blockSize < 8 ) break;

			uint32_t numEntries = ( blockSize - 8 ) / 2;
			for ( uint32_t i = 0; i < numEntries && relocOff + 8 + ( i + 1 ) * 2 <= size; i++ )
			{
				uint16_t entry;
				memcpy( &entry, data + relocOff + 8 + i * 2, 2 );
				uint8_t type = entry >> 12;
				uint16_t offset = entry & 0xFFF;
				if ( type == IMAGE_REL_BASED_DIR64 || type == IMAGE_REL_BASED_HIGHLOW )
					out.relocs.push_back( { blockVa + offset, type } );
			}
			relocOff += blockSize;
		}
	}
	return true;
}

// =====================================================================
//  PEB walk — enumerate remote modules
// =====================================================================

inline std::map<std::string, uint64_t> GetRemoteModules( uint32_t pid, HANDLE hProc )
{
	std::map<std::string, uint64_t> modules;

	typedef NTSTATUS( NTAPI* NtQIPFn )( HANDLE, ULONG, PVOID, ULONG, PULONG );
	auto NtQIP = reinterpret_cast<NtQIPFn>(
		GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtQueryInformationProcess" ) );
	if ( !NtQIP ) return modules;

	struct PBI {
		int32_t  ExitStatus;
		uint32_t _pad1;
		uint64_t PebBaseAddress;
		uint64_t AffinityMask;
		int32_t  BasePriority;
		uint32_t _pad2;
		uint64_t UniqueProcessId;
		uint64_t InheritedFrom;
	} pbi{};
	ULONG rlen = 0;
	NtQIP( hProc, 0, &pbi, sizeof( pbi ), &rlen );

	uint64_t peb = pbi.PebBaseAddress;
	if ( !peb ) { LogFn( "[mm] PEB=0\n" ); return modules; }

	uint64_t ldr = 0;
	RpmRead( hProc, peb + 0x18, &ldr, 8 );
	if ( !ldr ) return modules;

	uint64_t headAddr = ldr + 0x10;
	uint64_t current = 0;
	RpmRead( hProc, headAddr, &current, 8 );

	for ( int i = 0; i < 512 && current && current != headAddr; i++ )
	{
		uint8_t entry[0x68]{};
		if ( !RpmRead( hProc, current, entry, sizeof( entry ) ) ) break;

		uint64_t nextFlink, dllBase;
		uint16_t nameLen;
		uint64_t nameBuf;
		memcpy( &nextFlink, entry + 0x00, 8 );
		memcpy( &dllBase,   entry + 0x30, 8 );
		memcpy( &nameLen,   entry + 0x58, 2 );
		memcpy( &nameBuf,   entry + 0x60, 8 );

		if ( nameLen > 0 && nameLen < 520 && nameBuf > 0 && dllBase > 0 )
		{
			std::vector<wchar_t> wname( nameLen / 2 + 1, 0 );
			if ( RpmRead( hProc, nameBuf, wname.data(), nameLen ) )
			{
				std::string name;
				for ( int j = 0; j < nameLen / 2 && wname[j]; j++ )
					name += static_cast<char>( tolower( static_cast<unsigned char>(
						wname[j] < 128 ? static_cast<char>( wname[j] ) : '?' ) ) );
				if ( !name.empty() ) modules[name] = dllBase;
			}
		}
		current = nextFlink;
	}
	return modules;
}

// =====================================================================
//  Remote export-table parser
// =====================================================================

inline ExportTable BuildRemoteExports( HANDLE hProc, uint64_t dllBase )
{
	ExportTable result{};
	result.dllBase = dllBase;

	uint8_t dos[0x40]{};
	if ( !RpmRead( hProc, dllBase, dos, sizeof( dos ) ) ) return result;
	if ( dos[0] != 'M' || dos[1] != 'Z' ) return result;

	uint32_t elfanew;
	memcpy( &elfanew, dos + 0x3C, 4 );

	uint8_t hdr[0x110]{};
	if ( !RpmRead( hProc, dllBase + elfanew, hdr, sizeof( hdr ) ) ) return result;
	if ( memcmp( hdr, "PE\0\0", 4 ) != 0 ) return result;

	uint16_t magic;
	memcpy( &magic, hdr + 0x18, 2 );

	uint32_t expRva, expSize;
	if ( magic == 0x20B ) { memcpy( &expRva, hdr + 0x88, 4 ); memcpy( &expSize, hdr + 0x8C, 4 ); }
	else if ( magic == 0x10B ) { memcpy( &expRva, hdr + 0x78, 4 ); memcpy( &expSize, hdr + 0x7C, 4 ); }
	else return result;
	if ( !expRva ) return result;
	result.expRva = expRva;
	result.expSize = expSize;

	uint8_t ed[0x28]{};
	if ( !RpmRead( hProc, dllBase + expRva, ed, sizeof( ed ) ) ) return result;

	uint32_t ordBase, numFuncs, numNames, rvaFuncs, rvaNames, rvaOrds;
	memcpy( &ordBase,  ed + 0x10, 4 );
	memcpy( &numFuncs, ed + 0x14, 4 );
	memcpy( &numNames, ed + 0x18, 4 );
	memcpy( &rvaFuncs, ed + 0x1C, 4 );
	memcpy( &rvaNames, ed + 0x20, 4 );
	memcpy( &rvaOrds,  ed + 0x24, 4 );
	if ( !numFuncs || !rvaFuncs ) return result;

	result.ordinalBase = ordBase;
	result.numFunctions = numFuncs;

	result.funcTableRaw.resize( numFuncs * 4 );
	if ( !RpmRead( hProc, dllBase + rvaFuncs, result.funcTableRaw.data(), numFuncs * 4 ) )
		return result;

	if ( !numNames ) return result;

	std::vector<uint8_t> nameTbl( numNames * 4 );
	std::vector<uint8_t> ordTbl( numNames * 2 );
	RpmRead( hProc, dllBase + rvaNames, nameTbl.data(), numNames * 4 );
	RpmRead( hProc, dllBase + rvaOrds,  ordTbl.data(),  numNames * 2 );

	std::vector<uint32_t> nameRvas( numNames );
	uint32_t minRva = UINT32_MAX, maxRva = 0;
	for ( uint32_t i = 0; i < numNames; i++ )
	{
		memcpy( &nameRvas[i], nameTbl.data() + i * 4, 4 );
		if ( nameRvas[i] < minRva ) minRva = nameRvas[i];
		if ( nameRvas[i] > maxRva ) maxRva = nameRvas[i];
	}

	size_t blockSize = ( maxRva - minRva + 300 < 0x200000 ) ? ( maxRva - minRva + 300 ) : 0x200000;
	std::vector<uint8_t> nameBlock( blockSize );
	if ( !RpmRead( hProc, dllBase + minRva, nameBlock.data(), blockSize ) )
		return result;

	for ( uint32_t i = 0; i < numNames; i++ )
	{
		uint32_t off = nameRvas[i] - minRva;
		if ( off >= nameBlock.size() ) continue;
		size_t end = off;
		while ( end < nameBlock.size() && nameBlock[end] ) end++;
		if ( end >= nameBlock.size() ) continue;

		std::string fname( reinterpret_cast<const char*>( nameBlock.data() + off ), end - off );

		uint16_t oidx;
		memcpy( &oidx, ordTbl.data() + i * 2, 2 );
		if ( oidx >= numFuncs ) continue;

		uint32_t frva;
		memcpy( &frva, result.funcTableRaw.data() + oidx * 4, 4 );

		ExportEntry ee{};
		if ( expRva <= frva && frva < expRva + expSize )
		{
			uint8_t fwdBuf[128]{};
			if ( RpmRead( hProc, dllBase + frva, fwdBuf, sizeof( fwdBuf ) ) )
			{
				size_t fwdEnd = 0;
				while ( fwdEnd < 127 && fwdBuf[fwdEnd] ) fwdEnd++;
				ee.isForwarder = true;
				ee.forwarder = std::string( reinterpret_cast<const char*>( fwdBuf ), fwdEnd );
			}
		}
		else ee.address = dllBase + frva;
		result.byName[fname] = ee;
	}
	return result;
}

// =====================================================================
//  Import resolution helpers
// =====================================================================

static uint64_t FindModuleBase( const std::map<std::string, uint64_t>& modules, const std::string& dllName )
{
	std::string lower = dllName;
	std::transform( lower.begin(), lower.end(), lower.begin(),
		[]( unsigned char c ) { return static_cast<char>( tolower( c ) ); } );

	auto it = modules.find( lower );
	if ( it != modules.end() ) return it->second;

	std::string stem = lower;
	auto pos = stem.rfind( ".dll" );
	if ( pos != std::string::npos ) stem = stem.substr( 0, pos );

	for ( auto& [name, base] : modules )
	{
		std::string nstem = name;
		auto npos = nstem.rfind( ".dll" );
		if ( npos != std::string::npos ) nstem = nstem.substr( 0, npos );
		if ( nstem == stem ) return base;
	}
	return 0;
}

static std::string ResolveApiSet( const std::string& name )
{
	HMODULE h = LoadLibraryA( name.c_str() );
	if ( !h ) return name;
	char buf[MAX_PATH]{};
	GetModuleFileNameA( h, buf, MAX_PATH );
	char* slash = strrchr( buf, '\\' );
	std::string result = slash ? ( slash + 1 ) : buf;
	std::transform( result.begin(), result.end(), result.begin(),
		[]( unsigned char c ) { return static_cast<char>( tolower( c ) ); } );
	return result;
}

static uint64_t ResolveImportFunc(
	HANDLE hProc,
	std::map<std::string, uint64_t>& modules,
	std::map<std::string, ExportTable>& cache,
	const std::string& dllName,
	const std::string& funcName = "",
	int ordinal = -1,
	int depth = 0 )
{
	if ( depth > 5 ) return 0;

	std::string dllLower = dllName;
	std::transform( dllLower.begin(), dllLower.end(), dllLower.begin(),
		[]( unsigned char c ) { return static_cast<char>( tolower( c ) ); } );

	if ( dllLower.substr( 0, 7 ) == "api-ms-" )
	{
		std::string real = ResolveApiSet( dllLower );
		if ( real != dllLower ) dllLower = real;
	}

	uint64_t base = FindModuleBase( modules, dllLower );
	if ( !base ) return 0;

	if ( cache.find( dllLower ) == cache.end() )
		cache[dllLower] = BuildRemoteExports( hProc, base );
	auto& exp = cache[dllLower];

	if ( !funcName.empty() )
	{
		auto it = exp.byName.find( funcName );
		if ( it == exp.byName.end() ) return 0;
		if ( !it->second.isForwarder ) return it->second.address;
		auto dot = it->second.forwarder.find( '.' );
		if ( dot == std::string::npos ) return 0;
		std::string fwdDll = it->second.forwarder.substr( 0, dot );
		std::string fwdFunc = it->second.forwarder.substr( dot + 1 );
		if ( fwdDll.find( '.' ) == std::string::npos ) fwdDll += ".dll";
		return ResolveImportFunc( hProc, modules, cache, fwdDll, fwdFunc, -1, depth + 1 );
	}

	if ( ordinal >= 0 )
	{
		int idx = ordinal - static_cast<int>( exp.ordinalBase );
		if ( idx < 0 || idx >= static_cast<int>( exp.numFunctions ) ) return 0;
		if ( exp.funcTableRaw.size() < static_cast<size_t>( ( idx + 1 ) * 4 ) ) return 0;

		uint32_t frva;
		memcpy( &frva, exp.funcTableRaw.data() + idx * 4, 4 );
		if ( exp.expRva <= frva && frva < exp.expRva + exp.expSize )
		{
			uint8_t fwdBuf[128]{};
			if ( RpmRead( hProc, exp.dllBase + frva, fwdBuf, sizeof( fwdBuf ) ) )
			{
				size_t fwdEnd = 0;
				while ( fwdEnd < 127 && fwdBuf[fwdEnd] ) fwdEnd++;
				std::string fwd( reinterpret_cast<const char*>( fwdBuf ), fwdEnd );
				auto dot = fwd.find( '.' );
				if ( dot != std::string::npos )
				{
					std::string fwdDll = fwd.substr( 0, dot ) + ".dll";
					std::string fwdFunc = fwd.substr( dot + 1 );
					return ResolveImportFunc( hProc, modules, cache, fwdDll, fwdFunc, -1, depth + 1 );
				}
			}
			return 0;
		}
		return exp.dllBase + frva;
	}
	return 0;
}

// =====================================================================
//  Relocations
// =====================================================================

inline void ApplyRelocations( uint8_t* image, uint32_t imageSize, uint64_t allocBase, const PeInfo& pe )
{
	int64_t delta = static_cast<int64_t>( allocBase ) - static_cast<int64_t>( pe.imageBase );
	if ( delta == 0 ) return;

	int count = 0;
	for ( auto& r : pe.relocs )
	{
		if ( r.type == IMAGE_REL_BASED_DIR64 && r.rva + 8 <= imageSize )
		{
			uint64_t old; memcpy( &old, image + r.rva, 8 );
			uint64_t fixed = old + delta;
			memcpy( image + r.rva, &fixed, 8 );
			count++;
		}
		else if ( r.type == IMAGE_REL_BASED_HIGHLOW && r.rva + 4 <= imageSize )
		{
			uint32_t old; memcpy( &old, image + r.rva, 4 );
			uint32_t fixed = static_cast<uint32_t>( old + delta );
			memcpy( image + r.rva, &fixed, 4 );
			count++;
		}
	}
	LogFn( "[mm] relocs: %d applied (delta=0x%llX)\n", count, (long long)delta );
}

// =====================================================================
//  Shellcode helpers
// =====================================================================

static void AppendBytes( std::vector<uint8_t>& v, const uint8_t* data, size_t len )
{
	v.insert( v.end(), data, data + len );
}
static void AppendU64( std::vector<uint8_t>& v, uint64_t val )
{
	AppendBytes( v, reinterpret_cast<const uint8_t*>( &val ), 8 );
}
static void AppendU32( std::vector<uint8_t>& v, uint32_t val )
{
	AppendBytes( v, reinterpret_cast<const uint8_t*>( &val ), 4 );
}

// =====================================================================
//  FinalizeSectionPerms — apply per-section page protections after the
//  image is written. Mapper allocates RWX up front for relocation/IAT
//  patching convenience; this tightens each section to match its real
//  Characteristics so the mapped image looks closer to a loader-mapped
//  module and EXECUTE pages aren't writable in steady state.
// =====================================================================

inline DWORD ProtFromCharacteristics( uint32_t ch )
{
	bool x = ( ch & IMAGE_SCN_MEM_EXECUTE ) != 0;
	bool w = ( ch & IMAGE_SCN_MEM_WRITE ) != 0;
	bool r = ( ch & IMAGE_SCN_MEM_READ ) != 0;
	if ( x && w ) return PAGE_EXECUTE_READWRITE;
	if ( x && r ) return PAGE_EXECUTE_READ;
	if ( x )       return PAGE_EXECUTE;
	if ( w )       return PAGE_READWRITE;
	if ( r )       return PAGE_READONLY;
	return PAGE_NOACCESS;
}

inline void FinalizeSectionPerms( HANDLE hProc, uint64_t allocBase, const PeInfo& pe )
{
	int applied = 0;
	for ( auto& sh : pe.sectionHdrs )
	{
		if ( sh.virtualSize == 0 ) continue;
		DWORD prot = ProtFromCharacteristics( sh.characteristics );
		DWORD oldProt = 0;
		LPVOID addr = reinterpret_cast<LPVOID>( allocBase + sh.virtualAddress );
		if ( !VirtualProtectEx( hProc, addr, sh.virtualSize, prot, &oldProt ) )
		{
			LogFn( "[mm] VPE(+0x%X, size=0x%X, prot=0x%X) failed: 0x%lX\n",
				sh.virtualAddress, sh.virtualSize, prot, GetLastError() );
			continue;
		}
		applied++;
	}
	LogFn( "[mm] finalized perms on %d/%zu sections\n", applied, pe.sectionHdrs.size() );
}

// =====================================================================
//  ExecuteEntryViaThread — RtlAddFunctionTable + DllMain + RET shellcode,
//  spawned via NtCreateThreadEx in the target.
// =====================================================================

typedef NTSTATUS( NTAPI* PNtCreateThreadEx )(
	PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, PVOID ObjectAttributes,
	HANDLE ProcessHandle, PVOID StartAddress, PVOID Argument,
	ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize,
	SIZE_T MaximumStackSize, PVOID AttributeList );

inline bool ExecuteEntryViaThread( HANDLE hProc, uint64_t allocBase, const PeInfo& pe )
{
	LogFn( "[mm] trigger via NtCreateThreadEx\n" );

	HMODULE hNtdll = GetModuleHandleA( "ntdll.dll" );
	auto sc_NtCreateThreadEx = reinterpret_cast<PNtCreateThreadEx>(
		GetProcAddress( hNtdll, "NtCreateThreadEx" ) );

	HMODULE hK32 = GetModuleHandleA( "kernel32.dll" );
	auto rtlAddFT = reinterpret_cast<uint64_t>( GetProcAddress( hNtdll, "RtlAddFunctionTable" ) );
	(void)hK32;

	LPVOID pageVA = VirtualAllocEx( hProc, nullptr, 8192,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE );
	if ( !pageVA )
	{
		LogFn( "[mm] VirtualAllocEx(sc) failed: 0x%lX\n", GetLastError() );
		return false;
	}
	uint64_t page = reinterpret_cast<uint64_t>( pageVA );
	uint64_t flagAddr = page + 0x300;
	// Stage breadcrumbs at +0x301..+0x306 — let us see how far the shellcode
	// progressed even when the remote process crashes mid-execution.
	uint64_t stageBase = page + 0x301;

	std::vector<uint8_t> sc;
	uint8_t movRcxImm[] = { 0x48, 0xB9 };
	uint8_t movR8Imm[]  = { 0x49, 0xB8 };
	uint8_t movRax[]    = { 0x48, 0xB8 };
	uint8_t callRax[]   = { 0xFF, 0xD0 };

	// Helper lambda: append `mov rax, imm64 / mov byte [rax], 1` for stage marker.
	auto markStage = [&]( uint64_t addr )
	{
		AppendBytes( sc, movRax, 2 ); AppendU64( sc, addr ); // mov rax, addr
		AppendBytes( sc, (const uint8_t*)"\xC6\x00\x01", 3 ); // mov byte [rax], 1
	};

	// Prologue: push rbx (TLS cursor) + sub rsp, 0x20 (shadow space, 16B aligned).
	AppendBytes( sc, (const uint8_t*)"\x53", 1 );             // push rbx
	AppendBytes( sc, (const uint8_t*)"\x48\x83\xEC\x20", 4 ); // sub rsp, 0x20
	markStage( stageBase + 0 ); // stage[0] = entered shellcode + prologue done

	// RtlAddFunctionTable(pdata, count, baseAddr)
	if ( pe.pdataRva && rtlAddFT )
	{
		uint64_t pdataAddr = allocBase + pe.pdataRva;
		AppendBytes( sc, movRcxImm, 2 ); AppendU64( sc, pdataAddr );
		AppendBytes( sc, (const uint8_t*)"\xBA", 1 ); AppendU32( sc, pe.pdataEntryCount );
		AppendBytes( sc, movR8Imm, 2 );  AppendU64( sc, allocBase );
		AppendBytes( sc, movRax, 2 );    AppendU64( sc, rtlAddFT );
		AppendBytes( sc, callRax, 2 );
	}
	markStage( stageBase + 1 ); // stage[1] = past RtlAddFunctionTable

	// SecurityCookie init: *LoadConfig.SecurityCookie = rdtsc ^ PID
	// Requires SecurityCookie field (offset 0x58) to be inside Size.
	if ( pe.loadCfgRva && pe.loadCfgSize >= 0x60 )
	{
		uint64_t cookieFieldAddr = allocBase + pe.loadCfgRva + 0x58;
		AppendBytes( sc, (const uint8_t*)"\x0F\x31", 2 );                  // rdtsc
		AppendBytes( sc, (const uint8_t*)"\x48\xC1\xE2\x20", 4 );          // shl rdx, 32
		AppendBytes( sc, (const uint8_t*)"\x48\x09\xD0", 3 );              // or  rax, rdx
		AppendBytes( sc, (const uint8_t*)"\x65\x48\x8B\x0C\x25\x40\x00\x00\x00", 9 ); // mov rcx, gs:[0x40] (TEB.ClientId.UniqueProcess = PID)
		AppendBytes( sc, (const uint8_t*)"\x48\x31\xC8", 3 );              // xor rax, rcx
		AppendBytes( sc, movRcxImm, 2 ); AppendU64( sc, cookieFieldAddr ); // mov rcx, imm64 (&SecurityCookie)
		AppendBytes( sc, (const uint8_t*)"\x48\x8B\x09", 3 );              // mov rcx, [rcx] (deref → cookie VA)
		AppendBytes( sc, (const uint8_t*)"\x48\x85\xC9", 3 );              // test rcx, rcx
		AppendBytes( sc, (const uint8_t*)"\x74\x03", 2 );                  // jz +3 (skip mov)
		AppendBytes( sc, (const uint8_t*)"\x48\x89\x01", 3 );              // mov [rcx], rax
	}
	markStage( stageBase + 2 ); // stage[2] = past SecurityCookie init

	// TLS callbacks walk: for each cb in *AddressOfCallBacks until NULL, call cb(allocBase, 1, 0)
	if ( pe.tlsDirRva )
	{
		uint64_t tlsCbField = allocBase + pe.tlsDirRva + 0x18; // IMAGE_TLS_DIRECTORY64.AddressOfCallBacks
		AppendBytes( sc, (const uint8_t*)"\x48\xBB", 2 ); AppendU64( sc, tlsCbField ); // mov rbx, imm64
		AppendBytes( sc, (const uint8_t*)"\x48\x8B\x1B", 3 );              // mov rbx, [rbx] (rbx = &callbacks[0])
		AppendBytes( sc, (const uint8_t*)"\x48\x85\xDB", 3 );              // test rbx, rbx
		AppendBytes( sc, (const uint8_t*)"\x74\x22", 2 );                  // jz tls_done  (+34)
		// tls_loop:
		AppendBytes( sc, (const uint8_t*)"\x48\x8B\x03", 3 );              // mov rax, [rbx]
		AppendBytes( sc, (const uint8_t*)"\x48\x85\xC0", 3 );              // test rax, rax
		AppendBytes( sc, (const uint8_t*)"\x74\x1A", 2 );                  // jz tls_done  (+26)
		AppendBytes( sc, movRcxImm, 2 ); AppendU64( sc, allocBase );       // mov rcx, allocBase
		AppendBytes( sc, (const uint8_t*)"\xBA\x01\x00\x00\x00", 5 );      // mov edx, 1 (DLL_PROCESS_ATTACH)
		AppendBytes( sc, (const uint8_t*)"\x4D\x31\xC0", 3 );               // xor r8, r8
		AppendBytes( sc, callRax, 2 );                                      // call rax
		AppendBytes( sc, (const uint8_t*)"\x48\x83\xC3\x08", 4 );          // add rbx, 8
		AppendBytes( sc, (const uint8_t*)"\xEB\xDE", 2 );                  // jmp tls_loop (-34)
		// tls_done:
	}
	markStage( stageBase + 3 ); // stage[3] = past TLS callbacks walk

	// DllMain(allocBase, DLL_PROCESS_ATTACH=1, NULL)
	AppendBytes( sc, movRcxImm, 2 ); AppendU64( sc, allocBase );
	AppendBytes( sc, (const uint8_t*)"\xBA\x01\x00\x00\x00", 5 ); // mov edx, 1
	AppendBytes( sc, (const uint8_t*)"\x4D\x31\xC0", 3 );         // xor r8, r8
	uint64_t entryPoint = allocBase + pe.entryPointRva;
	AppendBytes( sc, movRax, 2 ); AppendU64( sc, entryPoint );
	AppendBytes( sc, callRax, 2 );
	markStage( stageBase + 4 ); // stage[4] = DllMain returned

	// *flag = 2 (success marker)
	AppendBytes( sc, movRax, 2 ); AppendU64( sc, flagAddr );
	AppendBytes( sc, (const uint8_t*)"\xC6\x00\x02", 3 );

	AppendBytes( sc, (const uint8_t*)"\x48\x83\xC4\x20", 4 ); // add rsp, 0x20
	AppendBytes( sc, (const uint8_t*)"\x5B", 1 );             // pop rbx
	markStage( stageBase + 5 ); // stage[5] = epilogue OK (just before ret)
	sc.push_back( 0xC3 );                                     // ret

	LogFn( "[mm] sc=%zuB at 0x%llX\n", sc.size(), (long long)page );

	if ( !WpmWrite( hProc, page, sc.data(), sc.size() ) )
	{
		LogFn( "[mm] WPM(sc) failed: 0x%lX\n", GetLastError() );
		VirtualFreeEx( hProc, pageVA, 0, MEM_RELEASE );
		return false;
	}
	uint8_t zeros[8] = { 0 };
	WpmWrite( hProc, flagAddr, &zeros[0], 1 );
	WpmWrite( hProc, stageBase, zeros, 6 );

	HANDLE hThread = nullptr;
	bool spawned = false;
	if ( sc_NtCreateThreadEx )
	{
		NTSTATUS st = sc_NtCreateThreadEx( &hThread, THREAD_ALL_ACCESS, nullptr,
			hProc, pageVA, nullptr, 0, 0, 0x400000, 0x400000, nullptr );
		spawned = ( st >= 0 && hThread != nullptr );
		if ( !spawned ) LogFn( "[mm] NtCreateThreadEx failed: 0x%X\n", (unsigned)st );
	}
	if ( !spawned )
	{
		// Fallback to CreateRemoteThread (Win7 / restricted-PIDs)
		hThread = CreateRemoteThread( hProc, nullptr, 0x400000,
			reinterpret_cast<LPTHREAD_START_ROUTINE>( pageVA ),
			nullptr, 0, nullptr );
		spawned = ( hThread != nullptr );
		if ( !spawned ) LogFn( "[mm] CreateRemoteThread failed: 0x%lX\n", GetLastError() );
	}
	if ( !spawned )
	{
		VirtualFreeEx( hProc, pageVA, 0, MEM_RELEASE );
		return false;
	}

	int finalFlag = 0;
	uint8_t lastStages[6] = { 0 };
	for ( int i = 0; i < 300; i++ )
	{
		Sleep( 100 );
		uint8_t fv = 0;
		// Read breadcrumbs on every poll so we have the *latest* progress
		// snapshot even if RpmRead(flag) below fails (process dead).
		uint8_t stages[6] = { 0 };
		if ( RpmRead( hProc, stageBase, stages, sizeof(stages) ) )
			memcpy( lastStages, stages, sizeof(stages) );
		if ( !RpmRead( hProc, flagAddr, &fv, 1 ) )
		{
			LogFn( "[mm] process dead during wait (stages: prologue=%u rtlFT=%u cookie=%u tls=%u dllmain=%u epilogue=%u)\n",
				lastStages[0], lastStages[1], lastStages[2], lastStages[3], lastStages[4], lastStages[5] );
			break;
		}
		if ( fv == 2 ) { finalFlag = 2; LogFn( "[mm] DllMain OK (%.1fs)\n", ( i + 1 ) * 0.1f ); break; }
	}
	if ( finalFlag != 2 )
		LogFn( "[mm] thread injection timeout (last stages: prologue=%u rtlFT=%u cookie=%u tls=%u dllmain=%u epilogue=%u)\n",
			lastStages[0], lastStages[1], lastStages[2], lastStages[3], lastStages[4], lastStages[5] );

	WaitForSingleObject( hThread, 5000 );
	CloseHandle( hThread );
	// Don't free pageVA — thread shellcode page must stay until thread fully exits.
	return finalFlag == 2;
}

// =====================================================================
//  Main entry point
// =====================================================================

inline bool ManualMap( uint32_t pid, const uint8_t* dllData, size_t dllSize )
{
	LogFn( "[mm] === MANUAL MAP (pid=%u, dll=%zuB) ===\n", pid, dllSize );

	HANDLE hProc = OpenProcess( PROCESS_ALL_ACCESS, FALSE, pid );
	if ( !hProc )
	{
		LogFn( "[mm] OpenProcess(%u) failed: 0x%lX\n", pid, GetLastError() );
		return false;
	}

	PeInfo pe{};
	if ( !ParsePE( dllData, dllSize, pe ) )
	{
		LogFn( "[mm] PE parse failed\n" );
		CloseHandle( hProc );
		return false;
	}
	LogFn( "[mm] PE: size=0x%X, entry=+0x%X, sections=%zu, imports=%zu\n",
		pe.sizeOfImage, pe.entryPointRva, pe.sections.size(), pe.imports.size() );

	// Reserve+commit in target
	LPVOID rawAlloc = VirtualAllocEx( hProc, nullptr, pe.sizeOfImage,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE );
	if ( !rawAlloc )
	{
		LogFn( "[mm] VirtualAllocEx(image) failed: 0x%lX\n", GetLastError() );
		CloseHandle( hProc );
		return false;
	}
	uint64_t allocBase = reinterpret_cast<uint64_t>( rawAlloc );
	int64_t delta = static_cast<int64_t>( allocBase ) - static_cast<int64_t>( pe.imageBase );
	LogFn( "[mm] base=0x%llX (delta=0x%llX)\n", (long long)allocBase, (long long)delta );

	// Build mapped image
	std::vector<uint8_t> image( pe.sizeOfImage, 0 );
	size_t hdrCopy = ( pe.headerSize < dllSize ) ? pe.headerSize : dllSize;
	memcpy( image.data(), dllData, hdrCopy );
	for ( auto& sec : pe.sections )
	{
		if ( !sec.data.empty() && sec.virtualAddress + sec.data.size() <= pe.sizeOfImage )
			memcpy( image.data() + sec.virtualAddress, sec.data.data(), sec.data.size() );
	}

	ApplyRelocations( image.data(), pe.sizeOfImage, allocBase, pe );

	// Write image
	if ( !WpmWrite( hProc, allocBase, image.data(), pe.sizeOfImage ) )
	{
		LogFn( "[mm] WPM(image) failed: 0x%lX\n", GetLastError() );
		VirtualFreeEx( hProc, rawAlloc, 0, MEM_RELEASE );
		CloseHandle( hProc );
		return false;
	}

	// Resolve imports against the live target.
	auto modules = GetRemoteModules( pid, hProc );
	if ( modules.empty() )
	{
		LogFn( "[mm] no remote modules — abort\n" );
		VirtualFreeEx( hProc, rawAlloc, 0, MEM_RELEASE );
		CloseHandle( hProc );
		return false;
	}
	LogFn( "[mm] %zu remote modules\n", modules.size() );

	std::map<std::string, ExportTable> cache;
	std::vector<std::pair<uint32_t, uint64_t>> patches;
	std::vector<std::string> missing;

	for ( auto& block : pe.imports )
	{
		for ( auto& e : block.entries )
		{
			uint64_t addr = 0;
			if ( !e.name.empty() )
				addr = ResolveImportFunc( hProc, modules, cache, block.dll, e.name );
			else if ( e.isOrdinal )
				addr = ResolveImportFunc( hProc, modules, cache, block.dll, "", e.ordinal );
			if ( addr ) patches.push_back( { e.iatRva, addr } );
			else
			{
				std::string tag = e.name.empty() ? ( "#" + std::to_string( e.ordinal ) ) : e.name;
				missing.push_back( block.dll + "!" + tag );
			}
		}
	}

	if ( !missing.empty() )
	{
		LogFn( "[mm] %zu UNRESOLVED imports:\n", missing.size() );
		for ( size_t i = 0; i < missing.size() && i < 20; i++ )
			LogFn( "    %s\n", missing[i].c_str() );
	}
	LogFn( "[mm] resolved %zu/%zu\n", patches.size(), patches.size() + missing.size() );

	int patched = 0;
	for ( auto& [iatOff, addr] : patches )
		if ( WpmWrite( hProc, allocBase + iatOff, &addr, 8 ) ) patched++;
	LogFn( "[mm] patched %d/%zu IAT entries\n", patched, patches.size() );

	// Tighten page protections per IMAGE_SECTION_HEADER.Characteristics
	// (allocation started as RWX so relocs/IAT writes work; lock down now).
	FinalizeSectionPerms( hProc, allocBase, pe );

	// Trigger DllMain via remote thread
	bool ok = ExecuteEntryViaThread( hProc, allocBase, pe );

	if ( ok && !missing.empty() )
	{
		auto modules2 = GetRemoteModules( pid, hProc );
		if ( !modules2.empty() )
		{
			std::map<std::string, ExportTable> cache2;
			int fixed = 0;
			for ( auto& m : missing )
			{
				auto bangPos = m.find( '!' );
				if ( bangPos == std::string::npos ) continue;
				std::string dll = m.substr( 0, bangPos );
				std::string tag = m.substr( bangPos + 1 );

				for ( auto& block : pe.imports )
				{
					if ( block.dll != dll ) continue;
					for ( auto& e : block.entries )
					{
						std::string etag = e.name.empty() ? ( "#" + std::to_string( e.ordinal ) ) : e.name;
						if ( etag != tag ) continue;
						uint64_t addr = 0;
						if ( !e.name.empty() )
							addr = ResolveImportFunc( hProc, modules2, cache2, dll, e.name );
						else if ( e.isOrdinal )
							addr = ResolveImportFunc( hProc, modules2, cache2, dll, "", e.ordinal );
						if ( addr && WpmWrite( hProc, allocBase + e.iatRva, &addr, 8 ) )
							fixed++;
					}
				}
			}
			LogFn( "[mm] post-resolve fixed %d/%zu\n", fixed, missing.size() );
		}
	}

	// Erase PE header bytes to confuse simple module scanners (best-effort)
	if ( ok )
	{
		std::vector<uint8_t> zeros( 0x1000, 0 );
		WpmWrite( hProc, allocBase, zeros.data(), zeros.size() );
		LogFn( "[mm] PE headers erased\n" );
	}

	CloseHandle( hProc );
	return ok;
}

} // namespace manual_mapper
