#include "stdafx.h"
#include "dxUIShader.h"

namespace
{
constexpr size_t kMaxCachedUIShaders = 2048;
}

xr_unordered_map<std::string, dx_ui_shader_cache_entry> g_UIShadersCache;

void TrimCachedUIShaders(u32 current_frame, u32 max_age_frames, u32 max_releases)
{
	if (!max_releases || g_UIShadersCache.empty())
		return;

	u32 released = 0;

	// First retire genuinely stale cache ownership. Live dxUIShader instances keep
	// their own ref_shader copy, so removing a cache entry cannot invalidate UI.
	for (auto it = g_UIShadersCache.begin(); it != g_UIShadersCache.end() && released < max_releases;)
	{
		const u32 age = current_frame - it->second.last_used_frame;
		if (age >= max_age_frames)
		{
			it = g_UIShadersCache.erase(it);
			++released;
		}
		else
		{
			++it;
		}
	}

	// A script/mod can synthesize unique UI shader+texture pairs. Keep a hard
	// ownership budget as a backstop instead of allowing permanent session growth.
	for (auto it = g_UIShadersCache.begin();
		g_UIShadersCache.size() > kMaxCachedUIShaders && it != g_UIShadersCache.end() && released < max_releases;)
	{
		it = g_UIShadersCache.erase(it);
		++released;
	}

	if (g_UIShadersCache.empty())
		g_UIShadersCache.rehash(0);
}

static ref_shader& GetCachedUIShader(const char* sh, const char* tex)
{
	// Do not wait for the periodic renderer maintenance if a malformed/dynamic UI
	// starts generating cache keys rapidly in one frame.
	if (g_UIShadersCache.size() >= kMaxCachedUIShaders + 64)
		TrimCachedUIShaders(Device.dwFrame, 1800, 64);

	const char* const texture_name = tex ? tex : "";
	const char* const shader_name = sh ? sh : "";
	std::string key;
	key.reserve(xr_strlen(texture_name) + xr_strlen(shader_name) + 1);
	key.append(texture_name);
	key.push_back('\0');
	key.append(shader_name);

	if (const auto it = g_UIShadersCache.find(key); it != g_UIShadersCache.end())
	{
		it->second.last_used_frame = Device.dwFrame;
		return it->second.shader;
	}

	dx_ui_shader_cache_entry& entry = g_UIShadersCache[std::move(key)];
	entry.shader.create(sh, tex);
	entry.last_used_frame = Device.dwFrame;
	return entry.shader;
}

void dxUIShader::Copy(IUIShader& _in)
{
	*this = *((dxUIShader*)&_in);
}

void dxUIShader::create(LPCSTR sh, LPCSTR tex, bool no_cache)
{
	if (no_cache)
	{
		hShader.create(sh, tex);
	}
	else
	{
		hShader = GetCachedUIShader(sh, tex);
	}
}

//void dxUIShader::destroy() { hShader.destroy(); }
