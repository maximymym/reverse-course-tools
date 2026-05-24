// hero_portraits.h — lazy-loaded Dota 2 hero portrait textures (PNG → DX11 SRV).
//
// Files: assets/heroes/<short_name>.png (e.g. skeleton_king.png).
// Names accepted by HeroPortraits_Get():
//   * full   "npc_dota_hero_skeleton_king"
//   * short  "skeleton_king"
//   * mixed-case variants (normalised internally)
//
// First call for a hero loads the file via stb_image, creates a Texture2D +
// ShaderResourceView, and caches the SRV in a process-wide map keyed by short
// name. Misses (file not found / decode fail) cache nullptr so subsequent
// calls don't pound the disk.
#pragma once

#include <imgui.h>
#include <string>

struct ID3D11Device;

namespace hero_portraits
{

// Bind to the orchestrator's DX11 device + the on-disk assets/heroes/ folder.
// Call once after D3D11CreateDeviceAndSwapChain() and before the first GUI frame.
void Init( ID3D11Device* device, const std::string& assetsDir );

// Lazy-load (or fetch from cache) the SRV for a hero. `name` may be the full
// `npc_dota_hero_*` string from BotState.hero or a bare `skeleton_king`. Empty
// / "default" / "none" returns nullptr.
//
// Returned ImTextureID is owned by the cache — don't release it.
ImTextureID Get( const char* name );

// Image dimensions for the loaded SRV (zero if not loaded). Useful for crop
// math in the renderer (Valve serves ~256x144 ≈ 16:9 right now).
ImVec2 GetSize( const char* name );

// Release every SRV + Texture2D. Call before the DX11 device is released
// (i.e. inside gui::Shutdown() before g_pd3dDevice->Release()).
void Shutdown();

} // namespace hero_portraits
