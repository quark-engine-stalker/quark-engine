// ModelPool.h: interface for the CModelPool class.
//////////////////////////////////////////////////////////////////////
#ifndef ModelPoolH
#define ModelPoolH
#pragma once

// refs
class dxRender_Visual;

namespace PS
{
	struct SEmitter;
};

// defs
class ECORE_API CModelPool
{
public:
	struct AsyncStorage;

private:
	friend class CRender;

	struct str_pred
	{
		IC bool operator()(const shared_str& x, const shared_str& y) const
		{
			return xr_strcmp(x, y) < 0;
		}
	};

	struct ModelDef
	{
		shared_str name;
		dxRender_Visual* model;
		u32 refs;

		ModelDef()
		{
			refs = 0;
			model = 0;
		}
	};

	typedef xr_multimap<shared_str, dxRender_Visual*, str_pred> POOL;
	typedef POOL::iterator POOL_IT;
	typedef xr_unordered_map<dxRender_Visual*, shared_str> REGISTRY;
	typedef REGISTRY::iterator REGISTRY_IT;
	typedef xr_unordered_map<shared_str, dxRender_Visual*> MODEL_BY_NAME;
	typedef xr_unordered_map<dxRender_Visual*, u32> MODEL_INDEX_BY_POINTER;
private:
	xr_vector<ModelDef> Models; // Reference / Base
	MODEL_BY_NAME ModelByName;
	MODEL_INDEX_BY_POINTER ModelIndexByPointer;
	xr_vector<dxRender_Visual*> ModelsToDelete; // 
	REGISTRY Registry; // Just pairing of pointer / Name
	POOL Pool; // Unused / Inactive
	BOOL bLogging;
	BOOL bForceDiscard;
	xrCriticalSection modelPoolGuard;
	// Uncached OGF/OMF loading touches shared renderer and motions state. Keep
	// those loads ordered, but do not hold the registry/pool lock across disk IO,
	// parsing and D3D resource creation.
	xrCriticalSection modelLoadGuard;

	AsyncStorage* asyncStorage;

	void Destroy();
	dxRender_Visual* ExtractUnreferencedBaseForChild(const shared_str& name);
	void EraseModelIndex(u32 index);
	dxRender_Visual* CreateFromPrepared(LPCSTR normalized_name, LPCSTR requested_name, bool assert,
		u64* wait_ticks = nullptr, u64* finalize_ticks = nullptr);
	void RecordLevelModel(LPCSTR normalized_name);
public:
	CModelPool();
	virtual ~CModelPool();
	dxRender_Visual* Instance_Create(u32 Type);
	dxRender_Visual* Instance_Duplicate(dxRender_Visual* V);
	dxRender_Visual* Instance_Load(LPCSTR N, BOOL allow_register, bool assert = true);
	dxRender_Visual* Instance_Load(LPCSTR N, IReader* data, BOOL allow_register);
	void Instance_Register(LPCSTR N, dxRender_Visual* V);
	dxRender_Visual* Instance_Find(LPCSTR N);

	dxRender_Visual* CreatePE(PS::CPEDef* source);
	dxRender_Visual* CreatePG(PS::CPGDef* source);
	dxRender_Visual* Create(LPCSTR name, IReader* data = 0, bool assert = true);
	dxRender_Visual* CreateChild(LPCSTR name, IReader* data);
	void Delete(dxRender_Visual* & V, BOOL bDiscard = FALSE);
	void Discard(dxRender_Visual* & V, BOOL b_complete);
	void DeleteInternal(dxRender_Visual* & V, BOOL bDiscard = FALSE);
	void DeleteQueue();

	void Logging(BOOL bEnable) { bLogging = bEnable; }

	void Prefetch();
	void Prefetch_One(LPCSTR N, bool assert = true);

	// Package 3: worker-side OGF/OMF preparation with render-thread finalization.
	void ProcessAsyncFinalization(u32 budget_ms = 2, bool wait_for_all = false);
	void BeginLevelPrefetch(LPCSTR level_name);
	void FinishLevelPrefetch();
	void SaveLevelPrefetchManifest();

	// Internal hooks used by the package-3 worker/finalizer implementation.
	AsyncStorage* AsyncStorageData() { return asyncStorage; }
	xrCriticalSection& ModelLoadGuard() { return modelLoadGuard; }
	bool Exists(LPCSTR N);
	void ClearPool(BOOL b_complete);

	void dump();

	void memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system);
#ifdef _EDITOR
	void					OnDeviceDestroy		();
	void 					Render				(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD);
	void 					RenderSingle		(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD);
#endif
};
#endif //ModelPoolH
