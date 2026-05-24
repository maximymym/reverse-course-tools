// DotaFarm Bootstrap — downloads latest DotaFarm package from server, extracts, runs.
// Splash window с анимированным progress bar'ом. UI живёт на главном потоке и
// постоянно pump'ит сообщения, тяжёлые операции (HTTP, zip extract) делает
// worker thread — поэтому окно никогда не "Не отвечает".
//
// Progress bar работает в двух режимах:
//   MARQUEE  — бегущая полоска, когда мы ждём неопределённое время (версия,
//              zip extract, launch) — Windows анимирует сам ~30 FPS.
//   SMOOTH   — процент закачки, когда считаем байты.

#include <Windows.h>
#include <winhttp.h>
#include <CommCtrl.h>
#include <Shlobj.h>
#include <atomic>
#include <mutex>
#include <string>
#include <fstream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")

// ── Config ──────────────────────────────────────────────────────
static constexpr const wchar_t* DOWNLOAD_HOST = L"v1per.tech";
static constexpr const wchar_t* VERSION_URL   = L"/dota/version.txt";
static constexpr const wchar_t* PACKAGE_URL   = L"/dota/DotaFarm.zip";
static constexpr const wchar_t* PRODUCT_NAME  = L"DotaFarm";
static constexpr int WND_W = 420;
static constexpr int WND_H = 130;

// ── Globals ─────────────────────────────────────────────────────
static HWND g_hWnd      = nullptr;
static HWND g_hStatus   = nullptr;
static HWND g_hProgress = nullptr;
static std::wstring g_installDir;

// Worker thread state — все члены читает UI thread раз в ~33мс, пишет worker.
struct WorkerState
{
	std::atomic<bool>        done{ false };
	std::atomic<bool>        error{ false };
	std::atomic<int>         pct{ 0 };        // 0..100 для MARQUEE тоже читаем, игнорим
	std::atomic<bool>        marquee{ true }; // true → ждём неопределённо, false → показываем pct
	std::mutex               mu;              // защищает status/errorMsg/exePath
	std::wstring             status;
	std::wstring             errorMsg;
	std::wstring             exePath;
	bool                     launchExe = false;
};
static WorkerState g_ws;

static void SetStatus( const wchar_t* text )
{
	std::lock_guard<std::mutex> lk( g_ws.mu );
	g_ws.status = text;
}

static void SetMarquee( bool on )
{
	g_ws.marquee.store( on );
}

static void SetPctProgress( int pct )
{
	g_ws.pct.store( pct );
}

// UI-тик: копирует снимок state'а в контролы окна. Вызывается из WM_TIMER.
static void RefreshUI()
{
	if ( !g_hStatus || !g_hProgress ) return;

	std::wstring statusCopy;
	{
		std::lock_guard<std::mutex> lk( g_ws.mu );
		statusCopy = g_ws.status;
	}

	// Status text
	wchar_t cur[512] = {};
	GetWindowTextW( g_hStatus, cur, 512 );
	if ( wcscmp( cur, statusCopy.c_str() ) != 0 )
		SetWindowTextW( g_hStatus, statusCopy.c_str() );

	// Progress bar mode
	static bool lastMarquee = true;
	bool marqueeNow = g_ws.marquee.load();
	if ( marqueeNow != lastMarquee )
	{
		lastMarquee = marqueeNow;
		if ( marqueeNow )
		{
			// Включаем marquee (Comctl32 v6: PBM_SETMARQUEE меняет стиль на лету).
			SendMessageW( g_hProgress, PBM_SETMARQUEE, TRUE, 30 );
		}
		else
		{
			// Выключаем marquee — возвращаемся в determinate mode.
			SendMessageW( g_hProgress, PBM_SETMARQUEE, FALSE, 0 );
		}
	}
	if ( !marqueeNow )
	{
		int pct = g_ws.pct.load();
		if ( pct < 0 ) pct = 0;
		if ( pct > 100 ) pct = 100;
		SendMessageW( g_hProgress, PBM_SETPOS, (WPARAM)pct, 0 );
	}
}

// ── Window ──────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc( HWND h, UINT m, WPARAM w, LPARAM l )
{
	if ( m == WM_DESTROY ) { PostQuitMessage( 0 ); return 0; }
	if ( m == WM_TIMER && w == 1 ) { RefreshUI(); return 0; }
	return DefWindowProcW( h, m, w, l );
}

static bool CreateSplash( HINSTANCE hInst )
{
	INITCOMMONCONTROLSEX icc{ sizeof( icc ), ICC_PROGRESS_CLASS };
	InitCommonControlsEx( &icc );

	WNDCLASSEXW wc{};
	wc.cbSize        = sizeof( wc );
	wc.lpfnWndProc   = WndProc;
	wc.hInstance      = hInst;
	wc.hCursor        = LoadCursorW( nullptr, IDC_APPSTARTING );
	wc.hbrBackground  = (HBRUSH)( COLOR_WINDOW + 1 );
	wc.lpszClassName  = L"DotaFarmBS";
	RegisterClassExW( &wc );

	int cx = GetSystemMetrics( SM_CXSCREEN );
	int cy = GetSystemMetrics( SM_CYSCREEN );

	g_hWnd = CreateWindowExW( WS_EX_TOPMOST, L"DotaFarmBS", L"DotaFarm Updater",
		WS_POPUP | WS_BORDER | WS_VISIBLE,
		( cx - WND_W ) / 2, ( cy - WND_H ) / 2, WND_W, WND_H,
		nullptr, nullptr, hInst, nullptr );
	if ( !g_hWnd ) return false;

	HFONT hFont = CreateFontW( 16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
		DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI" );

	g_hStatus = CreateWindowExW( 0, L"STATIC", L"",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		20, 20, WND_W - 40, 24, g_hWnd, nullptr, hInst, nullptr );
	SendMessageW( g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE );

	// PBS_SMOOTH — нужен для determinate-режима. MARQUEE включаем через
	// PBM_SETMARQUEE message динамически (Comctl32 v6).
	g_hProgress = CreateWindowExW( 0, PROGRESS_CLASSW, nullptr,
		WS_CHILD | WS_VISIBLE | PBS_SMOOTH | PBS_MARQUEE,
		20, 58, WND_W - 40, 22, g_hWnd, nullptr, hInst, nullptr );
	SendMessageW( g_hProgress, PBM_SETRANGE, 0, MAKELPARAM( 0, 100 ) );
	// Стартуем в marquee-режиме (ждём версию — она неопределённо долгая).
	SendMessageW( g_hProgress, PBM_SETMARQUEE, TRUE, 30 );

	// Тик refresh раз в ~33мс — 30 FPS. Marquee анимируется Windows'ом сам,
	// но status text мы обновляем только здесь.
	SetTimer( g_hWnd, 1, 33, nullptr );

	UpdateWindow( g_hWnd );
	return true;
}

// ── HTTP (WinHTTP + HTTPS) ──────────────────────────────────────

struct HttpSession
{
	HINTERNET hSession = nullptr;
	HINTERNET hConnect = nullptr;
	HINTERNET hRequest = nullptr;

	bool Open( const wchar_t* host, const wchar_t* path )
	{
		hSession = WinHttpOpen( L"DotaFarm/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
		if ( !hSession ) return false;

		DWORD timeout = 30000;
		WinHttpSetOption( hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof( timeout ) );
		WinHttpSetOption( hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof( timeout ) );

		hConnect = WinHttpConnect( hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0 );
		if ( !hConnect ) { Close(); return false; }

		hRequest = WinHttpOpenRequest( hConnect, L"GET", path,
			nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
		if ( !hRequest ) { Close(); return false; }

		DWORD flags = WINHTTP_DISABLE_COOKIES;
		WinHttpSetOption( hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof( flags ) );

		if ( !WinHttpSendRequest( hRequest,
			L"Cache-Control: no-cache\r\nPragma: no-cache", (DWORD)-1,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) )
		{ Close(); return false; }

		if ( !WinHttpReceiveResponse( hRequest, nullptr ) ) { Close(); return false; }

		DWORD status = 0, sz = sizeof( status );
		WinHttpQueryHeaders( hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX );
		if ( status != 200 ) { Close(); return false; }

		return true;
	}

	DWORD GetContentLength()
	{
		DWORD len = 0, sz = sizeof( len );
		WinHttpQueryHeaders( hRequest,
			WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX );
		return len;
	}

	void Close()
	{
		if ( hRequest ) { WinHttpCloseHandle( hRequest ); hRequest = nullptr; }
		if ( hConnect ) { WinHttpCloseHandle( hConnect ); hConnect = nullptr; }
		if ( hSession ) { WinHttpCloseHandle( hSession ); hSession = nullptr; }
	}

	~HttpSession() { Close(); }
};

static std::string DownloadString( const wchar_t* host, const wchar_t* path )
{
	std::string result;
	HttpSession http;
	if ( !http.Open( host, path ) ) return result;

	char buf[4096];
	DWORD bytesRead;
	while ( WinHttpReadData( http.hRequest, buf, sizeof( buf ), &bytesRead ) && bytesRead > 0 )
		result.append( buf, bytesRead );

	return result;
}

static bool DownloadFile( const wchar_t* host, const wchar_t* path, const wchar_t* outPath )
{
	HttpSession http;
	if ( !http.Open( host, path ) ) return false;

	DWORD contentLen = http.GetContentLength();

	HANDLE hFile = CreateFileW( outPath, GENERIC_WRITE, 0,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
	if ( hFile == INVALID_HANDLE_VALUE ) return false;

	BYTE buf[16384];
	DWORD bytesRead, totalRead = 0;
	bool ok = true;

	while ( WinHttpReadData( http.hRequest, buf, sizeof( buf ), &bytesRead ) && bytesRead > 0 )
	{
		DWORD written;
		if ( !WriteFile( hFile, buf, bytesRead, &written, nullptr ) || written != bytesRead )
		{
			ok = false;
			break;
		}
		totalRead += bytesRead;
		if ( contentLen > 0 )
			SetPctProgress( (int)( (ULONGLONG)totalRead * 100 / contentLen ) );
	}

	CloseHandle( hFile );
	if ( !ok ) DeleteFileW( outPath );
	return ok;
}

// ── Helpers ─────────────────────────────────────────────────────

static std::string Trim( const std::string& s )
{
	size_t a = s.find_first_not_of( " \t\r\n" );
	size_t b = s.find_last_not_of( " \t\r\n" );
	return ( a == std::string::npos ) ? "" : s.substr( a, b - a + 1 );
}

static std::string ReadLocalVersion( const std::wstring& dir )
{
	std::wstring path = dir + L"\\version.txt";
	std::ifstream f( path );
	if ( !f.is_open() ) return "";
	std::string line;
	std::getline( f, line );
	return Trim( line );
}

static void WriteLocalVersion( const std::wstring& dir, const std::string& ver )
{
	std::ofstream f( dir + L"\\version.txt" );
	f << ver;
}

static bool ExtractZip( const std::wstring& zipPath, const std::wstring& destDir )
{
	std::wstring cmd =
		L"powershell.exe -NoProfile -NonInteractive -Command \""
		L"Expand-Archive -LiteralPath '" + zipPath + L"' "
		L"-DestinationPath '" + destDir + L"' -Force\"";

	STARTUPINFOW si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi{};
	if ( !CreateProcessW( nullptr, cmd.data(), nullptr, nullptr,
		FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
		return false;

	WaitForSingleObject( pi.hProcess, 120000 );

	DWORD exitCode = 1;
	GetExitCodeProcess( pi.hProcess, &exitCode );
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return exitCode == 0;
}

static std::wstring GetInstallDir()
{
	wchar_t* appData = nullptr;
	if ( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_LocalAppData, 0, nullptr, &appData ) ) )
	{
		std::wstring dir = std::wstring( appData ) + L"\\DotaFarm";
		CoTaskMemFree( appData );
		return dir;
	}
	wchar_t buf[MAX_PATH];
	GetTempPathW( MAX_PATH, buf );
	return std::wstring( buf ) + L"DotaFarm";
}

static bool FileExists( const std::wstring& path )
{
	return GetFileAttributesW( path.c_str() ) != INVALID_FILE_ATTRIBUTES;
}

// ── Worker thread ───────────────────────────────────────────────
//
// Делает всю тяжёлую работу. UI thread пишет статусы/progress в g_ws и
// обновляет контролы по WM_TIMER.

static DWORD WINAPI WorkerProc( LPVOID )
{
	SetStatus( L"Проверка обновлений..." );
	SetMarquee( true );

	std::string remoteVer = Trim( DownloadString( DOWNLOAD_HOST, VERSION_URL ) );
	std::string localVer  = ReadLocalVersion( g_installDir );

	bool needUpdate = false;
	std::wstring exePath = g_installDir + L"\\DotaFarm.exe";

	if ( remoteVer.empty() )
	{
		// Server unreachable — try local
		if ( FileExists( exePath ) )
		{
			SetStatus( L"Сервер недоступен. Запуск локальной версии..." );
			SetMarquee( true );
			Sleep( 700 );
		}
		else
		{
			std::lock_guard<std::mutex> lk( g_ws.mu );
			g_ws.errorMsg = L"Не удалось подключиться к серверу обновлений.\n"
				L"Проверьте подключение к интернету.";
			g_ws.error.store( true );
			g_ws.done.store( true );
			return 1;
		}
	}
	else
	{
		needUpdate = ( remoteVer != localVer ) || !FileExists( exePath );
	}

	if ( needUpdate )
	{
		SetStatus( L"Скачивание обновления..." );
		SetMarquee( false );
		SetPctProgress( 0 );

		std::wstring zipPath = g_installDir + L"\\~update.zip";

		if ( !DownloadFile( DOWNLOAD_HOST, PACKAGE_URL, zipPath.c_str() ) )
		{
			DeleteFileW( zipPath.c_str() );
			std::lock_guard<std::mutex> lk( g_ws.mu );
			g_ws.errorMsg = L"Ошибка скачивания.\nПроверьте подключение к интернету.";
			g_ws.error.store( true );
			g_ws.done.store( true );
			return 1;
		}

		SetStatus( L"Установка обновления..." );
		SetMarquee( true );  // Expand-Archive не даёт прогресс

		if ( !ExtractZip( zipPath, g_installDir ) )
		{
			DeleteFileW( zipPath.c_str() );
			std::lock_guard<std::mutex> lk( g_ws.mu );
			g_ws.errorMsg = L"Ошибка распаковки.\nЗакройте DotaFarm если он запущен.";
			g_ws.error.store( true );
			g_ws.done.store( true );
			return 1;
		}

		DeleteFileW( zipPath.c_str() );

		// Delete farm.json so DotaFarm.exe recreates it with current defaults
		// (accounts.json is preserved — user config)
		DeleteFileW( ( g_installDir + L"\\config\\farm.json" ).c_str() );

		WriteLocalVersion( g_installDir, remoteVer );

		SetStatus( L"Обновление установлено!" );
		SetMarquee( false );
		SetPctProgress( 100 );
		Sleep( 400 );
	}
	else if ( !remoteVer.empty() )
	{
		SetStatus( L"Версия актуальна." );
		SetMarquee( false );
		SetPctProgress( 100 );
		Sleep( 300 );
	}

	if ( !FileExists( exePath ) )
	{
		std::lock_guard<std::mutex> lk( g_ws.mu );
		g_ws.errorMsg = L"DotaFarm.exe не найден после установки.";
		g_ws.error.store( true );
		g_ws.done.store( true );
		return 1;
	}

	SetStatus( L"Запуск DotaFarm..." );
	SetMarquee( true );

	{
		std::lock_guard<std::mutex> lk( g_ws.mu );
		g_ws.exePath = exePath;
		g_ws.launchExe = true;
	}
	g_ws.done.store( true );
	return 0;
}

// ── Entry point ─────────────────────────────────────────────────

int WINAPI WinMain( HINSTANCE hInst, HINSTANCE, LPSTR, int )
{
	g_installDir = GetInstallDir();
	CreateDirectoryW( g_installDir.c_str(), nullptr );

	if ( !CreateSplash( hInst ) )
	{
		MessageBoxW( nullptr, L"Failed to create window.", PRODUCT_NAME, MB_ICONERROR );
		return 1;
	}

	// Spawn worker, ждём через MsgWaitForMultipleObjects — UI thread pump'ит
	// WM_TIMER, WM_PAINT и WM_CTLCOLOR, окно всегда отзывчиво.
	HANDLE hWorker = CreateThread( nullptr, 0, WorkerProc, nullptr, 0, nullptr );
	if ( !hWorker )
	{
		DestroyWindow( g_hWnd );
		MessageBoxW( nullptr, L"Failed to start worker thread.",
			PRODUCT_NAME, MB_ICONERROR );
		return 1;
	}

	while ( true )
	{
		DWORD r = MsgWaitForMultipleObjects( 1, &hWorker, FALSE,
			INFINITE, QS_ALLINPUT );
		if ( r == WAIT_OBJECT_0 )
			break; // worker done

		MSG msg;
		while ( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) )
		{
			if ( msg.message == WM_QUIT ) { CloseHandle( hWorker ); return 1; }
			TranslateMessage( &msg );
			DispatchMessageW( &msg );
		}
	}

	// Final UI refresh
	RefreshUI();
	CloseHandle( hWorker );

	if ( g_ws.error.load() )
	{
		std::wstring msg;
		std::wstring exePath;
		bool launch = false;
		{
			std::lock_guard<std::mutex> lk( g_ws.mu );
			msg = g_ws.errorMsg;
			exePath = g_ws.exePath;
			launch = g_ws.launchExe;
		}
		DestroyWindow( g_hWnd );
		MessageBoxW( nullptr, msg.c_str(), PRODUCT_NAME, MB_ICONERROR );
		return 1;
	}

	std::wstring exePath;
	{
		std::lock_guard<std::mutex> lk( g_ws.mu );
		exePath = g_ws.exePath;
	}

	STARTUPINFOW si{};
	si.cb = sizeof( si );
	PROCESS_INFORMATION pi{};

	if ( !CreateProcessW( exePath.c_str(), nullptr, nullptr, nullptr,
		FALSE, 0, nullptr, g_installDir.c_str(), &si, &pi ) )
	{
		DWORD err = GetLastError();
		DestroyWindow( g_hWnd );
		wchar_t err_msg[256];
		swprintf_s( err_msg, L"Не удалось запустить DotaFarm.exe\nКод ошибки: %lu", err );
		MessageBoxW( nullptr, err_msg, PRODUCT_NAME, MB_ICONERROR );
		return 1;
	}

	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	DestroyWindow( g_hWnd );
	return 0;
}
