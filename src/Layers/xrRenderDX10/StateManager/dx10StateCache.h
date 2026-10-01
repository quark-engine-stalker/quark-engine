#ifndef dx10StateCache_included
#define dx10StateCache_included
#pragma once

template <class IDeviceState, class StateDecs>
class dx10StateCache
{
public:
	dx10StateCache();
	~dx10StateCache();

	void ClearStateArray();

	IDeviceState* GetState(SimulatorStates& state_code);
	IDeviceState* GetState(StateDecs& desc);
	bool GetDesc(IDeviceState* pState, StateDecs& desc) const;

private:
	struct StateRecord
	{
		u32 m_crc;
		StateDecs m_desc;
		IDeviceState* m_pState;
	};

	void CreateState(StateDecs desc, IDeviceState** ppIState);
	IDeviceState* FindState(const StateDecs& desc, u32 StateCRC);
	void RegisterStateIndex(u32 StateCRC, u32 index, IDeviceState* pState);

private:
	// State storage owns the D3D objects. Hash buckets keep stable vector indices,
	// avoiding the old O(N) scan on every cache lookup while still resolving
	// CRC collisions with an exact descriptor comparison.
	xr_vector<StateRecord> m_StateArray;
	xr_unordered_flat_map<u32, xr_vector<u32>> m_StateBuckets;
	xr_unordered_flat_map<IDeviceState*, u32> m_StateByPointer;
};

extern dx10StateCache<ID3DRasterizerState, D3D_RASTERIZER_DESC> RSManager;
extern dx10StateCache<ID3DDepthStencilState, D3D_DEPTH_STENCIL_DESC> DSSManager;
extern dx10StateCache<ID3DBlendState, D3D_BLEND_DESC> BSManager;

#include "dx10StateCacheImpl.h"

#endif // dx10StateCache_included
