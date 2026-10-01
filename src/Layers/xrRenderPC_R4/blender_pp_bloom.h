#pragma once

class CBlender_pp_bloom : public IBlender
{
private:
    bool m_useGenericSource;
public:
    virtual		LPCSTR		getComment()	{ return "Nice bloom bro!"; }
    virtual		BOOL		canBeDetailed()	{ return FALSE; }
    virtual		BOOL		canBeLMAPped()	{ return FALSE; }

    virtual		void		Compile(CBlender_Compile& C);

    explicit CBlender_pp_bloom(bool useGenericSource = false);
    virtual ~CBlender_pp_bloom();
}; 