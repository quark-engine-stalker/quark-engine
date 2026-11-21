////////////////////////////////////////////////////////////////////////////
//	Module 		: id_generator.h
//	Created 	: 28.08.2003
//  Modified 	: 28.08.2003
//	Author		: Dmitriy Iassenev and Oles' Shyshkovtsov
//	Description : ID generation class template
////////////////////////////////////////////////////////////////////////////

#pragma once

template <
	typename TIME_ID,
	typename TYPE_ID,
	typename VALUE_ID,
	typename BLOCK_ID,
	typename CHUNK_ID,
	VALUE_ID tMinValue,
	VALUE_ID tMaxValue,
	CHUNK_ID tBlockSize,
	VALUE_ID tInvalidValueID = tMaxValue,
	TIME_ID tStartTime = 0>
class CID_Generator
{
private:
	struct SID_Block
	{
		CHUNK_ID m_tCount;
		TIME_ID m_tTimeID;
		TYPE_ID m_tpIDs[tBlockSize];

		IC SID_Block() : m_tCount(0)
		{
		}

		IC bool operator<(const SID_Block& b) const
		{
			return (m_tCount && ((m_tTimeID < b.m_tTimeID) || !b.m_tCount));
		}
	};

private:
	enum
	{
		m_tBlockCount = u32(tMaxValue - tMinValue) / tBlockSize + 1,
	};

private:
	u32 m_available_count;
	SID_Block m_tppBlocks [m_tBlockCount];

private:
	IC BLOCK_ID tfGetBlockByValue(VALUE_ID tValueID)
	{
		BLOCK_ID l_tBlockID = BLOCK_ID((tValueID - tMinValue) / tBlockSize);
		R_ASSERT2(l_tBlockID < m_tBlockCount, "Requesting ID is invalid!");
		return (l_tBlockID);
	}

	IC VALUE_ID tfGetFromBlock(SID_Block& l_tID_Block, VALUE_ID tValueID)
	{
		VERIFY(l_tID_Block.m_tCount);
		BLOCK_ID l_tBlockID = BLOCK_ID(&l_tID_Block - m_tppBlocks);

		if (l_tID_Block.m_tCount == 1)
		{
			--m_available_count;
			VERIFY(m_available_count >= 0);
		}

		if (tInvalidValueID == tValueID)
			return (VALUE_ID(l_tID_Block.m_tpIDs[--l_tID_Block.m_tCount]) + l_tBlockID * tBlockSize + tMinValue);

		TYPE_ID* l_tpBlockID = std::find(l_tID_Block.m_tpIDs, l_tID_Block.m_tpIDs + l_tID_Block.m_tCount,
		                                 TYPE_ID((tValueID - tMinValue) % tBlockSize));
		R_ASSERT2(l_tID_Block.m_tpIDs + l_tID_Block.m_tCount != l_tpBlockID, "Requesting ID has already been used!");
		*l_tpBlockID = *(l_tID_Block.m_tpIDs + --l_tID_Block.m_tCount);
		return (tValueID);
	}

public:
	IC CID_Generator()
	{
		m_available_count = 0;
		for (VALUE_ID i = tMinValue; ; ++i)
		{
			vfFreeID(i, tStartTime);
			if (i >= tMaxValue)
				break;
		}
		VERIFY(m_available_count == m_tBlockCount);
		for (u32 j = 0; j < m_tBlockCount; ++j)
			std::reverse(m_tppBlocks[j].m_tpIDs, m_tppBlocks[j].m_tpIDs + m_tppBlocks[j].m_tCount);
	}

	IC VALUE_ID tfGetID(VALUE_ID tValueID = tInvalidValueID)
	{
		return tfGetID(tValueID, [](VALUE_ID) { return false; });
	}

	template <typename IsInUse>
	IC VALUE_ID tfGetID(VALUE_ID tValueID, IsInUse is_in_use)
	{
		if (tInvalidValueID != tValueID)
			return (tfGetFromBlock(m_tppBlocks[tfGetBlockByValue(tValueID)], tValueID));

		R_ASSERT2(m_available_count, "Not enough IDs");
		SID_Block* oldest = std::min_element(m_tppBlocks, m_tppBlocks + m_tBlockCount);
		const auto id_at = [this](const SID_Block& block, CHUNK_ID index)
		{
			return VALUE_ID(block.m_tpIDs[index] + (&block - m_tppBlocks) * tBlockSize + tMinValue);
		};
		if (!is_in_use(id_at(*oldest, oldest->m_tCount - 1)))
			return tfGetFromBlock(*oldest, tInvalidValueID);

		// Keep blocked IDs free in the pool so they can be claimed as soon as the client releases them.
		SID_Block* selected = nullptr;
		VALUE_ID selected_id = tInvalidValueID;
		for (SID_Block& block : m_tppBlocks)
		{
			if (!block.m_tCount || (selected && block.m_tTimeID >= selected->m_tTimeID))
				continue;
			for (CHUNK_ID index = block.m_tCount; index > 0; --index)
			{
				const VALUE_ID id = id_at(block, index - 1);
				if (is_in_use(id))
					continue;
				selected = &block;
				selected_id = id;
				break;
			}
		}
		R_ASSERT2(selected, "No free server ID has been released by the local client");
		return tfGetFromBlock(*selected, selected_id);
	}

	IC void vfFreeID(VALUE_ID tValueID, TIME_ID tTimeID)
	{
		BLOCK_ID l_tBlockID = tfGetBlockByValue(tValueID);
		SID_Block& l_tID_Block = m_tppBlocks[l_tBlockID];

		VERIFY(l_tID_Block.m_tCount < tBlockSize);

		if (!l_tID_Block.m_tCount)
		{
			++m_available_count;
			VERIFY(m_available_count <= m_tBlockCount);
		}

#ifdef DEBUG
		TYPE_ID					*l_tpBlockID = std::find(l_tID_Block.m_tpIDs, l_tID_Block.m_tpIDs + l_tID_Block.m_tCount, TYPE_ID((tValueID - tMinValue)%tBlockSize));	
		VERIFY					(l_tpBlockID == l_tID_Block.m_tpIDs + l_tID_Block.m_tCount);
#endif
		l_tID_Block.m_tpIDs[l_tID_Block.m_tCount++] = TYPE_ID((tValueID - tMinValue) % tBlockSize);
		l_tID_Block.m_tTimeID = tTimeID;
	}
};
