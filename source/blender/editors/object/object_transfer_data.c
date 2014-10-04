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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Ove M Henriksen.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/object/object_transfer_data.c
 *  \ingroup edobj
 */

#include <string.h>
#include <stddef.h>
#include <math.h>
#include <assert.h>

#include "MEM_guardedalloc.h"

#include "DNA_cloth_types.h"
#include "DNA_curve_types.h"
#include "DNA_lattice_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_object_force.h"
#include "DNA_scene_types.h"
#include "DNA_particle_types.h"

#include "BLI_alloca.h"
#include "BLI_array.h"
#include "BLI_math.h"
#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_linklist_stack.h"

#include "BLF_translation.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_depsgraph.h"
#include "BKE_mesh_mapping.h"
#include "BKE_editmesh.h"
#include "BKE_modifier.h"
#include "BKE_report.h"
#include "BKE_DerivedMesh.h"
#include "BKE_object_deform.h"
#include "BKE_object.h"
#include "BKE_lattice.h"

#include "DNA_armature_types.h"
#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_object.h"
#include "ED_mesh.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "object_intern.h"


#define MDT_DATATYPE_IS_VERT(_dt) ELEM(_dt, CD_FAKE_MDEFORMVERT, CD_FAKE_SHAPEKEY, CD_MVERT_SKIN, CD_FAKE_BWEIGHT)
#define MDT_DATATYPE_IS_EDGE(_dt) ELEM(_dt, CD_FAKE_CREASE, CD_FAKE_SHARP, CD_FAKE_SEAM, CD_FAKE_BWEIGHT)
#define MDT_DATATYPE_IS_POLY(_dt) (_dt == CD_FAKE_SHARP)
#define MDT_DATATYPE_IS_LOOP(_dt) (false)

#define MDT_DATATYPE_IS_MULTILAYERS(_dt) ELEM(_dt, CD_FAKE_MDEFORMVERT, CD_FAKE_SHAPEKEY)

/* All possible data to transfer.
 * Note some are 'fake' ones, i.e. they are not hold by real CDLayers. */
static EnumPropertyItem MDT_layer_items[] = {
	{0, "", 0, "Vertex Data", ""},
	{CD_FAKE_MDEFORMVERT, "VGROUP_WEIGHTS", 0, "Vertex Group(s)", "Transfer active or all vertex groups"},
	{CD_FAKE_SHAPEKEY, "SHAPEKEYS", 0, "Shapekey(s)", "Transfer active or all shape keys"},
	/* XXX When SkinModifier is enabled, it seems to erase its own CD_MVERT_SKIN layer from final DM :( */
	{CD_MVERT_SKIN, "SKIN", 0, "Skin Weight", "Transfer skin weights"},
	{0, "", 0, "Edge Data", ""},
	{CD_FAKE_SHARP, "SHARP", 0, "Sharp", "Transfer sharp flag"},
	{CD_FAKE_SEAM, "SEAM", 0, "Seam", "Transfer UV seam flag"},
	{CD_FAKE_CREASE, "CREASE", 0, "Subsurf Crease", "Transfer crease values"},
	{0, "", 0, "Face Data", ""},
	/* TODO */
	{0, "", 0, "Face Corner Data", ""},
	/* TODO */
	{0, NULL, 0, NULL, NULL}
};

/* Mapping methods, on a per-element type basis. */
static EnumPropertyItem MDT_method_vertex_items[] = {
	{M2MMAP_MODE_TOPOLOGY, "TOPOLOGY", 0, "Topology", "Copy from identical topology meshes"},
	{M2MMAP_MODE_VERT_NEAREST, "NEAREST", 0, "Nearest vertex", "Copy from closest vertex"},
	{M2MMAP_MODE_VERT_EDGE_NEAREST, "EDGE_NEAREST", 0, "Nearest Edge Vertex",
			"Copy from closest vertex of closest edge"},
	{M2MMAP_MODE_VERT_EDGEINTERP_NEAREST, "EDGEINTERP_NEAREST", 0, "Nearest Edge Interpolated",
			"Copy from interpolated values of vertices from closest point on closest edge"},
	{M2MMAP_MODE_VERT_POLY_NEAREST, "POLY_NEAREST", 0, "Nearest Face Vertex",
			"Copy from closest vertex of closest face"},
	{M2MMAP_MODE_VERT_POLYINTERP_NEAREST, "POLYINTERP_NEAREST", 0, "Nearest Face Interpolated",
			"Copy from interpolated values of vertices from closest point on closest face"},
	{M2MMAP_MODE_VERT_POLYINTERP_VNORPROJ, "POLYINTERP_VNORPROJ", 0, "Projected Face Interpolated",
			"Copy from interpolated values of vertices from point on closest face hit by normal-projection"},
	{0, NULL, 0, NULL, NULL}
};

static EnumPropertyItem MDT_method_edge_items[] = {
	{M2MMAP_MODE_TOPOLOGY, "TOPOLOGY", 0, "Topology", "Copy from identical topology meshes"},
	{M2MMAP_MODE_EDGE_VERT_NEAREST, "VERT_NEAREST", 0, "Nearest Vertices",
			"Copy from most similar edge (edge which vertices are the closest of destination edge’s ones)"},
	{M2MMAP_MODE_EDGE_NEAREST, "NEAREST", 0, "Nearest Edge", "Copy from closest edge (using midpoints)"},
	{M2MMAP_MODE_EDGE_POLY_NEAREST, "POLY_NEAREST", 0, "Nearest Face Edge",
			"Copy from closest edge of closest face (using midpoints)"},
	{0, NULL, 0, NULL, NULL}
};

static EnumPropertyItem MDT_method_poly_items[] = {
	{M2MMAP_MODE_TOPOLOGY, "TOPOLOGY", 0, "Topology", "Copy from identical topology meshes"},
	/* TODO add other modes */
	{0, NULL, 0, NULL, NULL}
};

static EnumPropertyItem MDT_method_loop_items[] = {
	{M2MMAP_MODE_TOPOLOGY, "TOPOLOGY", 0, "Topology", "Copy from identical topology meshes"},
	/* TODO add other modes */
	{0, NULL, 0, NULL, NULL}
};

/* How to filter out some elements (to leave untouched).
 * Note those options are highly dependent on type of transferred data! */
static EnumPropertyItem MDT_replace_mode_items[] = {
	{MDT_REPLACE, "REPLACE", 0, "All", "Overwrite all elements' data"},
	{MDT_REPLACE_THRESHOLD, "REPLACE_THRESHOLD", 0, "Below Threshold",
			"Only affect dest elements where data is below given threshold (exact behavior depends on data type)"},
	{0, NULL, 0, NULL, NULL}
};

/* How to select data layers, for types supporting multi-layers.
 * Here too, some options are highly dependent on type of transferred data! */
static EnumPropertyItem MDT_fromlayers_select_items[] = {
	{MDT_FROMLAYERS_ACTIVE, "ACTIVE", 0, "Active Layer", "Only transfer active data layer"},
	{MDT_FROMLAYERS_ALL, "ALL", 0, "All Layers", "Transfer all data layers"},
	{MDT_FROMLAYERS_VGROUP_BONE_SELECTED, "BONE_SELECT", 0, "Selected Pose Bones",
			"Transfer all vertex groups used by selected posebones"},
	{MDT_FROMLAYERS_VGROUP_BONE_DEFORM, "BONE_DEFORM", 0, "Deform Pose Bones",
			"Transfer all vertex groups used by deform bones"},
	{0, NULL, 0, NULL, NULL}
};

static EnumPropertyItem *mdt_fromlayers_select_itemf(bContext *C, PointerRNA *ptr, PropertyRNA *UNUSED(prop), bool *r_free)
{
	EnumPropertyItem *item = NULL;
	int totitem = 0;

	const int data_type = RNA_enum_get(ptr, "data_type");

	if (!C) {  /* needed for docs and i18n tools */
		return MDT_fromlayers_select_items;
	}

	RNA_enum_items_add_value(&item, &totitem, MDT_fromlayers_select_items, MDT_FROMLAYERS_ACTIVE);
	RNA_enum_items_add_value(&item, &totitem, MDT_fromlayers_select_items, MDT_FROMLAYERS_ALL);

	if (data_type == CD_FAKE_MDEFORMVERT) {
		Object *ob = CTX_data_active_object(C);
		if (BKE_object_pose_armature_get(ob)) {
			RNA_enum_items_add_value(&item, &totitem, MDT_fromlayers_select_items, MDT_FROMLAYERS_VGROUP_BONE_SELECTED);
			RNA_enum_items_add_value(&item, &totitem, MDT_fromlayers_select_items, MDT_FROMLAYERS_VGROUP_BONE_DEFORM);
		}
	}

	RNA_enum_item_end(&item, &totitem);
	*r_free = true;

	return item;
}

/* How to map a source layer to a destination layer, for types supporting multi-layers.
 * Note: if no matching layer can be found, it will be created. */
static EnumPropertyItem MDT_tolayers_select_items[] = {
	{MDT_TOLAYERS_ACTIVE, "ACTIVE", 0, "Active Layer", "Affect active data layer of all targets"},
	{MDT_TOLAYERS_NAME, "NAME", 0, "By Name", "Match target data layers to affect by name"},
	{MDT_TOLAYERS_INDEX, "INDEX", 0, "By Position", "Match target data layers to affect by position (indices)"},
	{0, NULL, 0, NULL, NULL}
};

static EnumPropertyItem *mdt_tolayers_select_itemf(bContext *C, PointerRNA *ptr, PropertyRNA *UNUSED(prop), bool *r_free)
{
	EnumPropertyItem *item = NULL;
	int totitem = 0;

	const int fromlayers_select = RNA_enum_get(ptr, "fromlayers_select");

	if (!C) {  /* needed for docs and i18n tools */
		return MDT_tolayers_select_items;
	}

	if (fromlayers_select == MDT_FROMLAYERS_ACTIVE) {
		RNA_enum_items_add_value(&item, &totitem, MDT_tolayers_select_items, MDT_TOLAYERS_ACTIVE);
	}
	RNA_enum_items_add_value(&item, &totitem, MDT_tolayers_select_items, MDT_TOLAYERS_NAME);
	RNA_enum_items_add_value(&item, &totitem, MDT_tolayers_select_items, MDT_TOLAYERS_INDEX);

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

	if ((fromlayers_select != MDT_FROMLAYERS_ACTIVE) && (tolayers_select == MDT_TOLAYERS_ACTIVE)) {
		RNA_property_enum_set(op->ptr, prop, MDT_TOLAYERS_NAME);
		return true;
	}

	return false;
}

/* ********** */

static void data_transfer_interp_char(const DataTransferLayerMapping *UNUSED(laymap), void **sources,
                                      const float *weights, int count, void *dest)
{
	char **data_src = (char **)sources;
	char *data_dst = (char *)dest;
	int i;

	float weight_dst = 0.0f;

	for (i = count; i--;) {
		weight_dst += ((float)(*data_src[i]) / 255.0f) * weights[i];
	}

	*data_dst = (char)(weight_dst * 255.0f);
}

/* Helpers to match sources and destinations data layers (also handles 'conversions' in CD_FAKE cases). */

void data_transfer_layersmapping_add_item(
        ListBase *r_map, const int data_type, void *data_src, void *data_dst, const int data_n_src, const int data_n_dst,
        const size_t elem_size, const size_t data_size, const size_t data_offset, const uint64_t data_flag,
        cd_datatransfer_interp interp)
{
	DataTransferLayerMapping *item = MEM_mallocN(sizeof(*item), __func__);

	BLI_assert(data_dst != NULL);

	item->data_type = data_type;

	item->data_src = data_src;
	item->data_dst = data_dst;
	item->data_n_src = data_n_src;
	item->data_n_dst = data_n_dst;
	item->elem_size = elem_size;

	item->data_size = data_size;
	item->data_offset = data_offset;
	item->data_flag = data_flag;

	item->interp = interp;

	BLI_addtail(r_map, item);
}

static void data_transfer_layersmapping_add_item_cd(ListBase *r_map, const int data_type, void *data_src, void *data_dst)
{
	data_transfer_layersmapping_add_item(r_map, data_type, data_src, data_dst, 0, 0, 0, 0, 0, 0, NULL);
}

static bool data_transfer_layersmapping_cdlayers_multisrc_to_dst(
        ListBase *r_map, const int data_type, const int num_create, CustomData *cd_src, CustomData *cd_dst,
        const int tolayers_select, bool *use_layers_src, const int num_layers_src)
{
	void *data_src, *data_dst = NULL;
	int idx_src = num_layers_src;
	int idx_dst;

	switch (tolayers_select) {
		case MDT_TOLAYERS_INDEX:
			{
				idx_dst = CustomData_number_of_layers(data_dst, data_type);

				/* Find last source actually used! */
				while (idx_src-- && !use_layers_src[idx_src]);
				idx_src++;

				if (idx_dst < idx_src) {
					if (!num_create) {
						return false;
					}
					/* Create as much data layers as necessary! */
					for (; idx_dst < idx_src; idx_dst++) {
						CustomData_add_layer(cd_dst, data_type, CD_CALLOC, NULL, num_create);
					}
				}
				while (idx_src--) {
					if (!use_layers_src[idx_src]) {
						continue;
					}
					data_src = CustomData_get_layer_n(cd_src, data_type, idx_src);
					data_dst = CustomData_get_layer_n(cd_dst, data_type, idx_src);
					data_transfer_layersmapping_add_item_cd(r_map, data_type, data_src, data_dst);
				}
			}
			break;
		case MDT_TOLAYERS_NAME:
			while (idx_src--) {
				const char *name;

				if (!use_layers_src[idx_src]) {
					continue;
				}

				name = CustomData_get_layer_name(cd_src, data_type, idx_src);
				if ((idx_dst = CustomData_get_named_layer(cd_dst, data_type, name)) == -1) {
					if (!num_create) {
						BLI_freelistN(r_map);
						return false;
					}
					CustomData_add_layer_named(data_dst, data_type, CD_CALLOC, NULL, num_create, name);
					idx_dst = CustomData_get_named_layer(data_dst, data_type, name);
				}
				data_src = CustomData_get_layer_n(cd_src, data_type, idx_src);
				data_dst = CustomData_get_layer_n(cd_dst, data_type, idx_dst);
				data_transfer_layersmapping_add_item_cd(r_map, data_type, data_src, data_dst);
			}
			break;
		default:
			return false;
	}

	return true;
}

bool ED_data_transfer_layersmapping_cdlayers(
        ListBase *r_map, const int data_type, const int num_create, CustomData *cd_src, CustomData *cd_dst,
        const int fromlayers_select, const int tolayers_select)
{
	int idx_src, idx_dst;
	void *data_src, *data_dst = NULL;

	if (CustomData_layertype_is_singleton(data_type)) {
		if (!(data_src = CustomData_get_layer(cd_src, data_type))) {
			return false;
		}
		if (!(data_dst = CustomData_get_layer(cd_dst, data_type))) {
			if (!num_create) {
				return false;
			}
			data_dst = CustomData_add_layer(cd_dst, data_type, CD_CALLOC, NULL, num_create);
		}

		data_transfer_layersmapping_add_item_cd(r_map, data_type, data_src, data_dst);
	}
	else if (fromlayers_select == MDT_FROMLAYERS_ACTIVE) {
		if ((idx_src = CustomData_get_active_layer(cd_src, data_type)) == -1) {
			return false;
		}
		data_src = CustomData_get_layer_n(cd_src, data_type, idx_src);
		switch (tolayers_select) {
			case MDT_TOLAYERS_ACTIVE:
				if ((idx_dst = CustomData_get_active_layer(cd_dst, data_type)) == -1) {
					if (!num_create) {
						return false;
					}
					data_dst = CustomData_add_layer(cd_dst, data_type, CD_CALLOC, NULL, num_create);
				}
				else {
					data_dst = CustomData_get_layer_n(cd_dst, data_type, idx_dst);
				}
				break;
			case MDT_TOLAYERS_INDEX:
				{
					int num = CustomData_number_of_layers(cd_dst, data_type);
					idx_dst = idx_src;
					if (num <= idx_dst) {
						if (!num_create) {
							return false;
						}
						/* Create as much data layers as necessary! */
						for (; num <= idx_dst; num++) {
							CustomData_add_layer(cd_dst, data_type, CD_CALLOC, NULL, num_create);
						}
					}
					data_dst = CustomData_get_layer_n(cd_dst, data_type, idx_dst);
				}
				break;
			case MDT_TOLAYERS_NAME:
				{
					const char *name = CustomData_get_layer_name(cd_src, data_type, idx_src);
					if ((idx_dst = CustomData_get_named_layer(cd_dst, data_type, name)) == -1) {
						if (!num_create) {
							return false;
						}
						CustomData_add_layer_named(cd_dst, data_type, CD_CALLOC, NULL, num_create, name);
						idx_dst = CustomData_get_named_layer(cd_dst, data_type, name);
					}
				}
				break;
			default:
				return false;
		}

		data_transfer_layersmapping_add_item_cd(r_map, data_type, data_src, data_dst);
	}
	else if (fromlayers_select == MDT_FROMLAYERS_ALL) {
		int num_src = CustomData_number_of_layers(cd_src, data_type);
		bool *use_layers_src = MEM_mallocN(sizeof(*use_layers_src) * (size_t)num_src, __func__);
		bool ret;

		memset(use_layers_src, true, sizeof(*use_layers_src) * num_src);

		ret = data_transfer_layersmapping_cdlayers_multisrc_to_dst(r_map, data_type, num_create, cd_src, cd_dst,
		                                                           tolayers_select, use_layers_src, num_src);

		MEM_freeN(use_layers_src);
		return ret;
	}
	else {
		return false;
	}

	return true;
}

static bool data_transfer_layersmapping_generate(
        ListBase *r_map, Object *ob_src, Object *ob_dst, DerivedMesh *dm_src, Mesh *me_dst,
        const int data_type, const int num_create, const int fromlayers_select, const int tolayers_select)
{
	CustomData *cd_src, *cd_dst;

	if (MDT_DATATYPE_IS_VERT(data_type)) {
		if (!(data_type & CD_FAKE)) {
			cd_src = dm_src->getVertDataLayout(dm_src);
			cd_dst = &me_dst->vdata;

			if (!CustomData_has_layer(cd_src, data_type)) {
				return false;
			}

			if (!ED_data_transfer_layersmapping_cdlayers(r_map, data_type, num_create, cd_src, cd_dst,
			                                             fromlayers_select, tolayers_select))
			{
				/* We handle specific source selection cases here. */
				return false;
			}
			return true;
		}
		else if (data_type == CD_FAKE_BWEIGHT) {
			const size_t elem_size = sizeof(*((MVert *)NULL));
			const size_t data_size = sizeof(((MVert *)NULL)->bweight);
			const size_t data_offset = offsetof(MVert, bweight);
			const uint64_t data_flag = 0;

			if (!(dm_src->cd_flag & ME_CDFLAG_VERT_BWEIGHT)) {
				return false;
			}
			me_dst->cd_flag |= ME_CDFLAG_VERT_BWEIGHT;
			data_transfer_layersmapping_add_item(r_map, data_type, dm_src->getVertArray(dm_src), me_dst->mvert,
			                                     dm_src->getNumVerts(dm_src), me_dst->totvert,
			                                     elem_size, data_size, data_offset, data_flag,
			                                     data_transfer_interp_char);
			return true;
		}
		else if (data_type == CD_FAKE_MDEFORMVERT) {
			cd_src = dm_src->getVertDataLayout(dm_src);
			cd_dst = &me_dst->vdata;

			return data_transfer_layersmapping_vgroups(r_map, num_create, ob_src, ob_dst, cd_src, cd_dst,
			                                           fromlayers_select, tolayers_select);
		}
		else if (data_type == CD_FAKE_SHAPEKEY) {
			/* TODO: leaving shapekeys asside for now, quite specific case, since we can't access them from MVert :/ */
			return false;
		}
	}
	else if (MDT_DATATYPE_IS_EDGE(data_type)) {
		if (!(data_type & CD_FAKE)) {  /* Unused for edges, currently... */
			cd_src = dm_src->getEdgeDataLayout(dm_src);
			cd_dst = &me_dst->edata;

			if (!CustomData_has_layer(cd_src, data_type)) {
				return false;
			}

			if (!ED_data_transfer_layersmapping_cdlayers(r_map, data_type, num_create, cd_src, cd_dst,
			                                             fromlayers_select, tolayers_select))
			{
				/* We handle specific source selection cases here. */
				return false;
			}
		}
		else if (data_type == CD_FAKE_CREASE) {
			const size_t elem_size = sizeof(*((MEdge *)NULL));
			const size_t data_size = sizeof(((MEdge *)NULL)->crease);
			const size_t data_offset = offsetof(MEdge, crease);
			const uint64_t data_flag = 0;

			if (!(dm_src->cd_flag & ME_CDFLAG_EDGE_CREASE)) {
				return false;
			}
			me_dst->cd_flag |= ME_CDFLAG_EDGE_CREASE;
			data_transfer_layersmapping_add_item(r_map, data_type, dm_src->getEdgeArray(dm_src), me_dst->medge,
			                                     dm_src->getNumEdges(dm_src), me_dst->totedge,
			                                     elem_size, data_size, data_offset, data_flag,
			                                     data_transfer_interp_char);
			return true;
		}
		else if (data_type == CD_FAKE_BWEIGHT) {
			const size_t elem_size = sizeof(*((MEdge *)NULL));
			const size_t data_size = sizeof(((MEdge *)NULL)->bweight);
			const size_t data_offset = offsetof(MEdge, bweight);
			const uint64_t data_flag = 0;

			if (!(dm_src->cd_flag & ME_CDFLAG_EDGE_BWEIGHT)) {
				return false;
			}
			me_dst->cd_flag |= ME_CDFLAG_EDGE_BWEIGHT;
			data_transfer_layersmapping_add_item(r_map, data_type, dm_src->getEdgeArray(dm_src), me_dst->medge,
			                                     dm_src->getNumEdges(dm_src), me_dst->totedge,
			                                     elem_size, data_size, data_offset, data_flag,
			                                     data_transfer_interp_char);
			return true;
		}
		else if (ELEM(data_type, CD_FAKE_SHARP, CD_FAKE_SEAM)) {
			const size_t elem_size = sizeof(*((MEdge *)NULL));
			const size_t data_size = sizeof(((MEdge *)NULL)->flag);
			const size_t data_offset = offsetof(MEdge, flag);
			const uint64_t data_flag = (data_type == CD_FAKE_SHARP) ? ME_SHARP : ME_SEAM;
			data_transfer_layersmapping_add_item(r_map, data_type, dm_src->getEdgeArray(dm_src), me_dst->medge,
			                                     dm_src->getNumEdges(dm_src), me_dst->totedge,
			                                     elem_size, data_size, data_offset, data_flag, NULL);
			return true;
		}
		else {
			return false;
		}
	}

	return false;
}

bool ED_data_transfer(
        Scene *scene, Object *ob_src, Object *ob_dst, const int data_type, const bool use_create,
        const int map_vert_mode, const int map_edge_mode, const int UNUSED(map_poly_mode), const int UNUSED(map_loop_mode),
        SpaceTransform *space_transform, const float max_distance,
        const int UNUSED(replace_mode), const float UNUSED(replace_threshold),
        const int fromlayers_select, const int tolayers_select)
{
	DerivedMesh *dm_src;
	Mesh *me_dst;
	CustomDataMask dm_src_mask = CD_MASK_BAREMESH;

	Mesh2MeshMapping geom_map = {0};
	ListBase lay_map = {0};
	bool changed = false;

	BLI_assert((ob_src != ob_dst) && (ob_src->type == OB_MESH) && (ob_dst->type == OB_MESH));

	/* Get meshes.*/
	if (!(data_type & CD_FAKE)) {
		dm_src_mask |= (1LL << data_type);
	}
	else if (data_type == CD_FAKE_MDEFORMVERT) {
		dm_src_mask |= (1LL << CD_MDEFORMVERT);  /* Exception for vgroups :/ */
	}
	dm_src = mesh_get_derived_final(scene, ob_src, dm_src_mask);
	me_dst = ob_dst->data;

	if (MDT_DATATYPE_IS_VERT(data_type)) {
		const int num_create = use_create ? me_dst->totvert : 0;

		BKE_dm2mesh_mapping_verts_compute(map_vert_mode, space_transform, max_distance,
		                                  me_dst->mvert, me_dst->totvert, dm_src, &geom_map);

		/* TODO add further filtering of mapping data here! */

		if (data_transfer_layersmapping_generate(&lay_map, ob_src, ob_dst, dm_src, me_dst, data_type, num_create,
		                                         fromlayers_select, tolayers_select))
		{
			DataTransferLayerMapping *lay_mapit;

			changed = (lay_map.first != NULL);

			for (lay_mapit = lay_map.first; lay_mapit; lay_mapit = lay_mapit->next) {
				CustomData_data_transfer(&geom_map, lay_mapit);
			}

			BLI_freelistN(&lay_map);
		}
	}
	else if (MDT_DATATYPE_IS_EDGE(data_type)) {
		const int num_create = use_create ? me_dst->totedge : 0;

		BKE_dm2mesh_mapping_edges_compute(map_edge_mode, space_transform, max_distance,
		                                  me_dst->mvert, me_dst->totvert, me_dst->medge, me_dst->totedge,
		                                  dm_src, &geom_map);

		/* TODO add further filtering of mapping data here! */

		if (data_transfer_layersmapping_generate(&lay_map, ob_src, ob_dst, dm_src, me_dst, data_type, num_create,
		                                         fromlayers_select, tolayers_select))
		{
			DataTransferLayerMapping *lay_mapit;

			changed = (lay_map.first != NULL);

			for (lay_mapit = lay_map.first; lay_mapit; lay_mapit = lay_mapit->next) {
				CustomData_data_transfer(&geom_map, lay_mapit);
			}

			BLI_freelistN(&lay_map);
		}
	}

	BKE_mesh2mesh_mapping_free(&geom_map);
	return changed;
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
	const int map_poly_mode = RNA_enum_get(op->ptr, "poly_mapping");
	const int map_loop_mode = RNA_enum_get(op->ptr, "loop_mapping");

	const bool use_object_transform = RNA_boolean_get(op->ptr, "use_object_transform");
	const bool use_max_distance = RNA_boolean_get(op->ptr, "use_max_distance");
	const float max_distance = use_max_distance ? RNA_float_get(op->ptr, "max_distance") : FLT_MAX;

	const int replace_mode = RNA_enum_get(op->ptr, "replace_mode");
	const float replace_threshold = RNA_float_get(op->ptr, "replace_threshold");

	const int fromlayers_select = RNA_enum_get(op->ptr, "fromlayers_select");
	const int tolayers_select = RNA_enum_get(op->ptr, "tolayers_select");

	SpaceTransform space_transform_data;
	SpaceTransform *space_transform = use_object_transform ? &space_transform_data : NULL;

	/* Macro to loop through selected objects and perform operation depending on function, option and method.*/
	CTX_DATA_BEGIN (C, Object *, ob_dst, selected_editable_objects)
	{
		if ((ob_dst == ob_src) || (ob_dst->type != OB_MESH)) {
			continue;
		}

		if (space_transform) {
			BLI_SPACE_TRANSFORM_SETUP(space_transform, ob_src, ob_dst);
		}

		if (ED_data_transfer(scene, ob_src, ob_dst, data_type, use_create,
		                     map_vert_mode, map_edge_mode, map_poly_mode, map_loop_mode,
		                     space_transform, max_distance, replace_mode, replace_threshold,
		                     fromlayers_select, tolayers_select))
		{
			changed = true;
		}
	}
	CTX_DATA_END;

	return OPERATOR_FINISHED;
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
	const int replace_mode = RNA_enum_get(ptr, "replace_mode");

	if (STREQ(prop_id, "max_distance") && !use_max_distance) {
		return false;
	}

	if (STREQ(prop_id, "vert_mapping") && !MDT_DATATYPE_IS_VERT(data_type)) {
		return false;
	}
	if (STREQ(prop_id, "edge_mapping") && !MDT_DATATYPE_IS_EDGE(data_type)) {
		return false;
	}
	if (STREQ(prop_id, "poly_mapping") && !MDT_DATATYPE_IS_POLY(data_type)) {
		return false;
	}
	if (STREQ(prop_id, "loop_mapping") && !MDT_DATATYPE_IS_LOOP(data_type)) {
		return false;
	}

	if (STREQ(prop_id, "replace_threshold") && (replace_mode != MDT_REPLACE_THRESHOLD)) {
		return false;
	}

	if ((STREQ(prop_id, "fromlayers_select") || STREQ(prop_id, "tolayers_select")) &&
	    !MDT_DATATYPE_IS_MULTILAYERS(data_type))
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
	ot->prop = RNA_def_enum(ot->srna, "data_type", MDT_layer_items, 0, "Data Type", "Which data to transfer");
	RNA_def_boolean(ot->srna, "use_create", true, "Create Data", "Add data layers on destination meshes if needed");

	/* Mapping methods. */
	RNA_def_enum(ot->srna, "vert_mapping", MDT_method_vertex_items, M2MMAP_MODE_TOPOLOGY, "Vertex Mapping",
	             "Method used to map source vertices to destination ones");
	RNA_def_enum(ot->srna, "edge_mapping", MDT_method_edge_items, M2MMAP_MODE_TOPOLOGY, "Edge Mapping",
	             "Method used to map source edges to destination ones");
	RNA_def_enum(ot->srna, "poly_mapping", MDT_method_poly_items, M2MMAP_MODE_TOPOLOGY, "Face Mapping",
	             "Method used to map source faces to destination ones");
	RNA_def_enum(ot->srna, "loop_mapping", MDT_method_loop_items, M2MMAP_MODE_TOPOLOGY, "Face Corner Mapping",
	             "Method used to map source faces' corners to destination ones");

	/* Mapping options and filtering. */
	RNA_def_boolean(ot->srna, "use_object_transform", true, "Object Transform",
	                "Evaluate source and destination meshes in their respective object spaces");
	RNA_def_boolean(ot->srna, "use_max_distance", false, "Only Neighbor Geometry",
	                "Source elements must be closer than given distance from destination one");
	prop = RNA_def_float(ot->srna, "max_distance", 1.0f, 0.0f, FLT_MAX, "Max Distance",
	                     "Maximum allowed distance between source and destination element, for non-topology mappings",
	                     0.0f, 100.0f);
	RNA_def_property_subtype(prop, PROP_DISTANCE);
	RNA_def_enum(ot->srna, "replace_mode", MDT_replace_mode_items, MDT_REPLACE, "Replace Mode",
	             "How to decide which destination elements to replace with source values, and which to leave as is");
	prop = RNA_def_float(ot->srna, "replace_threshold", 0.1f, 0.0f, 1.0f, "Replace Threshold",
	                     "Threshold to filter out destination data (exact behavior depends on data type)",
	                     0.0f, 1.0f);

	/* How to handle multi-layers types of data. */
	prop = RNA_def_enum(ot->srna, "fromlayers_select", MDT_fromlayers_select_items, MDT_FROMLAYERS_ACTIVE,
	                    "Source Layers Selection", "Which layers to transfer, in case of multi-layers types");
	RNA_def_property_enum_funcs_runtime(prop, NULL, NULL, mdt_fromlayers_select_itemf);

	prop = RNA_def_enum(ot->srna, "tolayers_select", MDT_tolayers_select_items, MDT_TOLAYERS_ACTIVE,
	                    "Destination Layers Matching", "How to match source and destination layers");
	RNA_def_property_enum_funcs_runtime(prop, NULL, NULL, mdt_tolayers_select_itemf);

	/* TODO: add some advanced mixing features too, for some layers like weights or vcol? */
}