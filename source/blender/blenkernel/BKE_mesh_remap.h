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
#ifndef __BKE_MESH_REMAP_H__
#define __BKE_MESH_REMAP_H__

/** \file BKE_mesh_remap.h
 *  \ingroup bke
 */

struct CustomData;
struct DerivedMesh;
struct MVert;
struct MeshElemMap;

/* Loop islands data helpers. */
enum {
	MISLAND_TYPE_VERT = 1,
	MISLAND_TYPE_EDGE = 2,
	MISLAND_TYPE_POLY = 3,
	MISLAND_TYPE_LOOP = 4,
};

typedef struct MeshIslands {
	short item_type;  /* MISLAND_TYPE_... */
	short island_type;  /* MISLAND_TYPE_... */

	int nbr_items;
	int *items_to_islands_idx;

	int nbr_islands;
	struct MeshElemMap **islands;  /* Array of pointers, one item per island. */

	void *mem;  /* Memory handler, internal use only. */
	size_t allocated_islands;
} MeshIslands;

void BKE_loop_islands_init(MeshIslands *islands, const short item_type, const int num_items, const short island_type);
void BKE_loop_islands_free(MeshIslands *islands);
void BKE_loop_islands_add_island(MeshIslands *islands, const int num_items, int *item_indices,
                                 const int num_island_items, int *island_item_indices);

typedef bool (*loop_island_compute)(struct DerivedMesh *dm, MeshIslands *r_islands);
/* Above vert/UV mapping stuff does not do what we need here, but does things we do not need here.
 * So better keep them separated for now, I think.
 */
bool BKE_loop_poly_island_compute_uv(struct DerivedMesh *dm, MeshIslands *r_islands);

/* Generic ways to map some geometry elements from a source mesh to a dest one. */

typedef struct Mesh2MeshMappingItem {
	int nbr_sources;
	int *indices_src;  /* NULL if no source found. */
	float *weights_src;  /* NULL if no source found, else, always normalized! */
	float hit_distance;  /* FLT_MAX if irrelevant or no source found. */
	int island;  /* For loops only. */
} Mesh2MeshMappingItem;

/* All mapping computing func return this. */
typedef struct Mesh2MeshMapping {
	int nbr_items;
	Mesh2MeshMappingItem *items;  /* Array, one item per dest element. */

	void *mem;  /* Memory handler, internal use only. */
} Mesh2MeshMapping;

/* Helpers! */
void BKE_mesh2mesh_mapping_init(Mesh2MeshMapping *map, const int num_items);
void BKE_mesh2mesh_mapping_free(Mesh2MeshMapping *map);

void BKE_mesh2mesh_mapping_item_define_invalid(Mesh2MeshMapping *map, const int idx);

/* TODO:
 * Add other 'from/to' mapping sources, like e.g. using an UVMap, etc.
 *     http://blenderartists.org/forum/showthread.php?346458-Move-Vertices-to-the-location-of-the-Reference-Mesh-based-on-the-UV-Position
 * We could also use similar topology mappings inside a same mesh
 * (cf. Campbell's 'select face islands from similar topology' wip work).
 * Also, users will have to check, whether we can get rid of some modes here, not sure all will be useful!
 */
enum {
	M2MMAP_USE_VERT                      = 1 << 4,
	M2MMAP_USE_EDGE                      = 1 << 5,
	M2MMAP_USE_LOOP                      = 1 << 6,
	M2MMAP_USE_POLY                      = 1 << 7,

	M2MMAP_USE_NEAREST                   = 1 << 8,
	M2MMAP_USE_NORPROJ                   = 1 << 9,
	M2MMAP_USE_INTERP                    = 1 << 10,
	M2MMAP_USE_NORMAL                    = 1 << 11,

	/* ***** Target's vertices ***** */
	M2MMAP_MODE_VERT                     = 1 << 24,
	/* Nearest source vert. */
	M2MMAP_MODE_VERT_NEAREST             = M2MMAP_MODE_VERT | M2MMAP_USE_VERT | M2MMAP_USE_NEAREST,

	/* Nearest vertex of nearest edge. */
	M2MMAP_MODE_VERT_EDGE_NEAREST        = M2MMAP_MODE_VERT | M2MMAP_USE_EDGE | M2MMAP_USE_NEAREST,
	/* This one uses two verts of selected edge (weighted interpolation). */
	/* Nearest point on nearest edge. */
	M2MMAP_MODE_VERT_EDGEINTERP_NEAREST  = M2MMAP_MODE_VERT | M2MMAP_USE_EDGE | M2MMAP_USE_NEAREST | M2MMAP_USE_INTERP,

	/* Nearest vertex of nearest poly. */
	M2MMAP_MODE_VERT_POLY_NEAREST        = M2MMAP_MODE_VERT | M2MMAP_USE_POLY | M2MMAP_USE_NEAREST,
	/* Those two use all verts of selected poly (weighted interpolation). */
	/* Nearest point on nearest poly. */
	M2MMAP_MODE_VERT_POLYINTERP_NEAREST  = M2MMAP_MODE_VERT | M2MMAP_USE_POLY | M2MMAP_USE_NEAREST | M2MMAP_USE_INTERP,
	/* Point on nearest face hit by ray from target vertex's normal. */
	M2MMAP_MODE_VERT_POLYINTERP_VNORPROJ = M2MMAP_MODE_VERT | M2MMAP_USE_POLY | M2MMAP_USE_NORPROJ | M2MMAP_USE_INTERP,

	/* ***** Target's edges ***** */
	M2MMAP_MODE_EDGE                     = 1 << 25,

	/* Source edge which both vertices are nearest of dest ones. */
	M2MMAP_MODE_EDGE_VERT_NEAREST        = M2MMAP_MODE_EDGE | M2MMAP_USE_VERT | M2MMAP_USE_NEAREST,

	/* Nearest source edge (using mid-point). */
	M2MMAP_MODE_EDGE_NEAREST             = M2MMAP_MODE_EDGE | M2MMAP_USE_EDGE | M2MMAP_USE_NEAREST,

	/* Nearest edge of nearest poly (using mid-point). */
	M2MMAP_MODE_EDGE_POLY_NEAREST        = M2MMAP_MODE_EDGE | M2MMAP_USE_POLY | M2MMAP_USE_NEAREST,

	/* Cast a set of rays from along dest edge, interpolating its vertices' normals, and use hit source edges. */
	M2MMAP_MODE_EDGE_EDGEINTERP_VNORPROJ = M2MMAP_MODE_EDGE | M2MMAP_USE_VERT | M2MMAP_USE_NORPROJ | M2MMAP_USE_INTERP,

	/* ***** Target's loops ***** */
	/* Note: when islands are given to loop mapping func, all loops from the same destination face will always be mapped
	 *       to loops of source faces within a same island, regardless of mapping mode. */
	M2MMAP_MODE_LOOP                     = 1 << 26,

	/* Best normal-matching loop from nearest vert. */
	M2MMAP_MODE_LOOP_NEAREST_LOOPNOR     = M2MMAP_MODE_LOOP | M2MMAP_USE_LOOP | M2MMAP_USE_VERT | M2MMAP_USE_NEAREST | M2MMAP_USE_NORMAL,
	/* Loop from best normal-matching poly from nearest vert. */
	M2MMAP_MODE_LOOP_NEAREST_POLYNOR     = M2MMAP_MODE_LOOP | M2MMAP_USE_POLY | M2MMAP_USE_VERT | M2MMAP_USE_NEAREST | M2MMAP_USE_NORMAL,

	/* Loop from nearest vertex of nearest poly. */
	M2MMAP_MODE_LOOP_POLY_NEAREST        = M2MMAP_MODE_LOOP | M2MMAP_USE_POLY | M2MMAP_USE_NEAREST,
	/* Those two use all verts of selected poly (weighted interpolation). */
	/* Nearest point on nearest poly. */
	M2MMAP_MODE_LOOP_POLYINTERP_NEAREST  = M2MMAP_MODE_LOOP | M2MMAP_USE_POLY | M2MMAP_USE_NEAREST | M2MMAP_USE_INTERP,
	/* Point on nearest face hit by ray from target loop's normal. */
	M2MMAP_MODE_LOOP_POLYINTERP_LNORPROJ = M2MMAP_MODE_LOOP | M2MMAP_USE_POLY | M2MMAP_USE_NORPROJ | M2MMAP_USE_INTERP,

	/* ***** Target's polygons ***** */
	M2MMAP_MODE_POLY                     = 1 << 27,

	/* Nearest source poly. */
	M2MMAP_MODE_POLY_NEAREST             = M2MMAP_MODE_POLY | M2MMAP_USE_POLY | M2MMAP_USE_NEAREST,
	/* Source poly from best normal-matching dest poly. */
	M2MMAP_MODE_POLY_NOR                 = M2MMAP_MODE_POLY | M2MMAP_USE_POLY | M2MMAP_USE_NORMAL,

	/* Project dest poly onto source mesh using its normal, and use interpolation of all intersecting source polys. */
	M2MMAP_MODE_POLY_POLYINTERP_PNORPROJ = M2MMAP_MODE_POLY | M2MMAP_USE_POLY | M2MMAP_USE_NORPROJ | M2MMAP_USE_INTERP,

	/* ***** Same topology, applies to all four elements types. ***** */
	M2MMAP_MODE_TOPOLOGY                 = M2MMAP_MODE_VERT | M2MMAP_MODE_EDGE | M2MMAP_MODE_LOOP | M2MMAP_MODE_POLY,
};

/* TODO add mesh2mesh versions (we'll need mesh versions of bvhtree funcs too, though!). */

void BKE_dm2mesh_mapping_verts_compute(
        const int mode, const struct SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        const struct MVert *verts_dst, const int numverts_dst, const bool dirty_nors_dst,
        struct DerivedMesh *dm_src, Mesh2MeshMapping *r_map);

void BKE_dm2mesh_mapping_edges_compute(
        const int mode, const struct SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        const struct MVert *verts_dst, const int numverts_dst, const struct MEdge *edges_dst, const int numedges_dst,
        const bool dirty_nors_dst, struct DerivedMesh *dm_src, Mesh2MeshMapping *r_map);

void BKE_dm2mesh_mapping_loops_compute(
        const int mode, const struct SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        struct MVert *verts_dst, const int numverts_dst, struct MEdge *edges_dst, const int numedges_dst,
        struct MLoop *loops_dst, const int numloops_dst, struct MPoly *polys_dst, const int numpolys_dst,
        struct CustomData *ldata_dst, struct CustomData *pdata_dst, const float split_angle_dst,
        const bool dirty_nors_dst,
        struct DerivedMesh *dm_src, loop_island_compute gen_islands_src, struct Mesh2MeshMapping *r_map);

void BKE_dm2mesh_mapping_polys_compute(
        const int mode, const struct SpaceTransform *space_transform, const float max_dist, const float ray_radius,
        struct MVert *verts_dst, const int numverts_dst, struct MLoop *loops_dst, const int numloops_dst,
        struct MPoly *polys_dst, const int numpolys_dst, struct CustomData *pdata_dst, const bool dirty_nors_dst,
        struct DerivedMesh *dm_src, struct Mesh2MeshMapping *r_map);

#endif  /* __BKE_MESH_REMAP_H__ */
