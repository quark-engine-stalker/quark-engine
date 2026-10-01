#include "stdafx.h"
#include "NotificationClient.h"

#include "SoundRender_CoreA.h"

namespace
{
	CSoundRender_CoreA* sound_render_instance()
	{
		return static_cast<CSoundRender_CoreA*>(InterlockedCompareExchangePointer(
			reinterpret_cast<PVOID volatile*>(&SoundRenderA), nullptr, nullptr));
	}
}

CNotificationClient::CNotificationClient()
	: m_cRef(1), m_pEnumerator(nullptr), m_bComInitialized(false)
{
	Start();
}

CNotificationClient::~CNotificationClient()
{
	Close();
}

void CNotificationClient::Shutdown()
{
	Close();
	Release();
}

inline bool CNotificationClient::Start()
{
	const HRESULT initialize_result = CoInitialize(nullptr);

	if (SUCCEEDED(initialize_result) || initialize_result == RPC_E_CHANGED_MODE)
	{
		m_bComInitialized = SUCCEEDED(initialize_result);

		IMMDeviceEnumerator* enumerator = nullptr;
		const HRESULT create_result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
		if (SUCCEEDED(create_result))
		{
			const HRESULT register_result = enumerator->RegisterEndpointNotificationCallback(this);
			if (SUCCEEDED(register_result))
			{
				m_pEnumerator = enumerator;
				return true;
			}

			enumerator->Release();
		}

		if (m_bComInitialized)
		{
			CoUninitialize();
			m_bComInitialized = false;
		}
	}

	return false;
}

inline void CNotificationClient::Close()
{
	if (m_pEnumerator)
	{
		m_pEnumerator->UnregisterEndpointNotificationCallback(this);
		m_pEnumerator->Release();
		m_pEnumerator = nullptr;
	}

	if (m_bComInitialized)
	{
		CoUninitialize();
		m_bComInitialized = false;
	}
}

inline STDMETHODIMP_(HRESULT __stdcall) CNotificationClient::OnDeviceAdded(LPCWSTR pwstrDeviceId)
{
	if (CSoundRender_CoreA* sound_render = sound_render_instance())
		sound_render->request_device_list_refresh();
	return S_OK;
}

// IMMNotificationClient methods

inline STDMETHODIMP_(HRESULT __stdcall) CNotificationClient::OnDefaultDeviceChanged(
	EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId)
{
	// Only handle render (output) devices with console (default) role.
	if (flow != eRender || role != eConsole)
		return S_OK;

	if (CSoundRender_CoreA* sound_render = sound_render_instance())
		sound_render->request_default_device_switch();
	return S_OK;
}

inline STDMETHODIMP_(HRESULT __stdcall) CNotificationClient::OnDeviceRemoved(LPCWSTR pwstrDeviceId)
{
	if (CSoundRender_CoreA* sound_render = sound_render_instance())
		sound_render->request_device_list_refresh();
	return S_OK;
}

inline STDMETHODIMP_(HRESULT __stdcall) CNotificationClient::OnDeviceStateChanged(
	LPCWSTR pwstrDeviceId, DWORD dwNewState)
{
	if (CSoundRender_CoreA* sound_render = sound_render_instance())
		sound_render->request_device_list_refresh();
	return S_OK;
}

inline STDMETHODIMP_(HRESULT __stdcall) CNotificationClient::OnPropertyValueChanged(
	LPCWSTR pwstrDeviceId, const PROPERTYKEY key)
{
	if (CSoundRender_CoreA* sound_render = sound_render_instance())
		sound_render->request_device_list_refresh();
	return S_OK;
}
