#include "pch_script.h"
#include "ai_space.h"
#include "script_engine.h"
#include "ActorEffector.h"
#include "../xrEngine/ObjectAnimator.h"
#include "Actor.h"

void CActorCameraManager::run_script_callbacks()
{
	xr_vector<shared_str> callbacks;
	callbacks.swap(m_script_callbacks);
	for (const shared_str& cb : callbacks)
	{
		::luabind::functor<LPCSTR> fl;
		R_ASSERT(ai().script_engine().functor<LPCSTR>(*cb, fl));
		fl();
	}
}

void CAnimatorCamEffectorScriptCB::ProcessIfInvalid(SCamEffectorInfo& info)
{
	if (m_bAbsolutePositioning)
	{
		const Fmatrix& m = m_objectAnimator->XFORM();
		info.d = m.k;
		info.n = m.j;
		info.p = m.c;
		if (m_fov > 0.0f)
			info.fFov = m_fov;
	}
}

BOOL CAnimatorCamEffectorScriptCB::Valid()
{
	BOOL res = inherited::Valid();
	if (!res && cb_name.size())
	{
		// CRASH-LOG 29.10: called inside CCameraManager::UpdateCamEffectors, a callback that removed
		// cam effectors left the list walk on a deleted one
		Actor()->Cameras().defer_script_callback(cb_name);
		cb_name = "";
	}
	return res;
}
