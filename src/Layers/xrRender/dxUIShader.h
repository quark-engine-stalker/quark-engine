#ifndef	dxUIShader_included
#define	dxUIShader_included
#pragma once

#include "..\..\Include\xrRender\UIShader.h"

struct dx_ui_shader_cache_entry
{
	ref_shader shader;
	u32 last_used_frame;

	dx_ui_shader_cache_entry() : last_used_frame(0) {}
};

extern xr_unordered_map<std::string, dx_ui_shader_cache_entry> g_UIShadersCache;
void TrimCachedUIShaders(u32 current_frame, u32 max_age_frames, u32 max_releases);

class dxUIShader : public IUIShader
{
	friend class dxUIRender;
	friend class dxDebugRender;
	friend class dxWallMarkArray;
	friend class CRender;
public:
	virtual ~dxUIShader() { ; }
	virtual void Copy(IUIShader& _in);
    virtual void create(LPCSTR sh, LPCSTR tex = nullptr, bool no_cache = false);
	virtual bool inited() { return hShader; }
    //virtual void destroy();
private:
	ref_shader hShader;
};

#endif	//	dxUIShader_included
