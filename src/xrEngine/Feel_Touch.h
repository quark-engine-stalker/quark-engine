#pragma once

#include "pure_relcase.h"

class ENGINE_API CObject;

namespace Feel
{
	class ENGINE_API Touch : private pure_relcase
	{
	public:
		struct DenyTouch
		{
			CObject* O;
			DWORD Expire;
		};

	protected:
		xr_vector<DenyTouch> feel_touch_disable;

		// Reused lookup/scratch storage for feel_touch_update().
		// The public feel_touch vector remains the authoritative ordered list.
		xr_unordered_flat_map<CObject*, u8> feel_touch_lookup;
		xr_vector<CObject*> feel_touch_removed;

	public:
		xr_vector<CObject*> feel_touch;
		xr_vector<CObject*> q_nearest;

	public:
		void __stdcall feel_touch_relcase(CObject* O);

	public:
		Touch();
		virtual ~Touch();

		virtual bool feel_touch_contact(CObject* O);
		virtual void feel_touch_update(Fvector& P, float R);
		virtual void feel_touch_deny(CObject* O, DWORD T);

		virtual void feel_touch_new(CObject* O)
		{
		};

		virtual void feel_touch_delete(CObject* O)
		{
		};
	};
};
