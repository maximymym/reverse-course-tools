#pragma once

#include <Windows.h>
#include <string>
#include <vector>

class Injector
{
public:
	bool Init();

	// LEGACY_DISK_PATH — kept as fallback only. Requires the DLL to exist on
	// disk and triggers LoadLibraryA in the target. Use InjectManualMap for
	// the production path (DLL stays encrypted inside DotaFarm.exe).
	bool InjectLoadLibrary( DWORD pid, const std::string& dllPath );

	// Production inject path — manually map an in-memory DLL image into the
	// target without ever touching disk in plaintext. Buffer is the decrypted
	// PE bytes (from payload_loader::LoadEmbeddedDll). Returns true once the
	// remote DllMain has run.
	bool InjectManualMap( DWORD pid, const uint8_t* dllBuf, size_t dllSize );

	// Fallback inject path — write the in-memory DLL buffer to a temp file with
	// a random GUID name, LoadLibrary it, then securely overwrite + delete the
	// temp file. Used when InjectManualMap fails on heavily protected DLLs
	// (VMProtect / runtime self-checks). DLL is on disk for ~tens of ms.
	bool InjectViaTempFile( DWORD pid, const uint8_t* dllBuf, size_t dllSize,
		const char* tag );

	// Write instance config JSON with hero pool.
	// reconnectLobbyId: если != 0, DLL стартует в режиме "reconnect": не дёргает
	// matchmaking init, а сразу вызывает reconnect_to_match(lobby_id) и
	// auto_queue=false. Используется при recovery после mid-match crash.
	// requirePeerReady: для two-stand pairing — DLL ждёт C:\\temp\\andromeda\\peer_ready.flag
	// перед FORMING_PARTY → QUEUING transition. Orchestrator выставляет когда pairing.enabled.
	static bool WriteInstanceConfig( DWORD pid, int instanceId, const char* role,
		const std::vector<std::string>& heroes,
		const uint64_t* partyMembers, int partyCount,
		uint32_t region, uint32_t gameMode,
		uint64_t reconnectLobbyId = 0,
		bool requirePeerReady = false,
		float peerReadyTimeoutS = 120.0f );

	// Write per-PID proxy config for ProxyHook.dll. Atomic write to
	// C:\temp\andromeda\proxy_<pid>.json. proxy string должен быть в формате
	// "socks5://user:pass@host:port" (пустой → DLL proxy-hooks no-op).
	//
	// hwidSeed: если не пустой → добавляет секцию "hwid_spoof" в JSON, что
	// активирует per-process HWID hooks в DLL. Seed формат рекомендуемый:
	// "<steamId>_<YYYY-MM>" для monthly-stable fingerprint.
	static bool WriteProxyConfig( DWORD pid, const std::string& proxy,
		const std::string& hwidSeed = "" );

private:
	bool m_bInitialized = false;
};
