#include "stdafx.h"
#pragma hdrstop

#include "../xrRender/r_constants_cache.h"

R_constants::R_constants() : m_any_dirty(false)
{
	ZeroMemory(m_dirty_masks, sizeof(m_dirty_masks));
}

void R_constants::invalidate(const R_constant_table& table)
{
	R_constant_table::cb_table::const_iterator it = table.m_CBTable.begin();
	const R_constant_table::cb_table::const_iterator end = table.m_CBTable.end();
	for (; it != end; ++it)
	{
		const u32 encoded_index = it->first;
		const u32 buffer_index = encoded_index & CB_BufferIndexMask;
		VERIFY(buffer_index < CBackend::MaxCBuffers);

		BufferType type;
		switch (encoded_index & CB_BufferTypeMask)
		{
		case CB_BufferPixelShader: type = BT_PixelBuffer; break;
		case CB_BufferVertexShader: type = BT_VertexBuffer; break;
		case CB_BufferGeometryShader: type = BT_GeometryBuffer; break;
		case CB_BufferHullShader: type = BT_HullBuffer; break;
		case CB_BufferDomainShader: type = BT_DomainBuffer; break;
		case CB_BufferComputeShader: type = BT_Compute; break;
		default:
			NODEFAULT;
			continue;
		}

		m_dirty_masks[type] |= static_cast<u16>(1u << buffer_index);
		m_any_dirty = true;
	}
}

u32 R_constants::GetCBufferIndex(R_constant* C, BufferType BType) const
{
	switch (BType)
	{
	case BT_PixelBuffer:
		return (C->destination & RC_dest_pixel_cb_index_mask) >> RC_dest_pixel_cb_index_shift;
	case BT_VertexBuffer:
		return (C->destination & RC_dest_vertex_cb_index_mask) >> RC_dest_vertex_cb_index_shift;
	case BT_GeometryBuffer:
		return (C->destination & RC_dest_geometry_cb_index_mask) >> RC_dest_geometry_cb_index_shift;
	case BT_HullBuffer:
		return (C->destination & RC_dest_hull_cb_index_mask) >> RC_dest_hull_cb_index_shift;
	case BT_DomainBuffer:
		return (C->destination & RC_dest_domain_cb_index_mask) >> RC_dest_domain_cb_index_shift;
	case BT_Compute:
		return (C->destination & RC_dest_compute_cb_index_mask) >> RC_dest_compute_cb_index_shift;
	default:
		NODEFAULT;
		return 0;
	}
}

dx10ConstantBuffer& R_constants::GetCBuffer(R_constant* C, BufferType BType, u32& buffer_index)
{
	buffer_index = GetCBufferIndex(C, BType);
	VERIFY(buffer_index < CBackend::MaxCBuffers);

	if (BType == BT_PixelBuffer)
	{
		VERIFY(RCache.m_aPixelConstants[buffer_index]);
		return *RCache.m_aPixelConstants[buffer_index];
	}
	else if (BType == BT_VertexBuffer)
	{
		VERIFY(RCache.m_aVertexConstants[buffer_index]);
		return *RCache.m_aVertexConstants[buffer_index];
	}
	else if (BType == BT_GeometryBuffer)
	{
		VERIFY(RCache.m_aGeometryConstants[buffer_index]);
		return *RCache.m_aGeometryConstants[buffer_index];
	}
	else if (BType == BT_HullBuffer)
	{
		VERIFY(RCache.m_aHullConstants[buffer_index]);
		return *RCache.m_aHullConstants[buffer_index];
	}
	else if (BType == BT_DomainBuffer)
	{
		VERIFY(RCache.m_aDomainConstants[buffer_index]);
		return *RCache.m_aDomainConstants[buffer_index];
	}
	else if (BType == BT_Compute)
	{
		VERIFY(RCache.m_aComputeConstants[buffer_index]);
		return *RCache.m_aComputeConstants[buffer_index];
	}

	FATAL("Unreachable code");
	//Just hack to avoid warning;
	dx10ConstantBuffer* ptr = 0;
	return *ptr;
}

void R_constants::flush_cache()
{
	ref_cbuffer* const stage_buffers[BT_Count] =
	{
		RCache.m_aPixelConstants,
		RCache.m_aVertexConstants,
		RCache.m_aGeometryConstants,
		RCache.m_aHullConstants,
		RCache.m_aDomainConstants,
		RCache.m_aComputeConstants
	};

	for (u32 stage = 0; stage < BT_Count; ++stage)
	{
		u16 mask = m_dirty_masks[stage];
		for (u32 index = 0; mask && index < CBackend::MaxCBuffers; ++index, mask >>= 1)
		{
			if ((mask & 1u) && stage_buffers[stage][index])
				stage_buffers[stage][index]->Flush();
		}
		m_dirty_masks[stage] = 0;
	}
	m_any_dirty = false;
}

/*
void R_constants::flush_cache()
{
	if (a_pixel.b_dirty)
	{
		// fp
		R_constant_array::t_f&	F	= a_pixel.c_f;
		{
			//if (F.r_lo() <= 32) //. hack
			{		
				void	*pBuffer;
				const int iVectorElements = 4;
				const int iVectorNumber = 256;
				RCache.m_pPixelConstants->Map(D3Dxx_MAP_WRITE_DISCARD, 0, &pBuffer);
				CopyMemory(pBuffer, F.access(0), iVectorNumber*iVectorElements*sizeof(float));
				RCache.m_pPixelConstants->Unmap();
			}
		}
		a_pixel.b_dirty		= false;
	}
	if (a_vertex.b_dirty)
	{
		// fp
		R_constant_array::t_f&	F	= a_vertex.c_f;
		{
			u32		count		= F.r_hi()-F.r_lo();
			if (count)			{
#ifdef DEBUG
				if (F.r_hi() > HW.Caps.geometry.dwRegisters)
				{
					Debug.fatal(DEBUG_INFO,"Internal error setting VS-constants: overflow\nregs[%d],hi[%d]",
						HW.Caps.geometry.dwRegisters,F.r_hi()
						);
				}
				PGO		(Msg("PGO:V_CONST:%d",count));
#endif
				{		
					void	*pBuffer;
					const int iVectorElements = 4;
					const int iVectorNumber = 256;
					RCache.m_pVertexConstants->Map(D3Dxx_MAP_WRITE_DISCARD, 0, &pBuffer);
					CopyMemory(pBuffer, F.access(0), iVectorNumber*iVectorElements*sizeof(float));
					RCache.m_pVertexConstants->Unmap();
				}
				F.flush	();
			}
		}
		a_vertex.b_dirty	= false;
	}
}
*/
