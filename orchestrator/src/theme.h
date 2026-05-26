// theme.h — DotaFarm "industrial telemetry / CRT oscilloscope" visual language
//
// Design language: tarnished gold accents on near-black, hard chamfered corners,
// monospaced everything, ASCII section bars, engineering brackets.
//
// Reference: design_mockup/index.html
#pragma once

#include <imgui.h>
#include <math.h>  // powf — used by EaseOutExpo inline
#include <string>

struct ImDrawList;

namespace theme
{

// ── Palette ─────────────────────────────────────────────────────────────
// Direction: Industrial Telemetry / CRT Oscilloscope — committed.
// Hierarchy: 1 dominant accent (tarnished gold = the brand), 1 "live"
// phosphor signal (deep amber-green, NOT pastel mint), 1 critical (deep
// CRT-red with cherenkov undertone), 1 cool secondary (queue, used
// sparingly). Backgrounds collapsed to 3 tiers — 4 was timid distribution
// of warm-near-black, 3 is decisive (page / panel / nested).
//
// All status accents are pulled DOWN in saturation+luma so gold remains
// the eye-anchor. The mockup CSS values are kept as fallback comments;
// the live values are CRT-tuned overrides.
constexpr ImU32 kColBg0       = IM_COL32( 0x07, 0x06, 0x04, 0xFF ); // page — deeper, near-true-black
constexpr ImU32 kColBg1       = IM_COL32( 0x11, 0x0e, 0x09, 0xFF ); // panel
constexpr ImU32 kColBg2       = IM_COL32( 0x1b, 0x16, 0x0d, 0xFF ); // nested
constexpr ImU32 kColBg3       = IM_COL32( 0x2c, 0x23, 0x12, 0xFF ); // hover — bumped delta so hover is *visible*
constexpr ImU32 kColLine      = IM_COL32( 0x2a, 0x24, 0x18, 0xFF );
constexpr ImU32 kColLineHot   = IM_COL32( 0x55, 0x44, 0x22, 0xFF ); // brighter than line so brackets read

constexpr ImU32 kColInk       = IM_COL32( 0xd8, 0xc9, 0xa8, 0xFF );
constexpr ImU32 kColInkDim    = IM_COL32( 0x8a, 0x7e, 0x64, 0xFF );
constexpr ImU32 kColInkMute   = IM_COL32( 0x5a, 0x51, 0x42, 0xFF );
constexpr ImU32 kColInkBright = IM_COL32( 0xf6, 0xe8, 0xc2, 0xFF ); // bumped highlight so headlines pop

// PRIMARY accent — tarnished gold. Used for brand, all primary CTAs,
// section headers, healthy/active emphasis. Two tiers (live + deep) plus
// a low-alpha glow for halos around LEDs and active tabs.
constexpr ImU32 kColGold      = IM_COL32( 0xd4, 0xa1, 0x4a, 0xFF );
constexpr ImU32 kColGoldDeep  = IM_COL32( 0x8a, 0x66, 0x22, 0xFF );
constexpr ImU32 kColGoldGlow  = IM_COL32( 0xd4, 0xa1, 0x4a, 0x4D );

// SECONDARY signals — CRT-phosphor tuned. Pulled DOWN in saturation so
// gold stays the eye-anchor. Naming preserved for ABI compatibility but
// the values are now part of a unified amber-spectrum for healthy/queue
// states (only crash breaks the spectrum, intentionally).
//
//   Signal  = phosphor amber-green   (CRT P3, healthy / in-game)
//   Queue   = cool steel             (matchmaking lobby)
//   Warn    = amber                  (loading, suspect)
//   Crash   = deep CRT red           (fault, dead)
//   Idle    = warm grey              (cold, standby)
constexpr ImU32 kColSignal    = IM_COL32( 0x9c, 0xc4, 0x4a, 0xFF ); // amber-green phosphor
constexpr ImU32 kColSignalDim = IM_COL32( 0x42, 0x52, 0x1d, 0xFF );
constexpr ImU32 kColQueue     = IM_COL32( 0x6d, 0x95, 0xb2, 0xFF ); // cooler, less candy
constexpr ImU32 kColQueueDim  = IM_COL32( 0x2a, 0x3b, 0x4a, 0xFF );
constexpr ImU32 kColWarn      = IM_COL32( 0xd9, 0x97, 0x3a, 0xFF ); // closer to gold family
constexpr ImU32 kColCrash     = IM_COL32( 0xc4, 0x3f, 0x32, 0xFF ); // deeper CRT-red, less candy
constexpr ImU32 kColCrashDim  = IM_COL32( 0x52, 0x1c, 0x14, 0xFF );
constexpr ImU32 kColIdle      = IM_COL32( 0x6d, 0x64, 0x53, 0xFF );

// ImVec4 wrappers (used by ImGui::TextColored etc.)
inline ImVec4 V( ImU32 c )
{
	return ImVec4(
		( ( c       ) & 0xFF ) / 255.f,
		( ( c >>  8 ) & 0xFF ) / 255.f,
		( ( c >> 16 ) & 0xFF ) / 255.f,
		( ( c >> 24 ) & 0xFF ) / 255.f );
}

// ── Fonts ───────────────────────────────────────────────────────────────
// Loaded by LoadDotaFarmFonts(). Use with ImGui::PushFont / PopFont.
extern ImFont* gFontSerifLg;   // 22px display serif (Cinzel-style) — wordmark, big numbers, indices
extern ImFont* gFontSerifMd;   // 16px serif — hero names, panel titles
extern ImFont* gFontMono;      // 13px monospace — body, PIDs, hex
extern ImFont* gFontMonoSm;    // 11px monospace — labels, badges
extern ImFont* gFontMonoLg;    // 15px monospace — sub-headers
extern ImFont* gFontSans;      // 13px sans — fallback body when serif is wrong

// Apply colors + StyleVars matching the mockup. Call once after ImGui::CreateContext().
void ApplyDotaFarmTheme();

// Add fonts to io.Fonts. Call before backend Init() (DX11). Returns false on
// missing system fonts — should be impossible on a Windows box.
bool LoadDotaFarmFonts( ImGuiIO& io );

// ── Custom drawing primitives ───────────────────────────────────────────
// Asymmetric chamfered rectangle (matches CSS clip-path from mockup).
// Cuts top-right and bottom-left corners by 'chamfer' pixels. thickness=0
// → filled, thickness>0 → border only.
void DrawChamferedRect( ImDrawList* dl, ImVec2 mn, ImVec2 mx,
	float chamfer, ImU32 col, float thickness = 0.f );

// Engineering corner brackets (4 short L-shapes at the 4 corners).
void DrawCornerBrackets( ImDrawList* dl, ImVec2 mn, ImVec2 mx,
	float len, float thickness, ImU32 col );

// Faint horizontal scanlines for CRT atmosphere.
void DrawScanlines( ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, int spacing = 3 );

// Dotted engineering grid (radial dots on a step grid).
void DrawDottedGrid( ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float step = 28.f );

// Pulsing / blinking LED indicator with optional outer glow halo.
// pulse_phase ∈ [0,1) — drives alpha modulation. Pass ImGui::GetTime() for default.
void DrawLED( ImDrawList* dl, ImVec2 center, float radius, ImU32 col,
	float pulse_phase, bool blink = false );

// Solid square LED (mockup .led — 10×10 with 1px border).
void DrawLEDSquare( ImDrawList* dl, ImVec2 center, float size, ImU32 col,
	ImU32 border, float pulse_phase, bool blink = false );

// Segmented HP bar — like the mockup .hpbar with diagonal hatching.
void DrawHpBar( ImDrawList* dl, ImVec2 mn, ImVec2 mx, float fill, ImU32 col, ImU32 bg );

// Animated streaming progress bar (loading/queue indicator).
void DrawStreamingProgress( ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col,
	float t /* ImGui::GetTime() */, float speed = 1.f );

// Boxed index plate (Cinzel digit framed in line-hot border on bg2).
void DrawIndexPlate( ImDrawList* dl, ImVec2 mn, ImVec2 mx, const char* digit );

// Small chamfered button border (used by RenderChamferedButton).
// Returns true on click. Mimics ImGui::Button but with mockup styling.
bool ChamferedButton( const char* label, ImVec2 size, ImU32 fill, ImU32 border,
	ImU32 text_col, bool primary = false );

// Section separator: "── LABEL ─────────────" (ASCII bar, gold-deep).
void SectionBar( const char* label );

// ── Animation helpers ───────────────────────────────────────────────────
// All animation helpers are frame-time aware (use ImGui::GetIO().DeltaTime)
// and self-contained — no manual phase tracking on the call site. Calling
// the same id repeatedly with a new target value produces a smooth easing
// curve toward that target. Sticky map is keyed by ImGuiID, so caller IDs
// must be stable across frames (use ImGui::GetID() with a stable label).

inline float Lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

// Ease-out-expo (single source-of-truth curve, matches design_mockup CSS
// cubic-bezier(0.22, 1, 0.36, 1) editorial timing).
inline float EaseOutExpo( float t )
{
	if ( t <= 0.f ) return 0.f;
	if ( t >= 1.f ) return 1.f;
	return 1.f - powf( 2.f, -10.f * t );
}

// Smooth-step the per-id float toward target with an exponential time
// constant of `ms` milliseconds (≈5τ → ~99% of target). Returns the new
// current value. Default 200ms matches the editorial enter-timing budget.
//   float a = Animate( ImGui::GetID("hp_bar"), (float)bot.hp, 240.f );
float Animate( ImGuiID id, float target, float ms = 200.f );

// Quick reset / snapshot — useful when the underlying object identity
// changes (e.g. bot slot reassigned to a different account) and we don't
// want a long-distance lerp. Sets the stored value directly.
void AnimateSnap( ImGuiID id, float value );

// Soft pulsing dot — radius modulates ±20% on a 1.6s sinusoid, plus a
// translucent halo ring. Used for SUSPECT / DEAD watchdog states where a
// gentle alarm is appropriate without LED-blink hostility. Alpha blink for
// `urgent=true` (DEAD) — sharper edge, matches mockup .led-crash semantics.
void PulseDot( ImDrawList* dl, ImVec2 center, float radius, ImU32 col,
	bool urgent = false );

// Cross-fade alpha for stateful blocks (screens, sections). Returns a 0..1
// alpha that animates between 0 (when active=false) and 1 (when active=true)
// over `ms` milliseconds. Wrap calls in PushStyleVar(ImGuiStyleVar_Alpha,...)
// so the whole block fades. Returns 0 when the block should be skipped.
float FadeInBlock( ImGuiID id, bool active, float ms = 200.f );

// Watchdog pill — small chamfered badge (HEALTHY / SUSPECT / DEAD) with a
// pulsing dot for non-healthy states. Caller passes already-classified
// state to keep render concerns separated from heartbeat math. Width
// auto-computed from label. Returns the pill's bounding box height.
enum class WatchdogState { Healthy, Suspect, Dead, Unknown };

float WatchdogPill( ImDrawList* dl, ImVec2 origin, WatchdogState state,
	const char* extra_label = nullptr );

// ── Generic helpers (Pair Code workflow, T9/T10) ────────────────────────
// Copy UTF-8 text to system clipboard (CF_UNICODETEXT). Synchronous, < 1ms.
// Returns true on success, false if clipboard could not be opened or
// allocation failed. Failures are silent — caller decides whether to surface.
bool CopyToClipboard( const std::string& text );

// Small rounded badge with text — minimal Pill (NOT the stateful WatchdogPill
// above). Renders inline at current cursor position via DrawList, reserves
// layout space via ImGui::Dummy so it composes with SameLine(). Text is
// drawn in white assuming the dark base palette. 16px nominal height (text
// height + 2*padY where padY=2).
void Pill( const char* label, ImU32 color );

} // namespace theme
