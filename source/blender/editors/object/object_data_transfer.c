/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2014 by Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Bastien Montagne.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/object/object_transfer_data.c
 *  \ingroup edobj
 */

#include "MEM_guardedalloc.h"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_math.h"
#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_object_data_transfer.h"
#include "BKE_DerivedMesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_remap.h"
#include "BKE_object.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_object.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "object_intern.h"

/* All possible data to transfer.
 * Note some are 'fake' ones, i.e. they are not hold by real CDLayers. */
/* Not shared with modifier, since we use a usual enum here, not a multi-choice one. */
static EnumPropertyItem DT_layer_items[] = {
	{0, "", 0, "Vertex Data", ""},
	{DT_TYPE_MDEFORMVERT, "VGROUP_WEIGHTS", 0, "Vertex Group(s)", "Transfer active or all vertex groups"},
#if 0  /* XXX For now, would like to finish/merge work from 2014 gsoc first. */
	{DT_TYPE_SHAPEKEY, "SHAPEKEYS", 0, "Shapekey(s)", "Transfer active or all shape keys"},
#endif
#if 0  /* XXX When SkinModifier is enabled, it seems to erase its own CD_MVERT_SKIN layer from final DM :( */
	{DT_TYPE_SKIN, "SKIN", 0, "Skin Weight", "Transfer skin weights"},
#endif
	{DT_TYPE_BWEIGHT_VERT, "BEVEL_WEIGHT_VERT", 0, "Bevel Weight", "Transfer bevel weights"},
	{0, "", 0, "Edge Data", ""},
	{DT_TYPE_SHARP_EDGE, "SHARP_EDGE", 0, "Sharp", "Transfer sharp mark"},
	{DT_TYPE_SEAM, "SEAM", 0, "UV Seam", "Transfer UV seam mark"},
	{DT_TYPE_CREASE, "CREASE", 0, "Subsurf Crease", "Transfer crease values"},
	{DT_TYPE_BWEIGHT_EDGE, "BEVEL_WEIGHT_EDGE", 0, "Bevel Weight", "Transfer bevel weights"},
	{DT_TYPE_FREESTYLE_EDGE, "FREESTYLE_EDGE", 0, "Freestyle Mark", "Transfer Freestyle edge mark"},
	{0, "", 0, "Face Corner Data", ""},
	{DT_TYPE_VCOL, "VCOL", 0, "VCol", "Vertex (face corners) colors"},
	{DT_TYPE_UV, "UV", 0, "UVs", "Transfer UV layers"},
	{0, "", 0, "Face Data", ""},
	{DT_TYPE_SHARP_FACE, "SMOOTH", 0, "Smooth", "Transfer flat/smooth mark"},
	{DT_TYPE_FREESTYLE_FACE, "FREESTYLE_FACE", 0, "Freestyle Mark", "Transfer Freestyle face mark"},
	{0, NULL, 0, NULL, NULL}
};

/* Note: DT_fromlayers_select_items enum is from rna_modifier.c */
static EnumPropertyItem *dt_fromlayers_select_itemf(
        bContext *C, PointerRNA *ptr, PropertyRNA *UNUSED(prop), bool *r_free)
{
	EnumPropertyItem *item = NULL, tmp_item = {0};
	int totitem = 0;

	const int data_type = RNA_enum_get(ptr, "data_type");

	if (!C) {  /* needed for docs and i18n tools */
		return DT_fromlayers_select_items;
	}

	RNA_enum_items_add_value(&item, &totitem, DT_fromlayers_select_items, DT_LAYERS_ACTIVE_SRC);
	RNA_enum_items_add_value(&item, &totitem, DT_fromlayers_select_items, DT_LAYERS_ALL_SRC);

	if (data_type == DT_TYPE_MDEFORMVERT) {
		Object *ob_src = CTX_data_active_object(C);

		if (BKE_object_pose_armature_get(ob_src)) {
			RNA_enum_items_add_value(&item, &totitem, DT_fromlayers_select_items, DT_LAYERS_VGROUP_SRC_BONE_SELECT);
			RNA_enum_items_add_value(&item, &totitem, DT_fromlayers_select_items, DT_LAYERS_VGROUP_SRC_BONE_DEFORM);
		}

		if (ob_src) {
			bDeformGroup *dg;
			int i;

			RNA_enum_item_add_separator(&item, &totitem);

			for (i = 0, dg = ob_src->defbase.first; dg; i++, dg = dg->next) {
				tmp_item.value = i;
				tmp_item.identifier = tmp_item.name = dg->name;
				RNA_enum_item_add(&item, &totitem, &tmp_item);
			}
		}
	}
	else if (data_type == DT_TYPE_SHAPEKEY) {
		/* TODO */
	}
	else if (data_type == DT_TYPE_UV) {
		Object *ob_src = CTX_data_active_object(C);
		Scene *scene = CTX_data_scene(C);

		if (ob_src) {
			DerivedMesh *dm_src;
			CustomData *pdata;
			int num_data, i;

			/* XXX Is this OK? */
			dm_src = mesh_get_derived_final(scene, ob_src, CD_MASK_BAREMESH | CD_MTEXPOLY);
			pdata = dm_src->getPolyDataLayout(dm_src);
			num_data = CustomData_number_of_layers(pdata, CD_MTEXPOLY);

			RNA_enum_item_add_separator(&item, &totitem);

			for (i = 0; i < num_data; i++) {
				tmp_item.value = i;
				tmp_item.identifier = tmp_item.name = CustomData_get_layer_name(pdata, CD_MTEXPOLY, i);
				RNA_enum_item_add(&item, &totitem, &tmp_item);
			}
		}
	}
	else if (data_type == DT_TYPE_VCOL) {
		Object *ob_src = CTX_data_active_object(C);
		Scene *scene = CTX_data_scene(C);

		if (ob_src) {
			DerivedMesh *dm_src;
			CustomData *ldata;
			int num_data, i;

			/* XXX Is this OK? */
			dm_src = mesh_get_derived_final(scene, ob_src, CD_MASK_BAREMESH | CD_MLOOPCOL);
			ldata = dm_src->getLoopDataLayout(dm_src);
			num_data = CustomData_number_of_layers(ldata, CD_MLOOPCOL);

			RNA_enum_item_add_separator(&item, &totitem);

			for (i = 0; i < num_data; i++) {
				tmp_item.value = i;
				tmp_item.identifier = tmp_item.name = CustomData_get_layer_name(ldata, CD_MLOOPCOL, i);
				RNA_enum_item_add(&item, &totitem, &tmp_item);
			}
		}
	}

	RNA_enum_item_end(&item, &totitem);
	*r_free = true;

	return item;
}

/* Note: DT_tolayers_select_items enum is from rna_modifier.c */
static EnumPropertyItem *dt_tolayers_select_itemf(bContext *C, PointerRNA *ptr, PropertyRNA *UNUSED(prop), bool *r_free)
{
	EnumPropertyItem *item = NULL;
	int totitem = 0;

	const int fromlayers_select = RNA_enum_get(ptr, "fromlayers_select");

	if (!C) {  /* needed for docs and i18n tools */
		return DT_tolayers_select_items;
	}

	if (fromlayers_select == DT_LAYERS_ACTIVE_SRC || fromlayers_select >= 0) {
		RNA_enum_items_add_value(&item, &totitem, DT_tolayers_select_items, DT_LAYERS_ACTIVE_DST);
	}
	RNA_enum_items_add_value(&item, &totitem, DT_tolayers_select_items, DT_LAYERS_NAME_DST);
	RNA_enum_items_add_value(&item, &totitem, DT_tolayers_select_items, DT_LAYERS_INDEX_DST);

	/* No 'specific' to-layers here, since we may transfer to several objects at once! */

	RNA_enum_item_end(&item, &totitem);
	*r_free = true;

	return item;
}

/* Note: DT_mix_mode_items enum is from rna_modifier.c */
static EnumPropertyItem *dt_mix_mode_itemf(bContext *C, PointerRNA *ptr, PropertyRNA *UNUSED(prop), bool *r_free)
{
	EnumPropertyItem *item = NULL;
	int totitem = 0;

	const int dtdata_type = RNA_enum_get(ptr, "data_type");
	bool support_advanced_mixing, support_threshold;

	if (!C) {  /* needed for docs and i18n tools */
		return DT_mix_mode_items;
	}

	RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_TRANSFER);

	BKE_object_data_transfer_get_dttypes_capacity(dtdata_type, &support_advanced_mixing, &support_threshold);

	if (support_advanced_mixing) {
		RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_REPLACE_ABOVE_THRESHOLD);
		RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_REPLACE_BELOW_THRESHOLD);
	}

	if (support_advanced_mixing) {
		RNA_enum_item_add_separator(&item, &totitem);
		RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_MIX);
		RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_ADD);
		RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_SUB);
		RNA_enum_items_add_value(&item, &totitem, DT_mix_mode_items, CDT_MIX_MUL);
	}

	RNA_enum_item_end(&item, &totitem);
	*r_free = true;

	return item;
}

static bool data_transfer_check(bContext *UNUSED(C), wmOperator *op)
{
	const int fromlayers_select = RNA_enum_get(op->ptr, "fromlayers_select");
	PropertyRNA *prop = RNA_struct_find_property(op->ptr, "tolayers_select");
	const int tolayers_select = RNA_property_enum_get(op->ptr, prop);

	/* TODO: check for invalid fromlayers select modes too! */

	if ((fromlayers_select != DT_LAYERS_ACTIVE_SRC) && (tolayers_select == DT_LAYERS_ACTIVE_DST)) {
		RNA_property_enum_set(op->ptr, prop, DT_LAYERS_NAME_DST);
		return true;
	}

	return false;
}

static int data_transfer_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *ob_src = CTX_data_active_object(C);

	bool changed = false;

	const int data_type = RNA_enum_get(op->ptr, "data_type");
	const bool use_create = RNA_boolean_get(op->ptr, "use_create");

	const int map_vert_mode = RNA_enum_get(op->ptr, "vert_mapping");
	const int map_edge_mode = RNA_enum_get(op->ptr, "edge_mapping");
	const int map_loop_mode = RNA_enum_get(op->ptr, "loop_mapping");
	const int map_poly_mode = RNA_enum_get(op->ptr, "poly_mapping");

	const bool use_object_transform = RNA_boolean_get(op->ptr, "use_object_transform");
	const bool use_max_distance = RNA_boolean_get(op->ptr, "use_max_distance");
	const float max_distance = use_max_distance ? RNA_float_get(op->ptr, "max_distance") : FLT_MAX;
	const float ray_radius = RNA_float_get(op->ptr, "ray_radius");

	const int fromlayers = RNA_enum_get(op->ptr, "fromlayers_select");
	const int tolayers = RNA_enum_get(op->ptr, "tolayers_select");
	int fromlayers_select[DT_MULTILAYER_INDEX_MAX] = {0};
	int tolayers_select[DT_MULTILAYER_INDEX_MAX] = {0};
	const int fromto_idx = BKE_object_data_transfer_dttype_to_srcdst_index(data_type);

	const int mix_mode = RNA_enum_get(op->ptr, "mix_mode");
	const float mix_factor = RNA_float_get(op->ptr, "mix_factor");

	SpaceTransform space_transform_data;
	SpaceTransform *space_transform = use_object_transform ? &space_transform_data : NULL;

	if (fromto_idx != DT_MULTILAYER_INDEX_INVALID) {
		fromlayers_select[fromto_idx] = fromlayers;
		tolayers_select[fromto_idx] = tolayers;
	}

	CTX_DATA_BEGIN (C, Object *, ob_dst, selected_editable_objects)
	{
		if ((ob_dst == ob_src) || (ob_dst->type != OB_MESH)) {
			continue;
		}

		if (space_transform) {
			BLI_SPACE_TRANSFORM_SETUP(space_transform, ob_dst, ob_src);
		}

		if (BKE_object_data_transfer_mesh(scene, ob_src, ob_dst, data_type, use_create,
		                           map_vert_mode, map_edge_mode, map_loop_mode, map_poly_mode, 
		                           space_transform, max_distance, ray_radius, fromlayers_select, tolayers_select,
		                           mix_mode, mix_factor, NULL, false, op->reports))
		{
			changed = true;
		}
	}
	CTX_DATA_END;

#if 0  /* TODO */
	/* Note: issue with that is that if canceled, operator cannot be redone... Nasty in our case. */
	return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
#else
	return OPERATOR_FINISHED;
#endif
}

static int data_transfer_poll(bContext *C)
{
	Object *ob = ED_object_context(C);
	ID *data = (ob) ? ob->data : NULL;
	return (ob && !ob->id.lib && ob->type == OB_MESH && data && !data->lib);
}

static bool data_transfer_draw_check_prop(PointerRNA *ptr, PropertyRNA *prop)
{
	const char *prop_id = RNA_property_identifier(prop);
	const int data_type = RNA_enum_get(ptr, "data_type");
	const bool use_max_distance = RNA_boolean_get(ptr, "use_max_distance");
	const int mix_mode = RNA_enum_get(ptr, "mix_mode");

	if (STREQ(prop_id, "max_distance") && !use_max_distance) {
		return false;
	}

	if (STREQ(prop_id, "vert_mapping") && !DT_DATATYPE_IS_VERT(data_type)) {
		return false;
	}
	if (STREQ(prop_id, "edge_mapping") && !DT_DATATYPE_IS_EDGE(data_type)) {
		return false;
	}
	if (STREQ(prop_id, "loop_mapping") && !DT_DATATYPE_IS_LOOP(data_type)) {
		return false;
	}
	if (STREQ(prop_id, "poly_mapping") && !DT_DATATYPE_IS_POLY(data_type)) {
		return false;
	}

	if (STREQ(prop_id, "mix_factor") && (mix_mode == CDT_MIX_TRANSFER)) {
		return false;
	}

	if ((STREQ(prop_id, "fromlayers_select") || STREQ(prop_id, "tolayers_select")) &&
	    !DT_DATATYPE_IS_MULTILAYERS(data_type))
	{
		return false;
	}

	/* Else, show it! */
	return true;
}

static void data_transfer_ui(bContext *C, wmOperator *op)
{
	uiLayout *layout = op->layout;
	wmWindowManager *wm = CTX_wm_manager(C);
	PointerRNA ptr;

	RNA_pointer_create(&wm->id, op->type->srna, op->properties, &ptr);

	/* Main auto-draw call */
	uiDefAutoButsRNA(layout, &ptr, data_transfer_draw_check_prop, '\0');
}

/* transfers weight from active to selected */
void OBJECT_OT_data_transfer(wmOperatorType *ot)
{
	PropertyRNA *prop;

	/* Identifiers.*/
	ot->name = "Transfer Mesh Data";
	ot->idname = "OBJECT_OT_data_transfer";
	ot->description = "Transfer data layer(s) (weights, edge sharp, ...) from active to selected meshes";

	/* API callbacks.*/
	ot->poll = data_transfer_poll;
	ot->invoke = WM_menu_invoke;
	ot->exec = data_transfer_exec;
	ot->check = data_transfer_check;
	ot->ui = data_transfer_ui;

	/* Flags.*/
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* Properties.*/
	/* Data type to transfer. */
	ot->prop = RNA_def_enum(ot->srna, "data_type", DT_layer_items, 0, "Data Type", "Which data to transfer");
	RNA_def_boolean(ot->srna, "use_create", true, "Create Data", "Add data layers on destination meshes if needed");

	/* Mapping methods. */
	RNA_def_enum(ot->srna, "vert_mapping", DT_method_vertex_items, MREMAP_MODE_VERT_NEAREST, "Vertex Mapping",
	             "Method used to map source vertices to destination ones");
	RNA_def_enum(ot->srna, "edge_mapping", DT_method_edge_items, MREMAP_MODE_EDGE_NEAREST, "Edge Mapping",
	             "Method used to map source edges to destination ones");
	RNA_def_enum(ot->srna, "loop_mapping", DT_method_loop_items, MREMAP_MODE_LOOP_NEAREST_POLYNOR,
	             "Face Corner Mapping", "Method used to map source faces' corners to destination ones");
	RNA_def_enum(ot->srna, "poly_mapping", DT_method_poly_items, MREMAP_MODE_POLY_NEAREST, "Face Mapping",
	             "Method used to map source faces to destination ones");

	/* Mapping options and filtering. */
	RNA_def_boolean(ot->srna, "use_object_transform", true, "Object Transform",
	                "Evaluate source and destination meshes in their respective object spaces");
	RNA_def_boolean(ot->srna, "use_max_distance", false, "Only Neighbor Geometry",
	                "Source elements must be closer than given distance from destination one");
	prop = RNA_def_float(ot->srna, "max_distance", 1.0f, 0.0f, FLT_MAX, "Max Distance",
	                     "Maximum allowed distance between source and destination element, for non-topology mappings",
	                     0.0f, 100.0f);
	RNA_def_property_subtype(prop, PROP_DISTANCE);
	prop = RNA_def_float(ot->srna, "ray_radius", 0.0f, 0.0f, FLT_MAX, "Ray Radius",
	                     "'Width' of rays (especially useful when raycasting against vertices or edges)",
	                     0.0f, 10.0f);
	RNA_def_property_subtype(prop, PROP_DISTANCE);

	/* How to handle multi-layers types of data. */
	prop = RNA_def_enum(ot->srna, "fromlayers_select", DT_fromlayers_select_items, DT_LAYERS_ACTIVE_SRC,
	                    "Source Layers Selection", "Which layers to transfer, in case of multi-layers types");
	RNA_def_property_enum_funcs_runtime(prop, NULL, NULL, dt_fromlayers_select_itemf);

	prop = RNA_def_enum(ot->srna, "tolayers_select", DT_tolayers_select_items, DT_LAYERS_ACTIVE_DST,
	                    "Destination Layers Matching", "How to match source and destination layers");
	RNA_def_property_enum_funcs_runtime(prop, NULL, NULL, dt_tolayers_select_itemf);

	prop = RNA_def_enum(ot->srna, "mix_mode", DT_mix_mode_items, CDT_MIX_TRANSFER, "Mix Mode",
	                   "How to affect destination elements with source values");
	RNA_def_property_enum_funcs_runtime(prop, NULL, NULL, dt_mix_mode_itemf);
	RNA_def_float(ot->srna, "mix_factor", 1.0f, 0.0f, 1.0f, "Mix Factor",
	              "Factor to use when applying data to destination (exact behavior depends on mix mode)", 0.0f, 1.0f);
}

/******************************************************************************/
/* Note: This operator is hybrid, it can work as a usual standalone Object operator,
 *       or as a DataTransfer modifier tool.
 */

static int datalayout_transfer_poll(bContext *C)
{
	return (edit_modifier_poll_generic(C, &RNA_DataTransferModifier, (1 << OB_MESH)) || data_transfer_poll(C));
}

static int datalayout_transfer_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *ob_act = ED_object_active_context(C);
	DataTransferModifierData *dtmd;

	dtmd = (DataTransferModifierData *)edit_modifier_property_get(op, ob_act, eModifierType_DataTransfer);

	/* If we have a modifier, we transfer data layout from this modifier's source object to active one.
	 * Else, we transfer data layout from active object to all selected ones. */
	if (dtmd) {
		Object *ob_src = dtmd->ob_source;
		Object *ob_dst = ob_act;

		const bool use_delete = false;  /* Never when used from modifier, for now. */

		BKE_object_data_transfer_layout(scene, ob_src, ob_dst, dtmd->data_types, use_delete,
		                                dtmd->layers_select_src, dtmd->layers_select_dst);
	}
	else {
		Object *ob_src = ob_act;
		Object *ob_dst;

		CTX_DATA_BEGIN (C, Object *, ob_dst, selected_editable_objects)
		{
			if ((ob_dst == ob_src) || (ob_dst->type != OB_MESH)) {
				continue;
			}
		}
		CTX_DATA_END;
	}

	return OPERATOR_FINISHED;
}

static int datalayout_transfer_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	edit_modifier_invoke_properties(C, op);
	return datalayout_transfer_exec(C, op);
}

void OBJECT_OT_datalayout_transfer(wmOperatorType *ot)
{
	ot->name = "Datalayout Transfer";
	ot->description = "Transfer layout of data layer(s) from active object to selected ones";
	ot->idname = "OBJECT_OT_datalayout_transfer";

	ot->poll = datalayout_transfer_poll;
	ot->invoke = datalayout_transfer_invoke;
	ot->exec = datalayout_transfer_exec;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	edit_modifier_properties(ot);
}
