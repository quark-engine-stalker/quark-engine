#include "stdafx.h"
#include "r4.h"
#include "../xrRender/ResourceManager.h"
#include "../xrRender/fbasicvisual.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/xrLevel.h"
#include "../../xrEngine/x_ray.h"
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrCore/stream_reader.h"

#include "../xrRender/dxRenderDeviceRender.h"

#include "../xrRenderDX10/dx10BufferUtils.h"
#include "../xrRenderDX10/3DFluid/dx103DFluidVolume.h"

#include "../xrRender/FHierrarhyVisual.h"

#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

void CRender::level_Load(IReader* fs)
{
	R_ASSERT(0!=g_pGameLevel);
	R_ASSERT(!b_loaded);

	// Static draw packets cache raw shader/pass/state pointers.  A load can be
	// reached after an interrupted or partially completed unload, so do not let
	// a packet from the previous level participate in shader selection here.
	r_dsgraph_static_packet_cache_clear();

	// Begin
	pApp->LoadBegin();
	dxRenderDeviceRender::Instance().Resources->DeferredLoad(TRUE);
	Models->BeginLevelPrefetch(g_pGameLevel ? g_pGameLevel->name().c_str() : "unknown_level");
	IReader* chunk;

	// Shaders
	//	g_pGamePersistent->LoadTitle		("st_loading_shaders");
	g_pGamePersistent->LoadTitle();
	{
		chunk = fs->open_chunk(fsL_SHADERS);
		R_ASSERT2(chunk, "Level doesn't builded correctly.");
		u32 count = chunk->r_u32();
		Shaders.resize(count);
		for (u32 i = 0; i < count; i++) // skip first shader as "reserved" one
		{
			string512 n_sh, n_tlist;
			LPCSTR n = LPCSTR(chunk->pointer());
			chunk->skip_stringZ();
			if (0 == n[0]) continue;
			xr_strcpy(n_sh, n);
			LPSTR delim = strchr(n_sh, '/');
			*delim = 0;
			xr_strcpy(n_tlist, delim + 1);
			Shaders[i] = dxRenderDeviceRender::Instance().Resources->Create(n_sh, n_tlist);
		}
		chunk->close();
	}
	Models->ProcessAsyncFinalization(4, false);

	// Components
	Wallmarks = xr_new<CWallmarksEngine>();
	Details = xr_new<CDetailManager>();

	if (!g_dedicated_server)
	{
		// VB,IB,SWI
		//		g_pGamePersistent->LoadTitle("st_loading_geometry");
		g_pGamePersistent->LoadTitle();
		{
			CStreamReader* geom = FS.rs_open("$level$", "level.geom");
			R_ASSERT2(geom, "level.geom");
			LoadBuffers(geom,FALSE);
			LoadSWIs(geom);
			FS.r_close(geom);
		}

		//...and alternate/fast geometry
		{
			CStreamReader* geom = FS.rs_open("$level$", "level.geomx");
			R_ASSERT2(geom, "level.geomX");
			LoadBuffers(geom,TRUE);
			FS.r_close(geom);
		}
		Models->ProcessAsyncFinalization(4, false);

		// Visuals
		//		g_pGamePersistent->LoadTitle("st_loading_spatial_db");
		g_pGamePersistent->LoadTitle();
		chunk = fs->open_chunk(fsL_VISUALS);
		LoadVisuals(chunk);
		chunk->close();

		// Details
		//		g_pGamePersistent->LoadTitle("st_loading_details");
		g_pGamePersistent->LoadTitle();
		Details->Load();
		Models->ProcessAsyncFinalization(4, false);
	}

	// Sectors
	//	g_pGamePersistent->LoadTitle("st_loading_sectors_portals");
	g_pGamePersistent->LoadTitle();
	LoadSectors(fs);
	Models->ProcessAsyncFinalization(4, false);

	// 3D Fluid
	Load3DFluid();

	// HOM
	HOM.Load();

	// Lights
	// pApp->LoadTitle			("Loading lights...");
	LoadLights(fs);

	// Finish all manifest preparation while the loading screen is still active.
	Models->FinishLevelPrefetch();

	// End
	pApp->LoadEnd();

	// G-buffer targets are persistent across saves.  Drop all temporal data from
	// the previous level before the first frame of this one is composed.
	Target->invalidate_temporal_history();

	// sanity-clear
	lstLODs.clear();
	lstLODgroups.clear();
	mapLOD.clear();
	mapWater.clear();

	// signal loaded
	b_loaded = TRUE;
}

void CRender::level_Unload()
{
	if (0 == g_pGameLevel) return;
	if (!b_loaded) return;

	// The static packet cache owns ShaderElements and stores raw D3D pass/state
	// pointers.  It must be dropped before level visuals and Shaders are
	// released; otherwise a subsequent save load can reuse stale static packets
	// for buildings, fences and other non-detail geometry.
	r_dsgraph_static_packet_cache_clear();

	// Drain runtime preparation before level archives/resources are released.
	Models->FinishLevelPrefetch();
	Models->SaveLevelPrefetchManifest();
	u32 I;

	// HOM
	HOM.Unload();

	//*** Details
	Details->Unload();

	//*** Sectors
	// 1.
	xr_delete(rmPortals);
	pLastSector = 0;
	vLastCameraPos.set(0, 0, 0);
	// 2.
	for (I = 0; I < Sectors.size(); I++) xr_delete(Sectors[I]);
	Sectors.clear_and_free();
	// 3.
	for (I = 0; I < Portals.size(); I++) xr_delete(Portals[I]);
	Portals.clear_and_free();
	PortalTraverser.release_level_memory();

	//*** Lights
	// Glows.Unload			();
	Lights.Unload();
	LP_normal.v_point.clear_and_free();
	LP_normal.v_spot.clear_and_free();
	LP_normal.v_shadowed.clear_and_free();
	LP_pending.v_point.clear_and_free();
	LP_pending.v_spot.clear_and_free();
	LP_pending.v_shadowed.clear_and_free();

	//*** Visuals
	for (I = 0; I < Visuals.size(); I++)
	{
		Visuals[I]->Release();
		xr_delete(Visuals[I]);
	}
	Visuals.clear_and_free();

	//*** SWI
	for (I = 0; I < SWIs.size(); I++)xr_free(SWIs[I].sw);
	SWIs.clear_and_free();

	//*** VB/IB
	for (I = 0; I < nVB.size(); I++) _RELEASE(nVB[I]);
	for (I = 0; I < xVB.size(); I++) _RELEASE(xVB[I]);
	nVB.clear_and_free();
	xVB.clear_and_free();
	for (I = 0; I < nIB.size(); I++) _RELEASE(nIB[I]);
	for (I = 0; I < xIB.size(); I++) _RELEASE(xIB[I]);
	nIB.clear_and_free();
	xIB.clear_and_free();
	nDC.clear_and_free();
	xDC.clear_and_free();

	//*** Components
	xr_delete(Details);
	xr_delete(Wallmarks);

	//*** Shaders
	Shaders.clear_and_free();

	// Level/frame scratch belongs to the unloaded scene; do not carry its high-water
	// capacity through the menu or into the next level.
	Lights_LastFrame.clear_and_free();
	m_staticVisibility.clear_and_free();
	m_dynamicVisibility.clear_and_free();
	m_forwardDynamicVisibility.clear_and_free();
	m_forwardDynamicVisibilityValid = false;
	m_hudLightStateScratch.clear_and_free();
	m_shadowRefactorScratch.clear_and_free();
	m_shadowSpotScratch.clear_and_free();

	if (psDeviceFlags2.test(rsClearModels))
	{
		Models->ClearPool(true);
		dxRenderDeviceRender::Instance().Resources->Dump(false);
		//static int unload_counter = 0;
		//Msg("The Level Unloaded.======================== %d", ++unload_counter);
	}

	b_loaded = FALSE;
}

void CRender::LoadBuffers(CStreamReader* base_fs, BOOL _alternative)
{
	R_ASSERT2(base_fs, "Could not load geometry. File not found.");
	dxRenderDeviceRender::Instance().Resources->Evict();
	//	u32	dwUsage					= D3DUSAGE_WRITEONLY;

	xr_vector<VertexDeclarator>& _DC = _alternative ? xDC : nDC;
	xr_vector<ID3DVertexBuffer*>& _VB = _alternative ? xVB : nVB;
	xr_vector<ID3DIndexBuffer*>& _IB = _alternative ? xIB : nIB;

	// Vertex buffers
	{
		// Use DX9-style declarators
		CStreamReader* fs = base_fs->open_chunk(fsL_VB);
		R_ASSERT2(fs, "Could not load geometry. File 'level.geom?' corrupted.");
		u32 count = fs->r_u32();
		_DC.resize(count);
		_VB.resize(count);
		u32 bufferSize = (MAXD3DDECLLENGTH + 1) * sizeof(D3DVERTEXELEMENT9);
		D3DVERTEXELEMENT9* dcl = (D3DVERTEXELEMENT9*)_alloca(bufferSize);
		for (u32 i = 0; i < count; i++)
		{
			// decl
			//			D3DVERTEXELEMENT9*	dcl		= (D3DVERTEXELEMENT9*) fs().pointer();
			fs->r(dcl, bufferSize);
			fs->advance(-(int)bufferSize);

			u32 dcl_len = D3DXGetDeclLength(dcl) + 1;
			_DC[i].resize(dcl_len);
			fs->r(_DC[i].begin(), dcl_len * sizeof(D3DVERTEXELEMENT9));

			// count, size
			u32 vCount = fs->r_u32();
			u32 vSize = D3DXGetDeclVertexSize(dcl, 0);
			Msg("* [Loading VB] %d verts, %d Kb", vCount, (vCount * vSize) / 1024);

			// Create and fill
			//BYTE*	pData		= 0;
			//R_CHK				(HW.pDevice->CreateVertexBuffer(vCount*vSize,dwUsage,0,D3DPOOL_MANAGED,&_VB[i],0));
			//R_CHK				(_VB[i]->Lock(0,0,(void**)&pData,0));
			//			CopyMemory			(pData,fs().pointer(),vCount*vSize);
			//fs->r				(pData,vCount*vSize);
			//_VB[i]->Unlock		();
			//	TODO: DX10: Check fragmentation.
			//	Check if buffer is less then 2048 kb
			const u32 data_size = vCount * vSize;
			const void* pData = fs->read_view(data_size);
			dx10BufferUtils::CreateVertexBuffer(&_VB[i], pData, data_size);

			//			fs->advance			(vCount*vSize);
		}
		fs->close();
	}

	// Index buffers
	{
		CStreamReader* fs = base_fs->open_chunk(fsL_IB);
		u32 count = fs->r_u32();
		_IB.resize(count);
		for (u32 i = 0; i < count; i++)
		{
			u32 iCount = fs->r_u32();
			Msg("* [Loading IB] %d indices, %d Kb", iCount, (iCount * 2) / 1024);

			// Create and fill
			//BYTE*	pData		= 0;
			//R_CHK				(HW.pDevice->CreateIndexBuffer(iCount*2,dwUsage,D3DFMT_INDEX16,D3DPOOL_MANAGED,&_IB[i],0));
			//R_CHK				(_IB[i]->Lock(0,0,(void**)&pData,0));
			//			CopyMemory			(pData,fs().pointer(),iCount*2);
			//fs->r				(pData,iCount*2);
			//_IB[i]->Unlock		();

			//	TODO: DX10: Check fragmentation.
			//	Check if buffer is less then 2048 kb
			const u32 data_size = iCount * 2;
			const void* pData = fs->read_view(data_size);
			dx10BufferUtils::CreateIndexBuffer(&_IB[i], pData, data_size);

			//			fs().advance		(iCount*2);
		}
		fs->close();
	}
}

void CRender::LoadVisuals(IReader* fs)
{
	if (Visuals.capacity() < 4096)
		Visuals.reserve(4096);

	u32 visual_id = 0;
	ogf_header H;

	for (IReader* chunk = fs->open_chunk_iterator(visual_id); chunk;
		chunk = fs->open_chunk_iterator(visual_id, chunk))
	{
		chunk->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
		dxRender_Visual* visual = Models->Instance_Create(H.type);
		visual->Load(nullptr, chunk, 0);
		Visuals.push_back(visual);
	}
}

void CRender::LoadLights(IReader* fs)
{
	// lights
	Lights.Load(fs);
	Lights.LoadHemi();
}

struct b_portal
{
	u16 sector_front;
	u16 sector_back;
	svector<Fvector, 6> vertices;
};

void CRender::LoadSectors(IReader* fs)
{
	// Keep the portal chunk open once. The old path searched the parent reader
	// twice and copied each portal record through a temporary stack object.
	IReader* portal_reader = fs->open_chunk(fsL_PORTALS);
	const u32 portal_size = portal_reader ? static_cast<u32>(portal_reader->length()) : 0;
	R_ASSERT((portal_size % sizeof(b_portal)) == 0);
	const u32 count = portal_size / sizeof(b_portal);
	const b_portal* portal_data = portal_reader ? static_cast<const b_portal*>(portal_reader->pointer()) : nullptr;

	Portals.resize(count);
	for (u32 portal = 0; portal < count; ++portal)
		Portals[portal] = xr_new<CPortal>();

	// load sectors
	if (Sectors.capacity() < 128)
		Sectors.reserve(128);
	IReader* S = fs->open_chunk(fsL_SECTORS);
	R_ASSERT(S);
	u32 sector_id = 0;
	for (IReader* P = S->open_chunk_iterator(sector_id); P;
		P = S->open_chunk_iterator(sector_id, P))
	{
		CSector* sector = xr_new<CSector>();
		sector->load(*P);
		Sectors.push_back(sector);
	}
	S->close();

	// load portals
	if (count)
	{
		CDB::Collector CL;
		CL.reserve(static_cast<size_t>(count) * 12u, static_cast<size_t>(count) * 4u);
		for (u32 i = 0; i < count; ++i)
		{
			const b_portal& P = portal_data[i];
			CPortal* __P = (CPortal*)Portals[i];
			__P->Setup(P.vertices.begin(), P.vertices.size(),
			           (CSector*)getSector(P.sector_front),
			           (CSector*)getSector(P.sector_back));
			for (u32 j = 2; j < P.vertices.size(); ++j)
				CL.add_face_packed_D(
					P.vertices[0], P.vertices[j - 1], P.vertices[j],
					u32(i)
				);
		}
		if (CL.getTS() < 2)
		{
			Fvector v1, v2, v3;
			v1.set(-20000.f, -20000.f, -20000.f);
			v2.set(-20001.f, -20001.f, -20001.f);
			v3.set(-20002.f, -20002.f, -20002.f);
			CL.add_face_packed_D(v1, v2, v3, 0);
		}

		// build portal model
		rmPortals = xr_new<CDB::MODEL>();
		rmPortals->build(CL.getV(), int(CL.getVS()), CL.getT(), int(CL.getTS()));
	}
	else
	{
		rmPortals = 0;
	}

	if (portal_reader)
		portal_reader->close();

	// debug
	//	for (int d=0; d<Sectors.size(); d++)
	//		Sectors[d]->DebugDump	();

	pLastSector = 0;
}

void CRender::LoadSWIs(CStreamReader* base_fs)
{
	// open_chunk() already performs the lookup; avoid a preceding find_chunk().
	CStreamReader* fs = base_fs->open_chunk(fsL_SWIS);
	if (fs)
	{
		u32 item_count = fs->r_u32();

		xr_vector<FSlideWindowItem>::iterator it = SWIs.begin();
		xr_vector<FSlideWindowItem>::iterator it_e = SWIs.end();

		for (; it != it_e; ++it)
			xr_free((*it).sw);

		SWIs.clear_not_free();

		SWIs.resize(item_count);
		for (u32 c = 0; c < item_count; c++)
		{
			FSlideWindowItem& swi = SWIs[c];
			swi.reserved[0] = fs->r_u32();
			swi.reserved[1] = fs->r_u32();
			swi.reserved[2] = fs->r_u32();
			swi.reserved[3] = fs->r_u32();
			swi.count = fs->r_u32();
			VERIFY(NULL==swi.sw);
			swi.sw = xr_alloc_uninitialized<FSlideWindow>(swi.count);
			fs->r(swi.sw, sizeof(FSlideWindow) * swi.count);
		}
		fs->close();
	}
}

void CRender::Load3DFluid()
{
	//if (strstr(Core.Params,"-no_volumetric_fog"))
	if (!RImplementation.o.volumetricfog)
		return;

	IReader* F = FS.r_open("$level$", "level.fog_vol");
	if (F)
	{
		u16 version = F->r_u16();

		if (version == 3)
		{
			u32 cnt = F->r_u32();
			for (u32 i = 0; i < cnt; ++i)
			{
				dx103DFluidVolume* pVolume = xr_new<dx103DFluidVolume>();
				pVolume->Load("", F, 0);

				//	Attach to sector's static geometry
				CSector* pSector = (CSector*)detectSector(pVolume->getVisData().sphere.P);
				//	3DFluid volume must be in render sector
				VERIFY(pSector);

				dxRender_Visual* pRoot = pSector->root();
				//	Sector must have root
				VERIFY(pRoot);
				VERIFY(pRoot->getType() == MT_HIERRARHY);

				((FHierrarhyVisual*)pRoot)->children.push_back(pVolume);
			}
		}

		FS.r_close(F);
	}
}
