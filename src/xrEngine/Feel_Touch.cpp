#include "stdafx.h"
#include "igame_level.h"
#include "feel_touch.h"
#include "xr_object.h"
using namespace Feel;

namespace
{
	constexpr u8 touch_lookup_current = 1u << 0;
	constexpr u8 touch_lookup_nearest = 1u << 1;
	constexpr u8 touch_lookup_denied = 1u << 2;

	IC bool touch_time_reached(DWORD now, DWORD deadline)
	{
		return static_cast<s32>(now - deadline) > 0;
	}

	IC bool touch_time_later(DWORD candidate, DWORD current)
	{
		return static_cast<s32>(candidate - current) > 0;
	}
}

Touch::Touch() : pure_relcase(this, &Touch::feel_touch_relcase)
{
}

Touch::~Touch()
{
}

bool Touch::feel_touch_contact(CObject* O)
{
	return true;
}

void Touch::feel_touch_deny(CObject* O, DWORD T)
{
	if (!O)
		return;

	const DWORD expire = Device.dwTimeGlobal + T;
	for (DenyTouch& denied : feel_touch_disable)
	{
		if (denied.O != O)
			continue;

		if (touch_time_later(expire, denied.Expire))
			denied.Expire = expire;
		return;
	}

	DenyTouch denied;
	denied.O = O;
	denied.Expire = expire;
	feel_touch_disable.push_back(denied);
}

void Touch::feel_touch_update(Fvector& C, float R)
{
	const DWORD dwT = Device.dwTimeGlobal;

	// Expire denied objects without releasing the vector capacity.
	feel_touch_disable.erase(std::remove_if(feel_touch_disable.begin(), feel_touch_disable.end(),
		[dwT](const DenyTouch& denied)
		{
			return touch_time_reached(dwT, denied.Expire);
		}), feel_touch_disable.end());

	// Find nearest objects.
	q_nearest.clear_not_free();
	q_nearest.reserve(feel_touch.size());
	g_pGameLevel->ObjectSpace.GetNearest(q_nearest, C, R, NULL);

	// One reusable flat hash table stores all membership states. This avoids
	// repeated linear scans while keeping feel_touch as the ordered public list.
	feel_touch_lookup.clear();
	feel_touch_lookup.reserve(static_cast<size_t>(feel_touch.size()) + q_nearest.size() + feel_touch_disable.size());

	for (CObject* object : feel_touch)
		feel_touch_lookup[object] |= touch_lookup_current;
	for (CObject* object : q_nearest)
		feel_touch_lookup[object] |= touch_lookup_nearest;
	for (const DenyTouch& denied : feel_touch_disable)
		feel_touch_lookup[denied.O] |= touch_lookup_denied;

	// Process new contacts in the exact order returned by ObjectSpace.
	for (CObject* object : q_nearest)
	{
		if (object->getDestroy())
			continue;
		u8& lookup = feel_touch_lookup[object];
		// Existing contacts are checked in the removal pass after new-contact
		// callbacks have run; checking them here repeats the same narrowphase.
		if (lookup & touch_lookup_current)
			continue;
		if (!feel_touch_contact(object))
			continue;

		if (lookup & touch_lookup_denied)
			continue;

		lookup |= touch_lookup_current;
		feel_touch.push_back(object);
		feel_touch_new(object);
	}

	// Collect removals in the original vector order, then perform one stable
	// compaction instead of erase(begin() + index) for every removed object.
	feel_touch_removed.clear_not_free();
	feel_touch_removed.reserve(feel_touch.size());

	const auto newEnd = std::remove_if(feel_touch.begin(), feel_touch.end(),
		[this](CObject* object)
		{
			const auto lookup = feel_touch_lookup.find(object);
			const bool isNearest = lookup != feel_touch_lookup.end() && (lookup->second & touch_lookup_nearest);
			const bool remove = object->getDestroy() || !feel_touch_contact(object) || !isNearest;
			if (remove)
				feel_touch_removed.push_back(object);
			return remove;
		});
	feel_touch.erase(newEnd, feel_touch.end());

	// Callbacks preserve the previous deletion order and observe the final,
	// already compacted public contact list.
	for (CObject* object : feel_touch_removed)
		feel_touch_delete(object);

	//. Engine.Sheduler.Slice ();
}

void Touch::feel_touch_relcase(CObject* O)
{
	xr_vector<CObject*>::iterator I = std::find(feel_touch.begin(), feel_touch.end(), O);
	if (I != feel_touch.end())
	{
		feel_touch.erase(I);
		feel_touch_delete(O);
	}
	feel_touch_disable.erase(std::remove_if(feel_touch_disable.begin(), feel_touch_disable.end(),
		[O](const DenyTouch& denied)
		{
			return denied.O == O;
		}), feel_touch_disable.end());
}
