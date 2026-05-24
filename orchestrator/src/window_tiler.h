#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// ── Window tiler ──
//
// Раскладка окон Dota 2 (по PID) в grid на одном дисплее. Используется
// чтобы оператор фермы мог одним кликом разбросать 5 окон по экрану и
// видеть всех ботов параллельно.
//
// Главное окно процесса находится через EnumWindows + критерий
// IsMainWindow (видимый top-level + has title + не tool-window).

namespace window_tiler
{

enum class Layout
{
	Grid_3_2,   // 3 окна сверху, 2 снизу — default для 5 ботов
	Grid_2_2_1, // 2+2+1 (последнее — внизу по центру)
	Strip_5,    // одна полоса 5 столбцов
	Cascade,    // лесенкой (overlap), полезно для focus-rotate
};

const char* LayoutName( Layout l );
Layout      LayoutFromString( const std::string& s );
std::string LayoutToString( Layout l );

struct TileOptions
{
	Layout    layout = Layout::Grid_3_2;

	// Если true — перед позиционированием окно принудительно переводится
	// в windowed режим (снимаем WS_POPUP / fullscreen-стили и ставим
	// WS_OVERLAPPEDWINDOW). По умолчанию false — Dota уже стартует с
	// `-w 640 -h 480 -windowed` и трогать стили лишний раз не нужно.
	bool      forceWindowed = false;

	// Какой монитор использовать. -1 = primary monitor.
	int       monitorIndex = -1;

	// Inset/padding в пикселях. Окна не лезут впритык к краю экрана.
	int       padding = 4;
};

// Найти главное (top-level visible titled) окно процесса по PID.
// Возвращает NULL если окно не найдено.
HWND FindMainWindowByPid( DWORD pid );

// Применить layout к набору PID. Окна, которые не нашлись, пропускаются.
// Возвращает количество окон, к которым успешно применён tile.
int ApplyTile( const std::vector<DWORD>& pids, const TileOptions& opt );

// Получить bounds primary/Nth monitor work area (без taskbar).
RECT GetMonitorWorkArea( int monitorIndex );

} // namespace window_tiler
