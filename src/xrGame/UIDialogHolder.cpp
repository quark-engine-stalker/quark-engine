#include "stdafx.h"
#include "UIDialogHolder.h"
#include "ui/UIDialogWnd.h"
#include "UIGameCustom.h"
#include "UICursor.h"
#include "level.h"
#include "actor.h"
#include "xr_level_controller.h"
#include "../xrEngine/CustomHud.h"

dlgItem::dlgItem(CUIWindow* pWnd)
{
	wnd = pWnd;
	enabled = true;
}

bool dlgItem::operator <(const dlgItem& itm) const
{
	return (int)enabled > (int)itm.enabled;
}

bool operator ==(const dlgItem& i1, const dlgItem& i2)
{
	return (i1.wnd == i2.wnd) && (i1.enabled == i2.enabled);
}

recvItem::recvItem(CUIDialogWnd* r)
{
	m_item = r;
	m_flags.zero();
}

bool operator ==(const recvItem& i1, const recvItem& i2)
{
	return i1.m_item == i2.m_item;
}

CDialogHolder::CDialogHolder()
{
	m_b_in_update = false;
}

CDialogHolder::~CDialogHolder()
{
	ReleaseWindowBacklinks();
}

#include "player_hud.h"

void CDialogHolder::StartMenu(CUIDialogWnd* pDialog, bool bDoHideIndicators)
{
	R_ASSERT(!pDialog->IsShown());

	AddDialogToRender(pDialog);
	SetMainInputReceiver(pDialog, false);

	if (UseIndicators() && !m_input_receivers.empty()) //Alundaio
	{
		bool b = !!psHUD_Flags.test(HUD_CROSSHAIR_RT);
		m_input_receivers.back().m_flags.set(recvItem::eCrosshair, b);

		b = CurrentGameUI()->GameIndicatorsShown();
		m_input_receivers.back().m_flags.set(recvItem::eIndicators, b);

		if (bDoHideIndicators)
		{
			if (!g_player_hud->m_adjust_mode)
				psHUD_Flags.set(HUD_CROSSHAIR_RT, FALSE);
			CurrentGameUI()->ShowGameIndicators(false);
		}
	}
	pDialog->SetHolder(this);

	if (pDialog->NeedCursor())
		GetUICursor().Show();

	if (g_pGameLevel)
	{
		CActor* A = smart_cast<CActor*>(Level().CurrentViewEntity());
		if (A && pDialog->StopAnyMove())
		{
			A->StopAnyMove();
		};
		if (A)
		{
			A->IR_OnKeyboardRelease(kWPN_ZOOM);
			A->IR_OnKeyboardRelease(kWPN_FIRE);
		}
	}
}


void CDialogHolder::StopMenu(CUIDialogWnd* pDialog)
{
	R_ASSERT(pDialog->IsShown());

	if (TopInputReceiver() == pDialog)
	{
		if (UseIndicators() && !m_input_receivers.empty()) //Alundaio
		{
			bool b = !!m_input_receivers.back().m_flags.test(recvItem::eCrosshair);
			psHUD_Flags.set(HUD_CROSSHAIR_RT, b);
			b = !!m_input_receivers.back().m_flags.test(recvItem::eIndicators);
			CurrentGameUI()->ShowGameIndicators(b);
		}

		SetMainInputReceiver(NULL, false);
	}
	else
		SetMainInputReceiver(pDialog, true);

	RemoveDialogToRender(pDialog);
	pDialog->SetHolder(NULL);

	HideCursorWithoutOwner();
}

void CDialogHolder::AddDialogToRender(CUIWindow* pDialog)
{
	if (!pDialog)
		return;

	// A window can be registered in only one holder. Moving it is safe because
	// the old holder only keeps non-owning pointers.
	if (pDialog->m_pDialogHolder && pDialog->m_pDialogHolder != this)
		pDialog->m_pDialogHolder->OnWindowDestroyed(pDialog);
	pDialog->m_pDialogHolder = this;

	dlgItem itm(pDialog);
	itm.enabled = true;

	bool bAdd = (m_dialogsToRender_new.end() == std::find(m_dialogsToRender_new.begin(), m_dialogsToRender_new.end(),
	                                                      itm));
	if (!bAdd) return;

	bAdd = (m_dialogsToRender.end() == std::find(m_dialogsToRender.begin(), m_dialogsToRender.end(), itm));
	if (!bAdd) return;

	if (m_b_in_update)
		m_dialogsToRender_new.push_back(itm);
	else
		m_dialogsToRender.push_back(itm);

	pDialog->Show(true);
}

void CDialogHolder::RemoveDialogToRender(CUIWindow* pDialog)
{
	if (!pDialog)
		return;

	if (TopInputReceiver() == pDialog)
		SetMainInputReceiver(NULL, false);

	dlgItem itm(pDialog);
	itm.enabled = true;
	xr_vector<dlgItem>::iterator it = std::find(m_dialogsToRender.begin(), m_dialogsToRender.end(), itm);

	if (it != m_dialogsToRender.end())
	{
		// Detach before callbacks: a script override of Show may delete the window.
		it->enabled = false;
		it->wnd = NULL;
		if (pDialog->m_pDialogHolder == this)
			pDialog->m_pDialogHolder = NULL;
		// Leave the virtual Show callback last: a script-backed implementation is
		// allowed to release itself while handling the hide notification.
		pDialog->CUIWindow::Enable(false);
		pDialog->Show(false);
		return;
	}
	
	it = std::find(m_dialogsToRender_new.begin(), m_dialogsToRender_new.end(), itm);

	if (it != m_dialogsToRender_new.end())
	{
		it->enabled = false;
		it->wnd = NULL;
		if (pDialog->m_pDialogHolder == this)
			pDialog->m_pDialogHolder = NULL;
		pDialog->CUIWindow::Enable(false);
		pDialog->Show(false);
	}
	else if (pDialog->m_pDialogHolder == this)
		pDialog->m_pDialogHolder = NULL;
}

void CDialogHolder::OnWindowDestroyed(CUIWindow* window)
{
	if (!window)
		return;
	bool removed_input_receiver = false;
	bool restore_top_state = false;
	bool restore_crosshair = false;
	bool restore_indicators = false;

	// Never erase render entries from inside Update/Draw callbacks. Nulling and
	// disabling preserves all active iterators; OnFrame compacts the tail later.
	for (dlgItem& item : m_dialogsToRender)
	{
		if (item.wnd == window)
		{
			item.wnd = NULL;
			item.enabled = false;
		}
	}
	for (dlgItem& item : m_dialogsToRender_new)
	{
		if (item.wnd == window)
		{
			item.wnd = NULL;
			item.enabled = false;
		}
	}

	// Input receivers are dialogs, but pointer comparison does not dereference
	// the object whose derived part may already be destructed.
	for (u32 index = 0; index < m_input_receivers.size();)
	{
		if (static_cast<CUIWindow*>(m_input_receivers[index].m_item) != window)
		{
			++index;
			continue;
		}

		const bool was_top = (index + 1 == m_input_receivers.size());
		if (was_top && UseIndicators())
		{
			restore_crosshair = !!m_input_receivers[index].m_flags.test(recvItem::eCrosshair);
			restore_indicators = !!m_input_receivers[index].m_flags.test(recvItem::eIndicators);
			restore_top_state = true;
		}
		else if (index + 1 < m_input_receivers.size())
		{
			m_input_receivers[index + 1].m_flags = m_input_receivers[index].m_flags;
		}

		m_input_receivers.erase(m_input_receivers.begin() + index);
		removed_input_receiver = true;
	}

	if (window->m_pDialogHolder == this)
		window->m_pDialogHolder = NULL;

	if (restore_top_state)
	{
		psHUD_Flags.set(HUD_CROSSHAIR_RT, restore_crosshair);
		if (CUIGameCustom* const ui = CurrentGameUI())
			ui->ShowGameIndicators(restore_indicators);
	}

	// A script window may be destroyed without HideDialog() during Lua VM/load
	// teardown. Its holder registration is non-owning, but the cursor is global
	// and otherwise keeps the destroyed dialog's visible state into gameplay.
	if (removed_input_receiver)
		HideCursorWithoutOwner();
}

void CDialogHolder::ReleaseWindowBacklinks()
{
	for (dlgItem& item : m_dialogsToRender)
		if (item.wnd && item.wnd->m_pDialogHolder == this)
			item.wnd->m_pDialogHolder = NULL;
	for (dlgItem& item : m_dialogsToRender_new)
		if (item.wnd && item.wnd->m_pDialogHolder == this)
			item.wnd->m_pDialogHolder = NULL;

	m_dialogsToRender.clear();
	m_dialogsToRender_new.clear();
	m_input_receivers.clear();
}


void CDialogHolder::DoRenderDialogs()
{
	xr_vector<dlgItem>::iterator it = m_dialogsToRender.begin();
	for (; it != m_dialogsToRender.end(); ++it)
	{
		if ((*it).enabled && (*it).wnd && (*it).wnd->IsShown())
			(*it).wnd->Draw();
	}
}

void CDialogHolder::OnExternalHideIndicators()
{
	xr_vector<recvItem>::iterator it = m_input_receivers.begin();
	xr_vector<recvItem>::iterator it_e = m_input_receivers.end();
	for (; it != it_e; ++it)
	{
		(*it).m_flags.set(recvItem::eIndicators, FALSE);
		(*it).m_flags.set(recvItem::eCrosshair, FALSE);
	}
}

CUIDialogWnd* CDialogHolder::TopInputReceiver()
{
	if (!m_input_receivers.empty())
		return m_input_receivers.back().m_item;
	return NULL;
};

void CDialogHolder::HideCursorWithoutOwner()
{
	CUIDialogWnd* const receiver = TopInputReceiver();
	if (!receiver || !receiver->IsShown() || !receiver->NeedCursor())
		GetUICursor().Hide();
}

void CDialogHolder::SetMainInputReceiver(CUIDialogWnd* ir, bool _find_remove)
{
	if (TopInputReceiver() == ir) return;

	if (!ir || _find_remove)
	{
		if (m_input_receivers.empty()) return;

		if (!ir)
			m_input_receivers.pop_back();
		else
		{
			VERIFY(ir && _find_remove);

			u32 cnt = m_input_receivers.size();
			for (; cnt > 0; --cnt)
				if (m_input_receivers[cnt - 1].m_item == ir)
				{
					m_input_receivers[cnt].m_flags.set(recvItem::eCrosshair,
					                                   m_input_receivers[cnt - 1].m_flags.test(recvItem::eCrosshair));
					m_input_receivers[cnt].m_flags.set(recvItem::eIndicators,
					                                   m_input_receivers[cnt - 1].m_flags.test(recvItem::eIndicators));
					xr_vector<recvItem>::iterator it = m_input_receivers.begin();
					std::advance(it, cnt - 1);
					m_input_receivers.erase(it);
					break;
				}
		}
	}
	else
	{
		m_input_receivers.push_back(recvItem(ir));
	}
};

void CDialogHolder::StartDialog(CUIDialogWnd* pDialog, bool bDoHideIndicators)
{
	if (pDialog && pDialog->NeedCenterCursor())
	{
		GetUICursor().SetUICursorPosition(Fvector2().set(512.0f, 384.0f));
	}
	StartMenu(pDialog, bDoHideIndicators);
}

void CDialogHolder::StopDialog(CUIDialogWnd* pDialog)
{
	StopMenu(pDialog);
}

void CDialogHolder::OnFrame()
{
	m_b_in_update = true;
	CUIDialogWnd* wnd = TopInputReceiver();
	if (wnd && wnd->IsEnabled())
	{
		wnd->Update();
	}
	//else
	{
		xr_vector<dlgItem>::iterator it = m_dialogsToRender.begin();
		for (; it != m_dialogsToRender.end(); ++it)
			if ((*it).enabled && (*it).wnd && (*it).wnd->IsEnabled())
				(*it).wnd->Update();
	}

	m_b_in_update = false;
	if (!m_dialogsToRender_new.empty())
	{
		m_dialogsToRender.insert(m_dialogsToRender.end(), m_dialogsToRender_new.begin(), m_dialogsToRender_new.end());
		m_dialogsToRender_new.clear();
	}

	std::sort(m_dialogsToRender.begin(), m_dialogsToRender.end());
	while (!m_dialogsToRender.empty() && (!m_dialogsToRender[m_dialogsToRender.size() - 1].enabled))
		m_dialogsToRender.pop_back();
}

void CDialogHolder::CleanInternals()
{
	ReleaseWindowBacklinks();
	GetUICursor().Hide();
}

bool CDialogHolder::IR_UIOnKeyboardPress(int dik)
{
	CUIDialogWnd* TIR = TopInputReceiver();
	if (!TIR) return false;
	if (!TIR->IR_process()) return false;
	//mouse click
	if (dik == MOUSE_1 || dik == MOUSE_2 || dik == MOUSE_3)
	{
		Fvector2 cp = GetUICursor().GetCursorPosition();
		EUIMessages action = (dik == MOUSE_1)
			                     ? WINDOW_LBUTTON_DOWN
			                     : (dik == MOUSE_2)
			                     ? WINDOW_RBUTTON_DOWN
			                     : WINDOW_CBUTTON_DOWN;
		if (TIR->OnMouseAction(cp.x, cp.y, action))
			return true;
	}

	if (TIR->OnKeyboardAction(dik, WINDOW_KEY_PRESSED))
		return true;

	if (!TIR->StopAnyMove() && g_pGameLevel)
	{
		CObject* O = Level().CurrentEntity();
		if (O)
		{
			IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(O));
			if (IR)
			{
				EGameActions action = get_binded_action(dik);
				if (action > kDOWN && action < kCAM_1)
					IR->IR_OnKeyboardPress(action);
			}
			return true;
		}
	}
	return true;
}

bool CDialogHolder::IR_UIOnKeyboardRelease(int dik)
{
	CUIDialogWnd* TIR = TopInputReceiver();
	if (!TIR) return false;
	if (!TIR->IR_process()) return false;

	//mouse click
	if (dik == MOUSE_1 || dik == MOUSE_2 || dik == MOUSE_3)
	{
		Fvector2 cp = GetUICursor().GetCursorPosition();
		EUIMessages action = (dik == MOUSE_1)
			                     ? WINDOW_LBUTTON_UP
			                     : (dik == MOUSE_2)
			                     ? WINDOW_RBUTTON_UP
			                     : WINDOW_CBUTTON_UP;
		if (TIR->OnMouseAction(cp.x, cp.y, action))
			return true;
	}

	if (TIR->OnKeyboardAction(dik, WINDOW_KEY_RELEASED))
		return true;

	if (!TIR->StopAnyMove() && g_pGameLevel)
	{
		CObject* O = Level().CurrentEntity();
		if (O)
		{
			IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(O));
			if (IR)
				IR->IR_OnKeyboardRelease(get_binded_action(dik));
			return (false);
		}
	}
	return true;
}

bool CDialogHolder::IR_UIOnKeyboardHold(int dik)
{
	CUIDialogWnd* TIR = TopInputReceiver();
	if (!TIR) return false;
	if (!TIR->IR_process()) return false;

	if (TIR->OnKeyboardHold(dik))
		return true;

	if (!TIR->StopAnyMove() && g_pGameLevel)
	{
		CObject* O = Level().CurrentEntity();
		if (O)
		{
			IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(O));
			if (IR)
				IR->IR_OnKeyboardHold(get_binded_action(dik));
			return false;
		}
	}
	return true;
}

bool CDialogHolder::IR_UIOnMouseWheel(int direction)
{
	CUIDialogWnd* TIR = TopInputReceiver();
	if (!TIR) return false;
	if (!TIR->IR_process()) return false;

	Fvector2 pos = GetUICursor().GetCursorPosition();

	TIR->OnMouseAction(pos.x, pos.y, (direction > 0) ? WINDOW_MOUSE_WHEEL_UP : WINDOW_MOUSE_WHEEL_DOWN);
	return true;
}

bool CDialogHolder::IR_UIOnMouseMove(int dx, int dy)
{
	CUIDialogWnd* TIR = TopInputReceiver();
	if (!TIR) return false;
	if (!TIR->IR_process()) return false;
	if (GetUICursor().IsVisible() || !TIR->NeedCursor())
	{
		GetUICursor().UpdateCursorPosition(dx, dy);
		Fvector2 cPos = GetUICursor().GetCursorPosition();
		TIR->OnMouseAction(cPos.x, cPos.y, WINDOW_MOUSE_MOVE);
	}
	else if (!TIR->StopAnyMove() && g_pGameLevel)
	{
		CObject* O = Level().CurrentEntity();
		if (O)
		{
			IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(O));
			if (IR)
				IR->IR_OnMouseMove(dx, dy);
			return false;
		}
	};
	return true;
}
