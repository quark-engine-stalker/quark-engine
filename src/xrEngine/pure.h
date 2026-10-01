#ifndef _PURE_H_AAA_
#define _PURE_H_AAA_

#include <algorithm>

// messages
#define REG_PRIORITY_LOW 0x11111111ul
#define REG_PRIORITY_NORMAL 0x22222222ul
#define REG_PRIORITY_HIGH 0x33333333ul
#define REG_PRIORITY_CAPTURE 0x7ffffffful
#define REG_PRIORITY_INVALID 0xfffffffful

typedef void __fastcall RP_FUNC(void* obj);
#define DECLARE_MESSAGE_CL(name,calling) extern ENGINE_API RP_FUNC rp_##name; class ENGINE_API pure##name { public: virtual void calling On##name(void)=0; }

#define DECLARE_MESSAGE( name ) DECLARE_MESSAGE_CL(name, )
#define DECLARE_RP(name) void __fastcall rp_##name(void *p) { ((pure##name *)p)->On##name(); }

DECLARE_MESSAGE_CL(Frame, _BCL);

DECLARE_MESSAGE(Render);

DECLARE_MESSAGE(AppActivate);

DECLARE_MESSAGE(AppDeactivate);

DECLARE_MESSAGE(AppStart);

DECLARE_MESSAGE(AppEnd);

DECLARE_MESSAGE(DeviceReset);

DECLARE_MESSAGE(ScreenResolutionChanged);


//-----------------------------------------------------------------------------
struct _REG_INFO
{
	void* Object;
	int Prio;
	u32 Flags;
};

template <class T>
class CRegistrator // the registrator itself
{
	static constexpr int invalid_priority = -1;
	static constexpr int capture_priority = 0x7fffffff;

	static bool _REG_Compare(const _REG_INFO& left, const _REG_INFO& right)
	{
		// Direct comparison avoids signed overflow from subtracting priorities.
		return left.Prio > right.Prio;
	}

	class registry_lock_guard
	{
		CRegistrator& owner;
		registry_lock_guard(const registry_lock_guard&) = delete;
		registry_lock_guard& operator=(const registry_lock_guard&) = delete;

	public:
		explicit registry_lock_guard(CRegistrator& registrator) : owner(registrator)
		{
			EnterCriticalSection(&owner.registry_lock);
		}

		~registry_lock_guard()
		{
			LeaveCriticalSection(&owner.registry_lock);
		}
	};

	class processing_guard
	{
		CRegistrator& owner;
		processing_guard(const processing_guard&) = delete;
		processing_guard& operator=(const processing_guard&) = delete;

	public:
		explicit processing_guard(CRegistrator& registrator) : owner(registrator)
		{
			++owner.processing_depth;
		}

		~processing_guard()
		{
			VERIFY(owner.processing_depth != 0);
			--owner.processing_depth;

			// Apply deferred Add/Remove operations only after the outermost Process call.
			if (owner.processing_depth == 0 && owner.changed)
				owner.ResortUnlocked();
		}
	};

	// CRITICAL_SECTION is recursive: callbacks are allowed to Add/Remove themselves.
	CRITICAL_SECTION registry_lock;
	u32 processing_depth;
	bool changed;
	xr_vector<_REG_INFO> R;

	void ResortUnlocked()
	{
		if (!R.empty())
			std::sort(R.begin(), R.end(), _REG_Compare);

		while (!R.empty() && R.back().Prio == invalid_priority)
			R.pop_back();

		changed = false;
	}

	CRegistrator(const CRegistrator&) = delete;
	CRegistrator& operator=(const CRegistrator&) = delete;

public:
	CRegistrator() : processing_depth(0), changed(false)
	{
		InitializeCriticalSection(&registry_lock);
	}

	~CRegistrator()
	{
		EnterCriticalSection(&registry_lock);
		VERIFY(processing_depth == 0);
		LeaveCriticalSection(&registry_lock);
		DeleteCriticalSection(&registry_lock);
	}

	void Add(T* obj, int priority = REG_PRIORITY_NORMAL, u32 flags = 0)
	{
		registry_lock_guard lock(*this);
#ifdef DEBUG
		VERIFY(priority != invalid_priority);
		VERIFY(obj);
		for (u32 i = 0; i < R.size(); ++i)
			VERIFY(!((R[i].Prio != invalid_priority) && (R[i].Object == static_cast<void*>(obj))));
#endif
		_REG_INFO item;
		item.Object = obj;
		item.Prio = priority;
		item.Flags = flags;
		R.push_back(item);

		if (processing_depth != 0)
			changed = true;
		else
			ResortUnlocked();
	}

	void Remove(T* obj)
	{
		registry_lock_guard lock(*this);
		for (u32 i = 0; i < R.size(); ++i)
		{
			if (R[i].Object == obj)
				R[i].Prio = invalid_priority;
		}

		if (processing_depth != 0)
			changed = true;
		else
			ResortUnlocked();
	}

	void Process(RP_FUNC* f)
	{
		registry_lock_guard lock(*this);
		VERIFY(f);
		processing_guard guard(*this);

		if (R.empty())
			return;

		if (R[0].Prio == capture_priority)
		{
			f(R[0].Object);
			return;
		}

		// Additions made by a callback are deliberately deferred until the next pass.
		// Removals still take effect immediately because they mark an entry invalid.
		const u32 process_count = static_cast<u32>(R.size());
		for (u32 i = 0; i < process_count; ++i)
		{
			if (R[i].Prio != invalid_priority)
				f(R[i].Object);
		}
	}

	void Clear()
	{
		registry_lock_guard lock(*this);
		if (processing_depth != 0)
		{
			for (u32 i = 0; i < R.size(); ++i)
				R[i].Prio = invalid_priority;
			changed = true;
			return;
		}

		R.clear();
		changed = false;
	}

	void Resort()
	{
		registry_lock_guard lock(*this);
		if (processing_depth != 0)
		{
			changed = true;
			return;
		}

		ResortUnlocked();
	}
};

#endif
