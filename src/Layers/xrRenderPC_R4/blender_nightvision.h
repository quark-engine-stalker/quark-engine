#pragma once


class CBlender_nightvision : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "nightvision"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_nightvision(bool useGenericSource = false);
	virtual ~CBlender_nightvision();
};

//crookr
class CBlender_fakescope : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "fakescope"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_fakescope(bool useGenericSource = false);
	virtual ~CBlender_fakescope();
};

//--DSR-- HeatVision_start
class CBlender_heatvision : public IBlender
{
private:
	bool m_useGenericSource;
public:
	virtual LPCSTR getComment() { return "heatvision"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	explicit CBlender_heatvision(bool useGenericSource = false);
	virtual ~CBlender_heatvision();
};
//--DSR-- HeatVision_end
