#include "stdafx.h"
#include "render.h"

thread_local bool IRender_interface::hud_loading = false;

IRender_interface::~IRender_interface()
{
};

//ENGINE_API IRender_interface* Render = NULL;

// resources
IRender_Light::~IRender_Light()
{
	::Render->light_destroy(this);
}

IRender_Glow::~IRender_Glow()
{
	::Render->glow_destroy(this);
}
