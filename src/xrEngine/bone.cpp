#include "stdafx.h"
#pragma hdrstop

#include "bone.h"
#include "gamemtllib.h"


//////////////////////////////////////////////////////////////////////////
// Worker-safe bone callback registry. Registration is lock-free and lookups only
// read slots that are written once (NULL -> callback), so workers can query it
// while the main thread registers a callback without any lock.
ENGINE_API int g_bone_worker_callbacks = 1;

namespace
{
constexpr u32 worker_safe_callback_slots = 16;
void* volatile g_worker_safe_callbacks[worker_safe_callback_slots] = {};
}

void ENGINE_API bone_callback_mark_worker_safe(BoneCallback callback)
{
	if (!callback)
		return;

	void* const key = reinterpret_cast<void*>(callback);
	for (u32 slot = 0; slot < worker_safe_callback_slots; ++slot)
	{
		void* current = g_worker_safe_callbacks[slot];
		if (current == key)
			return;
		if (current)
			continue;

		void* const previous = InterlockedCompareExchangePointer(
			&g_worker_safe_callbacks[slot], key, nullptr);
		if (!previous || previous == key)
			return;
	}
	// The table is full: the callback simply stays main-thread-only, which is the
	// original behaviour. Never assert here - this is purely an optimization.
}

bool ENGINE_API bone_callback_is_worker_safe(BoneCallback callback)
{
	if (!callback || !g_bone_worker_callbacks)
		return false;

	void* const key = reinterpret_cast<void*>(callback);
	for (u32 slot = 0; slot < worker_safe_callback_slots; ++slot)
	{
		void* const current = g_worker_safe_callbacks[slot];
		if (!current)
			return false;
		if (current == key)
			return true;
	}
	return false;
}

namespace
{
constexpr u32 blocker_slots = 24;
SBoneCallbackBlocker g_blockers[blocker_slots];
u32 g_blocker_count = 0;
}

void ENGINE_API bone_callback_note_blocker(const void* fn, u32 type)
{
	for (u32 i = 0; i < g_blocker_count; ++i)
	{
		if (g_blockers[i].fn == fn && g_blockers[i].type == type)
		{
			++g_blockers[i].count;
			return;
		}
	}
	if (g_blocker_count < blocker_slots)
	{
		g_blockers[g_blocker_count].fn = fn;
		g_blockers[g_blocker_count].type = type;
		g_blockers[g_blocker_count].count = 1;
		++g_blocker_count;
	}
}

u32 ENGINE_API bone_callback_get_blockers(SBoneCallbackBlocker* out, u32 max_count)
{
	const u32 n = g_blocker_count < max_count ? g_blocker_count : max_count;
	for (u32 i = 0; i < n; ++i)
		out[i] = g_blockers[i];
	return n;
}

void ENGINE_API bone_callback_reset_blockers()
{
	g_blocker_count = 0;
}

u16 CBone::get_game_mtl_idx() const
{
	return GMLib.GetMaterialIdx(game_mtl.c_str());
}

static const Fobb dummy = Fobb().identity();

const Fobb& CBone::get_obb() const
{
	return dummy;
}

//////////////////////////////////////////////////////////////////////////
// BoneInstance methods


void ENGINE_API CBoneInstance::set_param(u32 idx, float data)
{
	VERIFY(idx < MAX_BONE_PARAMS);
	param[idx] = data;
}

float ENGINE_API CBoneInstance::get_param(u32 idx)
{
	VERIFY(idx < MAX_BONE_PARAMS);
	return param[idx];
}

#ifdef DEBUG
void ENGINE_API CBoneData::DebugQuery(BoneDebug& L)
{
    for (u32 i = 0; i < children.size(); i++)
    {
        L.push_back(SelfID);
        L.push_back(children[i]->SelfID);
        children[i]->DebugQuery(L);
    }
}
#endif

void ENGINE_API CBoneData::CalculateM2B(const Fmatrix& parent)
{
	// Build matrix
	m2b_transform.mul_43(parent, bind_transform);

	// Calculate children
	for (xr_vector<CBoneData*>::iterator C = children.begin(); C != children.end(); C++)
		(*C)->CalculateM2B(m2b_transform);

	m2b_transform.invert();
}
