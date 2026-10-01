// HW.h: interface for the CHW class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_HW_H__0E25CF4A_FFEC_11D3_B4E3_4854E82A090D__INCLUDED_)
#define AFX_HW_H__0E25CF4A_FFEC_11D3_B4E3_4854E82A090D__INCLUDED_
#pragma once

#include <d3d11_4.h>
#include <dxgi1_4.h>

#include "hwcaps.h"

#include "../../build_config_defines.h"

#ifndef _MAYA_EXPORT
#include "stats_manager.h"
#endif

class CHW
	:	public pureAppActivate,
		public pureAppDeactivate
{
	//	Functions section
public:
	int maxRefreshRate; //ECO_RENDER add
	CHW();
	~CHW();

	void CreateD3D();
	void DestroyD3D();
	void CreateDevice(HWND hw, bool move_window);

	void DestroyDevice();

	void Reset(HWND hw);

	IDXGIOutput* FindOutputOnCurrentAdapter(HMONITOR hMon);

	void selectResolution(u32& dwWidth, u32& dwHeight, BOOL bWindowed);
	D3DFORMAT selectDepthStencil(D3DFORMAT);
	u32 selectPresentInterval();
	u32 selectGPU();
	u32 selectRefresh(u32 dwWidth, u32 dwHeight, D3DFORMAT fmt);
	void updateWindowProps(HWND hw);
	BOOL support(D3DFORMAT fmt, DWORD type, DWORD usage);

#ifdef DEBUG
	void	Validate(void)	{};
#else
	void Validate(void)
	{
	};
#endif

	//	Variables section
public:
    IDXGIFactory2*          m_pFactory; //  DXGI factory
	IDXGIAdapter1*			m_pAdapter;	//	pD3D equivalent
	IDXGIOutput*			m_pOutput;	//	the output we render to (belongs to m_pAdapter)
	ID3D11Device1*			pDevice;	//	combine with DX9 pDevice via typedef
	ID3D11DeviceContext1*   pContext;	//	combine with DX9 pDevice via typedef
	IDXGISwapChain1*        m_pSwapChain;
	ID3D11RenderTargetView*	pBaseRT;	//	combine with DX9 pBaseRT via typedef
	ID3D11DepthStencilView*	pBaseZB;
	ID3DUserDefinedAnnotation* pAnnotation;

	CHWCaps					Caps;

	DXGI_SWAP_CHAIN_DESC1			m_ChainDesc;	//	DevPP equivalent
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC m_ChainDescFullscreen;
    HWND                            m_hWnd;
	bool							m_bUsePerfhud;
	D3D_FEATURE_LEVEL				FeatureLevel;
	bool 							m_SupportsVRR; // whether we can use DXGI_PRESENT_ALLOW_TEARING etc.

#ifndef _MAYA_EXPORT
	stats_manager stats_manager;
#endif
	void			UpdateViews();
	DXGI_RATIONAL	selectRefresh(u32 dwWidth, u32 dwHeight, DXGI_FORMAT fmt);

	virtual	void	OnAppActivate();
	virtual void	OnAppDeactivate();
	bool TryRestoreFullscreen(bool& restored);

private:
	bool ResizeSwapChainAfterModeChange();
	void AcquireDefaultOutput();
	void SelectAdapterAndOutput(HMONITOR hTargetMonitor);
	bool m_move_window;
	bool m_fullscreen_restore_pending;
	HRESULT m_fullscreen_transition_result;
};

extern ECORE_API CHW HW;

#endif // !defined(AFX_HW_H__0E25CF4A_FFEC_11D3_B4E3_4854E82A090D__INCLUDED_)
