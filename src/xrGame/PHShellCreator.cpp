#include "stdafx.h"
#include "PHShellCreator.h"
#include "../xrphysics/PhysicsShell.h"
#include "gameobject.h"
#include "physicsshellholder.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../xrEngine/bone.h"
#include "../xrPhysics/Geometry.h"

void CPHShellSimpleCreator::CreatePhysicsShell()
{
	CPhysicsShellHolder* owner = smart_cast<CPhysicsShellHolder*>(this);
	VERIFY(owner);
	if (!owner->Visual()) return;

	IKinematics* pKinematics = smart_cast<IKinematics*>(owner->Visual());
	VERIFY(pKinematics);

	if (owner->PPhysicsShell()) return;

	// Pick shapes marked sfNoPhysics and hidden bones do not create collision geometry.
	// A dropped inventory item still needs a physical volume in that case.
	const bool use_visual_box = !has_physics_collision_shapes(*pKinematics);
	const u16 root_id = pKinematics->LL_GetBoneRoot();
	bool repair_single_bone_mass = false;
	if (!use_visual_box && pKinematics->LL_BoneCount() == 1 && root_id != BI_NONE)
	{
		const float root_mass = pKinematics->GetBoneData(root_id).get_mass();
		repair_single_bone_mass = !_valid(root_mass) || root_mass <= 0.f;
	}
	if (!use_visual_box)
		phys_shell_verify_object_model(*owner);

	owner->PPhysicsShell() = P_create_Shell();
#ifdef DEBUG
	owner->PPhysicsShell()->dbg_obj=owner;
#endif
	if (use_visual_box || repair_single_bone_mass)
	{
		R_ASSERT2(root_id != BI_NONE, "Physics fallback requires a root bone");
		const IBoneData& root = pKinematics->GetBoneData(root_id);
		R_ASSERT2(_valid(root.get_bind_transform()) && _valid(owner->XFORM()),
			"Invalid transform for physics fallback");

		const bool has_config_mass = pSettings->line_exist(owner->cNameSect_str(), "ph_mass");
		float mass = has_config_mass ? pSettings->r_float(owner->cNameSect_str(), "ph_mass") : root.get_mass();
		// Some furniture models have a valid collision shape but zero bone mass.
		// Rebuild that single element with item weight before ODE sees a zero mass.
		// Keep the shared visual's shape, mass and bind data unchanged.
		if (repair_single_bone_mass && !has_config_mass)
		{
			R_ASSERT2(pSettings->line_exist(owner->cNameSect_str(), "inv_weight"),
				"Invalid bone mass requires ph_mass or inv_weight");
			mass = pSettings->r_float(owner->cNameSect_str(), "inv_weight");
		}
		R_ASSERT2(_valid(mass) && mass > 0.f, "Invalid mass for physics fallback");

		Fobb box;
		Fvector mass_center;
		if (use_visual_box)
		{
			// A shell without geometry has infinite extents and a NaN spawn center.
			owner->Visual()->getVisData().box.get_CD(box.m_translate, box.m_halfsize);
			R_ASSERT2(_valid(box.m_translate) && _valid(box.m_halfsize) &&
				box.m_halfsize.x > 0.f && box.m_halfsize.y > 0.f && box.m_halfsize.z > 0.f,
				make_string("Invalid visual bounds for physics fallback: object: %s model: %s",
					owner->cName().c_str(), owner->cNameVisual().c_str()));

			// Geometry is local to the root element, while bounds are model-local.
			Fmatrix inverse_root;
			inverse_root.invert(root.get_bind_transform());
			inverse_root.transform_tiny(box.m_translate);
			box.m_rotate.set(inverse_root);
			mass_center.set(box.m_translate);
		}
		else
		{
			R_ASSERT2(_valid(root.get_center_of_mass()), "Invalid physics mass center");
			mass_center.set(root.get_center_of_mass());
		}

		CPhysicsElement* element = P_create_Element();
		element->m_SelfID = root_id;
		element->mXFORM.set(root.get_bind_transform());
		element->SetMaterial(root.get_game_mtl_idx());
		if (use_visual_box)
		{
			element->add_Box(box);
			element->last_geom()->set_shape_flags(Flags16().assign(0));
		}
		else
		{
			element->add_Shape(root.get_shape());
			element->last_geom()->set_shape_flags(root.get_shape().flags);
		}
		element->last_geom()->set_bone_id(element->m_SelfID);
		element->setMassMC(mass, mass_center);
		owner->m_pPhysicsShell->set_Kinematics(pKinematics);
		owner->m_pPhysicsShell->add_Element(element);
		if (use_visual_box)
			Msg("! [PHYSICS] Model has no collision shapes; using visual box. object: %s model: %s",
				owner->cName().c_str(), owner->cNameVisual().c_str());
		else
			Msg("! [PHYSICS] Invalid single-bone mass; using item mass %f. object: %s model: %s",
				mass, owner->cName().c_str(), owner->cNameVisual().c_str());
	}
	else
		owner->m_pPhysicsShell->build_FromKinematics(pKinematics, 0);

	owner->PPhysicsShell()->set_PhysicsRefObject(owner);
	//m_pPhysicsShell->SmoothElementsInertia(0.3f);
	owner->PPhysicsShell()->mXFORM.set(owner->XFORM());
	owner->PPhysicsShell()->SetAirResistance(0.001f, 0.02f);
}
