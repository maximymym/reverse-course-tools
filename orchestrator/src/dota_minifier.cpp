#include "dota_minifier.h"
#include "config.h"
#include "bot_dota_dir.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <chrono>

static const char* MINIFIER_BACKUP_ROOT = "C:\\temp\\andromeda\\minifier_backup";
static const char* FILE_MISSING_MARKER  = "<MISSING>";

namespace
{

int64_t NowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
}

bool MakeDirRecursive( const std::string& path )
{
	if ( path.empty() ) return false;
	if ( GetFileAttributesA( path.c_str() ) != INVALID_FILE_ATTRIBUTES )
		return true;

	size_t slash = path.find_last_of( "\\/" );
	if ( slash != std::string::npos )
	{
		std::string parent = path.substr( 0, slash );
		if ( !parent.empty() && parent.back() != ':' )
			MakeDirRecursive( parent );
	}
	return CreateDirectoryA( path.c_str(), nullptr ) != 0
		|| GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadFileToString( const std::string& path, std::string& out )
{
	std::ifstream f( path, std::ios::binary );
	if ( !f.is_open() ) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

bool WriteStringToFile( const std::string& path, const std::string& data )
{
	// Снимаем read-only если был выставлен (Dota могла поставить).
	DWORD attrs = GetFileAttributesA( path.c_str() );
	if ( attrs != INVALID_FILE_ATTRIBUTES && ( attrs & FILE_ATTRIBUTE_READONLY ) )
		SetFileAttributesA( path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY );

	size_t slash = path.find_last_of( "\\/" );
	if ( slash != std::string::npos )
		MakeDirRecursive( path.substr( 0, slash ) );

	std::ofstream f( path, std::ios::binary | std::ios::trunc );
	if ( !f.is_open() ) return false;
	f.write( data.data(), (std::streamsize)data.size() );
	return f.good();
}

// Convert backslashes for storing in marker (стабильно cross-platform).
std::string NormalizePathForStorage( std::string s )
{
	for ( char& c : s ) if ( c == '/' ) c = '\\';
	return s;
}

// Escape '|' и '\n' для marker file format.
std::string EscapeForMarker( const std::string& s )
{
	std::string out;
	out.reserve( s.size() );
	for ( char c : s )
	{
		if ( c == '\\' )      out += "\\\\";
		else if ( c == '|' )  out += "\\|";
		else if ( c == '\n' ) out += "\\n";
		else if ( c == '\r' ) out += "\\r";
		else                  out += c;
	}
	return out;
}

std::string UnescapeFromMarker( const std::string& s )
{
	std::string out;
	out.reserve( s.size() );
	for ( size_t i = 0; i < s.size(); i++ )
	{
		if ( s[i] == '\\' && i + 1 < s.size() )
		{
			char n = s[i + 1];
			if ( n == '\\' )      { out += '\\'; i++; }
			else if ( n == '|' )  { out += '|';  i++; }
			else if ( n == 'n' )  { out += '\n'; i++; }
			else if ( n == 'r' )  { out += '\r'; i++; }
			else                  { out += s[i]; }
		}
		else
		{
			out += s[i];
		}
	}
	return out;
}

// SteamID64 → SteamID32 (account id, lower 32 bits).
uint32_t SteamId64To32( uint64_t id64 )
{
	return (uint32_t)( id64 - 76561197960265728ULL );
}

bool LowerEquals( const std::string& a, const std::string& b )
{
	if ( a.size() != b.size() ) return false;
	for ( size_t i = 0; i < a.size(); i++ )
		if ( std::tolower( (unsigned char)a[i] ) != std::tolower( (unsigned char)b[i] ) )
			return false;
	return true;
}

bool LaunchArgsContainsToken( const std::string& args, const std::string& token )
{
	// Token может быть "-novid", "-w", "+fps_max" — ищем как whole token
	// (предшествует пробел/начало строки, после — пробел/конец/equal).
	size_t pos = 0;
	while ( ( pos = args.find( token, pos ) ) != std::string::npos )
	{
		bool leftOk  = ( pos == 0 ) || std::isspace( (unsigned char)args[pos - 1] );
		size_t end   = pos + token.size();
		bool rightOk = ( end == args.size() ) || std::isspace( (unsigned char)args[end] );
		if ( leftOk && rightOk )
			return true;
		pos++;
	}
	return false;
}

} // anonymous namespace

void DotaMinifier::SetConfig( const MinifierConfig& cfg )
{
	std::lock_guard<std::mutex> lk( m_mutex );
	m_cfg = cfg;
}

void DotaMinifier::Log( const char* fmt, ... )
{
	if ( !m_logFn ) return;
	char buf[1024];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	m_logFn( m_logCtx, buf );
}

std::string DotaMinifier::MinifierBackupRoot() const
{
	return MINIFIER_BACKUP_ROOT;
}

std::string DotaMinifier::GetBackupDir( int botIdx ) const
{
	char buf[MAX_PATH];
	snprintf( buf, sizeof( buf ), "%s\\%d", MINIFIER_BACKUP_ROOT, botIdx );
	return buf;
}

std::string DotaMinifier::GenerateAutoexec() const
{
	std::ostringstream ss;
	ss << "// === DotaFarm minifier autoexec — auto-generated, DO NOT EDIT ===\n";
	ss << "// Removed automatically when farm stops.\n";
	ss << "// AGGRESSIVE preset — visual quality sacrificed for max RAM/GPU savings.\n";
	ss << "// AI работает server-side, рендер для нас — overhead.\n\n";

	ss << "// ── FPS / threading ───────────────────────────────────────\n";
	ss << "fps_max " << m_cfg.fpsMax << "\n";
	ss << "fps_max_ui " << m_cfg.fpsMax << "\n";
	ss << "engine_no_focus_sleep 10000\n";  // 2026-05-22: было 250ms → 10s; ботам фокус не нужен
	ss << "mat_queue_mode 0\n";            // single-threaded render (меньше RAM)
	ss << "r_threaded_renderables 0\n";
	ss << "cl_forcepreload 0\n";

	ss << "\n// ── Panorama UI throttle (2026-05-22 from low-spec-researcher) ─\n";
	ss << "// HUD/dashboard рендерится непрерывно даже при статичной камере.\n";
	ss << "// На WARP это самый большой потенциальный win — UI thread на CPU.\n";
	ss << "@panorama_max_fps " << m_cfg.fpsMax << "\n";
	ss << "@panorama_max_overlay_fps 10\n";
	ss << "@panorama_vsync 0\n";

	ss << "\n// ── Threading distribution (4 vCPU дедик) ─────────────────\n";
	ss << "cl_threaded_bone_setup 1\n";
	ss << "r_threaded_particles 1\n";
	ss << "r_threadeddetailprops 1\n";
	ss << "vphysics_threadmode 1\n";
	ss << "cl_threaded_init 1\n";
	ss << "sv_threaded_init 1\n";
	ss << "snd_async_fullyasync 1\n";
	ss << "host_threaded_sound 1\n";

	ss << "\n// ── Particles / VFX ───────────────────────────────────────\n";
	ss << "r_drawparticles 0\n";
	ss << "cl_particle_fallback_base 4\n";
	ss << "cl_particle_fallback_multiplier 0\n";
	ss << "cl_particle_max_count 0\n";
	ss << "cl_particles_show_bbox 0\n";

	ss << "\n// ── Render scale / textures ───────────────────────────────\n";
	ss << "mat_viewportscale 0.20\n";       // 2026-05-22: 0.25 → 0.20, ~1/25 пикселей
	ss << "mat_picmip 4\n";                 // макс mipmap reduction (текстуры мутные)
	ss << "r_lod 4\n";                      // level-of-detail максимально дальний
	ss << "mat_filtertextures 0\n";         // no anisotropic
	ss << "mat_filterlightmaps 0\n";
	ss << "mat_specular 0\n";
	ss << "mat_bumpmap 0\n";
	ss << "mat_phong 0\n";
	ss << "r_dota_normal_maps 0\n";

	ss << "\n// ── Lighting / shadows / post ─────────────────────────────\n";
	ss << "cl_globallight_shadow_mode 0\n";
	ss << "r_shadowrendertotexture 0\n";
	ss << "r_shadowmaxrendered 0\n";
	ss << "r_flashlightdepthtexture 0\n";
	ss << "r_dynamic 0\n";
	ss << "r_3dsky 0\n";
	ss << "r_drawskybox 0\n";
	ss << "r_deferred_height_fog 0\n";
	ss << "r_deferred_simple_light 0\n";
	ss << "r_deferred_specular 0\n";
	ss << "r_deferred_specular_bloom 0\n";
	ss << "r_ssao 0\n";
	ss << "r_dota_fxaa 0\n";
	ss << "mat_postprocess_enable 0\n";
	ss << "mat_motion_blur_enabled 0\n";
	ss << "mat_motion_blur_forward_enabled 0\n";
	ss << "r_renderoverlayfragment 0\n";

	ss << "\n// ── Decals / blood / UI atmosphere ────────────────────────\n";
	ss << "r_decals 0\n";
	ss << "mp_decals 0\n";
	ss << "r_drawdecals 0\n";
	ss << "violence_ablood 0\n";
	ss << "violence_hblood 0\n";

	ss << "\n// ── Water / environment ───────────────────────────────────\n";
	ss << "dota_cheap_water 1\n";
	ss << "r_grass_quality 0\n";
	ss << "dota_ambient_creatures 0\n";
	ss << "dota_ambient_cloth 0\n";
	ss << "r_dota_allow_wind_on_trees 0\n";
	ss << "r_dashboard_render_quality 0\n";
	ss << "dota_portrait_animate 0\n";
	ss << "dota_no_minimap_creep_movement 1\n";

	ss << "\n// ── Viewmodel / HUD overhead ──────────────────────────────\n";
	ss << "r_drawviewmodel 0\n";
	ss << "dota_unit_use_player_color 0\n";
	ss << "dota_camera_smooth_count 0\n";
	ss << "dota_minimap_misclick_time 0\n";
	ss << "dota_screen_shake 0\n";
	ss << "dota_disable_range_finder 1\n";

	ss << "\n// ── Sound / voice ─────────────────────────────────────────\n";
	ss << "volume 0\n";
	ss << "snd_musicvolume 0\n";
	ss << "snd_mute_losefocus 1\n";
	ss << "voice_enable 0\n";
	ss << "snd_show 0\n";
	ss << "dota_speech_level 0\n";

	ss << "\n// ── Network ───────────────────────────────────────────────\n";
	ss << "rate 80000\n";
	ss << "cl_updaterate 30\n";
	ss << "cl_cmdrate 30\n";
	ss << "cl_interp 0\n";
	ss << "cl_smoothtime 0\n";

	return ss.str();
}

std::string DotaMinifier::GenerateVideoTxt( int width, int height ) const
{
	std::ostringstream ss;
	ss << "\"video.cfg\"\n";
	ss << "{\n";
	auto kv = [&]( const char* k, const char* v )
	{
		ss << "\t\"" << k << "\"\t\"" << v << "\"\n";
	};
	// AGGRESSIVE preset — все levels на минимум.
	kv( "setting.cpu_level",                       "0" );   // 0 (low) вместо 1
	kv( "setting.mem_level",                       "0" );
	kv( "setting.gpu_level",                       "0" );
	kv( "setting.gpu_mem_level",                   "0" );
	kv( "setting.shaderquality",                   "0" );
	kv( "setting.mat_vsync",                       "0" );
	kv( "setting.mat_triplebuffered",              "0" );
	kv( "setting.dota_cheap_water",                "1" );
	kv( "setting.r_deferred_height_fog",           "0" );
	kv( "setting.r_deferred_simple_light",         "0" );
	kv( "setting.r_ssao",                          "0" );
	kv( "setting.r_dota_fxaa",                     "0" );
	kv( "setting.r_deferred_specular",             "0" );
	kv( "setting.r_deferred_specular_bloom",       "0" );
	kv( "setting.dota_portrait_animate",           "0" );
	kv( "setting.r_dota_normal_maps",              "0" );
	kv( "setting.r_texture_stream_mip_bias",       "4" );   // макс 4 (был 2)
	kv( "setting.mat_picmip",                      "4" );   // макс blur
	kv( "setting.r_grass_quality",                 "0" );
	kv( "setting.dota_ambient_creatures",          "0" );
	kv( "setting.dota_ambient_cloth",              "0" );
	kv( "setting.r_dota_allow_wind_on_trees",      "0" );
	kv( "setting.r_dashboard_render_quality",      "0" );
	kv( "setting.cl_particle_fallback_base",       "4" );
	kv( "setting.cl_particle_fallback_multiplier", "0" );
	kv( "setting.r_drawparticles",                 "0" );
	kv( "setting.mat_viewportscale",               "0.20" ); // 2026-05-22: 0.25 → 0.20
	kv( "setting.mat_postprocess_enable",          "0" );
	kv( "setting.mat_motion_blur_enabled",         "0" );
	kv( "setting.dota_screen_shake",               "0" );
	kv( "setting.r_3dsky",                         "0" );
	kv( "setting.r_drawskybox",                    "0" );
	kv( "setting.r_dynamic",                       "0" );
	kv( "setting.r_shadowrendertotexture",         "0" );
	kv( "setting.r_shadowmaxrendered",             "0" );
	kv( "setting.r_decals",                        "0" );
	kv( "setting.fullscreen",                      "0" );

	// ── 2026-05-22: дубли autoexec cvars в video.txt для cold-start ──
	// video.txt применяется ДО autoexec → эти значения активны с самого
	// старта рендера. Иначе первые кадры до autoexec exec'а идут на
	// default (высоких) settings = lag spike при загрузке матча.
	kv( "setting.mat_queue_mode",                  "0" );
	kv( "setting.r_dota_normal_maps",              "0" );  // дубль (был выше тоже)
	kv( "setting.r_shadowrendertotexture",         "0" );
	kv( "setting.r_shadowmaxrendered",             "0" );
	kv( "setting.r_threaded_renderables",          "0" );
	kv( "setting.cl_globallight_shadow_mode",      "0" );
	kv( "setting.mat_specular",                    "0" );
	kv( "setting.mat_bumpmap",                     "0" );
	kv( "setting.mat_phong",                       "0" );
	kv( "setting.mat_filtertextures",              "0" );
	kv( "setting.mat_filterlightmaps",             "0" );
	kv( "setting.r_drawdecals",                    "0" );
	kv( "setting.dota_ambient_creatures",          "0" );  // дубль
	kv( "setting.dota_ambient_cloth",              "0" );  // дубль
	kv( "setting.r_dota_allow_wind_on_trees",      "0" );  // дубль
	kv( "setting.r_renderoverlayfragment",         "0" );

	char widthBuf[32], heightBuf[32];
	snprintf( widthBuf,  sizeof( widthBuf ),  "%d", width );
	snprintf( heightBuf, sizeof( heightBuf ), "%d", height );
	kv( "setting.defaultres",       widthBuf );
	kv( "setting.defaultresheight", heightBuf );
	kv( "setting.aspectratiomode",  "0" );
	kv( "setting.nowindowborder",   "1" );   // borderless = no chrome overhead

	ss << "}\n";
	return ss.str();
}

std::string DotaMinifier::ResolveAutoexecPath( int botIdx, const FarmConfig& farmCfg ) const
{
	std::string dotaInstallDir = bot_dota_dir::FindDotaInstallDir( farmCfg.steamExe );
	if ( dotaInstallDir.empty() )
		return "";
	std::string botDir = bot_dota_dir::GetBotDotaDir( botIdx, dotaInstallDir );
	if ( botDir.empty() ) botDir = dotaInstallDir; // fallback to main install
	return botDir + "\\game\\dota\\cfg\\autoexec.cfg";
}

std::string DotaMinifier::ResolveVideoTxtPath( int botIdx, uint64_t steamId,
	const FarmConfig& farmCfg ) const
{
	if ( steamId == 0 ) return "";

	uint32_t id32 = SteamId64To32( steamId );
	char id32Buf[32];
	snprintf( id32Buf, sizeof( id32Buf ), "%u", id32 );

	// Check per-bot Steam first (C:\BotSteam\<idx>\userdata\<id>\570\local\cfg)
	char botSteam[MAX_PATH];
	snprintf( botSteam, sizeof( botSteam ),
		"C:\\BotSteam\\%d\\userdata\\%s\\570\\local\\cfg\\video.txt",
		botIdx, id32Buf );
	std::string botDirOnly( botSteam );
	botDirOnly = botDirOnly.substr( 0, botDirOnly.find_last_of( "\\/" ) );
	std::string botSteamRoot = std::string( "C:\\BotSteam\\" )
		+ std::to_string( botIdx ) + "\\userdata";
	if ( GetFileAttributesA( botSteamRoot.c_str() ) != INVALID_FILE_ATTRIBUTES )
		return botSteam;

	// Fallback to main Steam install
	std::string steamDir = farmCfg.steamExe;
	auto lastSlash = steamDir.find_last_of( "\\/" );
	if ( lastSlash != std::string::npos )
		steamDir = steamDir.substr( 0, lastSlash );

	char videoPath[MAX_PATH];
	snprintf( videoPath, sizeof( videoPath ),
		"%s\\userdata\\%s\\570\\local\\cfg\\video.txt",
		steamDir.c_str(), id32Buf );
	return videoPath;
}

bool DotaMinifier::BackupFile( const std::string& path, MinifierBackupState& bs )
{
	if ( path.empty() ) return false;

	std::string content;
	if ( !ReadFileToString( path, content ) )
	{
		// Файла не было — помечаем как MISSING чтобы revert удалил наш created file.
		bs.savedFiles[path] = FILE_MISSING_MARKER;
	}
	else
	{
		bs.savedFiles[path] = content;
	}
	return true;
}

bool DotaMinifier::WriteMarker( const MinifierBackupState& bs )
{
	std::string dir = GetBackupDir( bs.botIdx );
	if ( !MakeDirRecursive( dir ) ) return false;

	std::string marker = dir + "\\.applied";
	std::ofstream f( marker, std::ios::binary | std::ios::trunc );
	if ( !f.is_open() ) return false;

	// Format: один файл на строку:
	//   v1|<botIdx>|<steamId>|<backedUpAtMs>|<originalLaunchArgs_escaped>
	//   F|<path_escaped>|<content_escaped>
	//   F|<path_escaped>|<content_escaped>
	f << "v1|" << bs.botIdx << "|" << bs.steamId << "|" << bs.backedUpAtMs
	  << "|" << EscapeForMarker( bs.originalLaunchArgs ) << "\n";
	for ( const auto& [path, content] : bs.savedFiles )
	{
		f << "F|" << EscapeForMarker( NormalizePathForStorage( path ) )
		  << "|" << EscapeForMarker( content ) << "\n";
	}
	return f.good();
}

bool DotaMinifier::LoadMarker( int botIdx, MinifierBackupState& bs )
{
	std::string marker = GetBackupDir( botIdx ) + "\\.applied";
	std::ifstream f( marker, std::ios::binary );
	if ( !f.is_open() ) return false;

	std::string line;
	bool gotHeader = false;

	while ( std::getline( f, line ) )
	{
		// strip trailing \r
		if ( !line.empty() && line.back() == '\r' ) line.pop_back();
		if ( line.empty() ) continue;

		// Split by unescaped '|'
		std::vector<std::string> parts;
		std::string cur;
		for ( size_t i = 0; i < line.size(); i++ )
		{
			if ( line[i] == '\\' && i + 1 < line.size() )
			{
				cur += line[i];
				cur += line[i + 1];
				i++;
			}
			else if ( line[i] == '|' )
			{
				parts.push_back( cur );
				cur.clear();
			}
			else
			{
				cur += line[i];
			}
		}
		parts.push_back( cur );

		if ( !gotHeader )
		{
			if ( parts.size() < 5 || parts[0] != "v1" )
				return false;
			bs.botIdx              = std::atoi( parts[1].c_str() );
			bs.steamId             = std::strtoull( parts[2].c_str(), nullptr, 10 );
			bs.backedUpAtMs        = std::strtoll( parts[3].c_str(), nullptr, 10 );
			bs.originalLaunchArgs  = UnescapeFromMarker( parts[4] );
			gotHeader = true;
		}
		else if ( parts.size() >= 3 && parts[0] == "F" )
		{
			std::string path    = UnescapeFromMarker( parts[1] );
			std::string content = UnescapeFromMarker( parts[2] );
			bs.savedFiles[path] = content;
		}
	}

	bs.valid = gotHeader;
	return gotHeader;
}

bool DotaMinifier::DeleteMarker( int botIdx )
{
	std::string marker = GetBackupDir( botIdx ) + "\\.applied";
	if ( GetFileAttributesA( marker.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return true; // already gone
	return DeleteFileA( marker.c_str() ) != 0;
}

bool DotaMinifier::RestoreFiles( const MinifierBackupState& bs )
{
	bool allOk = true;
	for ( const auto& [path, content] : bs.savedFiles )
	{
		if ( content == FILE_MISSING_MARKER )
		{
			// 2026-05-16: ДО 3 retry с 200мс delay. Sharing violation бывает когда
			// main Dota процесс юзера держит autoexec.cfg open (junction к bot path
			// шарит inode с main Dota install). Если игра как раз закрывается —
			// retry даст шанс файлу освободиться. После retries проверяем что файла
			// реально нет — иначе log как failed.
			bool deleted = false;
			for ( int attempt = 0; attempt < 3; ++attempt )
			{
				DWORD attrs = GetFileAttributesA( path.c_str() );
				if ( attrs == INVALID_FILE_ATTRIBUTES )
				{
					deleted = true;
					break;
				}
				if ( attrs & FILE_ATTRIBUTE_READONLY )
					SetFileAttributesA( path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY );
				if ( DeleteFileA( path.c_str() ) )
				{
					deleted = true;
					break;
				}
				DWORD err = GetLastError();
				Log( "[minifier] revert: DeleteFile attempt=%d failed: %s (err=%lu) — retry in 200ms",
					attempt + 1, path.c_str(), err );
				Sleep( 200 );
			}
			if ( !deleted )
			{
				Log( "[minifier] revert: DeleteFile GIVE UP after 3 retries: %s — "
					"file likely held by main Dota; close Dota и удали вручную, "
					"либо запусти Stop Farm повторно",
					path.c_str() );
				allOk = false;
			}
		}
		else
		{
			bool written = false;
			for ( int attempt = 0; attempt < 3; ++attempt )
			{
				if ( WriteStringToFile( path, content ) )
				{
					written = true;
					break;
				}
				Log( "[minifier] revert: WriteFile attempt=%d failed: %s (err=%lu) — retry in 200ms",
					attempt + 1, path.c_str(), GetLastError() );
				Sleep( 200 );
			}
			if ( !written ) allOk = false;
		}
	}
	return allOk;
}

bool DotaMinifier::ApplyToBot( int botIdx, uint64_t steamId, const FarmConfig& farmCfg )
{
	std::lock_guard<std::mutex> lk( m_mutex );

	if ( !m_cfg.enabled )
		return true; // no-op

	auto it = m_backups.find( botIdx );
	if ( it != m_backups.end() && it->second.valid )
	{
		Log( "[minifier] #%d already applied — skipping", botIdx );
		return true;
	}

	MinifierBackupState bs;
	bs.botIdx       = botIdx;
	bs.steamId      = steamId;
	bs.backedUpAtMs = NowMs();

	std::string autoexecPath, videoTxtPath;

	if ( m_cfg.applyAutoexec )
	{
		autoexecPath = ResolveAutoexecPath( botIdx, farmCfg );
		if ( autoexecPath.empty() )
		{
			Log( "[minifier] #%d: ResolveAutoexecPath failed", botIdx );
			return false;
		}
		if ( !BackupFile( autoexecPath, bs ) )
		{
			Log( "[minifier] #%d: backup autoexec.cfg failed", botIdx );
			return false;
		}
	}

	if ( m_cfg.applyVideoTxt )
	{
		videoTxtPath = ResolveVideoTxtPath( botIdx, steamId, farmCfg );
		if ( videoTxtPath.empty() )
		{
			Log( "[minifier] #%d: video.txt path unavailable (steamId=0?) — skipping",
				botIdx );
		}
		else if ( !BackupFile( videoTxtPath, bs ) )
		{
			Log( "[minifier] #%d: backup video.txt failed", botIdx );
			return false;
		}
	}

	// Marker FIRST — если crash после write_marker до apply, next start
	// увидит marker, попытается revert (no-op для unmodified files,
	// безопасно).
	if ( !WriteMarker( bs ) )
	{
		Log( "[minifier] #%d: WriteMarker failed", botIdx );
		return false;
	}

	// Apply minified content
	if ( m_cfg.applyAutoexec && !autoexecPath.empty() )
	{
		if ( !WriteStringToFile( autoexecPath, GenerateAutoexec() ) )
		{
			Log( "[minifier] #%d: write autoexec.cfg failed", botIdx );
			RestoreFiles( bs );
			DeleteMarker( botIdx );
			return false;
		}
	}

	if ( m_cfg.applyVideoTxt && !videoTxtPath.empty() )
	{
		if ( !WriteStringToFile( videoTxtPath, GenerateVideoTxt(
				m_cfg.resolutionWidth, m_cfg.resolutionHeight ) ) )
		{
			Log( "[minifier] #%d: write video.txt failed", botIdx );
			RestoreFiles( bs );
			DeleteMarker( botIdx );
			return false;
		}
	}

	bs.valid = true;
	m_backups[botIdx] = bs;

	// Verify written files (диагностика когда юзер говорит «не вижу эффекта»).
	auto fileSize = []( const std::string& p ) -> int64_t
	{
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if ( !GetFileAttributesExA( p.c_str(), GetFileExInfoStandard, &fad ) )
			return -1;
		return ( (int64_t)fad.nFileSizeHigh << 32 ) | fad.nFileSizeLow;
	};
	if ( m_cfg.applyAutoexec && !autoexecPath.empty() )
		Log( "[minifier] #%d: autoexec.cfg → %s (%lld bytes)",
			botIdx, autoexecPath.c_str(), fileSize( autoexecPath ) );
	if ( m_cfg.applyVideoTxt && !videoTxtPath.empty() )
		Log( "[minifier] #%d: video.txt → %s (%lld bytes)",
			botIdx, videoTxtPath.c_str(), fileSize( videoTxtPath ) );

	Log( "[minifier] #%d: applied (autoexec=%d video=%d files=%zu)",
		botIdx, (int)m_cfg.applyAutoexec, (int)m_cfg.applyVideoTxt,
		bs.savedFiles.size() );
	return true;
}

bool DotaMinifier::RevertBot( int botIdx )
{
	std::lock_guard<std::mutex> lk( m_mutex );

	auto it = m_backups.find( botIdx );
	if ( it == m_backups.end() || !it->second.valid )
		return true; // nothing to do

	bool ok = RestoreFiles( it->second );
	// 2026-05-16: Marker удаляем ТОЛЬКО при успешном RestoreFiles. Раньше marker
	// удалялся безусловно — если DeleteFile autoexec.cfg fail'ил (sharing violation
	// с main Dota), revert считался завершённым, на next start RevertStale не
	// видел marker и не пытался повторить. Юзер оставался с minified конфигом в
	// main Dota install (junction shares inode). Теперь: marker сохраняется,
	// RevertStale() на следующем Init подберёт и попробует ещё.
	if ( ok )
	{
		DeleteMarker( botIdx );
		m_backups.erase( it );
		Log( "[minifier] #%d: reverted ok", botIdx );
	}
	else
	{
		// In-memory state очищаем чтобы StopFarm не зацикливал retry в текущей
		// сессии (мы уже 3× попытались внутри RestoreFiles). Disk marker остаётся
		// для следующего запуска.
		m_backups.erase( it );
		Log( "[minifier] #%d: revert PARTIAL — marker kept on disk for next start",
			botIdx );
	}
	return ok;
}

bool DotaMinifier::RevertAll()
{
	bool allOk = true;
	std::vector<int> idxs;
	{
		std::lock_guard<std::mutex> lk( m_mutex );
		for ( const auto& [idx, bs] : m_backups )
			idxs.push_back( idx );
	}
	for ( int idx : idxs )
	{
		if ( !RevertBot( idx ) )
			allOk = false;
	}
	return allOk;
}

bool DotaMinifier::IsAppliedToAnyBot() const
{
	std::lock_guard<std::mutex> lk( m_mutex );
	if ( !m_backups.empty() ) return true;
	// Также учитываем VPK marker — apply мог быть session-wide без per-bot.
	std::string vpkMarker = GetVpkMarkerPath();
	if ( GetFileAttributesA( vpkMarker.c_str() ) != INVALID_FILE_ATTRIBUTES )
		return true;
	return false;
}

int DotaMinifier::DetectStaleBackups()
{
	std::string root = MinifierBackupRoot();
	if ( GetFileAttributesA( root.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return 0;

	int found = 0;
	std::string searchPattern = root + "\\*";
	WIN32_FIND_DATAA fd{};
	HANDLE h = FindFirstFileA( searchPattern.c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE )
		return 0;

	do
	{
		if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) continue;
		if ( fd.cFileName[0] == '.' ) continue;

		// Парсим botIdx из имени директории
		bool allDigits = ( fd.cFileName[0] != 0 );
		for ( const char* p = fd.cFileName; *p; p++ )
		{
			if ( !isdigit( (unsigned char)*p ) ) { allDigits = false; break; }
		}
		if ( !allDigits ) continue;

		std::string marker = root + "\\" + fd.cFileName + "\\.applied";
		if ( GetFileAttributesA( marker.c_str() ) != INVALID_FILE_ATTRIBUTES )
			found++;
	}
	while ( FindNextFileA( h, &fd ) );
	FindClose( h );

	return found;
}

bool DotaMinifier::RevertStale()
{
	std::string root = MinifierBackupRoot();
	if ( GetFileAttributesA( root.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return true;

	bool allOk = true;
	std::string searchPattern = root + "\\*";
	WIN32_FIND_DATAA fd{};
	HANDLE h = FindFirstFileA( searchPattern.c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE )
		return true;

	std::vector<int> staleIdxs;
	do
	{
		if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) continue;
		if ( fd.cFileName[0] == '.' ) continue;
		bool allDigits = ( fd.cFileName[0] != 0 );
		for ( const char* p = fd.cFileName; *p; p++ )
		{
			if ( !isdigit( (unsigned char)*p ) ) { allDigits = false; break; }
		}
		if ( !allDigits ) continue;

		std::string marker = std::string( root ) + "\\" + fd.cFileName + "\\.applied";
		if ( GetFileAttributesA( marker.c_str() ) != INVALID_FILE_ATTRIBUTES )
			staleIdxs.push_back( atoi( fd.cFileName ) );
	}
	while ( FindNextFileA( h, &fd ) );
	FindClose( h );

	for ( int idx : staleIdxs )
	{
		MinifierBackupState bs;
		if ( !LoadMarker( idx, bs ) )
		{
			Log( "[minifier] stale: LoadMarker #%d failed — deleting orphan marker", idx );
			DeleteMarker( idx );
			continue;
		}
		Log( "[minifier] stale: reverting #%d (backedUpAt=%lld files=%zu)",
			idx, (long long)bs.backedUpAtMs, bs.savedFiles.size() );
		bool ok = RestoreFiles( bs );
		DeleteMarker( idx );
		if ( !ok ) allOk = false;
	}
	return allOk;
}

// === Bundle G2: VPK patching через Python wrapper ============================

std::string DotaMinifier::GetVpkMarkerDir() const
{
	return std::string( MINIFIER_BACKUP_ROOT ) + "\\vpk";
}

std::string DotaMinifier::GetVpkMarkerPath() const
{
	return GetVpkMarkerDir() + "\\.applied";
}

bool DotaMinifier::JsonHasOkTrue( const std::string& json )
{
	// Минимальный лексический поиск без full JSON parse:
	//   "ok": true
	//   "ok":true
	//   "ok"  :  true
	size_t pos = 0;
	while ( ( pos = json.find( "\"ok\"", pos ) ) != std::string::npos )
	{
		size_t i = pos + 4;
		while ( i < json.size() && std::isspace( (unsigned char)json[i] ) ) i++;
		if ( i < json.size() && json[i] == ':' )
		{
			i++;
			while ( i < json.size() && std::isspace( (unsigned char)json[i] ) ) i++;
			if ( json.compare( i, 4, "true" ) == 0 )
				return true;
			return false;
		}
		pos++;
	}
	return false;
}

bool DotaMinifier::RunPython( const std::string& cmdLineArgs, std::string& outStdout,
	std::string& outStderr, int& outExitCode )
{
	outStdout.clear();
	outStderr.clear();
	outExitCode = -1;

	// Bundle H portable: пробуем standalone PyInstaller .exe сначала
	// (zero-install для прода). Fallback на pythonExe + wrapperScript для dev-машины.
	std::ostringstream cmd;
	bool useExe = !m_cfg.wrapperExe.empty()
		&& GetFileAttributesA( m_cfg.wrapperExe.c_str() ) != INVALID_FILE_ATTRIBUTES;
	if ( useExe )
	{
		cmd << "\"" << m_cfg.wrapperExe << "\" " << cmdLineArgs;
	}
	else
	{
		if ( m_cfg.pythonExe.empty() || m_cfg.wrapperScript.empty() )
		{
			Log( "[minifier] no wrapperExe (%s), no python fallback (pythonExe='%s' "
				"wrapperScript='%s') — VPK disabled",
				m_cfg.wrapperExe.c_str(), m_cfg.pythonExe.c_str(),
				m_cfg.wrapperScript.c_str() );
			return false;
		}
		cmd << "\"" << m_cfg.pythonExe << "\" \"" << m_cfg.wrapperScript << "\" "
			<< cmdLineArgs;
	}
	std::string cmdLine = cmd.str();
	Log( "[minifier] running (%s): %s", useExe ? "exe" : "py", cmdLine.c_str() );

	// Pipes для stdout / stderr. Inherit handles только write-side.
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof( sa );
	sa.bInheritHandle = TRUE;

	HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
	if ( !CreatePipe( &outR, &outW, &sa, 0 ) )
	{
		Log( "[minifier] CreatePipe stdout failed: err=%lu", GetLastError() );
		return false;
	}
	if ( !CreatePipe( &errR, &errW, &sa, 0 ) )
	{
		Log( "[minifier] CreatePipe stderr failed: err=%lu", GetLastError() );
		CloseHandle( outR ); CloseHandle( outW );
		return false;
	}
	SetHandleInformation( outR, HANDLE_FLAG_INHERIT, 0 );
	SetHandleInformation( errR, HANDLE_FLAG_INHERIT, 0 );

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdOutput = outW;
	si.hStdError  = errW;
	si.hStdInput  = nullptr;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi{};
	std::vector<char> mutCmd( cmdLine.begin(), cmdLine.end() );
	mutCmd.push_back( '\0' );

	// Portable vendor lookup: PyInstaller bundle резолвит __file__ внутри _MEI
	// temp dir, не туда где лежит .exe — wrapper.py не найдёт `dota2_minify/`.
	// Передаём абсолютный путь через env var DOTA2_MINIFY_VENDOR (wrapper читает
	// её первым приоритетом до auto-detect через __file__).
	// Vendor лежит рядом с wrapperExe: <dirname(wrapperExe)>/dota2_minify/.
	std::string vendorDir;
	if ( !m_cfg.wrapperExe.empty() )
	{
		std::string dir = m_cfg.wrapperExe;
		auto pos = dir.find_last_of( "\\/" );
		if ( pos != std::string::npos )
			dir.resize( pos );
		vendorDir = dir + "\\dota2_minify";
	}
	if ( !vendorDir.empty() )
		SetEnvironmentVariableA( "DOTA2_MINIFY_VENDOR", vendorDir.c_str() );

	BOOL ok = CreateProcessA(
		nullptr, mutCmd.data(), nullptr, nullptr,
		TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi );
	CloseHandle( outW );
	CloseHandle( errW );

	if ( !ok )
	{
		Log( "[minifier] CreateProcess failed: err=%lu", GetLastError() );
		CloseHandle( outR ); CloseHandle( errR );
		return false;
	}

	// Drain pipes пока процесс жив. Чередуем чтение из stdout и stderr.
	auto drain = []( HANDLE h, std::string& dst ) -> bool
	{
		char buf[4096];
		DWORD avail = 0;
		BOOL peeked = PeekNamedPipe( h, nullptr, 0, nullptr, &avail, nullptr );
		if ( !peeked || avail == 0 ) return false;
		DWORD read = 0;
		if ( !ReadFile( h, buf, (DWORD)std::min<size_t>( avail, sizeof( buf ) ),
				&read, nullptr ) )
			return false;
		dst.append( buf, read );
		return read > 0;
	};

	DWORD waitMs = (DWORD)m_cfg.vpkSubprocessTimeoutMs;
	DWORD start = GetTickCount();
	bool done = false;
	while ( !done )
	{
		DWORD waitRes = WaitForSingleObject( pi.hProcess, 50 );
		drain( outR, outStdout );
		drain( errR, outStderr );
		if ( waitRes == WAIT_OBJECT_0 )
			done = true;
		else if ( ( GetTickCount() - start ) > waitMs )
		{
			Log( "[minifier] python timeout (%lu ms) — killing", waitMs );
			TerminateProcess( pi.hProcess, 99 );
			WaitForSingleObject( pi.hProcess, 2000 );
			done = true;
		}
	}
	// Final drain после exit (могут остаться буферизованные данные)
	while ( drain( outR, outStdout ) ) {}
	while ( drain( errR, outStderr ) ) {}

	DWORD exitCode = (DWORD)-1;
	GetExitCodeProcess( pi.hProcess, &exitCode );
	outExitCode = (int)exitCode;

	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	CloseHandle( outR );
	CloseHandle( errR );

	// Stderr — построчно в Log (полезно для диагностики)
	if ( !outStderr.empty() )
	{
		std::string buf;
		for ( char c : outStderr )
		{
			if ( c == '\n' )
			{
				if ( !buf.empty() ) Log( "[minifier:py] %s", buf.c_str() );
				buf.clear();
			}
			else if ( c != '\r' )
			{
				buf += c;
			}
		}
		if ( !buf.empty() ) Log( "[minifier:py] %s", buf.c_str() );
	}

	return outExitCode == 0;
}

bool DotaMinifier::ApplyVpkPatches()
{
	std::lock_guard<std::mutex> lk( m_mutex );

	// VPK_DISABLED 2026-05-17 — навсегда. Safety net на случай если remained
	// какой-то callsite в коде. Config parser форсит applyVpkPatches=false,
	// поэтому ранний выход тут — это уже двойная защита.
	Log( "[minifier] vpk: ApplyVpkPatches called but VPK is permanently disabled (kill-switch). "
		"Skipping. Используй autoexec/video.txt настройки + memreduct вместо VPK." );
	return true;  // no-op success

	// ── original code below kept dead for reference ──
	if ( !m_cfg.enabled || !m_cfg.applyVpkPatches )
		return true; // no-op

	// Marker FIRST — содержит preset для DetectStaleVpkPatches/RevertVpkPatches.
	std::string markerDir = GetVpkMarkerDir();
	if ( !MakeDirRecursive( markerDir ) )
	{
		Log( "[minifier] vpk: MakeDirRecursive failed: %s", markerDir.c_str() );
		return false;
	}

	std::string markerPath = GetVpkMarkerPath();
	{
		std::ofstream f( markerPath, std::ios::binary | std::ios::trunc );
		if ( !f.is_open() )
		{
			Log( "[minifier] vpk: marker write failed: %s", markerPath.c_str() );
			return false;
		}
		// Format: v1|<preset>|<appliedAtMs>|<fixLaunchOptions>
		f << "v1|" << EscapeForMarker( m_cfg.vpkPreset )
		  << "|" << NowMs() << "|" << ( m_cfg.vpkFixLaunchOptions ? "1" : "0" )
		  << "\n";
	}

	std::ostringstream args;
	args << "apply --preset \"" << m_cfg.vpkPreset << "\"";
	if ( m_cfg.vpkFixLaunchOptions )
		args << " --fix-launch-options";

	std::string out, err;
	int exitCode = -1;
	bool procOk = RunPython( args.str(), out, err, exitCode );

	if ( !procOk || !JsonHasOkTrue( out ) )
	{
		Log( "[minifier] vpk apply FAILED (exit=%d, ok-true=%d)",
			exitCode, (int)JsonHasOkTrue( out ) );
		// Удаляем marker — apply не состоялся
		DeleteFileA( markerPath.c_str() );
		return false;
	}

	Log( "[minifier] vpk apply OK (preset=%s)", m_cfg.vpkPreset.c_str() );
	return true;
}

bool DotaMinifier::RevertVpkPatches()
{
	std::lock_guard<std::mutex> lk( m_mutex );

	std::string markerPath = GetVpkMarkerPath();
	bool hadMarker = ( GetFileAttributesA( markerPath.c_str() ) != INVALID_FILE_ATTRIBUTES );

	// Even if marker missing — try revert (idempotent — wrapper удалит только
	// наши pak'и, foreign не тронет).
	if ( !m_cfg.enabled && !hadMarker )
		return true;

	// Парсим marker чтобы понять, был ли --fix-launch-options
	bool needCleanupOpts = m_cfg.vpkFixLaunchOptions;
	if ( hadMarker )
	{
		std::ifstream mf( markerPath, std::ios::binary );
		std::string line;
		if ( std::getline( mf, line ) )
		{
			// v1|<preset>|<ms>|<fixOpts>
			size_t p1 = line.find( '|' );
			size_t p2 = ( p1 != std::string::npos ) ? line.find( '|', p1 + 1 ) : std::string::npos;
			size_t p3 = ( p2 != std::string::npos ) ? line.find( '|', p2 + 1 ) : std::string::npos;
			if ( p3 != std::string::npos )
				needCleanupOpts = ( line.substr( p3 + 1, 1 ) == "1" );
		}
	}

	std::ostringstream args;
	args << "revert";
	if ( needCleanupOpts )
		args << " --cleanup-launch-options";

	std::string out, err;
	int exitCode = -1;
	bool procOk = RunPython( args.str(), out, err, exitCode );

	if ( !procOk || !JsonHasOkTrue( out ) )
	{
		Log( "[minifier] vpk revert FAILED (exit=%d) — marker NOT deleted, will retry next start",
			exitCode );
		return false;
	}

	// Marker LAST (если crash до DeleteFile — next start снова revert,
	// idempotent — wrapper не найдёт наши pak'и → no-op).
	if ( hadMarker )
		DeleteFileA( markerPath.c_str() );

	Log( "[minifier] vpk revert OK" );
	return true;
}

bool DotaMinifier::DetectStaleVpkPatches()
{
	std::string markerPath = GetVpkMarkerPath();
	bool present = ( GetFileAttributesA( markerPath.c_str() ) != INVALID_FILE_ATTRIBUTES );
	if ( present )
		Log( "[minifier] vpk: stale marker detected at %s", markerPath.c_str() );
	return present;
}

// === end Bundle G2 ===========================================================

std::string DotaMinifier::BuildLaunchArgs( int botIdx, const std::string& originalArgs ) const
{
	(void)botIdx;
	if ( !m_cfg.enabled || !m_cfg.applyLaunchOptions )
		return originalArgs;

	std::ostringstream extra;
	auto add = [&]( const char* token )
	{
		if ( !LaunchArgsContainsToken( originalArgs, token ) )
			extra << " " << token;
	};

	add( "-novid" );           // skip intro video
	add( "-nojoy" );           // no joystick init
	add( "-noaafonts" );       // no font anti-alias
	add( "-novr" );            // no VR support init
	add( "-no-browser" );      // no embedded chromium (saves ~150 MB RAM!)
	add( "-noborder" );        // borderless window
	add( "-low" );             // process priority low (yields CPU to other instances)
	add( "-map" );
	if ( !LaunchArgsContainsToken( originalArgs, "-map" ) )
		extra << " dota";
	// -dx9 НЕ добавляем — Source 2 не имеет DX9 backend, no-op флаг.
	add( "-threads" );
	if ( !LaunchArgsContainsToken( originalArgs, "-threads" ) )
		extra << " 2";

	// Resolution через cmd line — дублируем video.txt (на случай если video.txt
	// не загрузился, например первый запуск без userdata cfg).
	if ( !LaunchArgsContainsToken( originalArgs, "-w " ) )
		extra << " -w " << m_cfg.resolutionWidth;
	if ( !LaunchArgsContainsToken( originalArgs, "-h " ) )
		extra << " -h " << m_cfg.resolutionHeight;

	// fps_max в autoexec.cfg тоже — но дублируем через cmd line на случай если
	// autoexec не загрузился (новый аккаунт без cfg dir).
	if ( !LaunchArgsContainsToken( originalArgs, "+fps_max" ) )
		extra << " +fps_max " << m_cfg.fpsMax;

	// volume 0 в autoexec тоже — дубль через console exec.
	if ( !LaunchArgsContainsToken( originalArgs, "+volume" ) )
		extra << " +volume 0";

	return originalArgs + extra.str();
}
