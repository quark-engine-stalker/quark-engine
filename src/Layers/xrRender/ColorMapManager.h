#ifndef	ColorMapManager_included
#define	ColorMapManager_included
#pragma once

//	Reduces amount of work if the texture was not changed.
// Retains a bounded history of LUTs; both active maps are always protected.

class ColorMapManager
{
public:
	ColorMapManager();

	void SetTextures(const shared_str& tex0, const shared_str& tex1);

private:

	void UpdateTexture(const shared_str& strTexName, int iTex);
	void TrimCache();

private:

	struct str_pred
	{
		IC bool operator()(const shared_str& x, const shared_str& y) const
		{
			return x < y;
		}
	};

	struct cached_texture
	{
		ref_texture texture;
		u64 last_used;
	};
	DEFINE_MAP_PRED(shared_str, cached_texture, map_Tex, map_TexIt, str_pred);

private:

	ref_texture m_CMap[2];
	shared_str m_strCMap[2];

	map_Tex m_TexCache;
	u64 m_CacheClock = 0;
};

#endif	//	ColorMapManager_included
