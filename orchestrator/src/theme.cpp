// theme.cpp — see theme.h for design intent.
#include "theme.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <Windows.h>

namespace theme
{

ImFont* gFontSerifLg = nullptr;
ImFont* gFontSerifMd = nullptr;
ImFont* gFontMono    = nullptr;
ImFont* gFontMonoSm  = nullptr;
ImFont* gFontMonoLg  = nullptr;
ImFont* gFontSans    = nullptr;

// ── Theme apply ─────────────────────────────────────────────────────────

static ImVec4 V4( ImU32 c ) { return V( c ); }

void ApplyDotaFarmTheme()
{
	ImGuiStyle& s = ImGui::GetStyle();

	// No rounding anywhere (industrial / engineering look).
	s.WindowRounding    = 0.f;
	s.FrameRounding     = 0.f;
	s.GrabRounding      = 0.f;
	s.PopupRounding     = 0.f;
	s.ScrollbarRounding = 0.f;
	s.TabRounding       = 0.f;
	s.ChildRounding     = 0.f;

	// Hairline 1px borders on everything.
	s.WindowBorderSize = 1.f;
	s.FrameBorderSize  = 1.f;
	s.PopupBorderSize  = 1.f;
	s.ChildBorderSize  = 1.f;
	s.TabBorderSize    = 1.f;

	// Tight, dense layout — telemetry console, not consumer app.
	s.WindowPadding     = ImVec2( 10, 10 );
	s.FramePadding      = ImVec2( 8, 5 );
	s.CellPadding       = ImVec2( 7, 4 );
	s.ItemSpacing       = ImVec2( 8, 5 );
	s.ItemInnerSpacing  = ImVec2( 6, 4 );
	s.IndentSpacing     = 16.f;
	s.ScrollbarSize     = 10.f;
	s.GrabMinSize       = 8.f;

	// Color palette (mirrors mockup CSS variables).
	auto& c = s.Colors;

	c[ImGuiCol_WindowBg]            = V4( kColBg0 );
	c[ImGuiCol_ChildBg]             = V4( kColBg1 );
	c[ImGuiCol_PopupBg]             = V4( kColBg1 );
	c[ImGuiCol_Border]              = V4( kColLine );
	c[ImGuiCol_BorderShadow]        = ImVec4( 0, 0, 0, 0 );

	c[ImGuiCol_Text]                = V4( kColInk );
	c[ImGuiCol_TextDisabled]        = V4( kColInkMute );
	c[ImGuiCol_TextSelectedBg]      = ImVec4( 0.83f, 0.63f, 0.29f, 0.40f );

	c[ImGuiCol_FrameBg]             = V4( kColBg2 );
	c[ImGuiCol_FrameBgHovered]      = V4( kColBg3 );
	c[ImGuiCol_FrameBgActive]       = V4( kColBg3 );

	c[ImGuiCol_TitleBg]             = V4( kColBg1 );
	c[ImGuiCol_TitleBgActive]       = V4( kColBg2 );
	c[ImGuiCol_TitleBgCollapsed]    = V4( kColBg1 );

	c[ImGuiCol_MenuBarBg]           = V4( kColBg1 );

	c[ImGuiCol_ScrollbarBg]         = V4( kColBg0 );
	c[ImGuiCol_ScrollbarGrab]       = V4( kColLineHot );
	c[ImGuiCol_ScrollbarGrabHovered]= V4( kColGoldDeep );
	c[ImGuiCol_ScrollbarGrabActive] = V4( kColGold );

	c[ImGuiCol_CheckMark]           = V4( kColGold );
	c[ImGuiCol_SliderGrab]          = V4( kColGoldDeep );
	c[ImGuiCol_SliderGrabActive]    = V4( kColGold );

	c[ImGuiCol_Button]              = V4( kColBg1 );
	c[ImGuiCol_ButtonHovered]       = V4( kColBg3 );
	c[ImGuiCol_ButtonActive]        = ImVec4( 0.16f, 0.13f, 0.08f, 1.f );

	c[ImGuiCol_Header]              = V4( kColBg2 );
	c[ImGuiCol_HeaderHovered]       = V4( kColBg3 );
	c[ImGuiCol_HeaderActive]        = ImVec4( 0.18f, 0.15f, 0.09f, 1.f );

	c[ImGuiCol_Separator]           = V4( kColLine );
	c[ImGuiCol_SeparatorHovered]    = V4( kColLineHot );
	c[ImGuiCol_SeparatorActive]     = V4( kColGoldDeep );

	c[ImGuiCol_ResizeGrip]          = V4( kColLine );
	c[ImGuiCol_ResizeGripHovered]   = V4( kColLineHot );
	c[ImGuiCol_ResizeGripActive]    = V4( kColGold );

	c[ImGuiCol_Tab]                 = V4( kColBg1 );
	c[ImGuiCol_TabHovered]          = V4( kColBg3 );
	c[ImGuiCol_TabActive]           = V4( kColBg2 );
	c[ImGuiCol_TabUnfocused]        = V4( kColBg1 );
	c[ImGuiCol_TabUnfocusedActive]  = V4( kColBg2 );

	c[ImGuiCol_TableHeaderBg]       = V4( kColBg2 );
	c[ImGuiCol_TableBorderStrong]   = V4( kColLineHot );
	c[ImGuiCol_TableBorderLight]    = V4( kColLine );
	c[ImGuiCol_TableRowBg]          = ImVec4( 0, 0, 0, 0 );
	c[ImGuiCol_TableRowBgAlt]       = ImVec4( 0.07f, 0.06f, 0.04f, 0.40f );
}

// ── Fonts ───────────────────────────────────────────────────────────────

// Try a list of paths in order, return first that exists.
static const char* PickFont( const char* const* candidates, int n )
{
	for ( int i = 0; i < n; i++ )
	{
		if ( GetFileAttributesA( candidates[i] ) != INVALID_FILE_ATTRIBUTES )
			return candidates[i];
	}
	return nullptr;
}

bool LoadDotaFarmFonts( ImGuiIO& io )
{
	// Cinzel-style serif: bundled asset → georgia bold → cambria bold → georgia.
	// Cinzel itself isn't on Windows by default; if user drops Cinzel-Regular.ttf
	// into assets/fonts/ next to EXE we pick it up first.
	const char* serifCandidates[] = {
		"assets\\fonts\\Cinzel-Bold.ttf",
		"assets\\fonts\\Cinzel-Regular.ttf",
		"C:\\Windows\\Fonts\\georgiab.ttf",
		"C:\\Windows\\Fonts\\cambriab.ttf",
		"C:\\Windows\\Fonts\\georgia.ttf",
	};
	// Mono picks (priority high→low). JetBrainsMono is the editorial choice
	// — characterful glyph design, ligatures, calibrated for dense telemetry.
	// Cascadia Mono is the secondary native fallback. Consolas only as last
	// resort — it reads as "default Windows app", which is the AI-slop tell.
	const char* monoCandidates[] = {
		"assets\\fonts\\JetBrainsMono-Regular.ttf",
		"C:\\Windows\\Fonts\\JetBrainsMonoNerdFontMono-Regular.ttf",
		"C:\\Windows\\Fonts\\JetBrainsMonoNerdFont-Regular.ttf",
		"C:\\Windows\\Fonts\\CascadiaMono.ttf",
		"C:\\Windows\\Fonts\\CascadiaCode.ttf",
		"C:\\Windows\\Fonts\\consola.ttf",
	};
	const char* monoBoldCandidates[] = {
		"assets\\fonts\\JetBrainsMono-Bold.ttf",
		"C:\\Windows\\Fonts\\JetBrainsMonoNerdFontMono-Bold.ttf",
		"C:\\Windows\\Fonts\\JetBrainsMonoNerdFont-Bold.ttf",
		"C:\\Windows\\Fonts\\CascadiaMono.ttf",
		"C:\\Windows\\Fonts\\consolab.ttf",
		"C:\\Windows\\Fonts\\consola.ttf",
	};
	// Sans body — IBM Plex Sans (vendored). Industrial-IBM heritage,
	// engineered counter-shapes, NOT the AI-slop generic-modern stack
	// (Inter / Roboto / SF Pro / Segoe). Inter especially is forbidden
	// — it's the giveaway of a "ChatGPT designed this" UI. Plex carries
	// engineering provenance that fits the telemetry/scope direction.
	const char* sansCandidates[] = {
		"assets\\fonts\\IBMPlexSans-Regular.ttf",
		"assets\\fonts\\IBMPlexSans-Medium.ttf",
		// LAST resort only — we'd rather refuse to render than show
		// generic-Windows-app sans. If we're here on a prod box the
		// post-build copy of assets failed.
		"C:\\Windows\\Fonts\\segoeui.ttf",
	};
	const char* sansBoldCandidates[] = {
		"assets\\fonts\\IBMPlexSans-Bold.ttf",
		"assets\\fonts\\IBMPlexSans-Medium.ttf",
		"C:\\Windows\\Fonts\\segoeuib.ttf",
	};

	const char* serifPath    = PickFont( serifCandidates,    (int)( sizeof( serifCandidates )    / sizeof( *serifCandidates ) ) );
	const char* monoPath     = PickFont( monoCandidates,     (int)( sizeof( monoCandidates )     / sizeof( *monoCandidates ) ) );
	const char* monoBoldPath = PickFont( monoBoldCandidates, (int)( sizeof( monoBoldCandidates ) / sizeof( *monoBoldCandidates ) ) );
	const char* sansPath     = PickFont( sansCandidates,     (int)( sizeof( sansCandidates )     / sizeof( *sansCandidates ) ) );
	const char* sansBoldPath = PickFont( sansBoldCandidates, (int)( sizeof( sansBoldCandidates ) / sizeof( *sansBoldCandidates ) ) );

	if ( !serifPath || !monoPath || !sansPath )
		return false;

	// Cyrillic-aware glyph range — telemetry text is English/ASCII but the
	// app surfaces Russian comments + region labels (sing-box, Karos), so we
	// pre-bake Cyrillic into the atlas to avoid ?-glyph fallback at runtime.
	const ImWchar* glyphsCyr = io.Fonts->GetGlyphRangesCyrillic();

	// Order matters: io.FontDefault is the first font added unless overridden.
	// Mono 13px is THE default — everything reads as telemetry until the
	// caller explicitly Push'es a serif/sans for a specific block.
	gFontMono    = io.Fonts->AddFontFromFileTTF( monoPath,                                  13.f, nullptr, glyphsCyr );
	gFontMonoSm  = io.Fonts->AddFontFromFileTTF( monoPath,                                  11.f, nullptr, glyphsCyr );
	gFontMonoLg  = io.Fonts->AddFontFromFileTTF( monoBoldPath ? monoBoldPath : monoPath,    15.f, nullptr, glyphsCyr );
	gFontSerifLg = io.Fonts->AddFontFromFileTTF( serifPath,                                 22.f, nullptr, glyphsCyr );
	gFontSerifMd = io.Fonts->AddFontFromFileTTF( serifPath,                                 16.f, nullptr, glyphsCyr );
	// Sans baked at 13 (body) AND 11 (label) — Plex tracking is wider than
	// JBM at small sizes; using one size for both creates uneven row rhythms.
	gFontSans    = io.Fonts->AddFontFromFileTTF( sansPath,                                  13.f, nullptr, glyphsCyr );

	io.FontDefault = gFontMono;
	(void)sansBoldPath;  // reserved for future "BOLD section" weight; bake on demand only
	return true;
}

// ── Drawing primitives ──────────────────────────────────────────────────

void DrawChamferedRect( ImDrawList* dl, ImVec2 mn, ImVec2 mx,
	float chamfer, ImU32 col, float thickness )
{
	if ( chamfer < 1.f ) chamfer = 1.f;
	// 6-vertex polygon mirroring the mockup CSS clip-path:
	//   0 0, calc(100%-c) 0, 100% c, 100% 100%, c 100%, 0 calc(100%-c)
	ImVec2 pts[6] = {
		ImVec2( mn.x,           mn.y           ),
		ImVec2( mx.x - chamfer, mn.y           ),
		ImVec2( mx.x,           mn.y + chamfer ),
		ImVec2( mx.x,           mx.y           ),
		ImVec2( mn.x + chamfer, mx.y           ),
		ImVec2( mn.x,           mx.y - chamfer ),
	};
	if ( thickness <= 0.f )
		dl->AddConvexPolyFilled( pts, 6, col );
	else
		dl->AddPolyline( pts, 6, col, ImDrawFlags_Closed, thickness );
}

void DrawCornerBrackets( ImDrawList* dl, ImVec2 mn, ImVec2 mx,
	float len, float thickness, ImU32 col )
{
	// TL
	dl->AddLine( ImVec2( mn.x, mn.y ), ImVec2( mn.x + len, mn.y ), col, thickness );
	dl->AddLine( ImVec2( mn.x, mn.y ), ImVec2( mn.x, mn.y + len ), col, thickness );
	// TR
	dl->AddLine( ImVec2( mx.x - len, mn.y ), ImVec2( mx.x, mn.y ), col, thickness );
	dl->AddLine( ImVec2( mx.x, mn.y ), ImVec2( mx.x, mn.y + len ), col, thickness );
	// BL
	dl->AddLine( ImVec2( mn.x, mx.y - len ), ImVec2( mn.x, mx.y ), col, thickness );
	dl->AddLine( ImVec2( mn.x, mx.y ), ImVec2( mn.x + len, mx.y ), col, thickness );
	// BR
	dl->AddLine( ImVec2( mx.x - len, mx.y ), ImVec2( mx.x, mx.y ), col, thickness );
	dl->AddLine( ImVec2( mx.x, mx.y - len ), ImVec2( mx.x, mx.y ), col, thickness );
}

void DrawScanlines( ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, int spacing )
{
	if ( spacing < 2 ) spacing = 2;
	float w = mx.x - mn.x;
	if ( w <= 0 ) return;
	for ( float y = mn.y; y < mx.y; y += (float)spacing )
		dl->AddLine( ImVec2( mn.x, y ), ImVec2( mx.x, y ), col, 1.f );
}

void DrawDottedGrid( ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float step )
{
	if ( step < 4.f ) step = 4.f;
	for ( float y = mn.y; y < mx.y; y += step )
		for ( float x = mn.x; x < mx.x; x += step )
			dl->AddRectFilled( ImVec2( x, y ), ImVec2( x + 1, y + 1 ), col );
}

static float Pulse01( float phase )
{
	// 0..1..0 sinusoid (period 1)
	return 0.5f + 0.5f * sinf( phase * 6.2831853f );
}

void DrawLED( ImDrawList* dl, ImVec2 center, float radius, ImU32 col,
	float pulse_phase, bool blink )
{
	float a;
	if ( blink )
		a = ( fmodf( pulse_phase, 1.f ) < 0.5f ) ? 1.f : 0.25f;
	else
		a = 0.55f + 0.45f * Pulse01( pulse_phase );
	ImU32 core = ( col & 0x00FFFFFF ) | ( (ImU32)( a * 255.f ) << 24 );
	// 3 layered glow rings + filled core
	ImU32 g1 = ( col & 0x00FFFFFF ) | ( (ImU32)( a * 80.f )  << 24 );
	ImU32 g2 = ( col & 0x00FFFFFF ) | ( (ImU32)( a * 32.f )  << 24 );
	dl->AddCircleFilled( center, radius * 2.6f, g2, 16 );
	dl->AddCircleFilled( center, radius * 1.6f, g1, 16 );
	dl->AddCircleFilled( center, radius,        core, 16 );
}

void DrawLEDSquare( ImDrawList* dl, ImVec2 center, float size, ImU32 col,
	ImU32 border, float pulse_phase, bool blink )
{
	float a;
	if ( blink )
		a = ( fmodf( pulse_phase, 1.f ) < 0.5f ) ? 1.f : 0.30f;
	else
		a = 0.65f + 0.35f * Pulse01( pulse_phase );
	ImU32 core = ( col & 0x00FFFFFF ) | ( (ImU32)( a * 255.f ) << 24 );
	ImU32 glow = ( col & 0x00FFFFFF ) | ( (ImU32)( a * 60.f )  << 24 );

	float h = size * 0.5f;
	dl->AddRectFilled( ImVec2( center.x - h - 3, center.y - h - 3 ),
		ImVec2( center.x + h + 3, center.y + h + 3 ), glow );
	dl->AddRectFilled( ImVec2( center.x - h, center.y - h ),
		ImVec2( center.x + h, center.y + h ), core );
	dl->AddRect( ImVec2( center.x - h, center.y - h ),
		ImVec2( center.x + h, center.y + h ), border, 0.f, 0, 1.f );
}

void DrawHpBar( ImDrawList* dl, ImVec2 mn, ImVec2 mx, float fill, ImU32 col, ImU32 bg )
{
	if ( fill < 0 ) fill = 0;
	if ( fill > 1 ) fill = 1;
	dl->AddRectFilled( mn, mx, bg );
	float w = ( mx.x - mn.x ) * fill;
	if ( w >= 1.f )
	{
		ImVec2 fmx( mn.x + w, mx.y );
		dl->AddRectFilled( mn, fmx, col );
		// diagonal hatch (every 6px stripe of darker overlay) — segmented vibe
		ImU32 dark = IM_COL32( 0, 0, 0, 60 );
		for ( float x = mn.x; x < fmx.x; x += 6.f )
			dl->AddRectFilled( ImVec2( x, mn.y ), ImVec2( x + 2, mx.y ), dark );
	}
	dl->AddRect( mn, mx, kColLineHot, 0.f, 0, 1.f );
}

void DrawStreamingProgress( ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col,
	float t, float speed )
{
	dl->AddRectFilled( mn, mx, kColBg0 );
	// Animated stripe traversing the bar
	float w  = mx.x - mn.x;
	float bw = w * 0.35f;
	float p  = fmodf( t * speed * 0.6f, 1.f );      // 0..1
	float x0 = mn.x + ( -bw ) + p * ( w + bw );
	float x1 = x0 + bw;
	if ( x1 < mn.x || x0 > mx.x ) {} else
	{
		float clipL = x0 < mn.x ? mn.x : x0;
		float clipR = x1 > mx.x ? mx.x : x1;
		// linear gradient via 6 sub-rects with rising→falling alpha
		int N = 8;
		for ( int i = 0; i < N; i++ )
		{
			float u0 = (float)i / N, u1 = (float)( i + 1 ) / N;
			float fa = ( u0 < 0.5f ? u0 / 0.5f : ( 1.f - u0 ) / 0.5f );
			ImU32 cc = ( col & 0x00FFFFFF ) | ( (ImU32)( fa * 220.f ) << 24 );
			ImVec2 a( clipL + ( clipR - clipL ) * u0, mn.y );
			ImVec2 b( clipL + ( clipR - clipL ) * u1, mx.y );
			dl->AddRectFilled( a, b, cc );
		}
	}
	dl->AddRect( mn, mx, kColLineHot, 0.f, 0, 1.f );
}

void DrawIndexPlate( ImDrawList* dl, ImVec2 mn, ImVec2 mx, const char* digit )
{
	dl->AddRectFilled( mn, mx, kColBg2 );
	dl->AddRect( mn, mx, kColLineHot, 0.f, 0, 1.f );
	if ( !digit || !*digit ) return;
	ImFont* f = gFontSerifLg ? gFontSerifLg : ImGui::GetFont();
	ImVec2 sz = f->CalcTextSizeA( f->FontSize, FLT_MAX, 0, digit );
	ImVec2 pos(
		mn.x + ( mx.x - mn.x - sz.x ) * 0.5f,
		mn.y + ( mx.y - mn.y - sz.y ) * 0.5f - 1.f );
	dl->AddText( f, f->FontSize, pos, kColGold, digit );
}

bool ChamferedButton( const char* label, ImVec2 size, ImU32 fill, ImU32 border,
	ImU32 text_col, bool primary )
{
	ImGuiContext& g  = *ImGui::GetCurrentContext();
	ImGuiWindow*  w  = g.CurrentWindow;
	if ( w->SkipItems ) return false;

	const ImGuiID id = w->GetID( label );
	ImVec2 pos = w->DC.CursorPos;
	if ( size.x <= 0 || size.y <= 0 )
	{
		ImVec2 ts = ImGui::CalcTextSize( label, nullptr, true );
		if ( size.x <= 0 ) size.x = ts.x + g.Style.FramePadding.x * 2.f + 18.f;
		if ( size.y <= 0 ) size.y = ts.y + g.Style.FramePadding.y * 2.f + 6.f;
	}
	ImRect bb( pos, ImVec2( pos.x + size.x, pos.y + size.y ) );
	ImGui::ItemSize( size, 0.f );
	if ( !ImGui::ItemAdd( bb, id ) ) return false;

	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior( bb, id, &hovered, &held );

	ImU32 useFill = fill;
	if ( hovered )
	{
		// Brighten ~12%
		int r = ( fill       ) & 0xFF;
		int gg= ( fill >> 8  ) & 0xFF;
		int b = ( fill >> 16 ) & 0xFF;
		int a = ( fill >> 24 ) & 0xFF;
		auto brighten = []( int v ) { int x = v + 28; if ( x > 255 ) x = 255; return x; };
		useFill = IM_COL32( brighten( r ), brighten( gg ), brighten( b ), a );
	}

	ImDrawList* dl = w->DrawList;
	float chamfer = 8.f;
	DrawChamferedRect( dl, bb.Min, bb.Max, chamfer, useFill );
	DrawChamferedRect( dl, bb.Min, bb.Max, chamfer, border, 1.f );

	// Optional inner indicator square (matches mockup .btn .ind)
	float pad = 10.f;
	ImVec2 indMn( bb.Min.x + pad, bb.Min.y + ( size.y - 8.f ) * 0.5f );
	ImVec2 indMx( indMn.x + 8.f, indMn.y + 8.f );
	dl->AddRectFilled( indMn, indMx, primary ? kColBg0 : kColInkMute );

	ImVec2 ts = ImGui::CalcTextSize( label, nullptr, true );
	ImVec2 tp(
		bb.Min.x + pad + 8.f + 9.f,
		bb.Min.y + ( size.y - ts.y ) * 0.5f );
	dl->AddText( gFontMonoSm ? gFontMonoSm : ImGui::GetFont(),
		gFontMonoSm ? gFontMonoSm->FontSize : ImGui::GetFontSize(), tp, text_col, label );

	return pressed;
}

void SectionBar( const char* label )
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float avail = ImGui::GetContentRegionAvail().x;
	float lineY = p.y + 7.f;

	ImFont* f = gFontMonoSm ? gFontMonoSm : ImGui::GetFont();
	float fs = gFontMonoSm ? gFontMonoSm->FontSize : ImGui::GetFontSize();

	// "└─ LABEL ──────…"
	char prefix[] = "[-- ";
	ImVec2 ps = f->CalcTextSizeA( fs, FLT_MAX, 0, prefix );
	ImVec2 ls = f->CalcTextSizeA( fs, FLT_MAX, 0, label );

	dl->AddText( f, fs, ImVec2( p.x, p.y ), kColGoldDeep, prefix );
	dl->AddText( f, fs, ImVec2( p.x + ps.x, p.y ), kColGold, label );
	float lineX0 = p.x + ps.x + ls.x + 6.f;
	dl->AddLine( ImVec2( lineX0, lineY ), ImVec2( p.x + avail - 4.f, lineY ),
		kColLineHot, 1.f );

	ImGui::Dummy( ImVec2( avail, fs + 4.f ) );
}

// ── Animation infrastructure ────────────────────────────────────────────
// Sticky map ImGuiID → current animated float. ImGui rebuilds IDs each
// frame from the same labels, so map persists across frames. We cap entry
// count so a runaway caller (e.g. id derived from a pointer) doesn't grow
// unbounded — the cap is generous (4096), well above any realistic console
// state count, and the eviction strategy is "oldest first" via insertion
// order tracked separately.
namespace
{
	struct AnimEntry { float value; bool initialized; };
	std::unordered_map<ImGuiID, AnimEntry>& animMap()
	{
		static std::unordered_map<ImGuiID, AnimEntry> m;
		return m;
	}
}

float Animate( ImGuiID id, float target, float ms )
{
	auto& m = animMap();
	auto it = m.find( id );
	if ( it == m.end() )
	{
		// First call — snap to target without easing.
		m[id] = { target, true };
		return target;
	}
	float dt = ImGui::GetIO().DeltaTime;
	if ( dt <= 0.f ) return it->second.value;
	if ( ms < 1.f ) ms = 1.f;
	// Exponential decay — half-life ≈ ms * ln2 / 5 (5τ to settle).
	// We use the same single curve everywhere — no overshoot, no bounce.
	float tau = ms * 0.001f * 0.2f;            // seconds
	float k = 1.f - expf( -dt / tau );
	float& v = it->second.value;
	v = Lerp( v, target, k );
	// Clamp residual when within sub-pixel of target — avoid forever-easing.
	if ( fabsf( v - target ) < 0.05f ) v = target;
	return v;
}

void AnimateSnap( ImGuiID id, float value )
{
	animMap()[id] = { value, true };
}

void PulseDot( ImDrawList* dl, ImVec2 center, float radius, ImU32 col,
	bool urgent )
{
	float t = (float)ImGui::GetTime();
	float phase, alpha, rmul;
	if ( urgent )
	{
		// Sharp 0.6s blink — 60% of cycle bright, 40% dim.
		float p = fmodf( t * 1.6f, 1.f );
		alpha = ( p < 0.6f ) ? 1.f : 0.30f;
		rmul  = ( p < 0.6f ) ? 1.0f : 0.85f;
	}
	else
	{
		// Gentle 1.6s sinusoid — soft alarm, ±20% radius modulation.
		phase = fmodf( t * 0.625f, 1.f );
		float s = 0.5f + 0.5f * sinf( phase * 6.2831853f );
		alpha = 0.55f + 0.45f * s;
		rmul  = 0.85f + 0.20f * s;
	}
	float r = radius * rmul;

	// CRT phosphor afterglow — outer halo decays through warm-amber regardless
	// of source colour, simulating the persistent glow phosphor leaves on a
	// scope tube. Subtle (alpha ≤ 28) so it reads as ambient, not chromatic.
	ImU32 phosphor = IM_COL32( 0xd4, 0xa1, 0x4a,
		(int)( alpha * 28.f ) );
	dl->AddCircleFilled( center, r * 3.2f, phosphor, 20 );

	// Halo ring + filled core.
	ImU32 halo  = ( col & 0x00FFFFFF ) | ( (ImU32)( alpha * 60.f ) << 24 );
	ImU32 mid   = ( col & 0x00FFFFFF ) | ( (ImU32)( alpha * 110.f ) << 24 );
	ImU32 core  = ( col & 0x00FFFFFF ) | ( (ImU32)( alpha * 255.f ) << 24 );
	dl->AddCircleFilled( center, r * 2.4f, halo, 18 );
	dl->AddCircleFilled( center, r * 1.5f, mid,  18 );
	dl->AddCircleFilled( center, r,        core, 18 );
}

// CRT scope sweep — a thin moving phosphor trace under a horizontal span.
// Visual reference: oscilloscope beam tracing left→right with afterglow.
// Used for SUSPECT/DEAD watchdog underlines instead of generic dot pulses
// — feels like the instrument is *measuring* the bot's heartbeat live.
//   period_s = full sweep duration (1.4s SUSPECT, 0.7s DEAD).
static void DrawScopeSweep( ImDrawList* dl, ImVec2 mn, ImVec2 mx,
	ImU32 col, float period_s )
{
	float t = (float)ImGui::GetTime();
	if ( period_s < 0.05f ) period_s = 0.05f;
	float p = fmodf( t / period_s, 1.f );  // 0..1

	float w = mx.x - mn.x;
	float headX = mn.x + w * p;

	// Background trace — full width 1px, very faint (the scope grid line).
	ImU32 dim = ( col & 0x00FFFFFF ) | ( 28u << 24 );
	dl->AddLine( ImVec2( mn.x, mn.y ), ImVec2( mx.x, mn.y ), dim, 1.f );

	// Afterglow — short tail behind the head, exponential alpha decay.
	const int N = 12;
	float tailLen = w * 0.28f;
	for ( int i = 0; i < N; i++ )
	{
		float u = (float)i / (float)( N - 1 );        // 0..1 along tail
		float x1 = headX - u * tailLen;
		float x0 = headX - ( u + 1.f / N ) * tailLen;
		if ( x1 < mn.x ) continue;
		if ( x0 < mn.x ) x0 = mn.x;
		float fa = expf( -u * 3.5f );                  // fast decay
		ImU32 cc = ( col & 0x00FFFFFF ) | ( (ImU32)( fa * 220.f ) << 24 );
		dl->AddRectFilled( ImVec2( x0, mn.y ),
			ImVec2( x1, mn.y + ( mx.y - mn.y ) ), cc );
	}
	// Hot head — 2px bright pixel.
	dl->AddRectFilled(
		ImVec2( headX - 1.f, mn.y - 1.f ),
		ImVec2( headX + 1.f, mn.y + ( mx.y - mn.y ) ),
		col );
}

float FadeInBlock( ImGuiID id, bool active, float ms )
{
	float a = Animate( id, active ? 1.f : 0.f, ms );
	if ( a < 0.f ) a = 0.f;
	if ( a > 1.f ) a = 1.f;
	return a;
}

float WatchdogPill( ImDrawList* dl, ImVec2 origin, WatchdogState state,
	const char* extra_label )
{
	const char* label;
	ImU32 dotCol, textCol, borderCol, bgCol;
	bool pulse, urgent;
	switch ( state )
	{
	case WatchdogState::Healthy:
		label = "HEALTHY"; dotCol = kColSignal;  textCol = kColSignal;
		borderCol = kColSignalDim; bgCol = IM_COL32( 0x14, 0x22, 0x18, 0xFF );
		pulse = false; urgent = false; break;
	case WatchdogState::Suspect:
		label = "SUSPECT"; dotCol = kColWarn;    textCol = kColWarn;
		borderCol = IM_COL32( 0x5a, 0x43, 0x1f, 0xFF );
		bgCol = IM_COL32( 0x2a, 0x20, 0x10, 0xFF );
		pulse = true;  urgent = false; break;
	case WatchdogState::Dead:
		label = "DEAD";    dotCol = kColCrash;   textCol = kColCrash;
		borderCol = kColCrashDim; bgCol = IM_COL32( 0x2a, 0x10, 0x0e, 0xFF );
		pulse = true;  urgent = true;  break;
	default:
		label = "—";       dotCol = kColIdle;    textCol = kColInkMute;
		borderCol = kColLineHot;  bgCol = kColBg2;
		pulse = false; urgent = false; break;
	}

	ImFont* f = gFontMonoSm ? gFontMonoSm : ImGui::GetFont();
	float fs = gFontMonoSm ? gFontMonoSm->FontSize : ImGui::GetFontSize();
	ImVec2 ts = f->CalcTextSizeA( fs, FLT_MAX, 0, label );
	float dotR = 3.f;
	float padX = 7.f, padY = 3.f;
	float gap  = 6.f;
	float extraW = 0.f;
	ImVec2 extraTs( 0, 0 );
	if ( extra_label && *extra_label )
	{
		extraTs = f->CalcTextSizeA( fs, FLT_MAX, 0, extra_label );
		extraW = extraTs.x + 6.f;
	}
	float w = padX + dotR * 2.f + gap + ts.x + extraW + padX;
	float h = ts.y + padY * 2.f;

	ImVec2 mn = origin;
	ImVec2 mx = ImVec2( origin.x + w, origin.y + h );

	// Filled body + border.
	dl->AddRectFilled( mn, mx, bgCol );
	dl->AddRect      ( mn, mx, borderCol, 0.f, 0, 1.f );

	// Dot.
	ImVec2 dotC( mn.x + padX + dotR, mn.y + h * 0.5f );
	if ( pulse )
		PulseDot( dl, dotC, dotR, dotCol, urgent );
	else
	{
		dl->AddCircleFilled( dotC, dotR + 1.f,
			( dotCol & 0x00FFFFFF ) | ( 60u << 24 ), 14 );
		dl->AddCircleFilled( dotC, dotR, dotCol, 14 );
	}

	// Label.
	dl->AddText( f, fs,
		ImVec2( mn.x + padX + dotR * 2.f + gap, mn.y + padY ),
		textCol, label );

	if ( extra_label && *extra_label )
	{
		dl->AddText( f, fs,
			ImVec2( mn.x + padX + dotR * 2.f + gap + ts.x + 6.f,
			        mn.y + padY ),
			kColInkDim, extra_label );
	}

	// CRT scope sweep underline — alarms (SUSPECT/DEAD) get a moving phosphor
	// trace under the pill instead of a flat highlight. Reads as the watchdog
	// is *actively* scanning the bot, not just sitting in a coloured state.
	// Healthy/Unknown skip the sweep — they're stable, no measurement is
	// ongoing.
	if ( pulse )
	{
		float swPad = 4.f;
		ImVec2 swMn( mn.x + swPad, mx.y + 1.f );
		ImVec2 swMx( mx.x - swPad, mx.y + 3.f );
		DrawScopeSweep( dl, swMn, swMx, dotCol, urgent ? 0.7f : 1.4f );
	}

	return h;
}

// ── Generic helpers ─────────────────────────────────────────────────────

bool CopyToClipboard( const std::string& text )
{
	if ( !OpenClipboard( nullptr ) ) return false;
	if ( !EmptyClipboard() ) { CloseClipboard(); return false; }

	// UTF-8 → UTF-16 — Windows clipboard CF_UNICODETEXT is wide.
	int wlen = MultiByteToWideChar( CP_UTF8, 0, text.c_str(), (int)text.size(),
	                                 nullptr, 0 );
	HGLOBAL hMem = GlobalAlloc( GMEM_MOVEABLE, ( wlen + 1 ) * sizeof( wchar_t ) );
	if ( !hMem ) { CloseClipboard(); return false; }
	auto* buf = (wchar_t*)GlobalLock( hMem );
	if ( !buf ) { GlobalFree( hMem ); CloseClipboard(); return false; }
	MultiByteToWideChar( CP_UTF8, 0, text.c_str(), (int)text.size(), buf, wlen );
	buf[wlen] = 0;
	GlobalUnlock( hMem );

	HANDLE r = SetClipboardData( CF_UNICODETEXT, hMem );
	CloseClipboard();
	if ( !r )
	{
		// Ownership returns to us only on failure; on success the system owns hMem.
		GlobalFree( hMem );
		return false;
	}
	return true;
}

void Pill( const char* label, ImU32 color )
{
	ImVec2 textSize = ImGui::CalcTextSize( label );
	float  padX     = 6.0f;
	float  padY     = 2.0f;
	ImVec2 cursor   = ImGui::GetCursorScreenPos();
	ImVec2 size( textSize.x + padX * 2.f, textSize.y + padY * 2.f );

	ImDrawList* dl  = ImGui::GetWindowDrawList();
	float radius    = size.y * 0.5f;
	dl->AddRectFilled( cursor,
		ImVec2( cursor.x + size.x, cursor.y + size.y ),
		color, radius );

	// White text — assumes dark theme base. Caller picks fill color
	// dark enough that white contrasts.
	dl->AddText(
		ImVec2( cursor.x + padX, cursor.y + padY ),
		IM_COL32( 255, 255, 255, 255 ),
		label );

	ImGui::Dummy( size );
}

} // namespace theme
