#pragma once

template <typename T>
struct CWrapperBase : public T, public ::luabind::wrap_base
{
	typedef T inherited;
	typedef CWrapperBase<T> self_type;

	bool detach_expired_lua_instance()
	{
		if (is_lua_instance_valid())
			return false;

		// The holder only borrows this window. A script window may nevertheless
		// outlive its Lua VM across quickload, so revoke both the input-dialog and
		// render-only registrations before any other virtual callback can reach it.
		if (this->GetHolder() && this->IsShown())
		{
			this->HideDialog();
			return true;
		}

		this->SetHolder(NULL);
		if (this->m_pDialogHolder)
		{
			this->m_pDialogHolder->RemoveDialogToRender(this);
			return true;
		}

		this->CUIWindow::Show(false);
		return true;
	}

	virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action)
	{
		if (detach_expired_lua_instance())
			return true;

		return call_member<bool>(this, "OnKeyboard", dik, keyboard_action);
	}

	static bool OnKeyboard_static(inherited* ptr, int dik, EUIMessages keyboard_action)
	{
		return ptr->self_type::inherited::OnKeyboardAction(dik, keyboard_action);
	}

	virtual void Update()
	{
		if (detach_expired_lua_instance())
			return;

		call_member<void>(this, "Update");
	}

	static void Update_static(inherited* ptr)
	{
		ptr->self_type::inherited::Update();
	}

	virtual bool Dispatch(int cmd, int param)
	{
		if (detach_expired_lua_instance())
			return true;

		return call_member<bool>(this, "Dispatch", cmd, param);
	}

	static bool Dispatch_static(inherited* ptr, int cmd, int param)
	{
		return ptr->self_type::inherited::Dispatch(cmd, param);
	}
};

typedef CWrapperBase<CUIDialogWndEx> WrapType;
typedef CUIDialogWndEx BaseType;

typedef luabind::class_<CUIDialogWndEx, luabind::bases<CUIDialogWnd, DLL_Pure>, WrapType> export_class;
