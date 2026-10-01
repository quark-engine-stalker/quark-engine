// file:		UITextureMaster.h
// description:	holds info about shared textures. able to initialize external
//				through IUITextureControl interface
// created:		11.05.2005
// author:		Serge Vynnychenko
// mail:		narrator@gsc-game.kiev.ua
//
// copyright 2005 GSC Game World

#pragma once

class CUIStaticItem;
#include "ui_defs.h"

struct TEX_INFO
{
	shared_str file;
	Frect rect;
	LPCSTR get_file_name() { return *file; }
	Frect get_rect() { return rect; }
};

struct sh_pair
{
	shared_str texture_name;
	shared_str shader_name;

	bool operator <(const sh_pair& other) const
	{
		if (texture_name < other.texture_name)
			return true;
		if (other.texture_name < texture_name)
			return false;
		return shader_name < other.shader_name;
	}
};

struct ui_shader_cache_entry
{
	ui_shader shader;
	u32 last_used_frame;

	ui_shader_cache_entry() : last_used_frame(0) {}
};

class CUITextureMaster
{
public:

	static void ParseShTexInfo(LPCSTR xml_file);
	static void FreeTexInfo();
	static void FreeCachedShaders();
	static void TrimCachedShaders(u32 current_frame, u32 max_age_frames, u32 max_releases);

	static void InitTexture(const shared_str& texture_name, CUIStaticItem* tc,
	                        const shared_str& shader_name = "hud\\default");
	static void InitTexture(const shared_str& texture_name, const shared_str& shader_name, ui_shader& out_shader,
	                        Frect& out_rect);
	static float GetTextureHeight(const shared_str& texture_name);
	static float GetTextureWidth(const shared_str& texture_name);
	static Frect GetTextureRect(const shared_str& texture_name);
	static void GetTextureShader(const shared_str& texture_name, ui_shader& sh);
	static TEX_INFO FindItem(const shared_str& texture_name);
protected:
	IC static bool IsSh(const shared_str& texture_name);

	static xr_map<shared_str, TEX_INFO> m_textures;

	static xr_map<sh_pair, ui_shader_cache_entry> m_shaders;
};
