#pragma once

// DotaMinifier — applies low-quality Dota 2 config (autoexec.cfg + video.txt)
// per-bot ДО запуска dota2.exe. Backup → write minified → bot launches → reads
// min config. Revert при StopFarm. Crash-recovery: при Init() детектит leftover
// .applied markers и автоматически revert.
//
// VAC-safety: использует ТОЛЬКО safe-path (launch options + autoexec + video.txt).
// VPK patching и D3D hook оставлены за флагами, но НЕ реализованы (см.
// docs/MINIFIER_RESEARCH.md §3).

#include <Windows.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct FarmConfig;

struct MinifierConfig
{
	bool enabled = false;
	bool applyLaunchOptions = true;
	bool applyAutoexec      = true;
	bool applyVideoTxt      = true;
	bool applyVpkPatches    = false;   // VPK_DISABLED 2026-05-17 — kill-switch навсегда
	// True если юзер в farm.json пытался поставить apply_vpk_patches=true.
	// Runtime-only (не сохраняется). Orchestrator Init залогирует для отладки.
	bool vpkAttemptedFromConfig = false;
	bool applyD3dHook       = false;   // RISKY — НЕ реализовано
	int  fpsMax             = 15;     // farm — не нужно >15 fps, AI server-side
	int  resolutionWidth    = 640;    // минимальная разумная (320 ломает UI hitboxes)
	int  resolutionHeight   = 480;

	// VPK preset (имя из dota2_minify_wrapper.py PRESETS):
	//   "minify_aggressive" — ВСЕ wrapper-supported моды (default, max savings)
	//   "minimal_visuals"   — почти то же без Remove River
	//   "performance_only"  — только GPU/CPU-нагружающие particle/sound
	//   "audio_mute_only"   — заглушить voice/announcer/taunt/ambient
	std::string vpkPreset = "minify_aggressive";

	// Bundle H portable: standalone PyInstaller .exe (vendor'ан рядом с DotaFarm).
	// Если файл существует — используется он (zero-install). Иначе — fallback на
	// pythonExe + wrapperScript (dev-машина с Python+vpk в PATH).
	std::string wrapperExe = "scripts\\dota2_minify_wrapper.exe";

	// Путь к python.exe (или "python" если в PATH). Используется как fallback,
	// если wrapperExe отсутствует. Wrapper требует Python ≥3.8 и `pip install vpk`.
	std::string pythonExe = "python.exe";

	// Путь к dota2_minify_wrapper.py. По умолчанию — рядом с DotaFarm.exe в
	// scripts/, при упаковке через package.sh туда копируется.
	std::string wrapperScript = "scripts\\dota2_minify_wrapper.py";

	// Если true — wrapper зовётся с --fix-launch-options / --cleanup-launch-options.
	// Меняет '-language minify' для всех Steam users у которых есть Dota 2 data.
	// ВАЖНО: Steam должен быть закрыт перед apply (иначе localconfig.vdf overwrite).
	// Default false — orchestrator уже сам ставит -language через launch args.
	bool vpkFixLaunchOptions = false;

	// Timeout subprocess вызова в миллисекундах. Apply на большой preset может
	// занимать 10-30 сек (1500+ blank-копий + zip pak).
	int  vpkSubprocessTimeoutMs = 60000;

	// ── RAM aggressive flags (Bundle RAM, 2026-05-13) ──
	// renderBackend: "dx11" (default — стабильно), "vulkan" (легче драйверы
	// nvwgf2umx/nvgpucomp64 на 30-50%), "empty" (rendersystemempty.dll — null
	// render, экономит ~160 MB NVidia stack но РИСК: panorama.dll/v8.dll/CUIEngine
	// требуют render context, могут не инициализироваться → бот afk без orders).
	// Текущий -dx9 НЕ работает (нет rendersystemdx9.dll, engine fallback'ает на
	// default = dx11), поэтому baseline = dx11.
	std::string renderBackend = "dx11";

	// ── Memory reclamation (Bundle MEMRED, 2026-05-16) ──
	//
	// Замена старого per-process EmptyWorkingSet (periodicWsTrim/wsTrimIntervalS,
	// удалены при миграции). Тот подход визуально убивал dota'у — после trim'а
	// активные текстуры/шейдеры принудительно re-page-ились с диска. Теперь
	// идём через system-wide NtSetSystemInformation(SystemMemoryListInformation)
	// — чистим standby/modified/free PFN lists без touch'а активных WS.
	// См. src/mem_reclaim.h для подробного маппинга на memreduct GUI checkbox'ы.
	bool memReclaimEnabled            = true;   // master switch
	int  memReclaimIntervalS          = 60;     // periodic tick (clamp 10-3600)
	int  memReclaimAutoThreshold      = 85;     // % memory load → auto-trigger (0=off)

	bool memReclaimPurgeStandby           = true;   // MemoryPurgeStandbyList
	bool memReclaimPurgeLowPrioStandby    = true;   // MemoryPurgeLowPriorityStandbyList
	bool memReclaimFlushModified          = false;  // MemoryFlushModifiedList (disk pressure)
	bool memReclaimCombinePages           = true;   // SystemCombinePhysicalMemoryInformation (Win10+)
	// EmptyWorkingSets — самый агрессивный, вызывает stutter. По умолчанию OFF
	// даже при миграции из legacy periodic_ws_trim=true (юзер должен включить
	// явно если хочет старое поведение).
	bool memReclaimEmptyAllWorkingSets    = false;

	// Runtime-only флаг (не сериализуется): true если параметры были взяты из
	// legacy periodic_ws_trim/ws_trim_interval_s ключей. Используется чтобы
	// Orchestrator::Init залогал миграцию один раз.
	bool memReclaimMigratedFromLegacy     = false;
};

struct MinifierBackupState
{
	int                                botIdx = -1;
	uint64_t                           steamId = 0;
	int64_t                            backedUpAtMs = 0;
	// path → original content (либо "<MISSING>" если файла не было).
	std::map<std::string, std::string> savedFiles;
	// Original launchArgs (подставляется в dota launch вместо minified).
	std::string                        originalLaunchArgs;
	bool                               valid = false;
};

class DotaMinifier
{
public:
	DotaMinifier() = default;

	// Конфигурация. Apply/Revert no-op'ятся если cfg.enabled=false.
	void               SetConfig( const MinifierConfig& cfg );
	const MinifierConfig& GetConfig() const { return m_cfg; }

	// Logger callback (опциональный). Если nullptr — silent.
	using LogFn = void (*)( void* ctx, const char* msg );
	void SetLogger( LogFn fn, void* ctx ) { m_logFn = fn; m_logCtx = ctx; }

	// Apply: backup current files → write minified content. Возвращает false при
	// невозможности backup'а (fail-safe — НЕ применять без backup).
	// steamId — для resolve userdata/<id>/570/local/cfg/video.txt.
	// Если steamId=0 — video.txt пропускается.
	bool ApplyToBot( int botIdx, uint64_t steamId, const FarmConfig& farmCfg );

	// Revert один бот. После успешного revert — backup state cleared.
	// Идемпотентно: повторный вызов = no-op.
	bool RevertBot( int botIdx );

	// Revert все боты. Вызывается из StopFarm + destructor.
	bool RevertAll();

	// Сканирует C:\temp\andromeda\minifier_backup\*\.applied. Возвращает кол-во
	// найденных stale backup'ов. Используется в Init() — если >0, вызвать
	// RevertStale() для cleanup'а после crash в предыдущей сессии.
	int  DetectStaleBackups();

	// Revert все detected stale backups (загружает state с диска и применяет
	// RevertBot для каждого). Безопасен если stale нет — возвращает true.
	bool RevertStale();

	// true если хотя бы один бот имеет активный backup (был применён minify
	// в текущей сессии или есть .applied маркер). Используется GUI чтобы
	// показать APPLY/REVERT кнопку в правильном состоянии.
	bool IsAppliedToAnyBot() const;

	// Modified launch args для бота: оригинальные + дополнительные minifier
	// options если applyLaunchOptions=true. Если cfg disabled или
	// applyLaunchOptions=false — возвращает original без изменений.
	// AppendIfMissing: не дублирует -novid если уже есть.
	std::string BuildLaunchArgs( int botIdx, const std::string& originalArgs ) const;

	// === Bundle G2: VPK patching через Python wrapper ===
	//
	// VPK apply/revert — session-wide (НЕ per-bot), т.к. все 10 ботов делят один
	// общий Dota install. Apply один раз перед стартом первого бота, revert один
	// раз после Stop. ApplyVpkPatches пишет marker файл в
	// MinifierBackupRoot/vpk/.applied — DetectStaleVpkPatches детектит его на
	// next start (после crash) для cleanup'а.
	//
	// Возвращают false при subprocess fail (process exit != 0 ИЛИ stdout не
	// содержит "ok": true). Логирование через SetLogger.

	bool ApplyVpkPatches();
	bool RevertVpkPatches();
	bool DetectStaleVpkPatches();   // true если есть наш применённый pak
	std::string GetVpkMarkerPath() const;  // public для тестов
	std::string GetVpkMarkerDir() const;

	// Backup directory (C:\temp\andromeda\minifier_backup\<botIdx>\).
	// Public для тестов.
	std::string GetBackupDir( int botIdx ) const;

	// Generate содержимое autoexec.cfg/video.txt по cfg. Public для тестов.
	std::string GenerateAutoexec() const;
	std::string GenerateVideoTxt( int width, int height ) const;

	// Resolve пути к файлам. Public для тестов.
	std::string ResolveAutoexecPath( int botIdx, const FarmConfig& farmCfg ) const;
	std::string ResolveVideoTxtPath( int botIdx, uint64_t steamId, const FarmConfig& farmCfg ) const;

private:
	void Log( const char* fmt, ... );
	bool BackupFile( const std::string& path, MinifierBackupState& bs );
	bool RestoreFiles( const MinifierBackupState& bs );
	bool WriteMarker( const MinifierBackupState& bs );
	bool LoadMarker( int botIdx, MinifierBackupState& bs );
	bool DeleteMarker( int botIdx );
	std::string MinifierBackupRoot() const;

	// Запустить python wrapper subprocess. Pipes stdout (json) + stderr (log)
	// в выходные параметры. Возвращает true при exit code == 0.
	bool RunPython( const std::string& cmdLineArgs, std::string& outStdout,
		std::string& outStderr, int& outExitCode );

	// JSON-search: ищет '"ok": true' substring (без полного JSON parse).
	bool JsonHasOkTrue( const std::string& json );

	MinifierConfig                       m_cfg;
	std::map<int, MinifierBackupState>   m_backups;
	mutable std::mutex                   m_mutex;
	LogFn                                m_logFn = nullptr;
	void*                                m_logCtx = nullptr;
};
