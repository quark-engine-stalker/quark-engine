#pragma once


class CBlender_smaa : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "SMAA"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_smaa(bool useGenericSource = false);
	virtual ~CBlender_smaa();
};

class CBlender_ssfx_taa : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "TAA"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_ssfx_taa(bool useGenericSource = false);
	virtual ~CBlender_ssfx_taa();
};
