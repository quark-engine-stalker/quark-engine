#pragma once


class CBlender_lut : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "Lookup table!"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_lut(bool useGenericSource = false);
	virtual ~CBlender_lut();
};
