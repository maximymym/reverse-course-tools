#include "window_tiler.h"

#include <algorithm>
#include <cstring>

namespace window_tiler
{

const char* LayoutName( Layout l )
{
	switch ( l )
	{
	case Layout::Grid_3_2:   return "Grid 3+2";
	case Layout::Grid_2_2_1: return "Grid 2+2+1";
	case Layout::Strip_5:    return "Strip 1x5";
	case Layout::Cascade:    return "Cascade";
	}
	return "?";
}

std::string LayoutToString( Layout l )
{
	switch ( l )
	{
	case Layout::Grid_3_2:   return "grid_3_2";
	case Layout::Grid_2_2_1: return "grid_2_2_1";
	case Layout::Strip_5:    return "strip_5";
	case Layout::Cascade:    return "cascade";
	}
	return "grid_3_2";
}

Layout LayoutFromString( const std::string& s )
{
	if ( s == "grid_2_2_1" ) return Layout::Grid_2_2_1;
	if ( s == "strip_5" )    return Layout::Strip_5;
	if ( s == "cascade" )    return Layout::Cascade;
	return Layout::Grid_3_2;
}

// ── Find main window by PID ──

namespace
{

struct EnumCtx
{
	DWORD targetPid = 0;
	HWND  result    = nullptr;
};

// Признак "главного" окна процесса:
//  - top-level (no owner)
//  - visible
//  - есть заголовок
//  - не tool-window (WS_EX_TOOLWINDOW исключаем)
static bool IsMainWindow( HWND hwnd )
{
	if ( !IsWindowVisible( hwnd ) ) return false;
	if ( GetWindow( hwnd, GW_OWNER ) != nullptr ) return false;

	LONG_PTR exStyle = GetWindowLongPtrW( hwnd, GWL_EXSTYLE );
	if ( exStyle & WS_EX_TOOLWINDOW ) return false;

	int len = GetWindowTextLengthW( hwnd );
	if ( len <= 0 ) return false;

	return true;
}

static BOOL CALLBACK EnumProc( HWND hwnd, LPARAM lp )
{
	auto* ctx = reinterpret_cast<EnumCtx*>( lp );
	DWORD pid = 0;
	GetWindowThreadProcessId( hwnd, &pid );
	if ( pid != ctx->targetPid ) return TRUE;
	if ( !IsMainWindow( hwnd ) ) return TRUE;
	ctx->result = hwnd;
	return FALSE; // stop enumeration
}

} // anon

HWND FindMainWindowByPid( DWORD pid )
{
	if ( !pid ) return nullptr;
	EnumCtx ctx;
	ctx.targetPid = pid;
	EnumWindows( EnumProc, reinterpret_cast<LPARAM>( &ctx ) );
	return ctx.result;
}

// ── Monitor work area ──

namespace
{

struct MonEnumCtx
{
	int   want     = 0;
	int   counter  = 0;
	RECT  result{};
	bool  found    = false;
};

static BOOL CALLBACK MonProc( HMONITOR hMon, HDC, LPRECT, LPARAM lp )
{
	auto* ctx = reinterpret_cast<MonEnumCtx*>( lp );
	if ( ctx->counter == ctx->want )
	{
		MONITORINFO mi{ sizeof( mi ) };
		if ( GetMonitorInfoW( hMon, &mi ) )
		{
			ctx->result = mi.rcWork;
			ctx->found  = true;
		}
		return FALSE;
	}
	ctx->counter++;
	return TRUE;
}

} // anon

RECT GetMonitorWorkArea( int monitorIndex )
{
	RECT fallback{ 0, 0, 1920, 1080 };

	if ( monitorIndex < 0 )
	{
		HMONITOR hMon = MonitorFromPoint( POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY );
		MONITORINFO mi{ sizeof( mi ) };
		if ( GetMonitorInfoW( hMon, &mi ) ) return mi.rcWork;
		return fallback;
	}

	MonEnumCtx ctx;
	ctx.want = monitorIndex;
	EnumDisplayMonitors( nullptr, nullptr, MonProc, reinterpret_cast<LPARAM>( &ctx ) );
	if ( ctx.found ) return ctx.result;
	return fallback;
}

// ── Force windowed style ──

static void ForceWindowedStyle( HWND hwnd )
{
	// Снимаем WS_POPUP и ставим стандартный WS_OVERLAPPEDWINDOW (с caption +
	// thick frame + sysmenu). Если окно было в exclusive fullscreen — после
	// этого мы сможем нормально его двигать.
	LONG_PTR style   = GetWindowLongPtrW( hwnd, GWL_STYLE );
	LONG_PTR exStyle = GetWindowLongPtrW( hwnd, GWL_EXSTYLE );

	style &= ~( WS_POPUP | WS_MAXIMIZE | WS_MINIMIZE );
	style |= ( WS_OVERLAPPEDWINDOW | WS_VISIBLE );

	exStyle &= ~( WS_EX_TOPMOST );

	SetWindowLongPtrW( hwnd, GWL_STYLE, style );
	SetWindowLongPtrW( hwnd, GWL_EXSTYLE, exStyle );

	// Если максимизировано — выходим из maximized.
	if ( IsZoomed( hwnd ) )
		ShowWindow( hwnd, SW_RESTORE );

	// FRAMECHANGED обязателен после смены стилей.
	SetWindowPos( hwnd, nullptr, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED );
}

// ── Layout computation ──

namespace
{

struct Cell
{
	int x, y, w, h;
};

// Раскладка для N окон. Возвращает до N ячеек.
static std::vector<Cell> ComputeCells( Layout layout, int n,
	const RECT& wa, int pad )
{
	std::vector<Cell> cells;
	if ( n <= 0 ) return cells;

	int areaX = wa.left + pad;
	int areaY = wa.top  + pad;
	int areaW = ( wa.right  - wa.left ) - pad * 2;
	int areaH = ( wa.bottom - wa.top  ) - pad * 2;
	if ( areaW < 320 ) areaW = 320;
	if ( areaH < 240 ) areaH = 240;

	auto pushGrid = [&]( int rows, const std::vector<int>& colsPerRow )
	{
		// rowH одинаковая для всех; в каждой строке колонки равной ширины.
		int rowH = areaH / rows;
		int idx  = 0;
		for ( int r = 0; r < rows && idx < n; r++ )
		{
			int cols = colsPerRow[r];
			int colW = areaW / cols;
			for ( int c = 0; c < cols && idx < n; c++ )
			{
				Cell cell;
				cell.x = areaX + c * colW;
				cell.y = areaY + r * rowH;
				cell.w = colW - pad;
				cell.h = rowH - pad;
				cells.push_back( cell );
				idx++;
			}
		}
	};

	switch ( layout )
	{
	case Layout::Grid_3_2:
	{
		// 3 сверху, 2 снизу. Если ботов <5 — заполняем столько сколько есть.
		pushGrid( 2, { 3, 2 } );
		break;
	}
	case Layout::Grid_2_2_1:
	{
		// 2 сверху, 2 в середине, 1 внизу по центру.
		int rowH = areaH / 3;
		int idx = 0;

		// row 0 — 2 окна
		{
			int cols = 2;
			int colW = areaW / cols;
			for ( int c = 0; c < cols && idx < n; c++ )
			{
				Cell cell;
				cell.x = areaX + c * colW;
				cell.y = areaY + 0 * rowH;
				cell.w = colW - pad;
				cell.h = rowH - pad;
				cells.push_back( cell );
				idx++;
			}
		}
		// row 1 — 2 окна
		if ( idx < n )
		{
			int cols = 2;
			int colW = areaW / cols;
			for ( int c = 0; c < cols && idx < n; c++ )
			{
				Cell cell;
				cell.x = areaX + c * colW;
				cell.y = areaY + 1 * rowH;
				cell.w = colW - pad;
				cell.h = rowH - pad;
				cells.push_back( cell );
				idx++;
			}
		}
		// row 2 — 1 окно по центру (полная ширина / 2, центрировано)
		if ( idx < n )
		{
			int colW = areaW / 2;
			Cell cell;
			cell.x = areaX + ( areaW - colW ) / 2;
			cell.y = areaY + 2 * rowH;
			cell.w = colW - pad;
			cell.h = rowH - pad;
			cells.push_back( cell );
			idx++;
		}
		break;
	}
	case Layout::Strip_5:
	{
		// Одна горизонтальная полоса. Если N>5 окон будут совсем узкими.
		int cols = ( n > 0 ) ? n : 1;
		int colW = areaW / cols;
		for ( int c = 0; c < cols; c++ )
		{
			Cell cell;
			cell.x = areaX + c * colW;
			cell.y = areaY;
			cell.w = colW - pad;
			cell.h = areaH;
			cells.push_back( cell );
		}
		break;
	}
	case Layout::Cascade:
	{
		// Лесенкой. Каждое окно ~70% area, смещение step.
		int winW = ( areaW * 7 ) / 10;
		int winH = ( areaH * 7 ) / 10;
		int maxStepX = ( n > 1 ) ? ( areaW - winW ) / ( n - 1 ) : 0;
		int maxStepY = ( n > 1 ) ? ( areaH - winH ) / ( n - 1 ) : 0;
		int stepX    = std::min( 64, maxStepX > 0 ? maxStepX : 0 );
		int stepY    = std::min( 48, maxStepY > 0 ? maxStepY : 0 );
		for ( int i = 0; i < n; i++ )
		{
			Cell cell;
			cell.x = areaX + i * stepX;
			cell.y = areaY + i * stepY;
			cell.w = winW;
			cell.h = winH;
			cells.push_back( cell );
		}
		break;
	}
	}

	return cells;
}

} // anon

// ── ApplyTile ──

int ApplyTile( const std::vector<DWORD>& pids, const TileOptions& opt )
{
	// Сначала находим окна — пропускаем PID без окна.
	std::vector<HWND> hwnds;
	hwnds.reserve( pids.size() );
	for ( DWORD pid : pids )
	{
		HWND h = FindMainWindowByPid( pid );
		if ( h ) hwnds.push_back( h );
	}
	if ( hwnds.empty() ) return 0;

	if ( opt.forceWindowed )
	{
		for ( HWND h : hwnds ) ForceWindowedStyle( h );
	}

	RECT wa = GetMonitorWorkArea( opt.monitorIndex );
	auto cells = ComputeCells( opt.layout, (int)hwnds.size(),
		wa, std::max( 0, opt.padding ) );

	int applied = 0;
	for ( size_t i = 0; i < hwnds.size() && i < cells.size(); i++ )
	{
		HWND h = hwnds[i];
		const Cell& c = cells[i];

		// Если окно minimized — restore (иначе SetWindowPos применится но
		// окно останется на taskbar).
		if ( IsIconic( h ) ) ShowWindow( h, SW_RESTORE );

		BOOL ok = SetWindowPos( h, nullptr,
			c.x, c.y, c.w, c.h,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER );
		if ( ok ) applied++;
	}

	return applied;
}

} // namespace window_tiler
