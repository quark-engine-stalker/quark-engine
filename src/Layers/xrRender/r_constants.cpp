#include "stdafx.h"
#pragma hdrstop

#pragma warning(disable:4995)
#include <d3dx9.h>
#pragma warning(default:4995)

#include "ResourceManager.h"

#include "../../xrCore/xrPool.h"
#include "r_constants.h"

#include "../xrRender/dxRenderDeviceRender.h"

// pool
//.static	poolSS<R_constant,512>			g_constant_allocator;

//R_constant_table::~R_constant_table	()	{	dxRenderDeviceRender::Instance().Resources->_DeleteConstantTable(this);	}


R_constant_table::~R_constant_table()
{
	//dxRenderDeviceRender::Instance().Resources->_DeleteConstantTable(this);	
	DEV->_DeleteConstantTable(this);
}


void R_constant_table::fatal(LPCSTR S)
{
	FATAL(S);
}

// predicates
IC bool p_sort(const ref_constant& C1, const ref_constant& C2) noexcept
{
	return C1->name < C2->name;
}

namespace
{
constexpr u32 kConstantNameLookupThreshold = 8;
}

void R_constant_table::rebuild_name_lookup()
{
	m_NameLookup.clear();
	if (table.size() < kConstantNameLookupThreshold)
		return;
	m_NameLookup.reserve(table.size());
	for (const ref_constant& constant : table)
	{
		if (constant)
			m_NameLookup[constant->name] = &*constant;
	}
}

void R_constant_table::rebuild_cb_bindings()
{
	m_CBBindings.pixel_count = 0;
	m_CBBindings.vertex_count = 0;
	m_CBBindings.geometry_count = 0;
	m_CBBindings.hull_count = 0;
	m_CBBindings.domain_count = 0;
	m_CBBindings.compute_count = 0;
	for (u32 i = 0; i < MaxCBuffers; ++i)
	{
		m_CBBindings.pixel[i] = 0;
		m_CBBindings.vertex[i] = 0;
		m_CBBindings.geometry[i] = 0;
		m_CBBindings.hull[i] = 0;
		m_CBBindings.domain[i] = 0;
		m_CBBindings.compute[i] = 0;
	}

	for (u32 i = 0; i < m_CBTable.size(); ++i)
	{
		const cb_table_record& record = m_CBTable[i];
		const u32 buffer_index = record.first & CB_BufferIndexMask;
		VERIFY(buffer_index < MaxCBuffers);

		switch (record.first & CB_BufferTypeMask)
		{
		case CB_BufferPixelShader:
			m_CBBindings.pixel[buffer_index] = record.second._get();
			m_CBBindings.pixel_count = _max(m_CBBindings.pixel_count, buffer_index + 1);
			break;
		case CB_BufferVertexShader:
			m_CBBindings.vertex[buffer_index] = record.second._get();
			m_CBBindings.vertex_count = _max(m_CBBindings.vertex_count, buffer_index + 1);
			break;
		case CB_BufferGeometryShader:
			m_CBBindings.geometry[buffer_index] = record.second._get();
			m_CBBindings.geometry_count = _max(m_CBBindings.geometry_count, buffer_index + 1);
			break;
		case CB_BufferHullShader:
			m_CBBindings.hull[buffer_index] = record.second._get();
			m_CBBindings.hull_count = _max(m_CBBindings.hull_count, buffer_index + 1);
			break;
		case CB_BufferDomainShader:
			m_CBBindings.domain[buffer_index] = record.second._get();
			m_CBBindings.domain_count = _max(m_CBBindings.domain_count, buffer_index + 1);
			break;
		case CB_BufferComputeShader:
			m_CBBindings.compute[buffer_index] = record.second._get();
			m_CBBindings.compute_count = _max(m_CBBindings.compute_count, buffer_index + 1);
			break;
		default:
			VERIFY(!"Invalid constant buffer type");
			break;
		}
	}
}

R_constant* R_constant_table::get(LPCSTR S)
{
    // demonized: make shared_str and use override, str most likely already exists, faster
    shared_str s(S);
    return get(s);
}

R_constant* R_constant_table::get(shared_str& S)
{
	const auto cached = m_NameLookup.find(S);
	if (cached != m_NameLookup.end())
		return cached->second;

	// Keep the sorted-table fallback for partially assembled tables, for example
	// while merge() is still appending stage constants.
	static auto sortFunc = [](const ref_constant& C, const shared_str& S) noexcept { return C->name < S; };
	auto it = std::lower_bound(table.begin(), table.end(), S, sortFunc);
	if (it != table.end() && (*it)->name.equal(S))
		return &**it;
	return nullptr;
}


#include <iterator>
/// !!!!!!!!FIX THIS FOR DX11!!!!!!!!!
void R_constant_table::merge(R_constant_table* T)
{
	if (0 == T) return;

	// Real merge
	xr_vector<ref_constant> table_tmp;
	table_tmp.reserve(table.size());
	for (u32 it = 0; it < T->table.size(); it++)
	{
		ref_constant src = T->table[it];
		ref_constant C = get(*src->name);
		if (!C)
		{
			C = xr_new<R_constant>(); //.g_constant_allocator.create();
			C->name = src->name;
			C->destination = src->destination;
			C->type = src->type;
			C->ps = src->ps;
			C->vs = src->vs;
			C->gs = src->gs;
			C->hs = src->hs;
			C->ds = src->ds;
			C->cs = src->cs;
			C->samp = src->samp;
			table_tmp.push_back(C);
		}
		else
		{
			VERIFY2(!(C->destination&src->destination&RC_dest_sampler),
			        "Can't have samplers or textures with the same name for PS, VS and GS.");
			C->destination |= src->destination;
			VERIFY(C->type == src->type);
			R_constant_load& sL = src->get_load(src->destination);
			R_constant_load& dL = C->get_load(src->destination);
			dL.index = sL.index;
			dL.cls = sL.cls;
		}
	}

	if (!table_tmp.empty())
	{
		// Append
		std::move(table_tmp.begin(), table_tmp.end(), std::back_inserter(table));

		// Sort
		std::sort(table.begin(), table.end(), p_sort);
	}
	rebuild_name_lookup();

	//	TODO:	DX10:	Implement merge with validity check
	m_CBTable.reserve(m_CBTable.size() + T->m_CBTable.size());
	for (u32 i = 0; i < T->m_CBTable.size(); ++i)
		m_CBTable.push_back(T->m_CBTable[i]);

	rebuild_cb_bindings();
}

void R_constant_table::clear()
{
	//.
	for (u32 it = 0; it < table.size(); it++)
		table[it] = 0; //.g_constant_allocator.destroy(table[it]);
	table.clear();
	m_NameLookup.clear();
	m_CBTable.clear();
	rebuild_cb_bindings();
}

BOOL R_constant_table::equal(R_constant_table& C)
{
	if (table.size() != C.table.size()) return FALSE;
	u32 size = table.size();
	for (u32 it = 0; it < size; it++)
	{
		if (!table[it]->equal(&*C.table[it])) return FALSE;
	}

	return TRUE;
}
