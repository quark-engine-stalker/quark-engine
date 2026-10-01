#pragma once


class CBlender_gasmask_drops : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "Gasmask_drops"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_gasmask_drops(bool useGenericSource = false);
	virtual ~CBlender_gasmask_drops();
};
