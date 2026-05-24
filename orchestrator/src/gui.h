#pragma once

#include "orchestrator.h"
#include "auth.h"

#include <d3d11.h>
#include <Windows.h>
#include <string>

struct ImFont;

namespace gui
{

// DX11 device objects
inline ID3D11Device*           g_pd3dDevice = nullptr;
inline ID3D11DeviceContext*    g_pd3dCtx = nullptr;
inline IDXGISwapChain*         g_pSwapChain = nullptr;
inline ID3D11RenderTargetView* g_pRTV = nullptr;
inline HWND                    g_hwnd = nullptr;

inline ImFont* g_fontBold = nullptr;
inline bool    g_exitRequested = false;

// Auth state (managed by GUI)
inline bool          g_authenticated = false;
inline std::string   g_licenseKey;
inline std::string   g_exeDir;
inline auth::AuthResult g_authResult;

// Create standalone window + DX11 + ImGui
bool Init( int width = 600, int height = 500 );

// Run main loop — blocks until window closed
void Run( Orchestrator& orch );

// Cleanup
void Shutdown();

// Key persistence
std::string LoadSavedKey( const std::string& exeDir );
void SaveKey( const std::string& exeDir, const std::string& key );

} // namespace gui
