#ifndef dx10SamplerStateCache_included
#define dx10SamplerStateCache_included
#pragma once

class dx10SamplerStateCache
{
public:
	enum
	{
		hInvalidHandle = 0xFFFFFFFF,
		hUnknownHandle = 0xFFFFFFFE
	};

	typedef u32 SHandle;
	typedef xr_vector<SHandle> HArray;

public:
	dx10SamplerStateCache();
	~dx10SamplerStateCache();

	void ClearStateArray();

	// Logical sampler state used by normal render passes. MaxAnisotropy and
	// MipLODBias are global runtime controls and are applied through immutable
	// physical-state variants without recreating an already cached variant.
	SHandle GetState(D3D_SAMPLER_DESC& desc);

	// Exact immutable sampler object for paths (for example compute shaders)
	// that provide a complete D3D11 descriptor and must not inherit the global
	// texture filtering controls. Returned pointer owns one reference.
	ID3DSamplerState* AcquireStateObject(const D3D_SAMPLER_DESC& desc);

	void VSApplySamplers(const HArray& samplers);
	void PSApplySamplers(const HArray& samplers);
	void GSApplySamplers(const HArray& samplers);
	void HSApplySamplers(const HArray& samplers);
	void DSApplySamplers(const HArray& samplers);
	void CSApplySamplers(const HArray& samplers);

	void SetMaxAnisotropy(u32 uiMaxAniso);
	void SetMipLODBias(float uiMipLODBias);

	void ResetDeviceState();
	void ResetCSDeviceState();

private:
	typedef ID3DSamplerState IDeviceState;
	typedef D3D_SAMPLER_DESC StateDecs;

	struct StateRecord
	{
		u32 m_crc;
		StateDecs m_baseDesc;
		IDeviceState* m_pState; // weak reference to m_PhysicalStates
	};

	struct PhysicalStateRecord
	{
		u32 m_crc;
		StateDecs m_desc;
		IDeviceState* m_pState; // owning reference
	};

	StateDecs MakeBaseDescription(const StateDecs& desc) const;
	StateDecs MakeEffectiveDescription(const StateDecs& baseDesc) const;

	SHandle FindState(const StateDecs& baseDesc, u32 StateCRC) const;
	IDeviceState* FindOrCreatePhysicalState(const StateDecs& desc);
	IDeviceState* FindPhysicalState(const StateDecs& desc, u32 StateCRC) const;
	void RemapStateObjects();

	bool PrepareSamplerStates(
		const HArray& samplers,
		ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT],
		SHandle pCurrentState[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT],
		u32& currentCount,
		u32& uiMin,
		u32& uiMax) const;

private:
	// Logical records keep stable handles used by dx10State objects.
	xr_vector<StateRecord> m_StateArray;
	xr_unordered_flat_map<u32, xr_vector<u32>> m_StateBuckets;

	// Physical D3D11 objects are immutable descriptor variants. This cache owns
	// them and allows global anisotropy/mip-bias switches to remap existing
	// logical handles instead of calling CreateSamplerState every switch.
	xr_vector<PhysicalStateRecord> m_PhysicalStates;
	xr_unordered_flat_map<u32, xr_vector<u32>> m_PhysicalBuckets;

	SHandle m_aPSSamplers[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	SHandle m_aVSSamplers[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	SHandle m_aGSSamplers[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	SHandle m_aHSSamplers[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	SHandle m_aDSSamplers[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	SHandle m_aCSSamplers[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 m_uiPSSamplerCount;
	u32 m_uiVSSamplerCount;
	u32 m_uiGSSamplerCount;
	u32 m_uiHSSamplerCount;
	u32 m_uiDSSamplerCount;
	u32 m_uiCSSamplerCount;

	u32 m_uiMaxAnisotropy;
	float m_uiMipLODBias;
};

extern dx10SamplerStateCache SSManager;

#endif // dx10SamplerStateCache_included
