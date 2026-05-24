#pragma once

#include "config.h"

#include <Windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Per-account SOCKS5 через tun2socks (sing-box). Orchestrator генерирует JSON
// конфиг из accounts[], стартует sing-box.exe как child через JobObject (kill
// on parent death), и полагается на sing-box'овский process_path_regex route:
// process living в C:\BotSteam\<N>\ → outbound acc<N> (per-account proxy).
//
// Не-Steam трафик идёт через route final: "direct" → physical NIC → real IP.
// auto_route отключён, default system route остаётся прежним.
class SingboxSupervisor
{
public:
	using LogFn = std::function<void( const char* )>;

	SingboxSupervisor();
	~SingboxSupervisor();

	void SetLogger( LogFn fn );

	// Собрать конфиг из accounts, записать на диск, спавн sing-box.exe,
	// дождаться появления TUN адаптера (до 5 сек). Возвращает true если
	// sing-box живой и outbound'ы проинициализированы.
	bool Start( const std::vector<AccountConfig>& accounts,
		const std::string& exeDir,
		const std::string& workDir );

	void Stop();
	bool IsRunning() const { return m_running.load(); }

	// Удалить leftover split-default routes 0.0.0.0/1 + 128.0.0.0/1.
	// Вызывается: (1) при Init orchestrator'а (если предыдущий процесс упал
	// некорректно и оставил routes); (2) в начале Start() чтобы routes из
	// предыдущей sing-box сессии не перехватывали трафик когда outbound=0.
	// Безопасно вызывать при отсутствии routes — route DELETE просто вернёт
	// error, trafic не ломает.
	static void CleanupStaleRoutes();

	// Перегенерировать конфиг и рестартнуть (kill + spawn, ~500 ms downtime).
	// Вызывать когда accounts изменились или юзер правит proxy'и в GUI.
	bool ReloadConfig( const std::vector<AccountConfig>& accounts );

	struct Stats
	{
		bool        running = false;
		DWORD       pid = 0;
		size_t      outbounds = 0;  // число per-account proxy outbound'ов
		std::string configPath;
		std::string logPath;
		std::string lastError;
	};
	Stats GetStats() const;

	// Detect physical NIC FriendlyName для bind_interface / default_interface.
	// Возвращает первый UP IPv4 adapter с gateway, не TUN/TAP/VPN. Пустая строка
	// если ничего не найдено (orchestrator упадёт в auto_detect_interface=true).
	// Public — вызывается из GUI для show current bind.
	std::string DetectDefaultInterface();

	// Последний найденный physical NIC (cached). Для отображения в GUI.
	std::string GetDefaultInterface() const { return m_defaultInterface; }

private:
	bool BuildConfig( const std::vector<AccountConfig>& accounts,
		const std::string& workDir,
		std::string& outJson,
		size_t& outOutbounds );

	bool SpawnProcess();
	void Log( const char* fmt, ... );

	std::string m_defaultInterface; // cached result of DetectDefaultInterface

	// Config state
	std::string  m_exeDir;
	std::string  m_workDir;
	std::string  m_singboxExePath;
	std::string  m_configPath;
	std::string  m_logPath;

	std::vector<AccountConfig> m_accounts;
	size_t       m_outbounds = 0;

	// Process state
	HANDLE       m_jobObject = nullptr;
	HANDLE       m_processHandle = nullptr;
	DWORD        m_pid = 0;

	std::atomic<bool> m_running{ false };

	mutable std::mutex m_mutex;
	std::string  m_lastError;

	LogFn        m_logger;
};
