#include "stdafx.h"
#pragma hdrstop

#include "detailmanager.h"

const u32 vs_size = 3000;

void CDetailManager::soft_Load()
{
	R_ASSERT(RCache.Vertex.Buffer());
	R_ASSERT(RCache.Index.Buffer());
	// Vertex Stream
	soft_Geom.create(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, RCache.Vertex.Buffer(), RCache.Index.Buffer());
}

void CDetailManager::soft_Unload()
{
	soft_Geom.destroy();
}

void CDetailManager::soft_Render()
{
	// Render itself
	// float	fPhaseRange	= PI/16;
	// float	fPhaseX		= _sin(RDEVICE.fTimeGlobal*0.1f)	*fPhaseRange;
	// float	fPhaseZ		= _sin(RDEVICE.fTimeGlobal*0.11f)*fPhaseRange;

	// Get index-stream
	_IndexStream& _IS = RCache.Index;
	_VertexStream& _VS = RCache.Vertex;
	for (u32 O = 0; O < objects.size(); O++)
	{
		CDetail& Object = *objects[O];
		u32 vCount_Object = Object.number_vertices;
		u32 iCount_Object = Object.number_indices;

		xr_vector<VisibleSlotItems>& _vis = m_visibles[0][O];
		xr_vector<VisibleSlotItems>::iterator _vI = _vis.begin();
		xr_vector<VisibleSlotItems>::iterator _vE = _vis.end();
		for (; _vI != _vE; _vI++)
		{
			SlotItemVec* items = _vI->items;
			u32 vCount_Total = items->size() * vCount_Object;
			// calculate lock count needed
			u32 lock_count = vCount_Total / vs_size;
			if (vCount_Total > (lock_count * vs_size)) lock_count++;

			// calculate objects per lock
			u32 o_total = items->size();
			u32 o_per_lock = o_total / lock_count;
			if (o_total > (o_per_lock * lock_count)) o_per_lock++;

			// Fill VB (and flush it as nesessary)
			RCache.set_Shader(Object.shader);

			Fmatrix mXform;
			for (u32 L_ID = 0; L_ID < lock_count; L_ID++)
			{
				// Calculate params
				u32 item_start = L_ID * o_per_lock;
				u32 item_end = item_start + o_per_lock;
				if (item_end > o_total) item_end = o_total;
				if (item_end <= item_start) break;
				u32 item_range = item_end - item_start;

				// Calc Lock params
				u32 vCount_Lock = item_range * vCount_Object;
				u32 iCount_Lock = item_range * iCount_Object;

				// Lock buffers
				u32 vBase, iBase, iOffset = 0;
				CDetail::fvfVertexOut* vDest = (CDetail::fvfVertexOut*)_VS.Lock(
					vCount_Lock, soft_Geom->vb_stride, vBase);
				u16* iDest = (u16*)_IS.Lock(iCount_Lock, iBase);

				// Filling itself
				for (u32 item_idx = item_start; item_idx < item_end; ++item_idx)
				{
					SlotItem& Instance = *items->at(item_idx);
					float scale = Instance.scale * _vI->slot->visibility_scale;

					// Build matrix
					const Fvector4* M = Instance.xform;
					mXform._11 = M[0].x * scale;
					mXform._12 = M[1].x * scale;
					mXform._13 = M[2].x * scale;
					mXform._14 = 0.f;
					mXform._21 = M[0].y * scale;
					mXform._22 = M[1].y * scale;
					mXform._23 = M[2].y * scale;
					mXform._24 = 0.f;
					mXform._31 = M[0].z * scale;
					mXform._32 = M[1].z * scale;
					mXform._33 = M[2].z * scale;
					mXform._34 = 0.f;
					mXform._41 = M[0].w;
					mXform._42 = M[1].w;
					mXform._43 = M[2].w;
					mXform._44 = 1;

					// Transfer vertices
					{
						u32 C = 0xffffffff;
						CDetail::fvfVertexIn *srcIt = Object.vertices, *srcEnd = Object.vertices + Object.
							                     number_vertices;
						CDetail::fvfVertexOut* dstIt = vDest;

						for (; srcIt != srcEnd; srcIt++, dstIt++)
						{
							mXform.transform_tiny(dstIt->P, srcIt->P);
							dstIt->C = C;
							dstIt->u = srcIt->u;
							dstIt->v = srcIt->v;
						}
					}

					// Transfer indices (in 32bit lines)
					VERIFY(iOffset<65535);
					{
						u32 item = (iOffset << 16) | iOffset;
						u32 count = Object.number_indices / 2;
						LPDWORD sit = LPDWORD(Object.indices);
						LPDWORD send = sit + count;
						LPDWORD dit = LPDWORD(iDest);
						for (; sit != send; dit++, sit++) *dit = *sit + item;
						if (Object.number_indices & 1)
							iDest[Object.number_indices - 1] = (u16)(Object.indices[Object.number_indices - 1] + u16(
								iOffset));
					}

					// Increment counters
					vDest += vCount_Object;
					iDest += iCount_Object;
					iOffset += vCount_Object;
				}
				_VS.Unlock(vCount_Lock, soft_Geom->vb_stride);
				_IS.Unlock(iCount_Lock);

				// Render
				u32 dwNumPrimitives = iCount_Lock / 3;
				RCache.set_Geometry(soft_Geom);
				RCache.Render(D3DPT_TRIANGLELIST, vBase, 0, vCount_Lock, iBase, dwNumPrimitives);
			}
		}
		// Clean up
		_vis.clear_not_free();
	}
}

/*
//.
                VERIFY(sizeof(CDetail::fvfVertexOut)==soft_Geom->vb_stride);
                
                CDetail::fvfVertexOut	*dstIt = vDest;

                VERIFY(items->size()*Object.number_vertices==vCount_Lock);
                
                for	(u32 k=0; k<vCount_Lock; k++)
                {
					// Transfer vertices
					{
						u32					C = 0xffffffff;
						CDetail::fvfVertexIn	*srcIt = Object.vertices, *srcEnd = Object.vertices+Object.number_vertices;
						CDetail::fvfVertexOut	*dstIt = vDest;

						for	(; srcIt!=srcEnd; srcIt++, dstIt++)
						{
							mXform.transform_tiny	(dstIt->P,srcIt->P);
							dstIt->C	= C;
							dstIt->u	= srcIt->u;
							dstIt->v	= srcIt->v;
						}
					}
                }
*/
