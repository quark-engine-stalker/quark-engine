#pragma once
#include "../xrEngine/fmesh.h"

// CPU-only state for HUD materials on an NPC-owned item. The owning item keeps
// this small state; no render object, motion pack or GPU resource is needed.
struct HudMaterialEntry
{
	u32 id;
	shared_str shader_default;
	shared_str texture_default;
	shared_str shader;
	shared_str texture;
	shared_str shader_request;
	bool changed = false;
};

struct HudMaterialState
{
	xr_vector<HudMaterialEntry> entries;

	void change(int id, LPCSTR shader, LPCSTR texture, bool reset)
	{
		for (HudMaterialEntry& entry : entries)
		{
			if (id > 0 && entry.id != static_cast<u32>(id))
				continue;
			if (reset)
			{
				shader = entry.shader_default.c_str();
				texture = entry.texture_default.c_str();
			}
			if (shader && shader[0])
			{
				entry.shader_request = shader;
				xr_string stripped = shader;
				const size_t no_shadow = stripped.find("$no_shadows");
				if (no_shadow != xr_string::npos)
					stripped.resize(no_shadow);
				entry.shader = stripped.c_str();
			}
			if (texture && texture[0])
				entry.texture = texture;
			entry.changed = entry.changed || reset || (shader && shader[0]) || (texture && texture[0]);
			if (id > 0)
				break;
		}
	}
};

namespace hud_material_detail
{
struct Chunk
{
	const u8* data = nullptr;
	u32 size = 0;
};

// Unsupported/compressed layouts return false and use the original visual
// loader. Bounds are checked before touching any chunk payload.
IC bool find_chunk(const u8* bytes, u32 length, u32 wanted, Chunk& result)
{
	u32 offset = 0;
	while (length - offset >= 2 * sizeof(u32))
	{
		u32 id, size;
		CopyMemory(&id, bytes + offset, sizeof(id));
		CopyMemory(&size, bytes + offset + sizeof(id), sizeof(size));
		offset += 2 * sizeof(u32);
		if (size > length - offset)
			return false;
		if ((id & 0x7fffffffu) == wanted)
		{
			result = {bytes + offset, size};
			return id == wanted;
		}
		offset += size;
	}
	return false;
}

IC bool read_name(const char*& cursor, u32& remaining, shared_str& name)
{
	const char* end = static_cast<const char*>(memchr(cursor, 0, remaining));
	if (!end || end == cursor || end - cursor >= sizeof(string256))
		return false;
	name = cursor;
	const u32 consumed = static_cast<u32>(end - cursor) + 1;
	cursor += consumed;
	remaining -= consumed;
	return true;
}

IC bool read_ogf(const u8* bytes, u32 length, HudMaterialState& state)
{
	Chunk root_header, children;
	if (!bytes || !find_chunk(bytes, length, OGF_HEADER, root_header) ||
		root_header.size != sizeof(ogf_header) || root_header.data[0] != xrOGF_FormatVersion ||
		root_header.data[1] != MT_SKELETON_ANIM ||
		!find_chunk(bytes, length, OGF_CHILDREN, children))
		return false;
	// Linked children are resolved by the real renderer, never by this path.
	Chunk links;
	find_chunk(bytes, length, OGF_CHILDREN_L, links);
	if (links.data)
		return false;

	xr_vector<HudMaterialEntry> entries;
	u32 offset = 0;
	while (offset < children.size && entries.size() < 256)
	{
		Chunk child, header, material;
		if (children.size - offset < 2 * sizeof(u32))
			return false;
		u32 index, size;
		CopyMemory(&index, children.data + offset, sizeof(index));
		CopyMemory(&size, children.data + offset + sizeof(index), sizeof(size));
		offset += 2 * sizeof(u32);
		if (index != entries.size() || size > children.size - offset)
			return false;
		child = {children.data + offset, size};
		offset += size;
		if (!find_chunk(child.data, child.size, OGF_HEADER, header) ||
			header.size != sizeof(ogf_header) || header.data[0] != xrOGF_FormatVersion ||
			(header.data[1] != MT_SKELETON_GEOMDEF_ST && header.data[1] != MT_SKELETON_GEOMDEF_PM) ||
			!find_chunk(child.data, child.size, OGF_TEXTURE, material))
			return false;
		HudMaterialEntry entry;
		entry.id = index + 1;
		const char* cursor = reinterpret_cast<const char*>(material.data);
		u32 remaining = material.size;
		if (!read_name(cursor, remaining, entry.texture_default) ||
			!read_name(cursor, remaining, entry.shader_default))
			return false;
		entry.texture = entry.texture_default;
		entry.shader_request = entry.shader_default;
		string256 stripped;
		xr_strcpy(stripped, entry.shader_default.c_str());
		if (char* no_shadow = strstr(stripped, "$no_shadows"))
			*no_shadow = 0;
		if (!stripped[0])
			return false;
		entry.shader = stripped;
		entries.push_back(entry);
	}
	if (entries.empty() || offset != children.size)
		return false;
	state.entries.swap(entries);
	return true;
}
} // namespace hud_material_detail
