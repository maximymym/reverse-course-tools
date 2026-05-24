// hero_portraits.cpp — see header.
#include "hero_portraits.h"

#include <d3d11.h>
#include <Windows.h>
#include <unordered_map>
#include <string>
#include <mutex>
#include <cstring>
#include <cctype>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace hero_portraits
{

namespace {

struct Entry
{
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11Texture2D*          tex = nullptr;
	int                       w = 0;
	int                       h = 0;
	bool                      tried = false;  // attempted load (success or fail)
};

ID3D11Device*                              g_dev = nullptr;
std::string                                g_assetsDir;
std::unordered_map<std::string, Entry>     g_cache;
std::mutex                                 g_mu;

// Strip "npc_dota_hero_" prefix and lower-case everything.
std::string Normalise( const char* in )
{
	if ( !in || !*in ) return {};
	std::string s = in;
	const std::string pfx = "npc_dota_hero_";
	if ( s.size() > pfx.size() &&
	     _strnicmp( s.c_str(), pfx.c_str(), (int)pfx.size() ) == 0 )
		s.erase( 0, pfx.size() );
	for ( auto& c : s ) c = (char)std::tolower( (unsigned char)c );
	// Trim any whitespace.
	while ( !s.empty() && ( s.back() == ' ' || s.back() == '\r' || s.back() == '\n' ) )
		s.pop_back();
	return s;
}

bool IsBlank( const std::string& s )
{
	if ( s.empty() ) return true;
	if ( s == "default" || s == "none" || s == "—" || s == "-" ) return true;
	return false;
}

// Load PNG from disk, create Texture2D + SRV.
bool LoadPng( const std::string& path, Entry& out )
{
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load( path.c_str(), &w, &h, &n, 4 );  // force RGBA
	if ( !px )
		return false;

	D3D11_TEXTURE2D_DESC td{};
	td.Width            = w;
	td.Height           = h;
	td.MipLevels        = 1;
	td.ArraySize        = 1;
	td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage            = D3D11_USAGE_DEFAULT;
	td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA sd{};
	sd.pSysMem     = px;
	sd.SysMemPitch = (UINT)( w * 4 );

	ID3D11Texture2D* tex = nullptr;
	HRESULT hr = g_dev->CreateTexture2D( &td, &sd, &tex );
	stbi_image_free( px );
	if ( FAILED( hr ) || !tex )
		return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format                    = td.Format;
	sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	sv.Texture2D.MostDetailedMip = 0;
	sv.Texture2D.MipLevels       = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	hr = g_dev->CreateShaderResourceView( tex, &sv, &srv );
	if ( FAILED( hr ) || !srv )
	{
		tex->Release();
		return false;
	}

	out.srv = srv;
	out.tex = tex;
	out.w   = w;
	out.h   = h;
	return true;
}

} // namespace

void Init( ID3D11Device* device, const std::string& assetsDir )
{
	std::lock_guard<std::mutex> lk( g_mu );
	g_dev       = device;
	g_assetsDir = assetsDir;
}

ImTextureID Get( const char* name )
{
	std::string key = Normalise( name );
	if ( IsBlank( key ) || !g_dev )
		return (ImTextureID)nullptr;

	std::lock_guard<std::mutex> lk( g_mu );

	auto it = g_cache.find( key );
	if ( it != g_cache.end() )
		return (ImTextureID)it->second.srv;  // may be nullptr if previous miss

	Entry& e = g_cache[ key ];
	e.tried  = true;

	std::string path = g_assetsDir + "\\" + key + ".png";
	if ( !LoadPng( path, e ) )
	{
		e.srv = nullptr;
		e.tex = nullptr;
		e.w = e.h = 0;
	}
	return (ImTextureID)e.srv;
}

ImVec2 GetSize( const char* name )
{
	std::string key = Normalise( name );
	if ( IsBlank( key ) ) return ImVec2( 0, 0 );

	std::lock_guard<std::mutex> lk( g_mu );
	auto it = g_cache.find( key );
	if ( it == g_cache.end() ) return ImVec2( 0, 0 );
	return ImVec2( (float)it->second.w, (float)it->second.h );
}

void Shutdown()
{
	std::lock_guard<std::mutex> lk( g_mu );
	for ( auto& kv : g_cache )
	{
		if ( kv.second.srv ) kv.second.srv->Release();
		if ( kv.second.tex ) kv.second.tex->Release();
	}
	g_cache.clear();
	g_dev = nullptr;
}

} // namespace hero_portraits
