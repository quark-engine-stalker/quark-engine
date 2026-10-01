#pragma once

#include "light.h"
#include "Light_Package.h"

class CLight_DB
{
private:
	xr_vector<ref_light> v_static;
	xr_vector<ref_light> v_hemi;
	// Rendering packages expose raw light pointers for the hot path. Keep dynamic
	// sources alive until the next package rebuild so a transient/precache/script
	// owner cannot release a light after submission but before render_lights().
	xr_vector<ref_light> frame_lights;
public:
	ref_light sun_original;
	ref_light sun_adapted;
	light_Package package;
public:
	void add_light(light* L);

	void Load(IReader* fs);
	void					LoadHemi			();
	void Unload();

	light* Create();
	void Update();

	CLight_DB();
	~CLight_DB();
};
