#include "pch_script.h"
#include "gameobject.h"
#include "script_game_object.h"
#include "../xrcdb/xr_collide_defs.h"
#include "material_manager.h"

struct script_rq_result
{
	// NOTE: this struct is copied by value into Lua and can be kept there for an arbitrary time,
	// while CScriptGameObject is freed in CGameObject::net_Destroy(). A raw CScriptGameObject* here
	// became dangling and crashed luabind (pointer_converter::apply) on `rq_result.object`.
	// Keep a weak handle instead and resolve it on access via get_object().
	std::weak_ptr<CScriptGameObject::lifetime_token> O_lifetime;
	bool has_object; // an object (not static geometry) was hit when the query was made
	bool has_static_geometry; // a static hit is distinct from triangle index 0 / no hit

	CScriptGameObject* get_object() const
	{
		const std::shared_ptr<CScriptGameObject::lifetime_token> token = O_lifetime.lock();
		return token ? token->object : nullptr;
	}

	float range;
	int element;

	// Material of tri of ray query result
	str_c pMaterialName;
	u32 pMaterialFlags;

	// physics part
	float fPHFriction; // ?
	float fPHDamping; // ?
	float fPHSpring; // ?
	float fPHBounceStartVelocity; // ?
	float fPHBouncing; // ?
					   // shoot&bounce&visibility&flotation
	float fFlotationFactor; // 0.f - 1.f (1.f-полностью проходимый)
	float fShootFactor; // 0.f - 1.f (1.f-полностью простреливаемый)
	float fShootFactorMP; // 0.f - 1.f (1.f-полностью простреливаемый)
	float fBounceDamageFactor; // 0.f - 100.f
	float fInjuriousSpeed; // 0.f - ... (0.f-не отбирает здоровье (скорость уменьшения здоровья))
	float fVisTransparencyFactor; // 0.f - 1.f (1.f-полностью прозрачный)
	float fSndOcclusionFactor; // 0.f - 1.f (1.f-полностью слышен)
	float fDensityFactor;

	script_rq_result()
	{
		// Initialise every field: previously only O/range/element were set, so material_name
		// (a const char*) and the material_* values were garbage for results without a hit.
		reset();
	};

	void reset() {
		O_lifetime.reset();
		has_object = false;
		has_static_geometry = false;
		range = 0;
		element = 0;

		pMaterialName = nullptr;
		pMaterialFlags = 0;

		fPHFriction = 0;
		fPHDamping = 0;
		fPHSpring = 0;
		fPHBounceStartVelocity = 0;
		fPHBouncing = 0;
		fFlotationFactor = 0;
		fShootFactor = 0;
		fShootFactorMP = 0;
		fBounceDamageFactor = 0;
		fInjuriousSpeed = 0;
		fVisTransparencyFactor = 0;
		fSndOcclusionFactor = 0;
		fDensityFactor = 0;
	}

	void set(collide::rq_result& R)
	{
		// The same rq_result may be reused as an out-parameter (ray_pick): never keep the
		// object of a previous query when the new hit is static geometry.
		O_lifetime.reset();
		has_object = false;
		has_static_geometry = (R.O == nullptr);
		if (R.O)
		{
			CGameObject* go = smart_cast<CGameObject *>(R.O);
			if (go)
			{
				CScriptGameObject* lua_object = go->lua_game_object();
				if (lua_object)
				{
					O_lifetime = lua_object->lifetime();
					has_object = true;
				}
			}
		}
		range = R.range;
		element = R.element;

		// demonized: set material params of ray pick result
		//Msg("no object, check material");
		auto pTri = Level().ObjectSpace.GetStaticTris() + R.element;
		auto pMaterial = GMLib.GetMaterialByIdx(pTri->material);
		auto pMaterialFlagsRQ = pMaterial->Flags;
		//pTri = pTri;
		//pMaterial = pMaterial;
		pMaterialFlags = pMaterialFlagsRQ.flags;
		pMaterialName = pMaterial->m_Name.c_str();

		fPHFriction = pMaterial->fPHFriction;
		fPHDamping = pMaterial->fPHDamping;
		fPHSpring = pMaterial->fPHSpring;
		fPHBounceStartVelocity = pMaterial->fPHBounceStartVelocity;
		fPHBouncing = pMaterial->fPHBouncing;
		fFlotationFactor = pMaterial->fFlotationFactor;
		fShootFactor = pMaterial->fShootFactor;
		fShootFactorMP = pMaterial->fShootFactorMP;
		fBounceDamageFactor = pMaterial->fBounceDamageFactor;
		fInjuriousSpeed = pMaterial->fInjuriousSpeed;
		fVisTransparencyFactor = pMaterial->fVisTransparencyFactor;
		fSndOcclusionFactor = pMaterial->fSndOcclusionFactor;
		fDensityFactor = pMaterial->fDensityFactor;
	};
};

// class for performing ray pick
struct CRayPick
{
	Fvector start_position;
	Fvector direction;
	float range;
	collide::rq_target flags;
	script_rq_result result;
	xr_vector<CObject*> ignore;

	CRayPick();
	CRayPick(const Fvector& P, const Fvector& D, float R, collide::rq_target F, CScriptGameObject* I);

	IC void set_position(Fvector& P) { start_position = P; };
	IC void set_direction(Fvector& D) { direction = D; };
	IC void set_range(float R) { range = R; };
	IC void set_flags(collide::rq_target F) { flags = F; };
	void set_ignore_object(CScriptGameObject* I) { 
		if (I) {
			CObject* obj = smart_cast<CObject*>(&(I->object()));
			if (obj && std::find(ignore.begin(), ignore.end(), obj) == ignore.end()) {
				ignore.push_back(obj);
			}
		}
	};

	bool query();
	Fvector get_normal();

	IC script_rq_result get_result() { return result; };
	IC CScriptGameObject* get_object() { return result.get_object(); };
	IC float get_distance() { return result.range; };
	IC int get_element() { return result.element; };
};
