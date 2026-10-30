////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder_object_script.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script object binder script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_binder_object.h"
#include "script_export_space.h"
#include "script_binder_object_wrapper.h"
#include "xrServer_Objects_ALife.h"
#include "ai_space.h"
#include "script_engine.h"

using namespace luabind;

static ::luabind::object binder_object(const CScriptBinderObject* self)
{
	if (const auto wrapper = dynamic_cast<const CScriptBinderObjectWrapper*>(self))
		return wrapper->m_lua_object;
	return ::luabind::object(ai().script_engine().lua(), self->m_object);
}

#pragma optimize("s",on)
void CScriptBinderObject::script_register(lua_State* L)
{
	module(L)
	[
		class_<CScriptBinderObject, CScriptBinderObjectWrapper>("object_binder")
		.property("object", &binder_object)
		.def(constructor<CScriptGameObject*>())
		.def("reinit", &CScriptBinderObject::reinit, &CScriptBinderObjectWrapper::reinit_static)
		.def("reload", &CScriptBinderObject::reload, &CScriptBinderObjectWrapper::reload_static)
		.def("net_spawn", &CScriptBinderObject::net_Spawn, &CScriptBinderObjectWrapper::net_Spawn_static)
		.def("net_destroy", &CScriptBinderObject::net_Destroy, &CScriptBinderObjectWrapper::net_Destroy_static)
		.def("net_import", &CScriptBinderObject::net_Import, &CScriptBinderObjectWrapper::net_Import_static)
		.def("net_export", &CScriptBinderObject::net_Export, &CScriptBinderObjectWrapper::net_Export_static)
		.def("update", &CScriptBinderObject::shedule_Update, &CScriptBinderObjectWrapper::shedule_Update_static)
		.def("save", &CScriptBinderObject::save, &CScriptBinderObjectWrapper::save_static)
		.def("load", &CScriptBinderObject::load, &CScriptBinderObjectWrapper::load_static)
		.def("net_save_relevant", &CScriptBinderObject::net_SaveRelevant,
		     &CScriptBinderObjectWrapper::net_SaveRelevant_static)
		.def("net_Relcase", &CScriptBinderObject::net_Relcase, &CScriptBinderObjectWrapper::net_Relcase_static)
	];
}
