#include "stdafx.h"
//#include "../../xrEngine/xr_effgamma.h"
#include "xr_effgamma.h"
#include "dxRenderDeviceRender.h"
#include "../xrRender/tga.h"
#include "../../xrEngine/xrImage_Resampler.h"

#define	GAMESAVE_SIZE	128

extern u32 ps_r_screenshot_token;

IC u32 convert(float c)
{
	u32 C = iFloor(c);
	if (C > 255) C = 255;
	return C;
}

IC void MouseRayFromPoint(Fvector& direction, int x, int y, Fmatrix& m_CamMat)
{
	int halfwidth = Device.dwWidth / 2;
	int halfheight = Device.dwHeight / 2;

	Ivector2 point2;
	point2.set(x - halfwidth, halfheight - y);

	float size_y = VIEWPORT_NEAR * tanf(deg2rad(60.f) * 0.5f);
	float size_x = size_y / (Device.fHeight_2 / Device.fWidth_2);

	float r_pt = float(point2.x) * size_x / (float)halfwidth;
	float u_pt = float(point2.y) * size_y / (float)halfheight;

	direction.mul(m_CamMat.k, VIEWPORT_NEAR);
	direction.mad(direction, m_CamMat.j, u_pt);
	direction.mad(direction, m_CamMat.i, r_pt);
	direction.normalize();
}

#define SM_FOR_SEND_WIDTH 640
#define SM_FOR_SEND_HEIGHT 480

extern int ps_r4_hdr10_on;

void CRender::ScreenshotImpl(ScreenshotMode mode, LPCSTR name, CMemoryWriter* memory_writer)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}

	ID3DResource* pSrcTexture;
	HW.pBaseRT->GetResource(&pSrcTexture);

	VERIFY(pSrcTexture);

	// Save
	switch (mode)
	{
	case IRender_interface::SM_FOR_GAMESAVE:
		{
			ID3DTexture2D* pSrcSmallTexture;

			D3D_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = GAMESAVE_SIZE;
			desc.Height = GAMESAVE_SIZE;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_BC1_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			CHK_DX(HW.pDevice->CreateTexture2D( &desc, NULL, &pSrcSmallTexture ));

			CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
				NULL, pSrcSmallTexture ));

			// save (logical & physical)
			ID3DBlob* saved = 0;
			HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
			if (hr == D3D_OK)
			{
				IWriter* fs = FS.w_open(name);
				if (fs)
				{
					fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
					FS.w_close(fs);
				}
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(pSrcSmallTexture);
		}
		break;
	case IRender_interface::SM_FOR_MPSENDING:
		{
			ID3DTexture2D* pSrcSmallTexture;

			D3D_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = SM_FOR_SEND_WIDTH;
			desc.Height = SM_FOR_SEND_HEIGHT;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_BC1_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			CHK_DX(HW.pDevice->CreateTexture2D( &desc, NULL, &pSrcSmallTexture ));

			CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
				NULL, pSrcSmallTexture ));
			// save (logical & physical)
			ID3DBlob* saved = 0;
			HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
			if (hr == D3D_OK)
			{
				if (!memory_writer)
				{
					IWriter* fs = FS.w_open(name);
					if (fs)
					{
						fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
						FS.w_close(fs);
					}
				}
				else
				{
					memory_writer->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
				}
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(pSrcSmallTexture);
		}
		break;
	case IRender_interface::SM_NORMAL:
		{
			string64 t_stemp;
			string_path buf;
			xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
				(g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", ps_r_screenshot_token == 0 ? "jpg" : (ps_r_screenshot_token == 1 ? "png" : "tga"));
			ID3DBlob* saved = 0;
			CHK_DX(D3DX11SaveTextureToMemory(HW.pContext, pSrcTexture, ps_r_screenshot_token == 0 ? D3DX11_IFF_JPG : (ps_r_screenshot_token == 1 ? D3DX11_IFF_PNG : D3DX11_IFF_BMP), &saved, 0));
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
			FS.w_close(fs);
			_RELEASE(saved);
		}
		break;
	case IRender_interface::SM_FOR_LEVELMAP:
	case IRender_interface::SM_FOR_CUBEMAP:
		{
		string_path buf;
		VERIFY(name);
		strconcat(sizeof(buf), buf, name, ".tga");

		// TODO: this won't work due to texture format incompat
		ID3DTexture2D* pTex = Target->t_ss_async;
		HW.pContext->CopyResource(pTex, pSrcTexture);

		D3D_MAPPED_TEXTURE2D MappedData;
		HW.pContext->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);
		// Swap r and b, but don't kill alpha
		{
			u32* pPixel = (u32*)MappedData.pData;
			u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);

			for (; pPixel != pEnd; pPixel++)
			{
				u32 p = *pPixel;
				*pPixel = color_argb(color_get_A(p), color_get_B(p), color_get_G(p), color_get_R(p));
			}
		}

		u32* data = (u32*)xr_malloc(Device.dwHeight * Device.dwHeight * 4);
		imf_Process(data, Device.dwHeight, Device.dwHeight, (u32*)MappedData.pData, Device.dwWidth, Device.dwHeight, imf_lanczos3);
		HW.pContext->Unmap(pTex, 0);
		IWriter* fs = FS.w_open("$screenshots$", buf);
		R_ASSERT(fs);

		TGAdesc p;
		p.format = IMG_24B;
		p.scanlenght = Device.dwHeight * 4;
		p.width = Device.dwHeight;
		p.height = Device.dwHeight;
		p.data = data;
		p.maketga(*fs);
		xr_free(data);
		FS.w_close(fs);
		}
		break;
	}

	_RELEASE(pSrcTexture);
}


void CRender::Screenshot(ScreenshotMode mode, LPCSTR name)
{
	ScreenshotImpl(mode, name, NULL);
}

void CRender::Screenshot(ScreenshotMode mode, CMemoryWriter& memory_writer)
{
	if (mode != SM_FOR_MPSENDING)
	{
		Log("~ Not implemented screenshot mode...");
		return;
	}
	ScreenshotImpl(mode, NULL, &memory_writer);
}

void CRender::ScreenshotAsyncBegin()
{
	VERIFY(!m_bMakeAsyncSS);
	m_bMakeAsyncSS = true;
}


void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer)
{
	VERIFY(!m_bMakeAsyncSS);

	//	Don't own. No need to release.
	ID3DTexture2D* pTex = Target->t_ss_async;

	D3D_MAPPED_TEXTURE2D MappedData;

	HW.pContext->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);

	{
		u32* pPixel = (u32*)MappedData.pData;
		u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);

		//	Kill alpha and swap r and b.
		for (; pPixel != pEnd; pPixel++)
		{
			u32 p = *pPixel;
			*pPixel = color_xrgb(
				color_get_B(p),
				color_get_G(p),
				color_get_R(p)
			);
		}

		memory_writer.w(&Device.dwWidth, sizeof(Device.dwWidth));
		memory_writer.w(&Device.dwHeight, sizeof(Device.dwHeight));
		memory_writer.w(MappedData.pData, (Device.dwWidth * Device.dwHeight) * 4);
	}

	HW.pContext->Unmap(pTex, 0);
}


void DoAsyncScreenshot()
{
	RImplementation.Target->DoAsyncScreenshot();
}

// Antglobes: Export Screenshot Func + variable resolution & encoding
void CRender::TakeScreenshot(LPCSTR path, Fvector2 dimensions, DxEncoding encoding)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}
	if (!Device.b_is_Ready) return;

	string_path fname;
	if (0 == strext(path)) strconcat(sizeof(fname), fname, path, ".dds");
	else xr_strcpy(fname, sizeof(fname), path);
	FS.update_path(fname, "$game_textures$", fname);

	// Clamp image dimensions
	int actual_width = Device.dwWidth;
	int actual_height = Device.dwHeight;
	int width = dimensions.x;
	int height = dimensions.y;
	clamp(width, 0, actual_width);
	clamp(height, 0, actual_height);

	ID3DResource* pSrcTexture;
	HW.pBaseRT->GetResource(&pSrcTexture);

	VERIFY(pSrcTexture);


	DXGI_FORMAT dx_encoding;
	clamp(encoding, eDXE_A8R8G8B8, eDXE_DXT5);
	switch (encoding)
	{
	case IRender_interface::eDXE_A8R8G8B8: 
	{
		dx_encoding = DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	break;
	case IRender_interface::eDXE_DXT1: 
	{
		dx_encoding = DXGI_FORMAT_BC1_UNORM;
	}
	break;
	case IRender_interface::eDXE_DXT5: 
	{
		dx_encoding = DXGI_FORMAT_BC5_UNORM;
	}
	break;
	case IRender_interface::eDXE_BC7:
	{
		dx_encoding = DXGI_FORMAT_BC7_UNORM;
	}
	break;
	default:
		dx_encoding = DXGI_FORMAT_R8G8B8A8_UNORM;
		break;
	}

	ID3DTexture2D* pSrcSmallTexture;

	D3D_TEXTURE2D_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = u32(width);
	desc.Height = u32(height);
	desc.MipLevels = encoding == eDXE_A8R8G8B8 ? 0: 1;
	desc.ArraySize = 1;
	desc.Format = dx_encoding;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	CHK_DX(HW.pDevice->CreateTexture2D(&desc, NULL, &pSrcSmallTexture));

	CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
		NULL, pSrcSmallTexture));

	

	// save (logical & physical)
	ID3DBlob* saved = 0;
	HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
	if (hr == D3D_OK)
	{
		IWriter* fs = FS.w_open(fname);
		if (fs)
		{
			fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
			FS.w_close(fs);
		}
	}
	_RELEASE(saved);

	// cleanup
	_RELEASE(pSrcSmallTexture);
}
