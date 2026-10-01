#ifndef dx10StateCacheImpl_included
#define dx10StateCacheImpl_included
#pragma once

#include "../dx10StateUtils.h"

using dx10StateUtils::operator==;

template <class IDeviceState, class StateDecs>
IDeviceState* dx10StateCache<IDeviceState, StateDecs>::GetState(SimulatorStates& state_code)
{
	StateDecs desc;
	dx10StateUtils::ResetDescription(desc);
	state_code.UpdateDesc(desc);
	return GetState(desc);
}

template <class IDeviceState, class StateDecs>
IDeviceState* dx10StateCache<IDeviceState, StateDecs>::GetState(StateDecs& desc)
{
	dx10StateUtils::ValidateState(desc);
	const u32 crc = dx10StateUtils::GetHash(desc);

	IDeviceState* pResult = FindState(desc, crc);
	if (pResult)
		return pResult;

	StateRecord rec;
	rec.m_crc = crc;
	rec.m_desc = desc;
	CreateState(desc, &rec.m_pState);

	const u32 index = static_cast<u32>(m_StateArray.size());
	m_StateArray.push_back(rec);
	RegisterStateIndex(crc, index, rec.m_pState);
	return rec.m_pState;
}

template <class IDeviceState, class StateDecs>
IDeviceState* dx10StateCache<IDeviceState, StateDecs>::FindState(const StateDecs& desc, u32 StateCRC)
{
	const auto bucketIt = m_StateBuckets.find(StateCRC);
	if (bucketIt == m_StateBuckets.end())
		return nullptr;

	const xr_vector<u32>& bucket = bucketIt->second;
	for (u32 index : bucket)
	{
		VERIFY(index < m_StateArray.size());
		const StateRecord& rec = m_StateArray[index];
		if (rec.m_desc == desc)
			return rec.m_pState;
	}

	return nullptr;
}



template <class IDeviceState, class StateDecs>
void dx10StateCache<IDeviceState, StateDecs>::RegisterStateIndex(u32 StateCRC, u32 index, IDeviceState* pState)
{
	m_StateBuckets[StateCRC].push_back(index);
	m_StateByPointer[pState] = index;
}

template <class IDeviceState, class StateDecs>
bool dx10StateCache<IDeviceState, StateDecs>::GetDesc(IDeviceState* pState, StateDecs& desc) const
{
	const auto it = m_StateByPointer.find(pState);
	if (it == m_StateByPointer.end())
		return false;

	const u32 index = it->second;
	VERIFY(index < m_StateArray.size());
	desc = m_StateArray[index].m_desc;
	return true;
}

#endif // dx10StateCacheImpl_included
