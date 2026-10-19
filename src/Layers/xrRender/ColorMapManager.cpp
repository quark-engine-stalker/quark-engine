#include "stdafx.h"
#include "ColorMapManager.h"

#include "dxRenderDeviceRender.h"


ColorMapManager::ColorMapManager()
{
	m_CMap[0] = dxRenderDeviceRender::Instance().Resources->CreateTextureRef("$user$cmap0");
	m_CMap[1] = dxRenderDeviceRender::Instance().Resources->CreateTextureRef("$user$cmap1");
}

void ColorMapManager::SetTextures(const shared_str& tex0, const shared_str& tex1)
{
	if (tex0 == m_strCMap[0] && tex1 == m_strCMap[1]) return;
	UpdateTexture(tex0, 0);
	UpdateTexture(tex1, 1);
	TrimCache();
}


void ColorMapManager::UpdateTexture(const shared_str& strTexName, int iTex)
{
	if (strTexName == m_strCMap[iTex]) return;

	m_strCMap[iTex] = strTexName;

	if (strTexName.size())
	{
		map_TexIt I = m_TexCache.find(strTexName);
		if (I != m_TexCache.end())
		{
			I->second.last_used = ++m_CacheClock;
			ID3DBaseTexture* e0 = I->second.texture->surface_get();
			m_CMap[iTex]->surface_set(e0);
			_RELEASE(e0);
		}
		else
		{
			ref_texture tmp;
			tmp.create(strTexName.c_str());

			m_TexCache.insert(mk_pair(strTexName, cached_texture{tmp, ++m_CacheClock}));

			ID3DBaseTexture* e0 = tmp->surface_get();
			m_CMap[iTex]->surface_set(e0);
			_RELEASE(e0);
		}
	}
	else
	{
		m_CMap[iTex]->surface_set(0);
	}
}

void ColorMapManager::TrimCache()
{
	// Retired LUTs are optional; active aliases and their image quality stay intact.
	for (;;)
	{
		u64 idle_bytes = 0;
		auto oldest = m_TexCache.end();
		for (auto it = m_TexCache.begin(); it != m_TexCache.end(); ++it)
		{
			if (it->first == m_strCMap[0] || it->first == m_strCMap[1]) continue;
			idle_bytes += it->second.texture->ResidentBytes();
			if (oldest == m_TexCache.end() || it->second.last_used < oldest->second.last_used)
				oldest = it;
		}
		if (oldest == m_TexCache.end() ||
			(m_TexCache.size() <= 8 && idle_bytes <= 16ull * 1024u * 1024u)) return;
		m_TexCache.erase(oldest);
	}
}
