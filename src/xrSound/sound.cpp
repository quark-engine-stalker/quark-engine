#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreA.h"

XRSOUND_API xr_token* snd_devices_token = NULL;
XRSOUND_API xr_string snd_device_name;

void CSound_manager_interface::_create(int stage)
{
	if (stage == 0)
	{
		CSoundRender_CoreA* sound_render = xr_new<CSoundRender_CoreA>();
		InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&SoundRenderA), sound_render);
		SoundRender = sound_render;
		Sound = SoundRender;

		if (strstr(Core.Params, "-nosound"))
		{
			SoundRender->bPresent = FALSE;
			return;
		}
		else
			SoundRender->bPresent = TRUE;
	}

	if (!SoundRender->bPresent) return;
	Sound->_initialize(stage);
}

void CSound_manager_interface::_destroy()
{
	InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&SoundRenderA), nullptr);
	Sound->_clear();
	Sound = nullptr;
	xr_delete(SoundRender);
}
