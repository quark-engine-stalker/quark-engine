#include "stdafx.h"
#include "dx10SamplerStateCache.h"

#include "../dx10StateUtils.h"

using dx10StateUtils::operator==;

dx10SamplerStateCache SSManager;

dx10SamplerStateCache::dx10SamplerStateCache() :
	m_uiMaxAnisotropy(1), m_uiMipLODBias(0.0f)
{
	m_StateArray.reserve(32);
	m_StateBuckets.reserve(32);
	m_PhysicalStates.reserve(64);
	m_PhysicalBuckets.reserve(64);
	ResetDeviceState();
}

dx10SamplerStateCache::~dx10SamplerStateCache()
{
	ClearStateArray();
}

dx10SamplerStateCache::StateDecs dx10SamplerStateCache::MakeBaseDescription(const StateDecs& desc) const
{
	StateDecs baseDesc = desc;

	// These two values are global runtime controls for normal material samplers.
	// Keep the logical key independent from them so handles stay stable when the
	// controls change. Physical variants contain the actual values.
	baseDesc.MaxAnisotropy = 1;
	baseDesc.MipLODBias = 0.0f;
	dx10StateUtils::ValidateState(baseDesc);
	return baseDesc;
}

dx10SamplerStateCache::StateDecs dx10SamplerStateCache::MakeEffectiveDescription(const StateDecs& baseDesc) const
{
	StateDecs desc = baseDesc;
	desc.MaxAnisotropy = m_uiMaxAnisotropy;
	desc.MipLODBias = m_uiMipLODBias;
	dx10StateUtils::ValidateState(desc);
	return desc;
}

dx10SamplerStateCache::SHandle dx10SamplerStateCache::GetState(D3D_SAMPLER_DESC& desc)
{
	const StateDecs baseDesc = MakeBaseDescription(desc);
	const u32 crc = dx10StateUtils::GetHash(baseDesc);

	SHandle handle = FindState(baseDesc, crc);
	if (handle == hInvalidHandle)
	{
		StateRecord rec;
		rec.m_crc = crc;
		rec.m_baseDesc = baseDesc;
		const StateDecs effectiveDesc = MakeEffectiveDescription(baseDesc);
		rec.m_pState = FindOrCreatePhysicalState(effectiveDesc);

		handle = static_cast<SHandle>(m_StateArray.size());
		m_StateArray.push_back(rec);
		m_StateBuckets[crc].push_back(handle);
	}

	// Preserve the historical contract that the caller receives a validated
	// descriptor with the currently active global filtering controls applied.
	desc = MakeEffectiveDescription(baseDesc);
	return handle;
}

ID3DSamplerState* dx10SamplerStateCache::AcquireStateObject(const D3D_SAMPLER_DESC& desc)
{
	StateDecs exactDesc = desc;
	dx10StateUtils::ValidateState(exactDesc);
	IDeviceState* state = FindOrCreatePhysicalState(exactDesc);
	state->AddRef();
	return state;
}

dx10SamplerStateCache::SHandle dx10SamplerStateCache::FindState(const StateDecs& baseDesc, u32 StateCRC) const
{
	const auto bucketIt = m_StateBuckets.find(StateCRC);
	if (bucketIt == m_StateBuckets.end())
		return static_cast<SHandle>(hInvalidHandle);

	const xr_vector<u32>& bucket = bucketIt->second;
	for (u32 index : bucket)
	{
		VERIFY(index < m_StateArray.size());
		if (m_StateArray[index].m_baseDesc == baseDesc)
			return static_cast<SHandle>(index);
	}

	return static_cast<SHandle>(hInvalidHandle);
}

dx10SamplerStateCache::IDeviceState* dx10SamplerStateCache::FindPhysicalState(
	const StateDecs& desc, u32 StateCRC) const
{
	const auto bucketIt = m_PhysicalBuckets.find(StateCRC);
	if (bucketIt == m_PhysicalBuckets.end())
		return nullptr;

	const xr_vector<u32>& bucket = bucketIt->second;
	for (u32 index : bucket)
	{
		VERIFY(index < m_PhysicalStates.size());
		const PhysicalStateRecord& rec = m_PhysicalStates[index];
		if (rec.m_desc == desc)
			return rec.m_pState;
	}

	return nullptr;
}

dx10SamplerStateCache::IDeviceState* dx10SamplerStateCache::FindOrCreatePhysicalState(const StateDecs& sourceDesc)
{
	StateDecs desc = sourceDesc;
	dx10StateUtils::ValidateState(desc);
	const u32 crc = dx10StateUtils::GetHash(desc);

	IDeviceState* state = FindPhysicalState(desc, crc);
	if (state)
		return state;

	PhysicalStateRecord rec;
	rec.m_crc = crc;
	rec.m_desc = desc;
	rec.m_pState = nullptr;
	CHK_DX(HW.pDevice->CreateSamplerState(&rec.m_desc, &rec.m_pState));

	const u32 index = static_cast<u32>(m_PhysicalStates.size());
	m_PhysicalStates.push_back(rec);
	m_PhysicalBuckets[crc].push_back(index);
	return rec.m_pState;
}

void dx10SamplerStateCache::RemapStateObjects()
{
	bool changed = false;
	for (u32 i = 0; i < m_StateArray.size(); ++i)
	{
		StateRecord& rec = m_StateArray[i];
		const StateDecs effectiveDesc = MakeEffectiveDescription(rec.m_baseDesc);
		IDeviceState* state = FindOrCreatePhysicalState(effectiveDesc);
		if (state != rec.m_pState)
		{
			rec.m_pState = state;
			changed = true;
		}
	}

	if (changed)
		ResetDeviceState();
}

void dx10SamplerStateCache::ClearStateArray()
{
	m_StateArray.clear_not_free();
	m_StateBuckets.clear();

	for (u32 i = 0; i < m_PhysicalStates.size(); ++i)
		_RELEASE(m_PhysicalStates[i].m_pState);

	m_PhysicalStates.clear_not_free();
	m_PhysicalBuckets.clear();
	ResetDeviceState();
}

bool dx10SamplerStateCache::PrepareSamplerStates(
	const HArray& samplers,
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT],
	SHandle pCurrentState[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT],
	u32& currentCount,
	u32& uiMin,
	u32& uiMax) const
{
	const u32 slotCount = D3D_COMMONSHADER_SAMPLER_SLOT_COUNT;
	const u32 desiredCount = static_cast<u32>(samplers.size());
	VERIFY(desiredCount <= slotCount);
	VERIFY(currentCount <= slotCount);

	uiMin = slotCount;
	uiMax = 0;

	const u32 scanCount = _max(currentCount, desiredCount);
	for (u32 i = 0; i < scanCount; ++i)
	{
		const SHandle desired = i < desiredCount ? samplers[i] : static_cast<SHandle>(hInvalidHandle);
		VERIFY(desired == hInvalidHandle || desired < m_StateArray.size());

		if (pCurrentState[i] == desired)
			continue;

		pCurrentState[i] = desired;
		uiMin = _min(uiMin, i);
		uiMax = _max(uiMax, i);
	}
	currentCount = desiredCount;

	if (uiMin == slotCount)
		return false;

	for (u32 i = uiMin; i <= uiMax; ++i)
	{
		const SHandle handle = pCurrentState[i];
		pSS[i] = handle == hInvalidHandle ? nullptr : m_StateArray[handle].m_pState;
	}

	return true;
}

void dx10SamplerStateCache::VSApplySamplers(const HArray& samplers)
{
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 uiMin, uiMax;
	if (PrepareSamplerStates(samplers, pSS, m_aVSSamplers, m_uiVSSamplerCount, uiMin, uiMax))
		HW.pContext->VSSetSamplers(uiMin, uiMax - uiMin + 1, &pSS[uiMin]);
}

void dx10SamplerStateCache::PSApplySamplers(const HArray& samplers)
{
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 uiMin, uiMax;
	if (PrepareSamplerStates(samplers, pSS, m_aPSSamplers, m_uiPSSamplerCount, uiMin, uiMax))
		HW.pContext->PSSetSamplers(uiMin, uiMax - uiMin + 1, &pSS[uiMin]);
}

void dx10SamplerStateCache::GSApplySamplers(const HArray& samplers)
{
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 uiMin, uiMax;
	if (PrepareSamplerStates(samplers, pSS, m_aGSSamplers, m_uiGSSamplerCount, uiMin, uiMax))
		HW.pContext->GSSetSamplers(uiMin, uiMax - uiMin + 1, &pSS[uiMin]);
}

void dx10SamplerStateCache::HSApplySamplers(const HArray& samplers)
{
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 uiMin, uiMax;
	if (PrepareSamplerStates(samplers, pSS, m_aHSSamplers, m_uiHSSamplerCount, uiMin, uiMax))
		HW.pContext->HSSetSamplers(uiMin, uiMax - uiMin + 1, &pSS[uiMin]);
}

void dx10SamplerStateCache::DSApplySamplers(const HArray& samplers)
{
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 uiMin, uiMax;
	if (PrepareSamplerStates(samplers, pSS, m_aDSSamplers, m_uiDSSamplerCount, uiMin, uiMax))
		HW.pContext->DSSetSamplers(uiMin, uiMax - uiMin + 1, &pSS[uiMin]);
}

void dx10SamplerStateCache::CSApplySamplers(const HArray& samplers)
{
	ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
	u32 uiMin, uiMax;
	if (PrepareSamplerStates(samplers, pSS, m_aCSSamplers, m_uiCSSamplerCount, uiMin, uiMax))
		HW.pContext->CSSetSamplers(uiMin, uiMax - uiMin + 1, &pSS[uiMin]);
}

void dx10SamplerStateCache::SetMaxAnisotropy(u32 uiMaxAniso)
{
	clamp(uiMaxAniso, static_cast<u32>(1), static_cast<u32>(16));
	if (m_uiMaxAnisotropy == uiMaxAniso)
		return;

	m_uiMaxAnisotropy = uiMaxAniso;
	RemapStateObjects();
}

void dx10SamplerStateCache::SetMipLODBias(float uiMipLODBias)
{
	if (m_uiMipLODBias == uiMipLODBias)
		return;

	m_uiMipLODBias = uiMipLODBias;
	RemapStateObjects();
}

void dx10SamplerStateCache::ResetCSDeviceState()
{
	for (u32 i = 0; i < D3D_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
		m_aCSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
	m_uiCSSamplerCount = D3D_COMMONSHADER_SAMPLER_SLOT_COUNT;
}

void dx10SamplerStateCache::ResetDeviceState()
{
	for (u32 i = 0; i < D3D_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
	{
		m_aPSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
		m_aVSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
		m_aGSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
		m_aHSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
		m_aDSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
		m_aCSSamplers[i] = static_cast<SHandle>(hUnknownHandle);
	}

	const u32 unknownCount = D3D_COMMONSHADER_SAMPLER_SLOT_COUNT;
	m_uiPSSamplerCount = unknownCount;
	m_uiVSSamplerCount = unknownCount;
	m_uiGSSamplerCount = unknownCount;
	m_uiHSSamplerCount = unknownCount;
	m_uiDSSamplerCount = unknownCount;
	m_uiCSSamplerCount = unknownCount;
}
