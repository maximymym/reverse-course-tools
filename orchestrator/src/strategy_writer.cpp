#include "strategy_writer.h"

#include <Windows.h>
#include <json.hpp>
#include <fstream>
#include <sstream>

namespace StrategyWriter
{

using json = nlohmann::json;

static const char* kJsonPath    = "C:\\temp\\andromeda\\strategy.json";
static const char* kTxtPath     = "C:\\temp\\andromeda\\strategy.txt";
static const char* kTxtTmpPath  = "C:\\temp\\andromeda\\strategy.txt.tmp";
static const char* kJsonTmpPath = "C:\\temp\\andromeda\\strategy.json.tmp";

// ── RAII wrapper над LockFileEx ───────────────────────────────────────
// Нужен чтобы master и slave orchestrator (на single-machine setup) не
// затирали записи друг друга в strategy.json. Мы открываем lock-файл рядом с
// JSON (отдельный файл — потому что MoveFileExA в Write() заменит inode JSON'а
// и lock на старый handle станет бесполезен).
class FileLock
{
public:
	FileLock( const char* path )
	{
		m_h = CreateFileA( path, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( m_h == INVALID_HANDLE_VALUE ) return;

		OVERLAPPED ovl{};
		if ( !LockFileEx( m_h, LOCKFILE_EXCLUSIVE_LOCK, 0,
				MAXDWORD, MAXDWORD, &ovl ) )
		{
			CloseHandle( m_h );
			m_h = INVALID_HANDLE_VALUE;
		}
	}

	~FileLock()
	{
		if ( m_h != INVALID_HANDLE_VALUE )
		{
			OVERLAPPED ovl{};
			UnlockFileEx( m_h, 0, MAXDWORD, MAXDWORD, &ovl );
			CloseHandle( m_h );
		}
	}

	bool ok() const { return m_h != INVALID_HANDLE_VALUE; }

	FileLock( const FileLock& ) = delete;
	FileLock& operator=( const FileLock& ) = delete;

private:
	HANDLE m_h = INVALID_HANDLE_VALUE;
};

static bool WriteAtomic( const char* finalPath, const char* tmpPath, const std::string& body )
{
	{
		std::ofstream f( tmpPath, std::ios::binary | std::ios::trunc );
		if ( !f ) return false;
		f << body;
		f.flush();
		if ( !f ) return false;
	}
	if ( !MoveFileExA( tmpPath, finalPath, MOVEFILE_REPLACE_EXISTING ) )
	{
		DeleteFileA( tmpPath );
		return false;
	}
	return true;
}

static json LoadExistingJson()
{
	std::ifstream f( kJsonPath, std::ios::binary );
	if ( !f ) return json::object();

	std::ostringstream ss;
	ss << f.rdbuf();
	std::string content = ss.str();
	if ( content.empty() ) return json::object();

	try
	{
		json j = json::parse( content );
		if ( !j.is_object() ) return json::object();
		return j;
	}
	catch ( ... )
	{
		// corrupt → начинаем с чистой таблицы; наши pids в любом случае мерджим.
		return json::object();
	}
}

bool Write( const std::vector<DWORD>& pids, const std::string& strategy )
{
	if ( strategy.empty() ) return false;

	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

	// LockFileEx над выделенным lock-файлом — strategy.json меняем через
	// MoveFileExA (новый inode), поэтому лочить сам JSON бесполезно.
	FileLock lk( "C:\\temp\\andromeda\\strategy.lock" );
	if ( !lk.ok() ) return false;

	json j = LoadExistingJson();

	// Merge: ставим/перезаписываем СВОИ pids значением strategy. Чужие записи
	// не трогаем (разные orchestrator'ы — у каждого свои pids).
	for ( DWORD pid : pids )
	{
		char key[16];
		snprintf( key, sizeof( key ), "%lu", (unsigned long)pid );
		j[ key ] = strategy;
	}

	bool jsonOk = WriteAtomic( kJsonPath, kJsonTmpPath, j.dump() );

	// Backward-compat зеркало: одно слово в strategy.txt — для single-instance
	// setup где Lua-poller не имеет PID (старая DLL без env var / global).
	bool txtOk  = WriteAtomic( kTxtPath, kTxtTmpPath, strategy );

	return jsonOk && txtOk;
}

bool Write( const std::string& strategy )
{
	// Старый интерфейс: пишет только strategy.txt. Используется в callsites где
	// pids ещё не известны (например ранний bootstrap).
	if ( strategy.empty() ) return false;

	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

	return WriteAtomic( kTxtPath, kTxtTmpPath, strategy );
}

std::string Read()
{
	std::ifstream f( kTxtPath, std::ios::binary );
	if ( !f ) return {};
	std::ostringstream ss;
	ss << f.rdbuf();
	std::string s = ss.str();
	while ( !s.empty() && ( s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t' ) )
		s.pop_back();
	return s;
}

} // namespace StrategyWriter
