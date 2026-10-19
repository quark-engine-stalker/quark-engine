GAMMA 0.9.5 item reference fixes
===============================

Copy mod_system_quark_gamma_item_fixes.ltx into the active gamedata/configs
directory. MO2 users can install it as a mod or put it in overwrite/gamedata/configs.
DLTX is required and is already enabled in GAMMA. Remove this file to undo the
content overrides. Existing mod files and scripts are left intact.

The file corrects two resolved references:

* document points at dynamics/equipments/item_document_1.ogf, which is absent in
  Anomaly's model archives. The existing model is equipments/item_document_1.ogf.
* placeable_pegboard returns pegboard_item on pickup, but that section does not
  exist. The correct inventory section is pegboard.

The native PHShellCreator.cpp fix separately repairs invalid mass in a single-bone
inventory model while retaining its authored collision shape and bind transform.
It uses positive ph_mass when specified, otherwise positive inv_weight. Shared
visual bone data are never modified. This fixes pegboard, pegboard_pistol and
headgear_display, whose root bone mass is zero in Gadgets for Hideout Furniture.

Verification is an isolated ODE/configuration audit, not a complete in-game
spawn/pickup/drop or ALife/UI/Lua/renderer regression test. The furniture pickup
reference is validated against the actual resolved CInifile section table and the
active bind_hf_base.script pickup implementation.
