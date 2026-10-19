#pragma once


class CBlender_dof : public IBlender
{
public:
	virtual LPCSTR getComment() { return "DoF"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_dof(bool useGenericSource = false);
	virtual ~CBlender_dof();

private:
	// true: the scene to blur currently lives in r2_RT_generic (post-process ping-pong)
	bool m_useGenericSource;
};
