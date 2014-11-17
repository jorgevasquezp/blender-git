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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/mesh_remap.c
 *  \ingroup bke
 *
 * Functions for mapping data between meshes.
 */

#include <limits.h>

#include "MEM_guardedalloc.h"

#include "DNA_meshdata_types.h"

#include "BLI_utildefines.h"
#include "BLI_bitmap.h"
#include "BLI_math.h"
#include "BLI_memarena.h"
#include "BLI_polyfill2d.h"
#include "BLI_rand.h"

#include "BKE_bvhutils.h"
#include "BKE_customdata.h"
#include "BKE_DerivedMesh.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_remap.h"  /* own include */


void BKE_loop_islands_init(MeshIslands *islands, const short item_type, const int num_items, const short island_type)
{
	MemArena *mem = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, __func__);

	BLI_assert(ELEM(item_type, MISLAND_TYPE_VERT, MISLAND_TYPE_EDGE, MISLAND_TYPE_POLY, MISLAND_TYPE_LOOP));
	BLI_assert(ELEM(island_type, MISLAND_TYPE_VERT, MISLAND_TYPE_EDGE, MISLAND_TYPE_POLY, MISLAND_TYPE_LOOP));

	BKE_loop_islands_free(islands);

	islands->item_type = item_type;
	islands->nbr_items = num_items;
	islands->items_to_islands_idx = BLI_memarena_alloc(mem, sizeof(*islands->items_to_islands_idx) * (size_t)num_items);

	islands->island_type = island_type;
	islands->allocated_islands = 64;
	islands->islands = BLI_memarena_alloc(mem, sizeof(*islands->islands) * islands->allocated_islands);

	islands->mem = mem;
}

void BKE_loop_islands_free(MeshIslands *islands)
{
	MemArena *mem = islands->mem;

	if (mem) {
		BLI_memarena_free(mem);
	}

	islands->item_type = 0;
	islands->nbr_items = 0;
	islands->items_to_islands_idx = NULL;

	islands->island_type = 0;
	islands->nbr_islands = 0;
	islands->islands = NULL;

	islands->mem = NULL;
	islands->allocated_islands = 0;
}

void BKE_loop_islands_add_island(MeshIslands *islands, const int num_items, int *items_indices,
                                 const int num_island_items, int *island_item_indices)
{
	MemArena *mem = islands->mem;

	MeshElemMap *isld;
	const int curr_island_idx = islands->nbr_islands++;
	const size_t curr_num_islands = (size_t)islands->nbr_islands;
	int i = num_items;

	islands->nbr_items = num_items;
	while (i--) {
		islands->items_to_islands_idx[items_indices[i]] = curr_island_idx;
	}

	if (UNLIKELY(curr_num_islands > islands->allocated_islands)) {
		MeshElemMap **islds;

		islands->allocated_islands *= 2;
		islds = BLI_memarena_alloc(mem, sizeof(*islds) * islands->allocated_islands);
		memcpy(islds, islands->islands, sizeof(*islds) * (curr_num_islands - 1));
		islands->islands = islds;
	}

	islands->islands[curr_island_idx] = isld = BLI_memarena_alloc(mem, sizeof(*isld));

	isld->count = num_island_items;
	isld->indices = BLI_memarena_alloc(mem, sizeof(*isld->indices) * (size_t)num_island_items);
	memcpy(isld->indices, island_item_indices, sizeof(*isld->indices) * (size_t)num_island_items);
}

/* TODO: I'm not sure edge seam flag is enough to define UV islands? Maybe we should also consider UVmaps values
 *       themselves (i.e. different UV-edges for a same mesh-edge => boundary edge too?).
 *       Would make things much more complex though, and each UVMap would then need its own mesh mapping,
 *       not sure we want that at all!
 */
static bool bke_check_island_boundary_uv(const MPoly *UNUSED(mp), const MLoop *UNUSED(ml), const MEdge *me,
                                         const int UNUSED(nbr_egde_users))
{
	/* Edge is UV boundary if tagged as seam. */
	return (me->flag & ME_SEAM) != 0;
}

/* Note: all this could be optimized... Not sure it would be worth the more complex code, though, those loops
 *       are supposed to be really quick to do... */
bool BKE_loop_poly_island_compute_uv(struct DerivedMesh *dm, MeshIslands *r_islands)
{
	MEdge *edges = dm->getEdgeArray(dm);
	MPoly *polys = dm->getPolyArray(dm);
	MLoop *loops = dm->getLoopArray(dm);
	const int num_edges = dm->getNumEdges(dm);
	const int num_polys = dm->getNumPolys(dm);
	const int num_loops = dm->getNumLoops(dm);

	int *poly_groups = NULL;
	int num_poly_groups;

	int *poly_indices = MEM_mallocN(sizeof(*poly_indices) * (size_t)num_polys, __func__);
	int *loop_indices = MEM_mallocN(sizeof(*loop_indices) * (size_t)num_loops, __func__);
	int num_pidx, num_lidx;

	int grp_idx, p_idx, pl_idx, l_idx;

	BKE_loop_islands_free(r_islands);
	BKE_loop_islands_init(r_islands, MISLAND_TYPE_LOOP, num_loops, MISLAND_TYPE_POLY);

	BKE_poly_loop_islands_compute(edges, num_edges, polys, num_polys, loops, num_loops, false,
	                              bke_check_island_boundary_uv, &poly_groups, &num_poly_groups);

	if (!num_poly_groups) {
		/* Should never happen... */
		return false;
	}

	/* Note: here we ignore '0' invalid group - this should *never* happen in this case anyway? */
	for (grp_idx = 1; grp_idx <= num_poly_groups; grp_idx++) {
		num_pidx = num_lidx = 0;

		for (p_idx = 0; p_idx < num_polys; p_idx++) {
			MPoly *mp;

			if (poly_groups[p_idx] != grp_idx) {
				continue;
			}

			mp = &polys[p_idx];
			poly_indices[num_pidx++] = p_idx;
			for (l_idx = mp->loopstart, pl_idx = 0; pl_idx < mp->totloop; l_idx++, pl_idx++) {
				loop_indices[num_lidx++] = l_idx;
			}
		}

		BKE_loop_islands_add_island(r_islands, num_lidx, loop_indices, num_pidx, poly_indices);
	}

	MEM_freeN(poly_indices);
	MEM_freeN(loop_indices);
	MEM_freeN(poly_groups);

	return true;
}

/** \} */

/* -------------------------------------------------------------------- */

/** \name Mesh to mesh mapping
 * \{ */

void BKE_mesh2mesh_mapping_init(Mesh2MeshMapping *map, const int num_items)
{
	MemArena *mem = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, __func__);

	BKE_mesh2mesh_mapping_free(map);

	map->items = BLI_memarena_alloc(mem, sizeof(*map->items) * (size_t)num_items);
	map->nbr_items = num_items;

	map->mem = mem;
}

void BKE_mesh2mesh_mapping_free(Mesh2MeshMapping *map)
{
	if (map->mem) {
		BLI_memarena_free((MemArena *)map->mem);
	}

	map->nbr_items = 0;
	map->items = NULL;
	map->mem = NULL;
}

static void bke_mesh2mesh_mapping_item_define(
        Mesh2MeshMapping *map, const int idx, const float hit_distance, const int island,
        const int nbr_sources, const int *indices_src, const float *weights_src)
{
	Mesh2MeshMappingItem *mapit = &map->items[idx];
	MemArena *mem = map->mem;

	if (nbr_sources) {
		mapit->nbr_sources = nbr_sources;
		mapit->indices_src = BLI_memarena_alloc(mem, sizeof(*mapit->indices_src) * (size_t)nbr_sources);
		memcpy(mapit->indices_src, indices_src, sizeof(*mapit->indices_src) * (size_t)nbr_sources);
		mapit->weights_src = BLI_memarena_alloc(mem, sizeof(*mapit->weights_src) * (size_t)nbr_sources);
		memcpy(mapit->weights_src, weights_src, sizeof(*mapit->weights_src) * (size_t)nbr_sources);
	}
	else {
		mapit->nbr_sources = 0;
		mapit->indices_src = NULL;
		mapit->weights_src = NULL;
	}
	mapit->hit_distance = hit_distance;
	mapit->island = island;
}

void BKE_mesh2mesh_mapping_item_define_invalid(Mesh2MeshMapping *map, const int idx)
{
	bke_mesh2mesh_mapping_item_define(map, idx, FLT_MAX, 0, 0, NULL, NULL);
}

static int bke_mesh2mesh_mapping_get_interp_poly_data(
        const MPoly *mp, MLoop *mloops, const float (*vcos_src)[3], const float point[3],
        size_t *buff_size, float (**vcos)[3], const bool use_loops, int **indices, float **weights,
        const bool do_weights, int *r_closest_idx)
{
	MLoop *ml;
	float (*vco)[3];
	float ref_dist_sq = FLT_MAX;
	int *idx;
	const int nbr_sources = mp->totloop;
	int i;

	if ((size_t)nbr_sources > *buff_size) {
		*buff_size = (size_t)nbr_sources;
		*vcos = MEM_reallocN(*vcos, sizeof(**vcos) * *buff_size);
		*indices = MEM_reallocN(*indices, sizeof(**indices) * *buff_size);
		if (do_weights) {
			*weights = MEM_reallocN(*weights, sizeof(**weights) * *buff_size);
		}
	}

	for (i = 0, ml = &mloops[mp->loopstart], vco = *vcos, idx = *indices; i < nbr_sources; i++, ml++, vco++, idx++) {
		*idx = use_loops ? (int)mp->loopstart + i : (int)ml->v;
		copy_v3_v3(*vco, vcos_src[ml->v]);
		if (r_closest_idx) {
			/* Find closest vert/loop in this case. */
			const float dist_sq = len_squared_v3v3(point, *vco);
			if (dist_sq < ref_dist_sq) {
				ref_dist_sq = dist_sq;
				*r_closest_idx = *idx;
			}
		}
	}

	if (do_weights) {
		interp_weights_poly_v3(*weights, *vcos, nbr_sources, point);
	}

	return nbr_sources;
}

static float bke_mesh2mesh_bvhtree_query_nearest(
        BVHTreeFromMesh *treedata, BVHTreeNearest *nearest, const SpaceTransform *space_transform,
        float co[3], const float max_dist_sq)
{
	/* Convert the vertex to tree coordinates, if needed. */
	if (space_transform) {
		BLI_space_transform_apply(space_transform, co);
	}

	/* Use local proximity heuristics (to reduce the nearest search). */
	if (nearest->index != -1) {
		nearest->dist_sq = min_ff(len_squared_v3v3(co, nearest->co), max_dist_sq);
	}
	else {
		nearest->dist_sq = max_dist_sq;
	}
	/* Compute and store result. If invalid (-1 idx), keep FLT_MAX dist. */
	BLI_bvhtree_find_nearest(treedata->tree, co, nearest, treedata->nearest_callback, treedata);
	return sqrtf(nearest->dist_sq);
}

static float bke_mesh2mesh_bvhtree_query_raycast(
        BVHTreeFromMesh *treedata, BVHTreeRayHit *rayhit, const SpaceTransform *space_transform,
        float co[3], float no[3], const float radius, const float max_dist)
{
	/* Convert the vertex to tree coordinates, if needed. */
	if (space_transform) {
		BLI_space_transform_apply(space_transform, co);
		BLI_space_transform_apply_normal(space_transform, no);
	}

	rayhit->index = -1;
	rayhit->dist = max_dist;
	BLI_bvhtree_ray_cast(treedata->tree, co, no, radius, rayhit, treedata->raycast_callback, treedata);

#if 0  /* Stupid in fact!? */
	/* If no ray hit along vertex normal, try in the other direction! */
	if (rayhit->index < 0 || (rayhit->dist * rayhit->dist) > max_dist_sq) {
		negate_v3(no);
		BLI_bvhtree_ray_cast(treedata->tree, co, no, radius, rayhit, treedata->raycast_callback, treedata);
	}
#endif

	return rayhit->dist;
}

/* Little helper when dealing with source islands */
typedef struct IslandResult {
	float factor;        /* A factor, based on which best island for a given set of elements will be selected. */
	int idx_src;         /* Index of the source. */
	float hit_distance;  /* The actual hit distance. */
	float hit_point[3];  /* The hit point, if relevant. */
} IslandResult;

/* Note about all bvh/raycasting stuff below:
 *      * We must use our ray radius as BVH epsilon too, else rays not hitting anything but 'passing near' an item
 *        would be missed (since BVH handling would not detect them, 'refining' callbacks won't be executed,
 *        even though they would return a valid hit).
 *      * However, in 'islands' case where each hit gets a weight, 'precise' hits should have a better weight than
 *        'approximate' hits. To address that, we simplify things with:
 *        ** A first raycast with default, given rayradius;
 *        ** If first one fails, we do more raycasting with bigger radius, but if hit is found
 *           it will get smaller weight.
 *        This only concerns loops, currently (because of islands), and 'sampled' edges/polys norproj.
 */

/* At most n raycasts per 'real' ray. */
#define M2MMAP_RAYCAST_APPROXIMATE_NR 3
/* Each approximated raycasts will have n times bigger radius than previous one. */
#define M2MMAP_RAYCAST_APPROXIMATE_FAC 5.0f
/* BVH epsilon value we have to give to bvh 'constructor' when doing approximated raycasting. */
#define M2MMAP_RAYCAST_APPROXIMATE_BVHEPSILON(_ray_radius) \
	((float)M2MMAP_RAYCAST_APPROXIMATE_NR * M2MMAP_RAYCAST_APPROXIMATE_FAC * (_ray_radius))

void BKE_dm2mesh_mapping_verts_compute(
        const int mode, const SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        const MVert *verts_dst, const int numverts_dst, const bool UNUSED(dirty_nors_dst), DerivedMesh *dm_src,
        Mesh2MeshMapping *r_map)
{
	const float full_weight = 1.0f;
	const float max_dist_sq = max_dist * max_dist;
	int i;

	BLI_assert(mode & M2MMAP_MODE_VERT);

	BKE_mesh2mesh_mapping_init(r_map, numverts_dst);

	if (mode == M2MMAP_MODE_TOPOLOGY) {
		BLI_assert(numverts_dst == dm_src->getNumVerts(dm_src));
		for (i = 0; i < numverts_dst; i++) {
			bke_mesh2mesh_mapping_item_define(r_map, i, FLT_MAX, 0, 1, &i, &full_weight);
		}
	}
	else {
		BVHTreeFromMesh treedata = {NULL};
		BVHTreeNearest nearest = {0};
		BVHTreeRayHit rayhit = {0};
		float hitdist;

		if (mode == M2MMAP_MODE_VERT_NEAREST) {
			bvhtree_from_mesh_verts(&treedata, dm_src, 0.0f, 2, 6);
			nearest.index = -1;

			for (i = 0; i < numverts_dst; i++) {
				float tmp_co[3];

				copy_v3_v3(tmp_co, verts_dst[i].co);

				hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
				                                              tmp_co, max_dist_sq);

				if (nearest.index >= 0) {
					bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 1, &nearest.index, &full_weight);
				}
				else {
					/* No source for this dest vertex! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}
		}
		else if (ELEM(mode, M2MMAP_MODE_VERT_EDGE_NEAREST, M2MMAP_MODE_VERT_EDGEINTERP_NEAREST)) {
			MEdge *edges_src = dm_src->getEdgeArray(dm_src);
			float (*vcos_src)[3] = MEM_mallocN(sizeof(*vcos_src) * (size_t)dm_src->getNumVerts(dm_src), __func__);
			dm_src->getVertCos(dm_src, vcos_src);

			bvhtree_from_mesh_edges(&treedata, dm_src, 0.0f, 2, 6);
			nearest.index = -1;

			for (i = 0; i < numverts_dst; i++) {
				float tmp_co[3];

				copy_v3_v3(tmp_co, verts_dst[i].co);

				hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
				                                              tmp_co, max_dist_sq);

				if (nearest.index >= 0) {
					MEdge *me = &edges_src[nearest.index];
					float (*v1cos)[3] = &vcos_src[me->v1];
					float (*v2cos)[3] = &vcos_src[me->v2];

					if (mode == M2MMAP_MODE_VERT_EDGE_NEAREST) {
						const float dist_v1 = len_squared_v3v3(tmp_co, *v1cos);
						const float dist_v2 = len_squared_v3v3(tmp_co, *v2cos);
						const int index = (int)((dist_v1 > dist_v2) ? me->v2 : me->v1);
						bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 1, &index, &full_weight);
					}
					else if (mode == M2MMAP_MODE_VERT_EDGEINTERP_NEAREST) {
						int indices[2];
						float weights[2];

						indices[0] = (int)me->v1;
						indices[1] = (int)me->v2;

						/* Weight is inverse of point factor here... */
						weights[0] = line_point_factor_v3(tmp_co, *v2cos, *v1cos);
						CLAMP(weights[0], 0.0f, 1.0f);
						weights[1] = 1.0f - weights[0];

						bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 2, indices, weights);
					}
				}
				else {
					/* No source for this dest vertex! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}

			MEM_freeN(vcos_src);
		}
		else if (ELEM(mode, M2MMAP_MODE_VERT_POLY_NEAREST, M2MMAP_MODE_VERT_POLYINTERP_NEAREST,
		                    M2MMAP_MODE_VERT_POLYINTERP_VNORPROJ))
		{
			MPoly *polys_src = dm_src->getPolyArray(dm_src);
			MLoop *loops_src = dm_src->getLoopArray(dm_src);
			float (*vcos_src)[3] = MEM_mallocN(sizeof(*vcos_src) * (size_t)dm_src->getNumVerts(dm_src), __func__);
			int *orig_poly_idx_src;

			size_t tmp_buff_size = 32;  /* Will be enough in 99% of cases. */
			float (*vcos)[3] = MEM_mallocN(sizeof(*vcos) * tmp_buff_size, __func__);
			int *indices = MEM_mallocN(sizeof(*indices) * tmp_buff_size, __func__);
			float *weights = MEM_mallocN(sizeof(*weights) * tmp_buff_size, __func__);

			dm_src->getVertCos(dm_src, vcos_src);
			bvhtree_from_mesh_faces(&treedata, dm_src, (mode & M2MMAP_USE_NORPROJ) ? ray_radius : 0.0f, 2, 6);
			/* bvhtree here uses tesselated faces... */
			orig_poly_idx_src = dm_src->getTessFaceDataArray(dm_src, CD_ORIGINDEX);

			if (mode == M2MMAP_MODE_VERT_POLYINTERP_VNORPROJ) {
				for (i = 0; i < numverts_dst; i++) {
					float tmp_co[3], tmp_no[3];

					copy_v3_v3(tmp_co, verts_dst[i].co);
					normal_short_to_float_v3(tmp_no, verts_dst[i].no);

					hitdist = bke_mesh2mesh_bvhtree_query_raycast(&treedata, &rayhit, space_transform,
					                                              tmp_co, tmp_no, ray_radius, max_dist);

					if (rayhit.index >= 0 && hitdist <= max_dist) {
						MPoly *mp_src = &polys_src[orig_poly_idx_src[rayhit.index]];
						const int nbr_sources = bke_mesh2mesh_mapping_get_interp_poly_data(
						                                mp_src, loops_src, (const float (*)[3])vcos_src, rayhit.co,
						                                &tmp_buff_size, &vcos, false, &indices, &weights, true, NULL);

						bke_mesh2mesh_mapping_item_define(r_map, i, rayhit.dist, 0, nbr_sources, indices, weights);
					}
					else {
						/* No source for this dest vertex! */
						BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
					}
				}
			}
			else {
				nearest.index = -1;

				for (i = 0; i < numverts_dst; i++) {
					float tmp_co[3];

					/* Convert the vertex to tree coordinates. */
					copy_v3_v3(tmp_co, verts_dst[i].co);

					hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
					                                              tmp_co, max_dist_sq);

					if (nearest.index >= 0) {
						MPoly *mp = &polys_src[orig_poly_idx_src[nearest.index]];

						if (mode == M2MMAP_MODE_VERT_POLY_NEAREST) {
							int index;
							bke_mesh2mesh_mapping_get_interp_poly_data(
							                                mp, loops_src, (const float (*)[3])vcos_src, nearest.co,
							                                &tmp_buff_size, &vcos, false, &indices, &weights, false,
							                                &index);

							bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 1, &index, &full_weight);
						}
						else if (mode == M2MMAP_MODE_VERT_POLYINTERP_NEAREST) {
							const int nbr_sources = bke_mesh2mesh_mapping_get_interp_poly_data(
							                                mp, loops_src, (const float (*)[3])vcos_src, nearest.co,
							                                &tmp_buff_size, &vcos, false, &indices, &weights, true,
							                                NULL);

							bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, nbr_sources, indices, weights);
						}
					}
					else {
						/* No source for this dest vertex! */
						BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
					}
				}
			}

			MEM_freeN(vcos_src);
			MEM_freeN(vcos);
			MEM_freeN(indices);
			MEM_freeN(weights);
		}
		else {
			printf("WARNING! Unsupported mesh-to-mesh vertex mapping mode (%d)!\n", mode);
			memset(r_map->items, 0, sizeof(*r_map->items) * (size_t)numverts_dst);
		}

		free_bvhtree_from_mesh(&treedata);
	}
}

void BKE_dm2mesh_mapping_edges_compute(
        const int mode, const SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        const MVert *verts_dst, const int numverts_dst, const MEdge *edges_dst, const int numedges_dst,
        const bool UNUSED(dirty_nors_dst), DerivedMesh *dm_src, Mesh2MeshMapping *r_map)
{
	const float full_weight = 1.0f;
	const float max_dist_sq = max_dist * max_dist;
	int i;

	BLI_assert(mode & M2MMAP_MODE_EDGE);

	BKE_mesh2mesh_mapping_init(r_map, numedges_dst);

	if (mode == M2MMAP_MODE_TOPOLOGY) {
		BLI_assert(numedges_dst == dm_src->getNumEdges(dm_src));
		for (i = 0; i < numedges_dst; i++) {
			bke_mesh2mesh_mapping_item_define(r_map, i, FLT_MAX, 0, 1, &i, &full_weight);
		}
	}
	else {
		BVHTreeFromMesh treedata = {NULL};
		BVHTreeNearest nearest = {0};
		BVHTreeRayHit rayhit = {0};
		float hitdist;

		if (mode == M2MMAP_MODE_EDGE_VERT_NEAREST) {
			const int numverts_src = dm_src->getNumVerts(dm_src);
			const int numedges_src = dm_src->getNumEdges(dm_src);
			MEdge *edges_src = dm_src->getEdgeArray(dm_src);
			float (*vcos_src)[3] = MEM_mallocN(sizeof(*vcos_src) * (size_t)dm_src->getNumVerts(dm_src), __func__);

			MeshElemMap *vert2edge_src_map;
			int *vert2edge_src_map_mem;

			/* Note we store an integer index in second element (first one is hitdist). */
			float (*v_dst2src_map)[2] = MEM_mallocN(sizeof(*v_dst2src_map) * (size_t)numverts_dst, __func__);
			fill_vn_fl((float *)v_dst2src_map, numverts_dst * 2, -1.0f);

			BKE_mesh_vert_edge_map_create(&vert2edge_src_map, &vert2edge_src_map_mem,
			                              edges_src, numverts_src, numedges_src);

			dm_src->getVertCos(dm_src, vcos_src);

			bvhtree_from_mesh_verts(&treedata, dm_src, 0.0f, 2, 6);
			nearest.index = -1;

			for (i = 0; i < numedges_dst; i++) {
				const MEdge *e_dst = &edges_dst[i];
				float best_totdist = FLT_MAX;
				int best_eidx_src = -1;
				int j = 2;

				while (j--) {
					const unsigned int vidx_dst = j ? e_dst->v1 : e_dst->v2;

					/* Compute closest verts only once! */
					if (v_dst2src_map[vidx_dst][0] < 0.0f) {
						float tmp_co[3];

						copy_v3_v3(tmp_co, verts_dst[vidx_dst].co);
						hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
						                                              tmp_co, max_dist_sq);

						if (nearest.index >= 0) {
							v_dst2src_map[vidx_dst][0] = hitdist;
							v_dst2src_map[vidx_dst][1] = (float)nearest.index;
						}
						else {
							/* No source for this dest vert! */
							v_dst2src_map[vidx_dst][0] = FLT_MAX;
						}
					}
				}

				/* Now, check all source edges of closest sources vertices, and select the one giving the smallest
				 * total verts-to-verts distance. */
				for (j = 2; j--;) {
					const unsigned int vidx_dst = j ? e_dst->v1 : e_dst->v2;
					const float first_dist = v_dst2src_map[vidx_dst][0];
					const int vidx_src = (int)v_dst2src_map[vidx_dst][1];
					int *eidx_src, k;

					if (vidx_src < 0) {
						continue;
					}

					eidx_src = vert2edge_src_map[vidx_src].indices;
					k = vert2edge_src_map[vidx_src].count;

					for (; k--; eidx_src++) {
						MEdge *e_src = &edges_src[*eidx_src];
						const float *other_co_src = vcos_src[BKE_mesh_edge_other_vert(e_src, vidx_src)];
						const float *other_co_dst = verts_dst[BKE_mesh_edge_other_vert(e_dst, (int)vidx_dst)].co;
						const float totdist = first_dist + len_v3v3(other_co_src, other_co_dst);

						if (totdist < best_totdist) {
							best_totdist = totdist;
							best_eidx_src = *eidx_src;
						}
					}
				}

				if (best_eidx_src >= 0) {
					const float *co1_src = vcos_src[edges_src[best_eidx_src].v1];
					const float *co2_src = vcos_src[edges_src[best_eidx_src].v2];
					const float *co1_dst = verts_dst[e_dst->v1].co;
					const float *co2_dst = verts_dst[e_dst->v2].co;
					float co_src[3], co_dst[3];

					/* TODO: would need an isect_seg_seg_v3(), actually! */
					const int isect_type = isect_line_line_v3(co1_src, co2_src, co1_dst, co2_dst, co_src, co_dst);
					if (isect_type != 0) {
						const float fac_src = line_point_factor_v3(co_src, co1_src, co2_src);
						const float fac_dst = line_point_factor_v3(co_dst, co1_dst, co2_dst);
						if (fac_src < 0.0f) {
							copy_v3_v3(co_src, co1_src);
						}
						else if (fac_src > 1.0f) {
							copy_v3_v3(co_src, co2_src);
						}
						if (fac_dst < 0.0f) {
							copy_v3_v3(co_dst, co1_dst);
						}
						else if (fac_dst > 1.0f) {
							copy_v3_v3(co_dst, co2_dst);
						}
					}
					hitdist = len_v3v3(co_dst, co_src);
					bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 1, &best_eidx_src, &full_weight);
				}
				else {
					/* No source for this dest edge! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}

			MEM_freeN(vcos_src);
			MEM_freeN(v_dst2src_map);
			MEM_freeN(vert2edge_src_map);
			MEM_freeN(vert2edge_src_map_mem);
		}
		else if (mode == M2MMAP_MODE_EDGE_NEAREST) {
			bvhtree_from_mesh_edges(&treedata, dm_src, 0.0f, 2, 6);
			nearest.index = -1;

			for (i = 0; i < numedges_dst; i++) {
				float tmp_co[3];

				interp_v3_v3v3(tmp_co, verts_dst[edges_dst[i].v1].co, verts_dst[edges_dst[i].v2].co, 0.5f);

				hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
				                                              tmp_co, max_dist_sq);

				if (nearest.index >= 0) {
					bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 1, &nearest.index, &full_weight);
				}
				else {
					/* No source for this dest edge! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}
		}
		else if (mode == M2MMAP_MODE_EDGE_POLY_NEAREST) {
			MEdge *edges_src = dm_src->getEdgeArray(dm_src);
			MPoly *polys_src = dm_src->getPolyArray(dm_src);
			MLoop *loops_src = dm_src->getLoopArray(dm_src);
			float (*vcos_src)[3] = MEM_mallocN(sizeof(*vcos_src) * (size_t)dm_src->getNumVerts(dm_src), __func__);
			int *orig_poly_idx_src;

			dm_src->getVertCos(dm_src, vcos_src);
			bvhtree_from_mesh_faces(&treedata, dm_src, 0.0f, 2, 6);
			/* bvhtree here uses tesselated faces... */
			orig_poly_idx_src = dm_src->getTessFaceDataArray(dm_src, CD_ORIGINDEX);

			for (i = 0; i < numedges_dst; i++) {
				float tmp_co[3];

				interp_v3_v3v3(tmp_co, verts_dst[edges_dst[i].v1].co, verts_dst[edges_dst[i].v2].co, 0.5f);

				hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
				                                              tmp_co, max_dist_sq);

				if (nearest.index >= 0) {
					MPoly *mp_src = &polys_src[orig_poly_idx_src[nearest.index]];
					MLoop *ml_src = &loops_src[mp_src->loopstart];
					int nloops = mp_src->totloop;
					float best_dist_sq = FLT_MAX;
					int best_eidx_src = -1;

					for (; nloops--; ml_src++) {
						MEdge *me_src = &edges_src[ml_src->e];
						float *co1_src = vcos_src[me_src->v1];
						float *co2_src = vcos_src[me_src->v2];
						float co_src[3];
						float dist_sq;

						interp_v3_v3v3(co_src, co1_src, co2_src, 0.5f);
						dist_sq = len_squared_v3v3(tmp_co, co_src);
						if (dist_sq < best_dist_sq) {
							best_dist_sq = dist_sq;
							best_eidx_src = (int)ml_src->e;
						}
					}
					if (best_eidx_src >= 0) {
						bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0, 1, &best_eidx_src, &full_weight);
					}
				}
				else {
					/* No source for this dest edge! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}

			MEM_freeN(vcos_src);
		}
		else if (mode == M2MMAP_MODE_EDGE_EDGEINTERP_VNORPROJ) {
			const int num_rays_min = 5, num_rays_max = 100;
			const int numedges_src = dm_src->getNumEdges(dm_src);

			/* Subtleness - this one we can allocate only max number of cast rays per edges! */
			int *indices = MEM_mallocN(sizeof(*indices) * (size_t)min_ii(numedges_src, num_rays_max), __func__);
			/* Here it's simpler to just allocate for all edges :/ */
			float *weights = MEM_mallocN(sizeof(*weights) * (size_t)numedges_src, __func__);

			bvhtree_from_mesh_edges(&treedata, dm_src, M2MMAP_RAYCAST_APPROXIMATE_BVHEPSILON(ray_radius), 2, 6);

			for (i = 0; i < numedges_dst; i++) {
				/* For each dst edge, we sample some rays from it (interpolated from its vertices)
				 * and use their hits to interpolate from source edges. */
				const MEdge *me = &edges_dst[i];
				float tmp_co[3], v1_co[3], v2_co[3];
				float tmp_no[3], v1_no[3], v2_no[3];

				int grid_size;
				float edge_dst_len;
				float grid_step;

				float totweights = 0.0f;
				float hitdist_accum = 0.0f;
				int nbr_sources = 0;
				int j;

				copy_v3_v3(v1_co, verts_dst[me->v1].co);
				copy_v3_v3(v2_co, verts_dst[me->v2].co);

				normal_short_to_float_v3(v1_no, verts_dst[me->v1].no);
				normal_short_to_float_v3(v2_no, verts_dst[me->v2].no);

				/* We do our transform here, allows to interpolate from normals already in src space. */
				if (space_transform) {
					BLI_space_transform_apply(space_transform, v1_co);
					BLI_space_transform_apply(space_transform, v2_co);
					BLI_space_transform_apply_normal(space_transform, v1_no);
					BLI_space_transform_apply_normal(space_transform, v2_no);
				}

				fill_vn_fl(weights, (int)numedges_src, 0.0f);

				/* We adjust our ray-casting grid to ray_radius (the smaller, the more rays are cast),
				 * with lower/upper bounds. */
				edge_dst_len = len_v3v3(v1_co, v2_co);

				grid_size = (int)((edge_dst_len / ray_radius) + 0.5f);
				CLAMP(grid_size, num_rays_min, num_rays_max);  /* min 5 rays/edge, max 100. */

				grid_step = 1.0f / (float)grid_size;  /* Not actual distance here, rather an interp fac... */

				/* And now we can cast all our rays, and see what we get! */
				for (j = 0; j < grid_size; j++) {
					const float fac = grid_step * (float)j;

					int n = (ray_radius > 0.0f) ? M2MMAP_RAYCAST_APPROXIMATE_NR : 1;
					float w = 1.0f;

					interp_v3_v3v3(tmp_co, v1_co, v2_co, fac);
					interp_v3_v3v3_slerp_safe(tmp_no, v1_no, v2_no, fac);

					while (n--) {
						/* Note we handle dest to src space conversion ourself, here! */
						hitdist = bke_mesh2mesh_bvhtree_query_raycast(&treedata, &rayhit, NULL,
						                                              tmp_co, tmp_no, ray_radius / w, max_dist);

						if (rayhit.index >= 0 && hitdist <= max_dist) {
							weights[rayhit.index] += w;
							totweights += w;
							hitdist_accum += hitdist;
							break;
						}
						/* Next iteration will get bigger radius but smaller weight! */
						w /= M2MMAP_RAYCAST_APPROXIMATE_FAC;
					}
				}
				/* A sampling is valid (as in, its result can be considered as valid sources) only if at least
				 * half of the rays found a source! */
				if (totweights > ((float)grid_size / 2.0f)) {
					for (j = 0; j < (int)numedges_src; j++) {
						if (!weights[j]) {
							continue;
						}
						/* Note: nbr_sources is always <= j! */
						weights[nbr_sources] = weights[j] / totweights;
						indices[nbr_sources] = j;
						nbr_sources++;
					}
					bke_mesh2mesh_mapping_item_define(r_map, i, hitdist_accum / totweights, 0,
					                                  nbr_sources, indices, weights);
				}
				else {
					/* No source for this dest edge! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}

			MEM_freeN(indices);
			MEM_freeN(weights);
		}
		else {
			printf("WARNING! Unsupported mesh-to-mesh edge mapping mode (%d)!\n", mode);
			memset(r_map->items, 0, sizeof(*r_map->items) * (size_t)numedges_dst);
		}

		free_bvhtree_from_mesh(&treedata);
	}
}

void BKE_dm2mesh_mapping_loops_compute(
        const int mode, const SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        MVert *verts_dst, const int numverts_dst, MEdge *edges_dst, const int numedges_dst,
        MLoop *loops_dst, const int numloops_dst, MPoly *polys_dst, const int numpolys_dst,
        CustomData *ldata_dst, CustomData *pdata_dst, const float split_angle_dst, const bool dirty_nors_dst,
        DerivedMesh *dm_src, loop_island_compute gen_islands_src, Mesh2MeshMapping *r_map)
{
	const float full_weight = 1.0f;
	const float max_dist_sq = max_dist * max_dist;

	int i;

	BLI_assert(mode & M2MMAP_MODE_LOOP);

	BKE_mesh2mesh_mapping_init(r_map, numloops_dst);

	if (mode == M2MMAP_MODE_TOPOLOGY) {
		/* In topology mapping, we assume meshes are identical, islands included! */
		BLI_assert(numloops_dst == dm_src->getNumLoops(dm_src));
		for (i = 0; i < numloops_dst; i++) {
			bke_mesh2mesh_mapping_item_define(r_map, i, FLT_MAX, 0, 1, &i, &full_weight);
		}
	}
	else {
		BVHTreeFromMesh *treedata = NULL;
		BVHTreeNearest nearest = {0};
		BVHTreeRayHit rayhit = {0};
		int num_trees = 0;
		float hitdist;

		const bool use_from_vert = (mode & M2MMAP_USE_VERT);

		MeshIslands islands = {0};
		bool use_islands = false;

		float (*poly_nors_src)[3] = NULL;
		float (*loop_nors_src)[3] = NULL;
		float (*poly_nors_dst)[3] = NULL;
		float (*loop_nors_dst)[3] = NULL;

		MeshElemMap *vert_to_loop_map_src = NULL;
		int *vert_to_loop_map_src_buff = NULL;
		MeshElemMap *vert_to_poly_map_src = NULL;
		int *vert_to_poly_map_src_buff = NULL;

		bool verts_allocated_src;
		MVert *verts_src = DM_get_vert_array(dm_src, &verts_allocated_src);
		const int num_verts_src = dm_src->getNumVerts(dm_src);
		float (*vcos_src)[3] = NULL;
		bool loops_allocated_src;
		MLoop *loops_src = DM_get_loop_array(dm_src, &loops_allocated_src);
		const int num_loops_src = dm_src->getNumLoops(dm_src);
		bool polys_allocated_src;
		MPoly *polys_src = DM_get_poly_array(dm_src, &polys_allocated_src);
		const int num_polys_src = dm_src->getNumPolys(dm_src);
		bool faces_allocated_src = false;
		MFace *faces_src = NULL;
		int num_faces_src;

		int *orig_poly_idx_src = NULL;

		size_t buff_size_interp = 32;  /* Will be enough in 99% of cases. */
		float (*vcos_interp)[3] = NULL;
		int *indices_interp = NULL;
		float *weights_interp = NULL;

		int tidx, pidx_dst, lidx_dst, plidx_dst, pidx_src, lidx_src, plidx_src;

		IslandResult **islands_res;
		size_t islands_res_buff_size = 32;

		const float bvh_epsilon = (mode & M2MMAP_USE_NORPROJ) ? M2MMAP_RAYCAST_APPROXIMATE_BVHEPSILON(ray_radius) : 0.0f;

		if (!use_from_vert) {
			vcos_src = MEM_mallocN(sizeof(*vcos_src) * (size_t)num_verts_src, __func__);
			dm_src->getVertCos(dm_src, vcos_src);

			vcos_interp = MEM_mallocN(sizeof(*vcos_interp) * buff_size_interp, __func__);
			indices_interp = MEM_mallocN(sizeof(*indices_interp) * buff_size_interp, __func__);
			weights_interp = MEM_mallocN(sizeof(*weights_interp) * buff_size_interp, __func__);
		}

		{
			const bool need_lnors_src = (mode & M2MMAP_USE_LOOP) && (mode & M2MMAP_USE_NORMAL);
			const bool need_lnors_dst = need_lnors_src || (mode & M2MMAP_USE_NORPROJ);
			const bool need_pnors_src = need_lnors_src || ((mode & M2MMAP_USE_POLY) && (mode & M2MMAP_USE_NORMAL));
			const bool need_pnors_dst = need_lnors_dst || need_pnors_src;

			if (need_pnors_dst) {
				/* Cache poly nors into a temp CDLayer. */
				poly_nors_dst = CustomData_get_layer(pdata_dst, CD_NORMAL);
				if (!poly_nors_dst) {
					poly_nors_dst = CustomData_add_layer(pdata_dst, CD_NORMAL, CD_CALLOC, NULL, numpolys_dst);
					CustomData_set_layer_flag(pdata_dst, CD_NORMAL, CD_FLAG_TEMPORARY);
				}
				if (dirty_nors_dst) {
					BKE_mesh_calc_normals_poly(verts_dst, numverts_dst, loops_dst, polys_dst,
					                           numloops_dst, numpolys_dst, poly_nors_dst, true);
				}
			}
			if (need_lnors_dst) {
				/* Cache poly nors into a temp CDLayer. */
				loop_nors_dst = CustomData_get_layer(ldata_dst, CD_NORMAL);
				if (!loop_nors_dst) {
					loop_nors_dst = CustomData_add_layer(ldata_dst, CD_NORMAL, CD_CALLOC, NULL, numloops_dst);
					CustomData_set_layer_flag(ldata_dst, CD_NORMAL, CD_FLAG_TEMPORARY);
				}
				if (dirty_nors_dst) {
					BKE_mesh_normals_loop_split(verts_dst, numverts_dst, edges_dst, numedges_dst,
					                            loops_dst, loop_nors_dst, numloops_dst,
					                            polys_dst, poly_nors_dst, numpolys_dst, split_angle_dst);
				}
			}
			if (need_pnors_src || need_lnors_src) {
				/* Simpler for now, calcNormals never stores pnors :( */
				dm_src->calcLoopNormals(dm_src, /* TODO */ (float)M_PI);

				if (need_pnors_src) {
					poly_nors_src = dm_src->getPolyDataArray(dm_src, CD_NORMAL);
				}
				if (need_lnors_src) {
					loop_nors_src = dm_src->getLoopDataArray(dm_src, CD_NORMAL);
				}
			}
		}

		if (use_from_vert) {
			BKE_mesh_vert_loop_map_create(&vert_to_loop_map_src, &vert_to_loop_map_src_buff,
			                              polys_src, loops_src, num_verts_src, num_polys_src, num_loops_src);
			if (mode & M2MMAP_USE_POLY) {
				BKE_mesh_vert_poly_map_create(&vert_to_poly_map_src, &vert_to_poly_map_src_buff,
				                              polys_src, loops_src, num_verts_src, num_polys_src, num_loops_src);
			}
		}

		/* Island makes things slightly more complex here.
		 * Basically, we:
		 *     * Make one treedata for each island's elements.
		 *     * Check all loops of a same dest poly against all treedata.
		 *     * Choose the island's elements giving the best results.
		 */

		/* First, generate the islands, if possible. */
		if (gen_islands_src) {
			use_islands = gen_islands_src(dm_src, &islands);
			num_trees = use_islands ? islands.nbr_islands : 1;
			treedata = MEM_callocN(sizeof(*treedata) * (size_t)num_trees, __func__);

			if (use_islands) {
				/* We expect our islands to contain poly indices, and a mapping loops -> islands indices.
				 * This implies all loops of a same poly are in the same island. */
				BLI_assert((islands.item_type == MISLAND_TYPE_LOOP) && (islands.island_type == MISLAND_TYPE_POLY));
			}
		}
		else {
			num_trees = 1;
			treedata = MEM_callocN(sizeof(*treedata), __func__);
		}

		/* Build our BVHtrees, either from verts or tessfaces. */
		if (use_from_vert) {
			if (use_islands) {
				BLI_bitmap *verts_active = BLI_BITMAP_NEW((size_t)num_verts_src, __func__);

				for (tidx = 0; tidx < num_trees; tidx++) {
					MeshElemMap *isld = islands.islands[tidx];
					int num_verts_active = 0;
					BLI_BITMAP_SET_ALL(verts_active, false, (size_t)num_verts_src);
					for (i = 0; i < isld->count; i++) {
						MPoly *mp_src = &polys_src[isld->indices[i]];
						for (lidx_src = mp_src->loopstart; lidx_src < mp_src->loopstart + mp_src->totloop; lidx_src++) {
							BLI_BITMAP_ENABLE(verts_active, loops_src[lidx_src].v);
							num_verts_active++;
						}
					}
					/* verts 'ownership' is transfered to treedata here, which will handle its freeing. */
					bvhtree_from_mesh_verts_ex(&treedata[tidx], verts_src, num_verts_src, verts_allocated_src,
					                           verts_active, num_verts_active, bvh_epsilon, 2, 6);
					if (verts_allocated_src) {
						verts_allocated_src = false;  /* Only 'give' our verts once, to first tree! */
					}
				}

				MEM_freeN(verts_active);
			}
			else {
				BLI_assert(num_trees == 1);
				bvhtree_from_mesh_verts(&treedata[0], dm_src, bvh_epsilon, 2, 6);
			}
		}
		else {  /* We use polygons. */
			if (use_islands) {
				/* bvhtree here uses tesselated faces... */
				const unsigned int dirty_tess_flag = dm_src->dirty & DM_DIRTY_TESS_CDLAYERS;
				BLI_bitmap *faces_active;

				/* We do not care about tessellated data here, only geometry itself is important. */
				if (dirty_tess_flag) {
					dm_src->dirty &= ~dirty_tess_flag;
				}
				DM_ensure_tessface(dm_src);
				if (dirty_tess_flag) {
					dm_src->dirty |= dirty_tess_flag;
				}
				faces_src = DM_get_tessface_array(dm_src, &faces_allocated_src);
				num_faces_src = dm_src->getNumTessFaces(dm_src);
				orig_poly_idx_src = dm_src->getTessFaceDataArray(dm_src, CD_ORIGINDEX);
				faces_active = BLI_BITMAP_NEW((size_t)num_faces_src, __func__);

				for (tidx = 0; tidx < num_trees; tidx++) {
					int num_faces_active = 0;
					BLI_BITMAP_SET_ALL(faces_active, false, (size_t)num_faces_src);
					for (i = 0; i < num_faces_src; i++) {
						MPoly *mp_src = &polys_src[orig_poly_idx_src[i]];
						if (islands.items_to_islands_idx[mp_src->loopstart] == tidx) {
							BLI_BITMAP_ENABLE(faces_active, i);
							num_faces_active++;
						}
					}
					/* verts 'ownership' is transfered to treedata here, which will handle its freeing. */
					bvhtree_from_mesh_faces_ex(&treedata[tidx], verts_src, verts_allocated_src,
					                           faces_src, num_faces_src, faces_allocated_src,
					                           faces_active, num_faces_active, bvh_epsilon, 2, 6);
					if (verts_allocated_src) {
						verts_allocated_src = false;  /* Only 'give' our verts once, to first tree! */
					}
					if (faces_allocated_src) {
						faces_allocated_src = false;  /* Only 'give' our faces once, to first tree! */
					}
				}

				MEM_freeN(faces_active);
			}
			else {
				BLI_assert(num_trees == 1);
				bvhtree_from_mesh_faces(&treedata[0], dm_src, bvh_epsilon, 2, 6);
				orig_poly_idx_src = dm_src->getTessFaceDataArray(dm_src, CD_ORIGINDEX);
			}
		}

		/* And check each dest poly! */
		islands_res = MEM_mallocN(sizeof(*islands_res) * (size_t)num_trees, __func__);
		for (tidx = 0; tidx < num_trees; tidx++) {
			islands_res[tidx] = MEM_mallocN(sizeof(**islands_res) * islands_res_buff_size, __func__);
		}

		for (pidx_dst = 0; pidx_dst < numpolys_dst; pidx_dst++) {
			MPoly *mp_dst = &polys_dst[pidx_dst];
			float (*pnor_dst)[3] = &poly_nors_dst[pidx_dst];

			if ((size_t)mp_dst->totloop > islands_res_buff_size) {
				islands_res_buff_size = (size_t)mp_dst->totloop;
				for (tidx = 0; tidx < num_trees; tidx++) {
					islands_res[tidx] = MEM_reallocN(islands_res[tidx], sizeof(**islands_res) * islands_res_buff_size);
				}
			}

			for (tidx = 0; tidx < num_trees; tidx++) {
				BVHTreeFromMesh *tdata = &treedata[tidx];
				MLoop *ml_dst = &loops_dst[mp_dst->loopstart];

				for (plidx_dst = 0; plidx_dst < mp_dst->totloop; plidx_dst++, ml_dst++) {
					if (use_from_vert) {
						float tmp_co[3];
						MeshElemMap *vert_to_refelem_map_src = NULL;

						copy_v3_v3(tmp_co, verts_dst[ml_dst->v].co);
						nearest.index = -1;

						hitdist = bke_mesh2mesh_bvhtree_query_nearest(tdata, &nearest, space_transform,
						                                              tmp_co, max_dist_sq);

						if (nearest.index >= 0) {
							float (*nor_dst)[3];
							float (*nors_src)[3];
							float best_nor_dot = -2.0f;
							int best_idx_src = -1;

							if (mode == M2MMAP_MODE_LOOP_NEAREST_LOOPNOR) {
								nor_dst = &loop_nors_dst[plidx_dst + mp_dst->loopstart];
								nors_src = loop_nors_src;
								vert_to_refelem_map_src = vert_to_loop_map_src;
							}
							else {  /* if (mode == M2MMAP_MODE_LOOP_NEAREST_POLYNOR) { */
								nor_dst = pnor_dst;
								nors_src = poly_nors_src;
								vert_to_refelem_map_src = vert_to_poly_map_src;
							}

							for (i = vert_to_refelem_map_src[nearest.index].count; i--;) {
								const int idx_src = vert_to_refelem_map_src[nearest.index].indices[i];
								const float dot = dot_v3v3(nors_src[idx_src], *nor_dst);
								if (dot > best_nor_dot) {
									best_nor_dot = dot;
									best_idx_src = idx_src;
								}
							}
							if (mode == M2MMAP_MODE_LOOP_NEAREST_POLYNOR) {
								/* Our best_idx_src is a poly one for now!
								 * Have to find its loop matching our closest vertex. */
								MPoly *mp_src = &polys_src[best_idx_src];
								MLoop *ml_src = &loops_src[mp_src->loopstart];
								for (plidx_src = 0; plidx_src < mp_src->totloop; plidx_src++, ml_src++) {
									if ((int)ml_src->v == nearest.index) {
										best_idx_src = plidx_src + mp_src->loopstart;
										break;
									}
								}
							}
							islands_res[tidx][plidx_dst].factor = hitdist ? (1.0f / hitdist * best_nor_dot) : 1e18f;
							islands_res[tidx][plidx_dst].hit_distance = hitdist;
							islands_res[tidx][plidx_dst].idx_src = best_idx_src;
						}
						else {
							/* No source for this dest loop! */
							islands_res[tidx][plidx_dst].factor = 0.0f;
							islands_res[tidx][plidx_dst].hit_distance = FLT_MAX;
							islands_res[tidx][plidx_dst].idx_src = -1;
						}
					}
					else if (mode & M2MMAP_USE_NORPROJ) {
						float tmp_co[3], tmp_no[3];

						int n = (ray_radius > 0.0f) ? M2MMAP_RAYCAST_APPROXIMATE_NR : 1;
						float w = 1.0f;

						copy_v3_v3(tmp_co, verts_dst[ml_dst->v].co);
						copy_v3_v3(tmp_no, loop_nors_dst[plidx_dst + mp_dst->loopstart]);

						while (n--) {
							hitdist = bke_mesh2mesh_bvhtree_query_raycast(tdata, &rayhit, space_transform,
							                                              tmp_co, tmp_no, ray_radius / w, max_dist);

							if (rayhit.index >= 0 && hitdist <= max_dist) {
								islands_res[tidx][plidx_dst].factor = (hitdist ? (1.0f / hitdist) : 1e18f) * w;
								islands_res[tidx][plidx_dst].hit_distance = hitdist;
								islands_res[tidx][plidx_dst].idx_src = orig_poly_idx_src[rayhit.index];
								copy_v3_v3(islands_res[tidx][plidx_dst].hit_point, rayhit.co);
								break;
							}
							/* Next iteration will get bigger radius but smaller weight! */
							w /= M2MMAP_RAYCAST_APPROXIMATE_FAC;
						}
						if (n == -1) {
							/* No source for this dest loop! */
							islands_res[tidx][plidx_dst].factor = 0.0f;
							islands_res[tidx][plidx_dst].hit_distance = FLT_MAX;
							islands_res[tidx][plidx_dst].idx_src = -1;
						}
					}
					else {  /* Nearest poly either to use all its loops/verts or just closest one. */
						float tmp_co[3];

						copy_v3_v3(tmp_co, verts_dst[ml_dst->v].co);
						nearest.index = -1;

						hitdist = bke_mesh2mesh_bvhtree_query_nearest(tdata, &nearest, space_transform,
						                                              tmp_co, max_dist_sq);

						if (nearest.index >= 0) {
							islands_res[tidx][plidx_dst].factor = hitdist ? (1.0f / hitdist) : 1e18f;
							islands_res[tidx][plidx_dst].hit_distance = hitdist;
							islands_res[tidx][plidx_dst].idx_src = orig_poly_idx_src[nearest.index];
							copy_v3_v3(islands_res[tidx][plidx_dst].hit_point, nearest.co);
						}
						else {
							/* No source for this dest loop! */
							islands_res[tidx][plidx_dst].factor = 0.0f;
							islands_res[tidx][plidx_dst].hit_distance = FLT_MAX;
							islands_res[tidx][plidx_dst].idx_src = -1;
						}
					}
				}
			}

			/* And now, find best island to use! */
			{
				float best_island_fac = 0.0f;
				int best_island_idx = -1;

				for (tidx = 0; tidx < num_trees; tidx++) {
					float island_fac = 0.0f;

					for (plidx_dst = 0; plidx_dst < mp_dst->totloop; plidx_dst++) {
						island_fac += islands_res[tidx][plidx_dst].factor;
					}
					island_fac /= (float)mp_dst->totloop;

					if (island_fac > best_island_fac) {
						best_island_fac = island_fac;
						best_island_idx = tidx;
					}
				}

				for (plidx_dst = 0; plidx_dst < mp_dst->totloop; plidx_dst++) {
					IslandResult *isld_res;
					lidx_dst = plidx_dst + mp_dst->loopstart;

					if (best_island_idx < 0) {
						/* No source for any loops of our dest poly in any source islands. */
						BKE_mesh2mesh_mapping_item_define_invalid(r_map, lidx_dst);
						continue;
					}

					isld_res = &islands_res[best_island_idx][plidx_dst];
					if (use_from_vert) {
						/* Indices stored in islands_res are those of loops, one per dest loop. */
						lidx_src = isld_res->idx_src;
						if (lidx_src >= 0) {
							bke_mesh2mesh_mapping_item_define(r_map, lidx_dst, isld_res->hit_distance,
							                                  best_island_idx, 1, &lidx_src, &full_weight);
						}
						else {
							/* No source for this loop in this island. */
							/* TODO: would probably be better to get a source at all cost in best island anyway? */
							bke_mesh2mesh_mapping_item_define(r_map, lidx_dst, FLT_MAX, best_island_idx, 0, NULL, NULL);
						}
					}
					else {
						/* Else, we use source poly, indices stored in facs are those of polygons. */
						pidx_src = isld_res->idx_src;
						if (pidx_src >= 0) {
							MPoly *mp_src = &polys_src[pidx_src];
							float *hit_co = isld_res->hit_point;
							int best_loop_idx_src;

							if (mode == M2MMAP_MODE_LOOP_POLY_NEAREST) {
								bke_mesh2mesh_mapping_get_interp_poly_data(
								        mp_src, loops_src, (const float (*)[3])vcos_src, hit_co,
								        &buff_size_interp, &vcos_interp, true, &indices_interp,
								        &weights_interp, false, &best_loop_idx_src);

								bke_mesh2mesh_mapping_item_define(r_map, lidx_dst, isld_res->hit_distance,
								                                  best_island_idx, 1, &best_loop_idx_src, &full_weight);
							}
							else {
								const int nbr_sources = bke_mesh2mesh_mapping_get_interp_poly_data(
								                                mp_src, loops_src, (const float (*)[3])vcos_src, hit_co,
								                                &buff_size_interp, &vcos_interp, true, &indices_interp,
								                                &weights_interp, true, NULL);

								bke_mesh2mesh_mapping_item_define(r_map, lidx_dst,
								                                  isld_res->hit_distance, best_island_idx,
								                                  nbr_sources, indices_interp, weights_interp);
							}
						}
						else {
							/* No source for this loop in this island. */
							/* TODO: would probably be better to get a source at all cost in best island anyway? */
							bke_mesh2mesh_mapping_item_define(r_map, lidx_dst, FLT_MAX, best_island_idx, 0, NULL, NULL);
						}
					}
				}
			}
		}

		for (tidx = 0; tidx < num_trees; tidx++) {
			MEM_freeN(islands_res[tidx]);
		}
		MEM_freeN(islands_res);
		if (verts_allocated_src) {
			MEM_freeN(verts_src);
		}
		if (vcos_src) {
			MEM_freeN(vcos_src);
		}
		if (loops_allocated_src) {
			MEM_freeN(loops_src);
		}
		if (polys_allocated_src) {
			MEM_freeN(polys_src);
		}
		if (faces_allocated_src) {
			MEM_freeN(faces_src);
		}
		if (vert_to_loop_map_src_buff) {
			MEM_freeN(vert_to_loop_map_src_buff);
		}
		if (vert_to_poly_map_src_buff) {
			MEM_freeN(vert_to_poly_map_src_buff);
		}
		if (vcos_interp) {
			MEM_freeN(vcos_interp);
		}
		if (indices_interp) {
			MEM_freeN(indices_interp);
		}
		if (weights_interp) {
			MEM_freeN(weights_interp);
		}
		BKE_loop_islands_free(&islands);
		MEM_freeN(treedata);
	}
}

void BKE_dm2mesh_mapping_polys_compute(
        const int mode, const SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        MVert *verts_dst, const int numverts_dst, MLoop *loops_dst, const int numloops_dst,
        MPoly *polys_dst, const int numpolys_dst, CustomData *pdata_dst, const bool dirty_nors_dst,
        DerivedMesh *dm_src, Mesh2MeshMapping *r_map)
{
	const float full_weight = 1.0f;
	const float max_dist_sq = max_dist * max_dist;
	float (*poly_nors_dst)[3] = NULL;
	int i;

	BLI_assert(mode & M2MMAP_MODE_POLY);

	if (mode & (M2MMAP_USE_NORMAL | M2MMAP_USE_NORPROJ)) {
		/* Cache poly nors into a temp CDLayer. */
		poly_nors_dst = CustomData_get_layer(pdata_dst, CD_NORMAL);
		if (!poly_nors_dst) {
			poly_nors_dst = CustomData_add_layer(pdata_dst, CD_NORMAL, CD_CALLOC, NULL, numpolys_dst);
			CustomData_set_layer_flag(pdata_dst, CD_NORMAL, CD_FLAG_TEMPORARY);
		}
		if (dirty_nors_dst) {
			BKE_mesh_calc_normals_poly(verts_dst, numverts_dst, loops_dst, polys_dst, numloops_dst, numpolys_dst,
			                           poly_nors_dst, true);
		}
	}

	BKE_mesh2mesh_mapping_init(r_map, numpolys_dst);

	if (mode == M2MMAP_MODE_TOPOLOGY) {
		BLI_assert(numpolys_dst == dm_src->getNumPolys(dm_src));
		for (i = 0; i < numpolys_dst; i++) {
			bke_mesh2mesh_mapping_item_define(r_map, i, FLT_MAX, 0, 1, &i, &full_weight);
		}
	}
	else {
		BVHTreeFromMesh treedata = {NULL};
		BVHTreeNearest nearest = {0};
		BVHTreeRayHit rayhit = {0};
		float hitdist;

		int *orig_poly_idx_src;

		bvhtree_from_mesh_faces(&treedata, dm_src,
		                        (mode & M2MMAP_USE_NORPROJ) ? M2MMAP_RAYCAST_APPROXIMATE_BVHEPSILON(ray_radius) : 0.0f,
		                        2, 6);
		/* bvhtree here uses tesselated faces... */
		orig_poly_idx_src = dm_src->getTessFaceDataArray(dm_src, CD_ORIGINDEX);

		if (mode == M2MMAP_MODE_POLY_NEAREST) {
			nearest.index = -1;

			for (i = 0; i < numpolys_dst; i++) {
				MPoly *mp = &polys_dst[i];
				float tmp_co[3];

				BKE_mesh_calc_poly_center(mp, &loops_dst[mp->loopstart], verts_dst, tmp_co);

				hitdist = bke_mesh2mesh_bvhtree_query_nearest(&treedata, &nearest, space_transform,
				                                              tmp_co, max_dist_sq);

				if (nearest.index >= 0) {
					bke_mesh2mesh_mapping_item_define(r_map, i, hitdist, 0,
					                                  1, &orig_poly_idx_src[nearest.index], &full_weight);
				}
				else {
					/* No source for this dest poly! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}
		}
		else if (mode == M2MMAP_MODE_POLY_NOR) {
			BLI_assert(poly_nors_dst);

			for (i = 0; i < numpolys_dst; i++) {
				MPoly *mp = &polys_dst[i];
				float tmp_co[3], tmp_no[3];

				BKE_mesh_calc_poly_center(mp, &loops_dst[mp->loopstart], verts_dst, tmp_co);
				copy_v3_v3(tmp_no, poly_nors_dst[i]);

				hitdist = bke_mesh2mesh_bvhtree_query_raycast(&treedata, &rayhit, space_transform,
				                                              tmp_co, tmp_no, ray_radius, max_dist);

				if (rayhit.index >= 0 && hitdist <= max_dist) {
					bke_mesh2mesh_mapping_item_define(r_map, i, rayhit.dist, 0,
					                                  1, &orig_poly_idx_src[rayhit.index], &full_weight);
				}
				else {
					/* No source for this dest poly! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}
		}
		else if (mode == M2MMAP_MODE_POLY_POLYINTERP_PNORPROJ) {
			/* We cast our rays randomly, with a pseudo-even distribution (since we spread across tessellated tris,
			 * with additional weighting based on each tri's relative area).
			 */
			RNG *rng = BLI_rng_new(0);

			const size_t numpolys_src = (size_t)dm_src->getNumPolys(dm_src);

			/* Here it's simpler to just allocate for all polys :/ */
			int *indices = MEM_mallocN(sizeof(*indices) * numpolys_src, __func__);
			float *weights = MEM_mallocN(sizeof(*weights) * numpolys_src, __func__);

			size_t tmp_poly_size = 32;  /* Will be enough in 99% of cases. */
			float (*poly_vcos_2d)[2] = MEM_mallocN(sizeof(*poly_vcos_2d) * tmp_poly_size, __func__);
			/* Tessellated 2D poly, always (num_loops - 2) triangles. */
			int (*tri_vidx_2d)[3] = MEM_mallocN(sizeof(*tri_vidx_2d) * (tmp_poly_size - 2), __func__);

			for (i = 0; i < numpolys_dst; i++) {
				/* For each dst poly, we sample some rays from it (2D grid in pnor space)
				 * and use their hits to interpolate from source polys. */
				/* Note: dst poly is early-converted into src space! */
				MPoly *mp = &polys_dst[i];
				float tmp_co[3], tmp_no[3];

				int tot_rays, done_rays = 0;
				float poly_area_2d_inv, done_area = 0.0f;

				const float zvec[3] = {0.0f, 0.0f, 1.0f};
				float pcent_dst[3];
				float to_pnor_2d_mat[3][3], from_pnor_2d_mat[3][3];
				float poly_dst_2d_min[2], poly_dst_2d_max[2], poly_dst_2d_z;
				float poly_dst_2d_size[2];

				float totweights = 0.0f;
				float hitdist_accum = 0.0f;
				int nbr_sources = 0;
				int nbr_tris = mp->totloop - 2;
				int j;

				BKE_mesh_calc_poly_center(mp, &loops_dst[mp->loopstart], verts_dst, pcent_dst);
				copy_v3_v3(tmp_no, poly_nors_dst[i]);
				/* We do our transform here, else it'd be redone by raycast helper for each ray, ugh! */
				if (space_transform) {
					BLI_space_transform_apply(space_transform, pcent_dst);
					BLI_space_transform_apply_normal(space_transform, tmp_no);
				}

				fill_vn_fl(weights, (int)numpolys_src, 0.0f);

				if ((size_t)mp->totloop > tmp_poly_size) {
					tmp_poly_size = (size_t)mp->totloop;
					poly_vcos_2d = MEM_reallocN(poly_vcos_2d, sizeof(*poly_vcos_2d) * tmp_poly_size);
					tri_vidx_2d = MEM_reallocN(tri_vidx_2d, sizeof(*tri_vidx_2d) * (tmp_poly_size - 2));
				}

				rotation_between_vecs_to_mat3(to_pnor_2d_mat, tmp_no, zvec);
				invert_m3_m3(from_pnor_2d_mat, to_pnor_2d_mat);

				mul_m3_v3(to_pnor_2d_mat, pcent_dst);
				poly_dst_2d_z = pcent_dst[2];

				/* Get (2D) bounding square of our poly. */
				INIT_MINMAX2(poly_dst_2d_min, poly_dst_2d_max);

				for (j = 0; j < mp->totloop; j++) {
					MLoop *ml = &loops_dst[j + mp->loopstart];
					copy_v3_v3(tmp_co, verts_dst[ml->v].co);
					if (space_transform) {
						BLI_space_transform_apply(space_transform, tmp_co);
					}
					mul_v2_m3v3(poly_vcos_2d[j], to_pnor_2d_mat, tmp_co);
					minmax_v2v2_v2(poly_dst_2d_min, poly_dst_2d_max, poly_vcos_2d[j]);
				}

				/* We adjust our ray-casting grid to ray_radius (the smaller, the more rays are cast),
				 * with lower/upper bounds. */
				sub_v2_v2v2(poly_dst_2d_size, poly_dst_2d_max, poly_dst_2d_min);

				if (ray_radius) {
					tot_rays = (int)((max_ff(poly_dst_2d_size[0], poly_dst_2d_size[1]) / ray_radius) + 0.5f);
					CLAMP(tot_rays, 4, 20);  /* min 16 rays/face, max 400. */
				}
				else {
					tot_rays = 20;  /* If no radius (pure rays), give max number of rays! */
				}
				tot_rays *= tot_rays;

				poly_area_2d_inv = 1.0f / area_poly_v2((const float(*)[2])poly_vcos_2d, (unsigned int)mp->totloop);

				/* Tessellate our poly. */
				if (mp->totloop == 3) {
					tri_vidx_2d[0][0] = 0;
					tri_vidx_2d[0][1] = 1;
					tri_vidx_2d[0][2] = 2;
				}
				if (mp->totloop == 4) {
					tri_vidx_2d[0][0] = 0;
					tri_vidx_2d[0][1] = 1;
					tri_vidx_2d[0][2] = 2;
					tri_vidx_2d[1][0] = 0;
					tri_vidx_2d[1][1] = 2;
					tri_vidx_2d[1][2] = 3;
				}
				else {
					BLI_polyfill_calc((const float(*)[2])poly_vcos_2d, (unsigned int)mp->totloop, -1,
					                  (unsigned int (*)[3])tri_vidx_2d);
				}

				for (j = 0; j < nbr_tris; j++) {
					float *v1 = poly_vcos_2d[tri_vidx_2d[j][0]];
					float *v2 = poly_vcos_2d[tri_vidx_2d[j][1]];
					float *v3 = poly_vcos_2d[tri_vidx_2d[j][2]];
					int nbr_rays;

					/* All this allows us to get 'absolute' number of rays for each tri, avoiding accumulating
					 * errors over iterations, and helping better even distribution. */
					done_area += area_tri_v2(v1, v2, v3);
					nbr_rays = (int)((float)tot_rays * done_area * poly_area_2d_inv + 0.5f) - done_rays;
					done_rays += nbr_rays;

					while (nbr_rays--) {
						int n = (ray_radius > 0.0f) ? M2MMAP_RAYCAST_APPROXIMATE_NR : 1;
						float w = 1.0f;

						BLI_rng_get_tri_sample_float_v2(rng, v1, v2, v3, tmp_co);

						tmp_co[2] = poly_dst_2d_z;
						mul_m3_v3(from_pnor_2d_mat, tmp_co);

						/* At this point, tmp_co is a point on our poly surface, in mesh_src space! */
						while (n--) {
							/* Note we handle dest to src space conversion ourself, here! */
							hitdist = bke_mesh2mesh_bvhtree_query_raycast(&treedata, &rayhit, NULL,
							                                              tmp_co, tmp_no, ray_radius / w, max_dist);

							if (rayhit.index >= 0 && hitdist <= max_dist) {
								weights[orig_poly_idx_src[rayhit.index]] += w;
								totweights += w;
								hitdist_accum += hitdist;
								break;
							}
							/* Next iteration will get bigger radius but smaller weight! */
							w /= M2MMAP_RAYCAST_APPROXIMATE_FAC;
						}
					}
				}

				if (totweights > 0.0f) {
					for (j = 0; j < (int)numpolys_src; j++) {
						if (!weights[j]) {
							continue;
						}
						/* Note: nbr_sources is always <= j! */
						weights[nbr_sources] = weights[j] / totweights;
						indices[nbr_sources] = j;
						nbr_sources++;
					}
					bke_mesh2mesh_mapping_item_define(r_map, i, hitdist_accum / totweights, 0,
					                                  nbr_sources, indices, weights);
				}
				else {
					/* No source for this dest poly! */
					BKE_mesh2mesh_mapping_item_define_invalid(r_map, i);
				}
			}

			MEM_freeN(tri_vidx_2d);
			MEM_freeN(poly_vcos_2d);
			MEM_freeN(indices);
			MEM_freeN(weights);
			BLI_rng_free(rng);
		}
		else {
			printf("WARNING! Unsupported mesh-to-mesh poly mapping mode (%d)!\n", mode);
			memset(r_map->items, 0, sizeof(*r_map->items) * (size_t)numpolys_dst);
		}

		free_bvhtree_from_mesh(&treedata);
	}
}

#undef M2MMAP_RAYCAST_APPROXIMATE_NR
#undef M2MMAP_RAYCAST_APPROXIMATE_FAC
#undef M2MMAP_RAYCAST_APPROXIMATE_BVHEPSILON

/** \} */
