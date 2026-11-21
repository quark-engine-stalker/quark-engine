#include "stdafx.h"
#pragma hdrstop

#pragma warning(disable:4995)
#include <d3dx9.h>
#ifndef _EDITOR
#pragma comment( lib, "d3dx9.lib"		)
#include "../../xrEngine/render.h"
#endif
#pragma warning(default:4995)

#include "../xrRender/ResourceManager.h"
#include "../xrRender/tss.h"
#include "../xrRender/blenders/blender.h"
#include "../xrRender/blenders/blender_recorder.h"

#include "../xrRenderDX10/dx10BufferUtils.h"
#include "../xrRenderDX10/dx10ConstantBuffer.h"

#include "../xrRender/ShaderResourceTraits.h"

ref_hs CResourceManager::_CreateHS(LPCSTR Name)
{
	return CreateShader<SHS>(Name);
}

void CResourceManager::_DeleteHS(const SHS* HS)
{
	DestroyShader(HS);
}

ref_ds CResourceManager::_CreateDS(LPCSTR Name)
{
	return CreateShader<SDS>(Name);
}

void CResourceManager::_DeleteDS(const SDS* DS)
{
	DestroyShader(DS);
}

ref_cs CResourceManager::_CreateCS(LPCSTR Name)
{
	return CreateShader<SCS>(Name);
}

void CResourceManager::_DeleteCS(const SCS* CS)
{
	DestroyShader(CS);
}

void fix_texture_name(LPSTR fn);

template <class T>
BOOL reclaim(xr_vector<T*>& vec, const T* ptr)
{
	xr_vector<T*>::iterator it = vec.begin();
	xr_vector<T*>::iterator end = vec.end();
	for (; it != end; it++)
		if (*it == ptr)
		{
			vec.erase(it);
			return TRUE;
		}
	return FALSE;
}

//--------------------------------------------------------------------------------------------------------------
ref_state CResourceManager::_CreateState(SimulatorStates& state_code)
{
	xrCriticalSectionGuard guard(shaderGraphGuard);
	// Search equal state-code 
	for (u32 it = 0; it < v_states.size(); it++)
	{
		SState* C = v_states[it];;
		SimulatorStates& base = C->state_code;
		if (base.equal(state_code) && C->pin_alive()) return ref_state(C, false);
	}

	// Create New
	v_states.push_back(xr_new<SState>());
	v_states.back()->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_states.back()->state = ID3DState::Create(state_code);
	v_states.back()->state_code = state_code;
	return ref_state(v_states.back());
}

void CResourceManager::_DeleteState(const SState* state)
{
	if (0 == (state->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderGraphGuard);
	if (reclaim(v_states, state)) return;
	Msg("! ERROR: Failed to find compiled stateblock");
}

//--------------------------------------------------------------------------------------------------------------
ref_pass CResourceManager::_CreatePass(const SPass& proto)
{
	xrCriticalSectionGuard guard(shaderGraphGuard);
	for (u32 it = 0; it < v_passes.size(); it++)
		if (v_passes[it]->equal(proto) && v_passes[it]->pin_alive())
			return ref_pass(v_passes[it], false);

	SPass* P = xr_new<SPass>();
	P->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	P->state = proto.state;
	P->ps = proto.ps;
	P->vs = proto.vs;
	P->gs = proto.gs;
	P->hs = proto.hs;
	P->ds = proto.ds;
	P->cs = proto.cs;
	P->constants = proto.constants;
	P->T = proto.T;
#ifdef _EDITOR
	P->M						=	proto.M;
#endif
	P->C = proto.C;

	v_passes.push_back(P);
	return P;
}

void CResourceManager::_DeletePass(const SPass* P)
{
	if (0 == (P->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderGraphGuard);
	if (reclaim(v_passes, P)) return;
	Msg("! ERROR: Failed to find compiled pass");
}

//--------------------------------------------------------------------------------------------------------------
ref_vs CResourceManager::_CreateVS(LPCSTR _name)
{
	PROF_EVENT();
	xrCriticalSectionGuard guard(shaderProgramGuard);
	int skinning = -1;
	string_path name;
	xr_strcpy(name, _name);
	if (0 == ::Render->m_skinning) { xr_strcat(name, "_0"); skinning = 0; }
	if (1 == ::Render->m_skinning) { xr_strcat(name, "_1"); skinning = 1; }
	if (2 == ::Render->m_skinning) { xr_strcat(name, "_2"); skinning = 2; }
	if (3 == ::Render->m_skinning) { xr_strcat(name, "_3"); skinning = 3; }
	if (4 == ::Render->m_skinning) { xr_strcat(name, "_4"); skinning = 4; }
	LPSTR N = LPSTR(name);
	map_VS::iterator I = m_vs.find(N);
	if (I != m_vs.end())
	{
		if (I->second->pin_alive()) return ref_vs(I->second, false);
		// dying: its destructor waits for this lock and leaves the new entry in the map alone
		m_vs.erase(I);
	}
	{
		SVS* _vs = xr_new<SVS>();
		_vs->skinning = skinning;
		_vs->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_vs.insert(mk_pair(_vs->set_name(name), _vs));
		//_vs->vs				= NULL;
		//_vs->signature		= NULL;
		if (0 == stricmp(_name, "null"))
		{
			return _vs;
		}

		string_path shName;
		{
			const char* pchr = strchr(_name, '(');
			ptrdiff_t size = pchr ? pchr - _name : xr_strlen(_name);
			strncpy(shName, _name, size);
			shName[size] = 0;
		}

		string_path cname;
		strconcat(sizeof(cname), cname, ::Render->getShaderPath(),/*_name*/shName, ".vs");
		FS.update_path(cname, "$game_shaders$", cname);
		//		LPCSTR						target		= NULL;

		// duplicate and zero-terminate
		IReader* file = FS.r_open(cname);
		//	TODO: DX10: HACK: Implement all shaders. Remove this for PS
		if (!file)
		{
			string1024 tmp;
			xr_sprintf(tmp, "DX10: %s is missing. Replace with stub_default.vs", cname);
			Msg(tmp);
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), "stub_default", ".vs");
			FS.update_path(cname, "$game_shaders$", cname);
			file = FS.r_open(cname);
		}
		u32 const size = file->length();
		char* const data = (LPSTR)_alloca(size + 1);
		CopyMemory(data, file->pointer(), size);
		data[size] = 0;
		FS.r_close(file);

		// Select target
		LPCSTR c_target = "vs_2_0";
		LPCSTR c_entry = "main";
		if (HW.Caps.geometry_major >= 2) c_target = "vs_2_0";
		else c_target = "vs_1_1";

		if (strstr(data, "main_vs_1_1"))
		{
			c_target = "vs_1_1";
			c_entry = "main_vs_1_1";
		}
		if (strstr(data, "main_vs_2_0"))
		{
			c_target = "vs_2_0";
			c_entry = "main_vs_2_0";
		}

		HRESULT const _hr = ::Render->shader_compile(name, (DWORD const*)data, size, c_entry, c_target,
		                                             D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, (void*&)_vs);

		VERIFY(SUCCEEDED(_hr));

		CHECK_OR_EXIT(
			!FAILED(_hr),
			make_string("Shader compilation failed, check your log file for additional information.")
		);

		return _vs;
	}
}

void CResourceManager::_DeleteVS(const SVS* vs)
{
	if (0 == (vs->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderProgramGuard);
	LPSTR N = LPSTR(*vs->cName);
	map_VS::iterator I = m_vs.find(N);
	if (I != m_vs.end())
	{
		if (I->second == vs) // else a creator replaced this dying entry
			m_vs.erase(I);
		xr_vector<SDeclaration*>::iterator iDecl;
		for (iDecl = v_declarations.begin(); iDecl != v_declarations.end(); ++iDecl)
		{
			xr_unordered_flat_map<ID3DBlob*, ID3DInputLayout*>::iterator iLayout;
			iLayout = (*iDecl)->vs_to_layout.find(vs->signature->signature);
			if (iLayout != (*iDecl)->vs_to_layout.end())
			{
				//	Release vertex layout
				_RELEASE(iLayout->second);
				(*iDecl)->vs_to_layout.erase(iLayout);
			}
		}
		return;
	}
	Msg("! ERROR: Failed to find compiled vertex-shader '%s'", *vs->cName);
}

//--------------------------------------------------------------------------------------------------------------
ref_ps CResourceManager::_CreatePS(LPCSTR _name)
{
	PROF_EVENT();
	xrCriticalSectionGuard guard(shaderProgramGuard);
	string_path name;
	xr_strcpy(name, _name);
	if (0 == ::Render->m_MSAASample) xr_strcat(name, "_0");
	if (1 == ::Render->m_MSAASample) xr_strcat(name, "_1");
	if (2 == ::Render->m_MSAASample) xr_strcat(name, "_2");
	if (3 == ::Render->m_MSAASample) xr_strcat(name, "_3");
	if (4 == ::Render->m_MSAASample) xr_strcat(name, "_4");
	if (5 == ::Render->m_MSAASample) xr_strcat(name, "_5");
	if (6 == ::Render->m_MSAASample) xr_strcat(name, "_6");
	if (7 == ::Render->m_MSAASample) xr_strcat(name, "_7");
	// A depth-only pass of the material: the same source, compiled for its discard only (CRender::shader_compile)
	if (m_depth_only_ps_thread == GetCurrentThreadId() && 0 != stricmp(_name, "null"))
		xr_strcat(name, "@depth");
	LPSTR N = LPSTR(name);
	map_PS::iterator I = m_ps.find(N);
	if (I != m_ps.end())
	{
		if (I->second->pin_alive()) return ref_ps(I->second, false);
		// dying: its destructor waits for this lock and leaves the new entry in the map alone
		m_ps.erase(I);
	}
	{
		SPS* _ps = xr_new<SPS>();
		_ps->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_ps.insert(mk_pair(_ps->set_name(name), _ps));
		if (0 == stricmp(_name, "null"))
		{
			_ps->ps = NULL;
			return _ps;
		}

		string_path shName;
		const char* pchr = strchr(_name, '(');
		ptrdiff_t strSize = pchr ? pchr - _name : xr_strlen(_name);
		strncpy(shName, _name, strSize);
		shName[strSize] = 0;

		// Open file
		string_path cname;
		strconcat(sizeof(cname), cname, ::Render->getShaderPath(),/*_name*/shName, ".ps");
		FS.update_path(cname, "$game_shaders$", cname);

		// duplicate and zero-terminate
		IReader* file = FS.r_open(cname);
		//	TODO: DX10: HACK: Implement all shaders. Remove this for PS
		if (!file)
		{
			string1024 tmp;
			//	TODO: HACK: Test failure
			//Memory.mem_compact();
			xr_sprintf(tmp, "DX10: %s is missing. Replace with stub_default.ps", cname);
			Msg(tmp);
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), "stub_default", ".ps");
			FS.update_path(cname, "$game_shaders$", cname);
			file = FS.r_open(cname);
		}

		R_ASSERT2(file, cname);
		u32 const size = file->length();
		char* const data = (LPSTR)_alloca(size + 1);
		CopyMemory(data, file->pointer(), size);
		data[size] = 0;
		FS.r_close(file);

		// Select target
		LPCSTR c_target = "ps_2_0";
		LPCSTR c_entry = "main";
		if (strstr(data, "main_ps_1_1"))
		{
			c_target = "ps_1_1";
			c_entry = "main_ps_1_1";
		}
		if (strstr(data, "main_ps_1_2"))
		{
			c_target = "ps_1_2";
			c_entry = "main_ps_1_2";
		}
		if (strstr(data, "main_ps_1_3"))
		{
			c_target = "ps_1_3";
			c_entry = "main_ps_1_3";
		}
		if (strstr(data, "main_ps_1_4"))
		{
			c_target = "ps_1_4";
			c_entry = "main_ps_1_4";
		}
		if (strstr(data, "main_ps_2_0"))
		{
			c_target = "ps_2_0";
			c_entry = "main_ps_2_0";
		}

		HRESULT const _hr = ::Render->shader_compile(name, (DWORD const*)data, size, c_entry, c_target,
		                                             D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, (void*&)_ps);

		VERIFY(SUCCEEDED(_hr));

		CHECK_OR_EXIT(
			!FAILED(_hr),
			make_string("Shader compilation failed, check your log file for additional information.")
		);

		return _ps;
	}
}

void CResourceManager::_DeletePS(const SPS* ps)
{
	if (0 == (ps->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderProgramGuard);
	LPSTR N = LPSTR(*ps->cName);
	map_PS::iterator I = m_ps.find(N);
	if (I != m_ps.end())
	{
		if (I->second == ps) // else a creator replaced this dying entry
			m_ps.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find compiled pixel-shader '%s'", *ps->cName);
}

//--------------------------------------------------------------------------------------------------------------
ref_gs CResourceManager::_CreateGS(LPCSTR name)
{
	xrCriticalSectionGuard guard(shaderProgramGuard);
	LPSTR N = LPSTR(name);
	map_GS::iterator I = m_gs.find(N);
	if (I != m_gs.end())
	{
		if (I->second->pin_alive()) return ref_gs(I->second, false);
		// dying: its destructor waits for this lock and leaves the new entry in the map alone
		m_gs.erase(I);
	}
	{
		SGS* _gs = xr_new<SGS>();
		_gs->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_gs.insert(mk_pair(_gs->set_name(name), _gs));
		if (0 == stricmp(name, "null"))
		{
			_gs->gs = NULL;
			return _gs;
		}

		// Open file
		string_path cname;
		strconcat(sizeof(cname), cname, ::Render->getShaderPath(), name, ".gs");
		FS.update_path(cname, "$game_shaders$", cname);

		// duplicate and zero-terminate
		IReader* file = FS.r_open(cname);
		//	TODO: DX10: HACK: Implement all shaders. Remove this for PS
		if (!file)
		{
			string1024 tmp;
			//	TODO: HACK: Test failure
			//Memory.mem_compact();
			xr_sprintf(tmp, "DX10: %s is missing. Replace with stub_default.gs", cname);
			Msg(tmp);
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), "stub_default", ".gs");
			FS.update_path(cname, "$game_shaders$", cname);
			file = FS.r_open(cname);
		}

		R_ASSERT2(file, cname);

		// Select target
		LPCSTR c_target = "gs_4_0";
		LPCSTR c_entry = "main";

		HRESULT const _hr = ::Render->shader_compile(name, (DWORD const*)file->pointer(), file->length(), c_entry,
		                                             c_target, D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, (void*&)_gs);

		VERIFY(SUCCEEDED(_hr));

		FS.r_close(file);

		CHECK_OR_EXIT(
			!FAILED(_hr),
			make_string("Shader compilation failed, check your log file for additional information.")
		);

		return _gs;
	}
}

void CResourceManager::_DeleteGS(const SGS* gs)
{
	if (0 == (gs->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderProgramGuard);
	LPSTR N = LPSTR(*gs->cName);
	map_GS::iterator I = m_gs.find(N);
	if (I != m_gs.end())
	{
		if (I->second == gs) // else a creator replaced this dying entry
			m_gs.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find compiled geometry shader '%s'", *gs->cName);
}

//--------------------------------------------------------------------------------------------------------------
static BOOL dcl_equal(D3DVERTEXELEMENT9* a, D3DVERTEXELEMENT9* b)
{
	// check sizes
	u32 a_size = D3DXGetDeclLength(a);
	u32 b_size = D3DXGetDeclLength(b);
	if (a_size != b_size) return FALSE;
	return 0 == memcmp(a, b, a_size * sizeof(D3DVERTEXELEMENT9));
}

ref_declaration CResourceManager::_CreateDecl(D3DVERTEXELEMENT9* dcl)
{
	xrCriticalSectionGuard guard(geometryGuard);
	// Search equal code
	for (u32 it = 0; it < v_declarations.size(); it++)
	{
		SDeclaration* D = v_declarations[it];;
		if (dcl_equal(dcl, &*D->dcl_code.begin()) && D->pin_alive()) return ref_declaration(D, false);
	}

	// Create _new
	SDeclaration* D = xr_new<SDeclaration>();
	u32 dcl_size = D3DXGetDeclLength(dcl) + 1;
	//	Don't need it for DirectX 10 here
	//CHK_DX					(HW.pDevice->CreateVertexDeclaration(dcl,&D->dcl));
	D->dcl_code.assign(dcl, dcl + dcl_size);
	dx10BufferUtils::ConvertVertexDeclaration(D->dcl_code, D->dx10_dcl_code);
	D->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_declarations.push_back(D);
	return D;
}

void CResourceManager::_DeleteDecl(const SDeclaration* dcl)
{
	if (0 == (dcl->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(geometryGuard);
	if (reclaim(v_declarations, dcl)) return;
	Msg("! ERROR: Failed to find compiled vertex-declarator");
}

//--------------------------------------------------------------------------------------------------------------
ref_ctable CResourceManager::_CreateConstantTable(R_constant_table& C)
{
	xrCriticalSectionGuard guard(shaderProgramGuard);
	if (C.empty()) return NULL;

	for (u32 it = 0; it < v_constant_tables.size(); it++)
		if (v_constant_tables[it]->equal(C) && v_constant_tables[it]->pin_alive())
			return ref_ctable(v_constant_tables[it], false);
	v_constant_tables.push_back(xr_new<R_constant_table>(C));
	v_constant_tables.back()->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	return ref_ctable(v_constant_tables.back());
}

void CResourceManager::_DeleteConstantTable(const R_constant_table* C)
{
	if (0 == (C->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderProgramGuard);
	if (reclaim(v_constant_tables, C)) return;
	Msg("! ERROR: Failed to find compiled constant-table");
}

//--------------------------------------------------------------------------------------------------------------
CRT* CResourceManager::_CreateRT(LPCSTR Name, u32 w, u32 h, D3DFORMAT f, u32 SampleCount, bool useUAV)
{
	R_ASSERT(Name && Name[0] && w && h);

	// ***** first pass - search already created RT
	LPSTR N = LPSTR(Name);
	xrCriticalSectionGuard guard(renderTargetGuard);
	map_RT::iterator I = m_rtargets.find(N);
	if (I != m_rtargets.end()) return I->second;
	else
	{
		CRT* RT = xr_new<CRT>();
		RT->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_rtargets.insert(mk_pair(RT->set_name(Name), RT));
		RT->dwWidth = w;
		RT->dwHeight = h;
		RT->fmt = f;
		RT->m_sample_count = SampleCount;
		RT->m_use_uav = useUAV;
		if (Device.b_is_Ready) RT->create(Name, w, h, f, SampleCount, useUAV);
		return RT;
	}
}

void CResourceManager::_DeleteRT(const CRT* RT)
{
	if (0 == (RT->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(renderTargetGuard);
	LPSTR N = LPSTR(*RT->cName);
	map_RT::iterator I = m_rtargets.find(N);
	if (I != m_rtargets.end())
	{
		m_rtargets.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find render-target '%s'", *RT->cName);
}

/*	//	DX10 cut 
//--------------------------------------------------------------------------------------------------------------
CRTC*	CResourceManager::_CreateRTC		(LPCSTR Name, u32 size,	D3DFORMAT f)
{
	R_ASSERT(Name && Name[0] && size);

	// ***** first pass - search already created RTC
	LPSTR N = LPSTR(Name);
	map_RTC::iterator I = m_rtargets_c.find	(N);
	if (I!=m_rtargets_c.end())	return I->second;
	else
	{
		CRTC *RT				=	xr_new<CRTC>();
		RT->dwFlags				|=	xr_resource_flagged::RF_REGISTERED;
		m_rtargets_c.insert		(mk_pair(RT->set_name(Name),RT));
		if (Device.b_is_Ready)	RT->create	(Name,size,f);
		return					RT;
	}
}
void	CResourceManager::_DeleteRTC		(const CRTC* RT)
{
	if (0==(RT->dwFlags&xr_resource_flagged::RF_REGISTERED))	return;
	LPSTR N				= LPSTR		(*RT->cName);
	map_RTC::iterator I	= m_rtargets_c.find	(N);
	if (I!=m_rtargets_c.end())	{
		m_rtargets_c.erase(I);
		return;
	}
	Msg	("! ERROR: Failed to find render-target '%s'",*RT->cName);
}
*/
//--------------------------------------------------------------------------------------------------------------
void CResourceManager::DBG_VerifyGeoms()
{
	/*
	for (u32 it=0; it<v_geoms.size(); it++)
	{
	SGeometry* G					= v_geoms[it];

	D3DVERTEXELEMENT9		test	[MAX_FVF_DECL_SIZE];
	u32						size	= 0;
	G->dcl->GetDeclaration			(test,(unsigned int*)&size);
	u32 vb_stride					= D3DXGetDeclVertexSize	(test,0);
	u32 vb_stride_cached			= G->vb_stride;
	R_ASSERT						(vb_stride == vb_stride_cached);
	}
	*/
}

ref_geom CResourceManager::CreateGeom(D3DVERTEXELEMENT9* decl, ID3DVertexBuffer* vb, ID3DIndexBuffer* ib)
{
	xrCriticalSectionGuard guard(geometryGuard);
	R_ASSERT(decl && vb);

	ref_declaration dcl = _CreateDecl(decl);
	u32 vb_stride = D3DXGetDeclVertexSize(decl, 0);

	// ***** first pass - search already loaded shader
	for (u32 it = 0; it < v_geoms.size(); it++)
	{
		SGeometry& G = *(v_geoms[it]);
		if ((G.dcl == dcl) && (G.vb == vb) && (G.ib == ib) && (G.vb_stride == vb_stride) && G.pin_alive())
			return ref_geom(&G, false);
	}

	SGeometry* Geom = xr_new<SGeometry>();
	Geom->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	Geom->dcl = dcl;
	Geom->vb = vb;
	Geom->vb_stride = vb_stride;
	Geom->ib = ib;
	v_geoms.push_back(Geom);
	return Geom;
}

ref_geom CResourceManager::CreateGeom(u32 FVF, ID3DVertexBuffer* vb, ID3DIndexBuffer* ib)
{
	xrCriticalSectionGuard guard(geometryGuard);
	D3DVERTEXELEMENT9 dcl [MAX_FVF_DECL_SIZE];
	CHK_DX(D3DXDeclaratorFromFVF(FVF,dcl));
	return CreateGeom(dcl, vb, ib);
}

void CResourceManager::DeleteGeom(const SGeometry* Geom)
{
	if (0 == (Geom->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(geometryGuard);
	if (reclaim(v_geoms, Geom)) return;
	Msg("! ERROR: Failed to find compiled geometry-declaration");
}

//--------------------------------------------------------------------------------------------------------------
CTexture* CResourceManager::_CreateTexture(LPCSTR _Name)
{
	if (0 == xr_strcmp(_Name, "null")) return nullptr;
	R_ASSERT(_Name && _Name[0]);

	string_path Name;
	xr_strcpy(Name, _Name);
	fix_texture_name(Name);

	// Fast registry lookup. Do not keep the registry lock while performing
	// filesystem I/O or creating D3D resources: unrelated textures must be
	// able to preload in parallel.
	{
		xrCriticalSectionGuard guard(textureGuard);
		map_TextureIt found = m_textures.find(LPSTR(Name));
		if (found != m_textures.end())
		{
			found->second->dwReference.fetch_add(1, std::memory_order_relaxed);
			return found->second;
		}
	}

	// A demand load and a speculative worker may request the same DDS. Serialize
	// only its creation, outside the registry lock; unrelated stripes still load
	// concurrently. The second lookup also acquires the caller's strong owner.
	u32 hash = 2166136261u;
	for (const unsigned char* c = reinterpret_cast<const unsigned char*>(Name); *c; ++c)
		hash = (hash ^ *c) * 16777619u;
	xrCriticalSectionGuard load_guard(textureLoadGuards[hash % 64u]);
	{
		xrCriticalSectionGuard guard(textureGuard);
		map_TextureIt found = m_textures.find(LPSTR(Name));
		if (found != m_textures.end())
		{
			found->second->dwReference.fetch_add(1, std::memory_order_relaxed);
			return found->second;
		}
	}

	CTexture* candidate = xr_new<CTexture>();
	candidate->set_name(Name);
	candidate->Preload();
	if (Device.b_is_Ready && !bDeferredLoad.load(std::memory_order_acquire))
		// This is a demanded shader/UI texture. Creating a staging resource here
		// merely defers a second allocation and CopyResource to its first draw.
		candidate->Load(false);

	// Publication happens before releasing the creation stripe. Registry readers
	// acquire their reference atomically with lookup, including cache retirement.
	{
		xrCriticalSectionGuard guard(textureGuard);
		map_TextureIt found = m_textures.find(LPSTR(Name));
		if (found != m_textures.end())
		{
			xr_delete(candidate);
			found->second->dwReference.fetch_add(1, std::memory_order_relaxed);
			return found->second;
		}

		candidate->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_textures.insert(mk_pair(*candidate->cName, candidate));
		candidate->dwReference.fetch_add(1, std::memory_order_relaxed);
	}

	return candidate;
}

ref_texture CResourceManager::CreateTextureRef(LPCSTR name)
{
	// Acquire under the same registry lock as lookup/publication. A speculative
	// cache may retire its final owner before the caller installs a shader ref.
	return ref_texture(_CreateTexture(name), false);
}

void CResourceManager::ReleaseTextureReference(CTexture* texture)
{
	{
		xrCriticalSectionGuard guard(textureGuard);
		if (texture->dwReference.fetch_sub(1, std::memory_order_acq_rel) != 1)
			return;
		if (texture->dwFlags & xr_resource_flagged::RF_REGISTERED)
		{
			const auto found = m_textures.find(texture->cName.c_str());
			if (found != m_textures.end() && found->second == texture)
				m_textures.erase(found);
			texture->dwFlags &= ~xr_resource_flagged::RF_REGISTERED;
		}
	}
	// Unload/destruction may release other resources. Registry lookup can now
	// publish a replacement with this name safely while the old object retires.
	xr_delete(texture);
}

void CResourceManager::PrefetchTextureGpuResident(LPCSTR _Name)
{
	if (!m_allow_speculative_texture_upload.load(std::memory_order_acquire) || !_Name || !_Name[0])
		return;

	string_path Name;
	xr_strcpy(Name, _Name);
	fix_texture_name(Name);
	const shared_str key = Name;

	// Speculative model workers should never duplicate the same expensive file
	// read + D3D resource creation. Demand loads share the creation stripe, so
	// they adopt the same object instead of allocating a temporary GPU duplicate.
	{
		xrCriticalSectionGuard guard(texturePrefetchGuard);
		if (m_texturePrefetchInFlight.find(key) != m_texturePrefetchInFlight.end())
			return;
		m_texturePrefetchInFlight.insert(key);
	}

	PrefetchTexture(Name);

	{
		xrCriticalSectionGuard guard(texturePrefetchGuard);
		m_texturePrefetchInFlight.erase(key);
	}
}

void CResourceManager::_DeleteTexture(const CTexture* T)
{
	// DBG_VerifyTextures	();

	if (0 == (T->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(textureGuard);
	LPSTR N = LPSTR(*T->cName);
	map_Texture::iterator I = m_textures.find(N);
	if (I != m_textures.end())
	{
		m_textures.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find texture surface '%s'", *T->cName);
}

#ifdef DEBUG
void	CResourceManager::DBG_VerifyTextures	()
{
	map_Texture::iterator I		= m_textures.begin	();
	map_Texture::iterator E		= m_textures.end	();
	for (; I!=E; I++) 
	{
		R_ASSERT(I->first);
		R_ASSERT(I->second);
		R_ASSERT(I->second->cName);
		R_ASSERT(0==xr_strcmp(I->first,*I->second->cName));
	}
}
#endif

//--------------------------------------------------------------------------------------------------------------
CMatrix* CResourceManager::_CreateMatrix(LPCSTR Name)
{
	R_ASSERT(Name && Name[0]);
	if (0 == stricmp(Name, "$null")) return NULL;

	xrCriticalSectionGuard guard(parameterGuard);
	LPSTR N = LPSTR(Name);
	map_Matrix::iterator I = m_matrices.find(N);
	if (I != m_matrices.end()) return I->second;
	else
	{
		CMatrix* M = xr_new<CMatrix>();
		M->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		M->dwReference.store(1, std::memory_order_relaxed);
		m_matrices.insert(mk_pair(M->set_name(Name), M));
		return M;
	}
}

void CResourceManager::_DeleteMatrix(const CMatrix* M)
{
	if (0 == (M->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(parameterGuard);
	LPSTR N = LPSTR(*M->cName);
	map_Matrix::iterator I = m_matrices.find(N);
	if (I != m_matrices.end())
	{
		m_matrices.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find xform-def '%s'", *M->cName);
}

void CResourceManager::ED_UpdateMatrix(LPCSTR Name, CMatrix* data)
{
	CMatrix* M = _CreateMatrix(Name);
	*M = *data;
}

//--------------------------------------------------------------------------------------------------------------
CConstant* CResourceManager::_CreateConstant(LPCSTR Name)
{
	R_ASSERT(Name && Name[0]);
	if (0 == stricmp(Name, "$null")) return NULL;

	xrCriticalSectionGuard guard(parameterGuard);
	LPSTR N = LPSTR(Name);
	map_Constant::iterator I = m_constants.find(N);
	if (I != m_constants.end()) return I->second;
	else
	{
		CConstant* C = xr_new<CConstant>();
		C->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		C->dwReference.store(1, std::memory_order_relaxed);
		m_constants.insert(mk_pair(C->set_name(Name), C));
		return C;
	}
}

void CResourceManager::_DeleteConstant(const CConstant* C)
{
	if (0 == (C->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(parameterGuard);
	LPSTR N = LPSTR(*C->cName);
	map_Constant::iterator I = m_constants.find(N);
	if (I != m_constants.end())
	{
		m_constants.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find R1-constant-def '%s'", *C->cName);
}

void CResourceManager::ED_UpdateConstant(LPCSTR Name, CConstant* data)
{
	CConstant* C = _CreateConstant(Name);
	*C = *data;
}

//--------------------------------------------------------------------------------------------------------------
bool cmp_tl(const std::pair<u32, ref_texture>& _1, const std::pair<u32, ref_texture>& _2)
{
	return _1.first < _2.first;
}

ref_texture_list CResourceManager::_CreateTextureList(STextureList& L)
{
	xrCriticalSectionGuard guard(textureGuard);
	std::sort(L.begin(), L.end(), cmp_tl);
	for (u32 it = 0; it < lst_textures.size(); it++)
	{
		STextureList* base = lst_textures[it];
		if (L.equal(*base) && base->pin_alive()) return ref_texture_list(base, false);
	}
	STextureList* lst = xr_new<STextureList>(L);
	lst->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	lst_textures.push_back(lst);
	return lst;
}

void CResourceManager::_DeleteTextureList(const STextureList* L)
{
	if (0 == (L->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(textureGuard);
	if (reclaim(lst_textures, L)) return;
	Msg("! ERROR: Failed to find compiled list of textures");
}

//--------------------------------------------------------------------------------------------------------------
ref_matrix_list CResourceManager::_CreateMatrixList(SMatrixList& L)
{
	BOOL bEmpty = TRUE;
	for (u32 i = 0; i < L.size(); i++)
		if (L[i])
		{
			bEmpty = FALSE;
			break;
		}
	if (bEmpty) return NULL;

	xrCriticalSectionGuard guard(parameterGuard);
	for (u32 it = 0; it < lst_matrices.size(); it++)
	{
		SMatrixList* base = lst_matrices[it];
		if (L.equal(*base) && base->pin_alive()) return ref_matrix_list(base, false);
	}
	SMatrixList* lst = xr_new<SMatrixList>(L);
	lst->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	lst_matrices.push_back(lst);
	return lst;
}

void CResourceManager::_DeleteMatrixList(const SMatrixList* L)
{
	if (0 == (L->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(parameterGuard);
	if (reclaim(lst_matrices, L)) return;
	Msg("! ERROR: Failed to find compiled list of xform-defs");
}

//--------------------------------------------------------------------------------------------------------------
ref_constant_list CResourceManager::_CreateConstantList(SConstantList& L)
{
	BOOL bEmpty = TRUE;
	for (u32 i = 0; i < L.size(); i++)
		if (L[i])
		{
			bEmpty = FALSE;
			break;
		}
	if (bEmpty) return NULL;

	xrCriticalSectionGuard guard(parameterGuard);
	for (u32 it = 0; it < lst_constants.size(); it++)
	{
		SConstantList* base = lst_constants[it];
		if (L.equal(*base) && base->pin_alive()) return ref_constant_list(base, false);
	}
	SConstantList* lst = xr_new<SConstantList>(L);
	lst->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	lst_constants.push_back(lst);
	return lst;
}

void CResourceManager::_DeleteConstantList(const SConstantList* L)
{
	if (0 == (L->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(parameterGuard);
	if (reclaim(lst_constants, L)) return;
	Msg("! ERROR: Failed to find compiled list of r1-constant-defs");
}

//--------------------------------------------------------------------------------------------------------------
ref_cbuffer CResourceManager::_CreateConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable)
{
	VERIFY(pTable);
	xrCriticalSectionGuard guard(shaderProgramGuard);
	dx10ConstantBuffer* pTempBuffer = xr_new<dx10ConstantBuffer>(pTable);

	for (u32 it = 0; it < v_constant_buffer.size(); it++)
	{
		dx10ConstantBuffer* buf = v_constant_buffer[it];
		if (pTempBuffer->Similar(*buf) && buf->pin_alive())
		{
			xr_delete(pTempBuffer);
			return ref_cbuffer(buf, false);
		}
	}

	pTempBuffer->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_constant_buffer.push_back(pTempBuffer);
	return pTempBuffer;
}

//--------------------------------------------------------------------------------------------------------------
void CResourceManager::_DeleteConstantBuffer(const dx10ConstantBuffer* pBuffer)
{
	if (0 == (pBuffer->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderProgramGuard);
	if (reclaim(v_constant_buffer, pBuffer)) return;
	Msg("! ERROR: Failed to find compiled constant buffer");
}

//--------------------------------------------------------------------------------------------------------------
ref_input_sign CResourceManager::_CreateInputSignature(ID3DBlob* pBlob)
{
	VERIFY(pBlob);
	xrCriticalSectionGuard guard(shaderProgramGuard);

	const u32 blob_size = (u32)pBlob->GetBufferSize();
	const u32 signature_crc = crc32(pBlob->GetBufferPointer(), blob_size);
	const auto bucket_it = m_input_signature_buckets.find(signature_crc);
	if (bucket_it != m_input_signature_buckets.end())
	{
		for (SInputSignature* sign : bucket_it->second)
		{
			if (blob_size == sign->signature->GetBufferSize() &&
				memcmp(pBlob->GetBufferPointer(), sign->signature->GetBufferPointer(), blob_size) == 0 && sign->pin_alive())
				return ref_input_sign(sign, false);
		}
	}

	SInputSignature* pSign = xr_new<SInputSignature>(pBlob);
	pSign->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_input_signature.push_back(pSign);
	m_input_signature_buckets[pSign->signature_crc].push_back(pSign);
	return pSign;
}

//--------------------------------------------------------------------------------------------------------------
void CResourceManager::_DeleteInputSignature(const SInputSignature* pSignature)
{
	if (0 == (pSignature->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(shaderProgramGuard);

	auto bucket_it = m_input_signature_buckets.find(pSignature->signature_crc);
	if (bucket_it != m_input_signature_buckets.end())
	{
		auto& bucket = bucket_it->second;
		bucket.erase(std::remove(bucket.begin(), bucket.end(), pSignature), bucket.end());
		if (bucket.empty())
			m_input_signature_buckets.erase(bucket_it);
	}

	if (reclaim(v_input_signature, pSignature)) return;
	Msg("! ERROR: Failed to find compiled input signature");
}
