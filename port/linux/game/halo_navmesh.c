/*
HALO_NAVMESH.C

Recast & Detour style 3D Navigation Mesh, Dynamic Forge Obstacle/Ramp
Integration, Full-Map Polygon A*, Simple Stupid Funnel String-Pulling,
Off-Mesh Connections (teleporters, ladders, ledge drops), and DetourCrowd
Reciprocal Velocity Obstacle (RVO) local steering for OpenCE.
*/

#include "cseries.h"
#include "ai/actors.h"
#include "ai/path.h"
#include "game/game.h"
#include "main/main.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "halo_navmesh.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Use standard C heap allocation so large navmesh tables do not consume
   Xbox contiguous memory or Android guest .bss space. */
#undef malloc
#undef calloc
#undef realloc
#undef free

void platform_log(char const *format, ...);

/* ---------- constants */

#define NAV_MAX_POLYS 16384
#define NAV_MAX_CORNERS 8
#define NAV_MAX_LINKS_PER_POLY 8
#define NAV_MAX_OFFMESH 256
#define NAV_MAX_CORRIDOR 512
#define NAV_MAX_PORTALS 516

#define NAV_WALKABLE_BIT 0
#define NAV_CLIMBABLE_BIT 3

#define NAV_MIN_WALKABLE_NORMAL_Z 0.65f
#define NAV_MIN_VEHICLE_NORMAL_Z 0.76f
#define NAV_CEILING_CLEARANCE 0.65f
#define NAV_STEP_HEIGHT 0.45f
#define NAV_BIPED_RADIUS 0.25f

#define NAV_DEBUG_MAX_PATHS 16
#define NAV_DEBUG_MAX_WAYPOINTS 12

/* ---------- structures */

struct halo_nav_link
{
	long neighbor_poly;
	real_point3d left;
	real_point3d right;
	real width;
	real cost;
	short offmesh_type;
	short offmesh_index;
};

struct halo_nav_poly
{
	long surface_index; /* BSP surface index, or closest BSP surface for Forge polys */
	real_point3d center;
	real reach;
	real clearance_radius;
	real normal_z;
	unsigned char corner_count;
	unsigned char link_count;
	unsigned char blocked;
	unsigned char is_forge_surface;
	unsigned char vehicle_passable;
	real_point3d corners[NAV_MAX_CORNERS];
	struct halo_nav_link links[NAV_MAX_LINKS_PER_POLY];
};

struct halo_nav_offmesh
{
	short type; /* _halo_nav_offmesh_* */
	long start_poly;
	long end_poly;
	real_point3d start_point;
	real_point3d end_point;
	long start_surface;
	long end_surface;
	real cost;
};

struct halo_nav_astar_workspace
{
	real g_score[NAV_MAX_POLYS];
	real f_score[NAV_MAX_POLYS];
	long parent_poly[NAV_MAX_POLYS];
	short parent_link[NAV_MAX_POLYS];
	unsigned char state[NAV_MAX_POLYS]; /* 0 = unseen, 1 = open, 2 = closed */
	long heap[NAV_MAX_POLYS];
	long heap_size;
	long corridor[NAV_MAX_CORRIDOR];
	short corridor_link[NAV_MAX_CORRIDOR];
	real_point3d portal_left[NAV_MAX_PORTALS];
	real_point3d portal_right[NAV_MAX_PORTALS];
	long portal_surface[NAV_MAX_PORTALS];
	short portal_offmesh_type[NAV_MAX_PORTALS];
	short portal_offmesh_index[NAV_MAX_PORTALS];
};

struct halo_nav_mesh
{
	boolean built;
	boolean dirty;
	struct scenario *scenario;
	struct structure_bsp *structure;
	long last_object_hash;
	long last_object_check_tick;
	long poly_count;
	long bsp_poly_count;
	long forge_poly_count;
	long offmesh_count;
	long *surface_to_poly; /* [bsp->surfaces.count], NONE if not walkable */
	long surface_to_poly_count;
	struct halo_nav_poly *polys; /* [NAV_MAX_POLYS] */
	struct halo_nav_offmesh offmesh[NAV_MAX_OFFMESH];
	struct halo_nav_astar_workspace *ws;
};

static struct halo_nav_mesh g_nav;
static pthread_mutex_t g_nav_mutex = PTHREAD_MUTEX_INITIALIZER;

struct halo_nav_debug_path
{
	boolean valid;
	long tick;
	real agent_radius;
	real_point3d start_point;
	long waypoint_count;
	real_point3d waypoints[NAV_DEBUG_MAX_WAYPOINTS];
};

static boolean g_nav_debug_overlay = FALSE;
static struct halo_nav_debug_path g_nav_debug_paths[NAV_DEBUG_MAX_PATHS];
static long g_nav_debug_path_cursor = 0;

/* ---------- math helpers */

static real nav_dist2d(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x;
	real dy = a->y - b->y;
	return (real)sqrt(dx * dx + dy * dy);
}

static real nav_dist3d(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x;
	real dy = a->y - b->y;
	real dz = a->z - b->z;
	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

/* 2D signed triangle area * 2 (cross product in XY plane).
   Positive when c is to the left of directed segment a -> b. */
static real nav_tri_area2d(real_point3d const *a, real_point3d const *b, real_point3d const *c)
{
	real ax = b->x - a->x;
	real ay = b->y - a->y;
	real bx = c->x - a->x;
	real by = c->y - a->y;
	return bx * ay - ax * by;
}

static boolean nav_vequal2d(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x;
	real dy = a->y - b->y;
	return (dx * dx + dy * dy) < (0.001f * 0.001f);
}

/* Shrinks a portal segment (left, right) inward by margin so string-pulled
   paths keep `margin` clearance from corners. */
static void nav_shrink_portal(
	real_point3d const *in_left,
	real_point3d const *in_right,
	real margin,
	real_point3d *out_left,
	real_point3d *out_right)
{
	real dx = in_right->x - in_left->x;
	real dy = in_right->y - in_left->y;
	real dz = in_right->z - in_left->z;
	real len = (real)sqrt(dx * dx + dy * dy);
	real t;

	if (len <= 0.01f || margin <= 0.0f)
	{
		*out_left = *in_left;
		*out_right = *in_right;
		return;
	}
	if (margin * 2.2f >= len)
	{
		out_left->x = out_right->x = 0.5f * (in_left->x + in_right->x);
		out_left->y = out_right->y = 0.5f * (in_left->y + in_right->y);
		out_left->z = out_right->z = 0.5f * (in_left->z + in_right->z);
		return;
	}
	t = margin / len;
	out_left->x = in_left->x + dx * t;
	out_left->y = in_left->y + dy * t;
	out_left->z = in_left->z + dz * t;
	out_right->x = in_right->x - dx * t;
	out_right->y = in_right->y - dy * t;
	out_right->z = in_right->z - dz * t;
}

/* ---------- polygon & BSP extraction */

static long nav_extract_surface_corners(
	struct collision_bsp const *bsp,
	long surface_index,
	real_point3d *corners,
	long max_corners)
{
	struct collision_surface const *surface;
	long edge_index, count = 0;

	if (surface_index < 0 || surface_index >= bsp->surfaces.count)
		return 0;
	surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct collision_surface);
	edge_index = surface->first_edge_index;
	do
	{
		struct collision_edge const *edge;
		boolean right;

		if (edge_index < 0 || edge_index >= bsp->edges.count)
			return 0;
		edge = TAG_BLOCK_GET_ELEMENT(&bsp->edges, edge_index, struct collision_edge);
		right = edge->surface_indices[1] == surface_index;
		if (!VALID_INDEX(edge->vertex_indices[right], bsp->vertices.count))
			return 0;
		if (count < max_corners)
			corners[count++] = TAG_BLOCK_GET_ELEMENT(&bsp->vertices, edge->vertex_indices[right], struct collision_vertex)->point;
		edge_index = edge->edge_indices[right];
	} while (edge_index != surface->first_edge_index && count < max_corners);
	return count;
}

static void nav_add_poly_link(
	struct halo_nav_poly *poly,
	long neighbor_poly,
	real_point3d const *p0,
	real_point3d const *p1,
	real cost,
	short offmesh_type,
	short offmesh_index)
{
	struct halo_nav_link *link;
	long i;

	if (poly->link_count >= NAV_MAX_LINKS_PER_POLY || neighbor_poly < 0)
		return;
	for (i = 0; i < poly->link_count; i++)
	{
		if (poly->links[i].neighbor_poly == neighbor_poly && poly->links[i].offmesh_type == offmesh_type)
			return;
	}
	link = &poly->links[poly->link_count++];
	link->neighbor_poly = neighbor_poly;
	/* Order (left, right) so that viewing from poly->center toward the portal,
	   left is on the left and right is on the right (nav_tri_area2d >= 0). */
	if (nav_tri_area2d(&poly->center, p0, p1) >= 0.0f)
	{
		link->left = *p0;
		link->right = *p1;
	}
	else
	{
		link->left = *p1;
		link->right = *p0;
	}
	link->width = nav_dist2d(p0, p1);
	link->cost = cost > 0.01f ? cost : 0.01f;
	link->offmesh_type = offmesh_type;
	link->offmesh_index = offmesh_index;
}

/* Finds the polygon containing or nearest to `point` (preferring `surface_index`
   if valid). */
static long nav_find_closest_poly(real_point3d const *point, long surface_index, real min_clearance)
{
	long i, best_poly = NONE;
	real best_score = 1e30f;

	if (surface_index >= 0 && surface_index < g_nav.surface_to_poly_count)
	{
		long poly_idx = g_nav.surface_to_poly[surface_index];
		if (poly_idx != NONE && poly_idx >= 0 && poly_idx < g_nav.poly_count)
		{
			struct halo_nav_poly const *poly = &g_nav.polys[poly_idx];
			if (!poly->blocked && poly->clearance_radius >= min_clearance)
			{
				/* Check if no Forge polygon is directly above/closer to the point */
				if (fabs(poly->center.z - point->z) < 1.2f)
					return poly_idx;
			}
		}
	}

	for (i = 0; i < g_nav.poly_count; i++)
	{
		struct halo_nav_poly const *poly = &g_nav.polys[i];
		real d2d, dz, score;

		if (poly->blocked || poly->clearance_radius < min_clearance)
			continue;
		dz = (real)fabs(poly->center.z - point->z);
		if (dz > 4.0f)
			continue;
		d2d = nav_dist2d(&poly->center, point);
		if (d2d > poly->reach + 4.0f)
			continue;
		score = d2d + dz * 2.0f;
		if (min_clearance >= 0.7f && !poly->vehicle_passable)
			score += 3.5f;
		if (score < best_score)
		{
			best_score = score;
			best_poly = i;
		}
	}
	return best_poly;
}

/* Computes a hash of placed scenery & machines so the navmesh automatically
   updates when Forge objects are spawned, moved, or deleted. */
static long nav_compute_object_hash(void)
{
	struct object_iterator iterator;
	unsigned long hash = 2166136261UL;

	if (!object_header_data)
		return 0;
	object_iterator_new(&iterator, _object_mask_scenery | _object_mask_machine, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *obj = object_get(iterator.index);
		long qx = (long)(obj->object.bounding_sphere_center.x * 10.0f);
		long qy = (long)(obj->object.bounding_sphere_center.y * 10.0f);
		long qz = (long)(obj->object.bounding_sphere_center.z * 10.0f);

		hash = (hash ^ (unsigned long)obj->definition_index) * 16777619UL;
		hash = (hash ^ (unsigned long)qx) * 16777619UL;
		hash = (hash ^ (unsigned long)qy) * 16777619UL;
		hash = (hash ^ (unsigned long)qz) * 16777619UL;
	}
	return (long)(hash & 0x7FFFFFFFUL);
}

/* Scans scenery and machines for:
   1. Blocking obstacles standing across BSP polygons at knee/chest height.
   2. Walkable top/ramp surfaces on large Forge scenery/structures, adding
      Dynamic Forge Polygons and linking them to nearby BSP/Forge polygons. */
static void nav_apply_dynamic_objects(struct collision_bsp const *bsp)
{
	struct object_iterator iterator;
	unsigned long obj_flags = FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_back_facing_surfaces_bit) |
		FLAG(_collision_test_objects_bit) |
		FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit);
	long i;

	if (!object_header_data)
		return;

	for (i = 0; i < g_nav.bsp_poly_count; i++)
		g_nav.polys[i].blocked = 0;
	g_nav.poly_count = g_nav.bsp_poly_count;
	g_nav.forge_poly_count = 0;

	object_iterator_new(&iterator, _object_mask_scenery | _object_mask_machine, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *obj = object_get(iterator.index);
		real radius = obj->object.bounding_sphere_radius;
		real_point3d center = obj->object.bounding_sphere_center;

		if (radius < 0.25f || radius > 80.0f)
			continue;

		/* 1. Check if this object is a walkable Forge platform/ramp/bridge
		   by sampling a grid of downward rays onto its bounding box. */
		if (radius >= 0.8f && g_nav.poly_count < NAV_MAX_POLYS - 8)
		{
			real step = radius > 3.0f ? 1.5f : 1.0f;
			real half = MIN(radius * 0.85f, 6.0f);
			real dx, dy;

			for (dx = -half; dx <= half && g_nav.poly_count < NAV_MAX_POLYS; dx += step)
			{
				for (dy = -half; dy <= half && g_nav.poly_count < NAV_MAX_POLYS; dy += step)
				{
					struct collision_result hit;
					real_point3d probe;
					real_vector3d down = { 0.0f, 0.0f, -(radius * 2.0f + 2.0f) };

					probe.x = center.x + dx;
					probe.y = center.y + dy;
					probe.z = center.z + radius + 0.5f;
					if (collision_test_vector(obj_flags, &probe, &down, NONE, &hit) &&
						hit.type == _collision_result_object &&
						hit.object_index == iterator.index &&
						hit.plane.n.k >= NAV_MIN_WALKABLE_NORMAL_Z)
					{
						/* Check headroom above the Forge surface */
						struct collision_result ceil_hit;
						real_point3d head_start = hit.point;
						real_vector3d up_vec = { 0.0f, 0.0f, NAV_CEILING_CLEARANCE };
						long poly_idx;
						struct halo_nav_poly *fpoly;
						real h = step * 0.5f;

						head_start.z += 0.05f;
						if (collision_test_vector(obj_flags | FLAG(_collision_test_structure_bit),
							&head_start, &up_vec, NONE, &ceil_hit))
						{
							continue;
						}
						/* Synthesize a walkable Forge polygon patch */
						poly_idx = g_nav.poly_count++;
						g_nav.forge_poly_count++;
						fpoly = &g_nav.polys[poly_idx];
						memset(fpoly, 0, sizeof(*fpoly));
						fpoly->surface_index = NONE;
						fpoly->center = hit.point;
						fpoly->reach = h * 1.42f;
						fpoly->clearance_radius = h;
						fpoly->normal_z = hit.plane.n.k;
						fpoly->corner_count = 4;
						fpoly->is_forge_surface = 1;
						fpoly->vehicle_passable = (hit.plane.n.k >= NAV_MIN_VEHICLE_NORMAL_Z && h >= 0.65f) ? 1 : 0;
						fpoly->corners[0].x = hit.point.x - h;
						fpoly->corners[0].y = hit.point.y - h;
						fpoly->corners[0].z = hit.point.z;
						fpoly->corners[1].x = hit.point.x + h;
						fpoly->corners[1].y = hit.point.y - h;
						fpoly->corners[1].z = hit.point.z;
						fpoly->corners[2].x = hit.point.x + h;
						fpoly->corners[2].y = hit.point.y + h;
						fpoly->corners[2].z = hit.point.z;
						fpoly->corners[3].x = hit.point.x - h;
						fpoly->corners[3].y = hit.point.y + h;
						fpoly->corners[3].z = hit.point.z;
					}
				}
			}
		}

		/* 2. Check if this object blocks any underlying BSP walkable polygons
		   at knee/chest height (unless the polygon is a ramp onto it). */
		for (i = 0; i < g_nav.bsp_poly_count; i++)
		{
			struct halo_nav_poly *poly = &g_nav.polys[i];
			struct collision_result hit;
			real_point3d start;
			real_vector3d up = { 0.0f, 0.0f, 1.4f };

			if (poly->blocked)
				continue;
			if (nav_dist3d(&poly->center, &center) > radius + poly->reach + 0.5f)
				continue;
			start = poly->center;
			start.z += 0.1f;
			if (collision_test_vector(obj_flags, &start, &up, NONE, &hit) &&
				hit.type == _collision_result_object &&
				hit.object_index == iterator.index &&
				hit.plane.n.k < NAV_MIN_WALKABLE_NORMAL_Z)
			{
				poly->blocked = 1;
			}
		}
	}

	/* 3. Link Dynamic Forge Polygons to adjacent Forge Polygons and nearby BSP Polygons */
	for (i = g_nav.bsp_poly_count; i < g_nav.poly_count; i++)
	{
		struct halo_nav_poly *fpoly = &g_nav.polys[i];
		long j, closest_bsp_surface = NONE;
		real closest_bsp_dist = 1e30f;

		for (j = 0; j < g_nav.poly_count; j++)
		{
			struct halo_nav_poly *other = &g_nav.polys[j];
			real d2d, dz;

			if (i == j || other->blocked)
				continue;
			dz = (real)fabs(fpoly->center.z - other->center.z);
			d2d = nav_dist2d(&fpoly->center, &other->center);
			if (j < g_nav.bsp_poly_count && d2d + dz < closest_bsp_dist)
			{
				closest_bsp_dist = d2d + dz;
				closest_bsp_surface = other->surface_index;
			}
			if (dz <= NAV_STEP_HEIGHT * 1.5f && d2d <= fpoly->reach + other->reach + 0.6f)
			{
				real_point3d mid, p0, p1;
				real dx = other->center.x - fpoly->center.x;
				real dy = other->center.y - fpoly->center.y;
				real len = (real)sqrt(dx * dx + dy * dy);
				real px = len > 0.01f ? -dy / len * 0.4f : 0.4f;
				real py = len > 0.01f ? dx / len * 0.4f : 0.0f;
				real cost = nav_dist3d(&fpoly->center, &other->center);

				mid.x = 0.5f * (fpoly->center.x + other->center.x);
				mid.y = 0.5f * (fpoly->center.y + other->center.y);
				mid.z = 0.5f * (fpoly->center.z + other->center.z);
				p0.x = mid.x + px;
				p0.y = mid.y + py;
				p0.z = mid.z;
				p1.x = mid.x - px;
				p1.y = mid.y - py;
				p1.z = mid.z;
				nav_add_poly_link(fpoly, j, &p0, &p1, cost, _halo_nav_offmesh_none, NONE);
				nav_add_poly_link(other, i, &p0, &p1, cost, _halo_nav_offmesh_none, NONE);
			}
		}
		fpoly->surface_index = closest_bsp_surface;
	}
	(void)bsp;
}

/* Registers Off-Mesh Connections:
   1. Teleporters (from scenario->netgame_flags)
   2. Climbable Ladders (from collision_bsp climbable surfaces)
   3. One-Way Ledge Jump-Downs (from elevated polygon boundary edges) */
static void nav_build_offmesh_connections(struct scenario const *scenario, struct collision_bsp const *bsp)
{
	long i;

	g_nav.offmesh_count = 0;

	/* 1. Scenario Teleporters */
	if (scenario && scenario->netgame_flags.count > 0)
	{
		for (i = 0; i < scenario->netgame_flags.count && g_nav.offmesh_count < NAV_MAX_OFFMESH; i++)
		{
			struct scenario_netgame_flag const *from_flag =
				TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, i, struct scenario_netgame_flag);
			long j;

			if (from_flag->type != _netgame_flag_teleporter_source)
				continue;
			for (j = 0; j < scenario->netgame_flags.count; j++)
			{
				struct scenario_netgame_flag const *to_flag =
					TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, j, struct scenario_netgame_flag);

				if (to_flag->type == _netgame_flag_teleporter_target &&
					to_flag->team_index == from_flag->team_index)
				{
					long start_poly = nav_find_closest_poly(&from_flag->position, NONE, NAV_BIPED_RADIUS);
					long end_poly = nav_find_closest_poly(&to_flag->position, NONE, NAV_BIPED_RADIUS);

					if (start_poly != NONE && end_poly != NONE && start_poly != end_poly)
					{
						short om_idx = (short)g_nav.offmesh_count++;
						struct halo_nav_offmesh *om = &g_nav.offmesh[om_idx];

						om->type = _halo_nav_offmesh_teleporter;
						om->start_poly = start_poly;
						om->end_poly = end_poly;
						om->start_point = from_flag->position;
						om->end_point = to_flag->position;
						om->start_surface = g_nav.polys[start_poly].surface_index;
						om->end_surface = g_nav.polys[end_poly].surface_index;
						om->cost = 2.0f;
						nav_add_poly_link(&g_nav.polys[start_poly], end_poly,
							&from_flag->position, &from_flag->position,
							om->cost, _halo_nav_offmesh_teleporter, om_idx);
					}
					break;
				}
			}
		}
	}

	/* 2. Climbable Ladders */
	for (i = 0; i < bsp->surfaces.count && g_nav.offmesh_count < NAV_MAX_OFFMESH; i++)
	{
		struct collision_surface const *surface =
			TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, i, struct collision_surface);
		real_point3d corners[NAV_MAX_CORNERS];
		real_vector3d surf_normal;
		long corner_count, c;
		real min_z = 1e30f, max_z = -1e30f;
		real_point3d foot = { 0.0f, 0.0f, 0.0f }, top = { 0.0f, 0.0f, 0.0f };
		long start_poly, end_poly;

		if (!TEST_FLAG(surface->flags, NAV_CLIMBABLE_BIT))
			continue;
		corner_count = nav_extract_surface_corners(bsp, i, corners, NAV_MAX_CORNERS);
		if (corner_count < 3)
			continue;
		{
			real ux = corners[1].x - corners[0].x;
			real uy = corners[1].y - corners[0].y;
			real uz = corners[1].z - corners[0].z;
			real vx = corners[2].x - corners[0].x;
			real vy = corners[2].y - corners[0].y;
			real vz = corners[2].z - corners[0].z;
			real nlen;

			surf_normal.i = uy * vz - uz * vy;
			surf_normal.j = uz * vx - ux * vz;
			surf_normal.k = ux * vy - uy * vx;
			nlen = (real)sqrt(surf_normal.i * surf_normal.i + surf_normal.j * surf_normal.j + surf_normal.k * surf_normal.k);
			if (nlen > 0.0001f)
			{
				surf_normal.i /= nlen;
				surf_normal.j /= nlen;
				surf_normal.k /= nlen;
			}
		}
		for (c = 0; c < corner_count; c++)
		{
			foot.x += corners[c].x / (real)corner_count;
			foot.y += corners[c].y / (real)corner_count;
			if (corners[c].z < min_z)
				min_z = corners[c].z;
			if (corners[c].z > max_z)
				max_z = corners[c].z;
		}
		if (max_z - min_z < 0.8f)
			continue;
		foot.z = min_z;
		top.x = foot.x - surf_normal.i * 0.45f;
		top.y = foot.y - surf_normal.j * 0.45f;
		top.z = max_z + 0.1f;
		foot.x += surf_normal.i * 0.35f;
		foot.y += surf_normal.j * 0.35f;
		start_poly = nav_find_closest_poly(&foot, NONE, NAV_BIPED_RADIUS);
		end_poly = nav_find_closest_poly(&top, NONE, NAV_BIPED_RADIUS);
		if (start_poly != NONE && end_poly != NONE && start_poly != end_poly &&
			g_nav.polys[end_poly].center.z - g_nav.polys[start_poly].center.z >= 0.7f)
		{
			short om_idx = (short)g_nav.offmesh_count++;
			struct halo_nav_offmesh *om = &g_nav.offmesh[om_idx];

			om->type = _halo_nav_offmesh_ladder;
			om->start_poly = start_poly;
			om->end_poly = end_poly;
			om->start_point = g_nav.polys[start_poly].center;
			om->end_point = g_nav.polys[end_poly].center;
			om->start_surface = g_nav.polys[start_poly].surface_index;
			om->end_surface = g_nav.polys[end_poly].surface_index;
			om->cost = (max_z - min_z) * 1.5f;
			nav_add_poly_link(&g_nav.polys[start_poly], end_poly,
				&om->start_point, &om->start_point,
				om->cost, _halo_nav_offmesh_ladder, om_idx);
		}
	}

	/* 3. One-Way Ledge Jump-Downs (0.5..2.2 wu drop onto a lower polygon) */
	for (i = 0; i < g_nav.poly_count && g_nav.offmesh_count < NAV_MAX_OFFMESH; i++)
	{
		struct halo_nav_poly *poly = &g_nav.polys[i];
		long j;

		if (poly->blocked || poly->link_count >= NAV_MAX_LINKS_PER_POLY)
			continue;
		/* Sample every 4th polygon to keep jump-down links compact and fast */
		if ((i & 3) != 0 && !poly->is_forge_surface)
			continue;
		for (j = 0; j < g_nav.poly_count && g_nav.offmesh_count < NAV_MAX_OFFMESH; j++)
		{
			struct halo_nav_poly const *below = &g_nav.polys[j];
			real drop, d2d;

			if (i == j || below->blocked)
				continue;
			drop = poly->center.z - below->center.z;
			if (drop < 0.55f || drop > 2.2f)
				continue;
			d2d = nav_dist2d(&poly->center, &below->center);
			if (d2d > poly->reach + below->reach + 0.6f)
				continue;
			{
				short om_idx = (short)g_nav.offmesh_count++;
				struct halo_nav_offmesh *om = &g_nav.offmesh[om_idx];

				om->type = _halo_nav_offmesh_jump_down;
				om->start_poly = i;
				om->end_poly = j;
				om->start_point = poly->center;
				om->end_point = below->center;
				om->start_surface = poly->surface_index;
				om->end_surface = below->surface_index;
				om->cost = d2d + drop * 1.5f;
				nav_add_poly_link(poly, j, &poly->center, &poly->center,
					om->cost, _halo_nav_offmesh_jump_down, om_idx);
				break;
			}
		}
	}
}

/* Builds the navigation mesh for the active structure BSP. */
static boolean nav_build_locked(void)
{
	struct scenario *scenario = global_scenario_try_and_get();
	struct structure_bsp *structure = global_structure_bsp_get();
	struct collision_bsp const *bsp;
	long s, e;

	if (!scenario || !structure || structure->collision_bsp.count <= 0 ||
		structure->pathfinding_surfaces.count <= 0)
	{
		return FALSE;
	}
	bsp = TAG_BLOCK_GET_ELEMENT(&structure->collision_bsp, 0, struct collision_bsp);
	if (bsp->surfaces.count <= 0 || bsp->edges.count <= 0 || bsp->vertices.count <= 0)
		return FALSE;

	/* Check if already built for this BSP */
	if (g_nav.built && !g_nav.dirty && g_nav.scenario == scenario && g_nav.structure == structure)
	{
		long now = game_time_initialized() ? game_time_get() : 0;
		if (now - g_nav.last_object_check_tick >= 60)
		{
			long obj_hash = nav_compute_object_hash();
			g_nav.last_object_check_tick = now;
			if (obj_hash != g_nav.last_object_hash)
			{
				g_nav.last_object_hash = obj_hash;
				nav_apply_dynamic_objects(bsp);
			}
		}
		return TRUE;
	}

	if (!g_nav.polys)
		g_nav.polys = (struct halo_nav_poly *)calloc(NAV_MAX_POLYS, sizeof(struct halo_nav_poly));
	if (!g_nav.ws)
		g_nav.ws = (struct halo_nav_astar_workspace *)calloc(1, sizeof(struct halo_nav_astar_workspace));
	if (g_nav.surface_to_poly_count < bsp->surfaces.count)
	{
		free(g_nav.surface_to_poly);
		g_nav.surface_to_poly = (long *)malloc((size_t)bsp->surfaces.count * sizeof(long));
		g_nav.surface_to_poly_count = g_nav.surface_to_poly ? bsp->surfaces.count : 0;
	}
	if (!g_nav.polys || !g_nav.ws || !g_nav.surface_to_poly)
		return FALSE;

	for (s = 0; s < bsp->surfaces.count; s++)
		g_nav.surface_to_poly[s] = NONE;

	g_nav.poly_count = 0;
	g_nav.bsp_poly_count = 0;
	g_nav.forge_poly_count = 0;
	g_nav.offmesh_count = 0;

	/* 1. Extract walkable BSP surfaces as polygons */
	{
		byte const *pf_bytes = (byte const *)structure->pathfinding_surfaces.address;
		long pf_count = structure->pathfinding_surfaces.count;

		for (s = 0; s < bsp->surfaces.count && g_nav.poly_count < NAV_MAX_POLYS - 256; s++)
		{
			real_point3d corners[NAV_MAX_CORNERS];
			long corner_count, c, poly_idx;
			struct halo_nav_poly *poly;

			if (s >= pf_count || !TEST_FLAG(pf_bytes[s], NAV_WALKABLE_BIT))
				continue;
			corner_count = nav_extract_surface_corners(bsp, s, corners, NAV_MAX_CORNERS);
			if (corner_count < 3)
				continue;

			poly_idx = g_nav.poly_count++;
			g_nav.bsp_poly_count++;
			g_nav.surface_to_poly[s] = poly_idx;
			poly = &g_nav.polys[poly_idx];
			memset(poly, 0, sizeof(*poly));
			poly->surface_index = s;
			poly->corner_count = (unsigned char)corner_count;
			for (c = 0; c < corner_count; c++)
			{
				poly->corners[c] = corners[c];
				poly->center.x += corners[c].x / (real)corner_count;
				poly->center.y += corners[c].y / (real)corner_count;
				poly->center.z += corners[c].z / (real)corner_count;
			}
			for (c = 0; c < corner_count; c++)
			{
				real d = nav_dist3d(&poly->center, &corners[c]);
				if (d > poly->reach)
					poly->reach = d;
			}
			{
				real ux = corners[1].x - corners[0].x;
				real uy = corners[1].y - corners[0].y;
				real uz = corners[1].z - corners[0].z;
				real vx = corners[2].x - corners[0].x;
				real vy = corners[2].y - corners[0].y;
				real vz = corners[2].z - corners[0].z;
				real nx = uy * vz - uz * vy;
				real ny = uz * vx - ux * vz;
				real nz = ux * vy - uy * vx;
				real nlen = (real)sqrt(nx * nx + ny * ny + nz * nz);
				poly->normal_z = nlen > 0.0001f ? (real)fabs(nz / nlen) : 1.0f;
			}
			poly->clearance_radius = 2.5f;
			poly->vehicle_passable = poly->normal_z >= NAV_MIN_VEHICLE_NORMAL_Z ? 1 : 0;
		}
	}

	/* 2. Build portal links across shared BSP edges */
	for (e = 0; e < bsp->edges.count; e++)
	{
		struct collision_edge const *edge = TAG_BLOCK_GET_ELEMENT(&bsp->edges, e, struct collision_edge);
		long s0 = edge->surface_indices[0];
		long s1 = edge->surface_indices[1];
		long p0 = (s0 >= 0 && s0 < bsp->surfaces.count) ? g_nav.surface_to_poly[s0] : NONE;
		long p1 = (s1 >= 0 && s1 < bsp->surfaces.count) ? g_nav.surface_to_poly[s1] : NONE;
		real_point3d v0, v1;

		if (!VALID_INDEX(edge->vertex_indices[0], bsp->vertices.count) ||
			!VALID_INDEX(edge->vertex_indices[1], bsp->vertices.count))
		{
			continue;
		}
		v0 = TAG_BLOCK_GET_ELEMENT(&bsp->vertices, edge->vertex_indices[0], struct collision_vertex)->point;
		v1 = TAG_BLOCK_GET_ELEMENT(&bsp->vertices, edge->vertex_indices[1], struct collision_vertex)->point;

		if (p0 != NONE && p1 != NONE)
		{
			real cost = nav_dist3d(&g_nav.polys[p0].center, &g_nav.polys[p1].center);
			nav_add_poly_link(&g_nav.polys[p0], p1, &v0, &v1, cost, _halo_nav_offmesh_none, NONE);
			nav_add_poly_link(&g_nav.polys[p1], p0, &v0, &v1, cost, _halo_nav_offmesh_none, NONE);
		}
		else if (p0 != NONE || p1 != NONE)
		{
			/* Boundary edge (wall or cliff): update clearance radius of the polygon */
			long p = p0 != NONE ? p0 : p1;
			real_point3d mid;
			real d;

			mid.x = 0.5f * (v0.x + v1.x);
			mid.y = 0.5f * (v0.y + v1.y);
			mid.z = 0.5f * (v0.z + v1.z);
			d = nav_dist2d(&g_nav.polys[p].center, &mid);
			if (d + 0.15f < g_nav.polys[p].clearance_radius)
			{
				g_nav.polys[p].clearance_radius = MAX(NAV_BIPED_RADIUS, d + 0.15f);
				if (g_nav.polys[p].clearance_radius < 0.85f)
					g_nav.polys[p].vehicle_passable = 0;
			}
		}
	}

	/* 3. Apply Dynamic Forge Scenery/Machine Obstacles & Walkable Surfaces */
	g_nav.last_object_hash = nav_compute_object_hash();
	g_nav.last_object_check_tick = game_time_initialized() ? game_time_get() : 0;
	nav_apply_dynamic_objects(bsp);

	/* 4. Register Off-Mesh Connections (teleporters, ladders, ledge drops) */
	nav_build_offmesh_connections(scenario, bsp);

	g_nav.scenario = scenario;
	g_nav.structure = structure;
	g_nav.built = TRUE;
	g_nav.dirty = FALSE;
	platform_log("navmesh: built %ld polys (%ld bsp + %ld forge), %ld off-mesh links",
		g_nav.poly_count, g_nav.bsp_poly_count, g_nav.forge_poly_count, g_nav.offmesh_count);
	return TRUE;
}

/* ---------- Binary Min-Heap for Detour Polygon A* */

static void nav_heap_push(struct halo_nav_astar_workspace *ws, long poly_idx)
{
	long i = ws->heap_size++;
	real f = ws->f_score[poly_idx];

	while (i > 0)
	{
		long parent = (i - 1) >> 1;
		long parent_poly = ws->heap[parent];
		if (ws->f_score[parent_poly] <= f)
			break;
		ws->heap[i] = parent_poly;
		i = parent;
	}
	ws->heap[i] = poly_idx;
}

static long nav_heap_pop(struct halo_nav_astar_workspace *ws)
{
	long result = ws->heap[0];
	long last = ws->heap[--ws->heap_size];
	real f = ws->f_score[last];
	long i = 0;

	while ((i * 2 + 1) < ws->heap_size)
	{
		long left = i * 2 + 1;
		long right = left + 1;
		long smallest = left;

		if (right < ws->heap_size && ws->f_score[ws->heap[right]] < ws->f_score[ws->heap[left]])
			smallest = right;
		if (f <= ws->f_score[ws->heap[smallest]])
			break;
		ws->heap[i] = ws->heap[smallest];
		i = smallest;
	}
	ws->heap[i] = last;
	return result;
}

/* ---------- Simple Stupid Funnel Algorithm (Mikko Mononen / Detour) */

static long nav_funnel_smooth(
	struct halo_nav_astar_workspace const *ws,
	long portal_count,
	struct halo_nav_waypoint *out_waypoints,
	long max_waypoints)
{
	real_point3d portal_apex, portal_left, portal_right;
	long apex_index = 0, left_index = 0, right_index = 0;
	long i, count = 0;

	if (portal_count <= 0 || max_waypoints <= 0)
		return 0;

	portal_apex = ws->portal_left[0];
	portal_left = ws->portal_left[0];
	portal_right = ws->portal_right[0];

	for (i = 1; i < portal_count && count < max_waypoints; i++)
	{
		real_point3d const *left = &ws->portal_left[i];
		real_point3d const *right = &ws->portal_right[i];

		/* If this portal is an off-mesh transition (teleporter/ladder/jump),
		   flush the current funnel apex directly to the off-mesh start! */
		if (ws->portal_offmesh_type[i] != _halo_nav_offmesh_none)
		{
			out_waypoints[count].point = *left;
			out_waypoints[count].surface_index = ws->portal_surface[i];
			out_waypoints[count].offmesh_type = ws->portal_offmesh_type[i];
			out_waypoints[count].offmesh_index = ws->portal_offmesh_index[i];
			count++;
			portal_apex = *right;
			portal_left = portal_apex;
			portal_right = portal_apex;
			apex_index = left_index = right_index = i;
			continue;
		}

		/* Update right vertex */
		if (nav_tri_area2d(&portal_apex, &portal_right, right) <= 0.0f)
		{
			if (nav_vequal2d(&portal_apex, &portal_right) ||
				nav_tri_area2d(&portal_apex, &portal_left, right) > 0.0f)
			{
				portal_right = *right;
				right_index = i;
			}
			else
			{
				/* Right over left: insert left as a corner waypoint */
				if (nav_dist2d(&portal_left, &ws->portal_left[0]) > 0.25f)
				{
					out_waypoints[count].point = portal_left;
					out_waypoints[count].surface_index = ws->portal_surface[left_index];
					out_waypoints[count].offmesh_type = _halo_nav_offmesh_none;
					out_waypoints[count].offmesh_index = NONE;
					count++;
					if (count >= max_waypoints)
						break;
				}
				portal_apex = portal_left;
				apex_index = left_index;
				portal_left = portal_apex;
				portal_right = portal_apex;
				left_index = apex_index;
				right_index = apex_index;
				i = apex_index;
				continue;
			}
		}

		/* Update left vertex */
		if (nav_tri_area2d(&portal_apex, &portal_left, left) >= 0.0f)
		{
			if (nav_vequal2d(&portal_apex, &portal_left) ||
				nav_tri_area2d(&portal_apex, &portal_right, left) < 0.0f)
			{
				portal_left = *left;
				left_index = i;
			}
			else
			{
				/* Left over right: insert right as a corner waypoint */
				if (nav_dist2d(&portal_right, &ws->portal_left[0]) > 0.25f)
				{
					out_waypoints[count].point = portal_right;
					out_waypoints[count].surface_index = ws->portal_surface[right_index];
					out_waypoints[count].offmesh_type = _halo_nav_offmesh_none;
					out_waypoints[count].offmesh_index = NONE;
					count++;
					if (count >= max_waypoints)
						break;
				}
				portal_apex = portal_right;
				apex_index = right_index;
				portal_left = portal_apex;
				portal_right = portal_apex;
				left_index = apex_index;
				right_index = apex_index;
				i = apex_index;
				continue;
			}
		}
	}

	/* Append final destination waypoint */
	if (count < max_waypoints && portal_count > 1)
	{
		real_point3d const *end_pt = &ws->portal_left[portal_count - 1];
		if (count == 0 || nav_dist2d(&out_waypoints[count - 1].point, end_pt) > 0.1f)
		{
			out_waypoints[count].point = *end_pt;
			out_waypoints[count].surface_index = ws->portal_surface[portal_count - 1];
			out_waypoints[count].offmesh_type = _halo_nav_offmesh_none;
			out_waypoints[count].offmesh_index = NONE;
			count++;
		}
	}
	return count;
}

/* Smooths sharp interior turns on vehicle paths (agent_radius >= 1.0f) by
   shifting corner waypoints outward along the interior angle bisector so
   wide vehicles (Warthogs, Scorpions, Wraiths, Ghosts) take sweeping arcs
   instead of clipping inner corners. */
static void nav_smooth_vehicle_path(
	real_point3d const *start_point,
	struct halo_nav_waypoint *waypoints,
	long wp_count,
	real agent_radius)
{
	long i;

	if (wp_count < 2 || agent_radius < 1.0f)
		return;
	for (i = 0; i < wp_count - 1; i++)
	{
		real_point3d const *prev = (i == 0) ? start_point : &waypoints[i - 1].point;
		real_point3d *curr = &waypoints[i].point;
		real_point3d const *next = &waypoints[i + 1].point;
		real ax = curr->x - prev->x, ay = curr->y - prev->y;
		real bx = next->x - curr->x, by = next->y - curr->y;
		real alen = (real)sqrt(ax * ax + ay * ay);
		real blen = (real)sqrt(bx * bx + by * by);
		real dot;

		if (alen < 0.8f || blen < 0.8f || waypoints[i].offmesh_type != _halo_nav_offmesh_none)
			continue;
		ax /= alen;
		ay /= alen;
		bx /= blen;
		by /= blen;
		dot = ax * bx + ay * by;
		if (dot < 0.75f)
		{
			/* Bisector pointing toward the outer side of the turn */
			real ox = bx - ax;
			real oy = by - ay;
			real olen = (real)sqrt(ox * ox + oy * oy);
			if (olen > 0.05f)
			{
				real shift = MIN(agent_radius * 0.55f, MIN(alen, blen) * 0.28f);
				real_point3d candidate = *curr;
				long poly_idx;

				candidate.x -= (ox / olen) * shift;
				candidate.y -= (oy / olen) * shift;
				poly_idx = nav_find_closest_poly(&candidate, waypoints[i].surface_index, agent_radius * 0.65f);
				if (poly_idx != NONE && !g_nav.polys[poly_idx].blocked)
				{
					curr->x = candidate.x;
					curr->y = candidate.y;
				}
			}
		}
	}
}

static void nav_record_debug_path(
	real_point3d const *start_point,
	real agent_radius,
	struct halo_nav_waypoint const *waypoints,
	long wp_count)
{
	struct halo_nav_debug_path *slot;
	long i, n;

	if (!start_point || !waypoints || wp_count <= 0)
		return;
	slot = &g_nav_debug_paths[g_nav_debug_path_cursor % NAV_DEBUG_MAX_PATHS];
	g_nav_debug_path_cursor = (g_nav_debug_path_cursor + 1) % NAV_DEBUG_MAX_PATHS;
	slot->valid = TRUE;
	slot->tick = game_time_initialized() ? game_time_get() : 0;
	slot->agent_radius = agent_radius;
	slot->start_point = *start_point;
	n = MIN(wp_count, (long)NAV_DEBUG_MAX_WAYPOINTS);
	slot->waypoint_count = n;
	for (i = 0; i < n; i++)
		slot->waypoints[i] = waypoints[i].point;
}

/* ---------- public API */

void halo_navmesh_mark_dirty(void)
{
	pthread_mutex_lock(&g_nav_mutex);
	g_nav.dirty = TRUE;
	pthread_mutex_unlock(&g_nav_mutex);
}

void halo_navmesh_dispose(void)
{
	pthread_mutex_lock(&g_nav_mutex);
	g_nav.built = FALSE;
	g_nav.dirty = FALSE;
	g_nav.scenario = NULL;
	g_nav.structure = NULL;
	g_nav.poly_count = 0;
	g_nav.bsp_poly_count = 0;
	g_nav.forge_poly_count = 0;
	g_nav.offmesh_count = 0;
	pthread_mutex_unlock(&g_nav_mutex);
}

boolean halo_navmesh_ensure_built(void)
{
	boolean ok;

	pthread_mutex_lock(&g_nav_mutex);
	ok = nav_build_locked();
	pthread_mutex_unlock(&g_nav_mutex);
	return ok;
}

long halo_navmesh_find_path(
	real_point3d const *start_point,
	long start_surface_index,
	real_point3d const *goal_point,
	long goal_surface_index,
	real agent_radius,
	boolean allow_offmesh,
	struct halo_nav_waypoint *out_waypoints,
	long max_waypoints,
	boolean *out_reached,
	real *out_total_distance)
{
	struct halo_nav_astar_workspace *ws;
	long start_poly, goal_poly, best_poly;
	real best_h = 1e30f;
	boolean reached = FALSE;
	long corridor_len = 0, portal_count = 0, wp_count = 0, i;
	real min_clearance;

	if (out_reached)
		*out_reached = FALSE;
	if (out_total_distance)
		*out_total_distance = 0.0f;
	if (!start_point || !goal_point || !out_waypoints || max_waypoints <= 0)
		return 0;

	pthread_mutex_lock(&g_nav_mutex);
	if (!nav_build_locked() || g_nav.poly_count <= 0)
	{
		pthread_mutex_unlock(&g_nav_mutex);
		return 0;
	}

	min_clearance = agent_radius > 0.5f ? agent_radius * 0.7f : NAV_BIPED_RADIUS;
	start_poly = nav_find_closest_poly(start_point, start_surface_index, min_clearance);
	goal_poly = nav_find_closest_poly(goal_point, goal_surface_index, min_clearance);
	if (start_poly == NONE || goal_poly == NONE)
	{
		pthread_mutex_unlock(&g_nav_mutex);
		return 0;
	}

	/* Trivial same-polygon path */
	if (start_poly == goal_poly)
	{
		out_waypoints[0].point = *goal_point;
		out_waypoints[0].surface_index = g_nav.polys[goal_poly].surface_index != NONE ?
			g_nav.polys[goal_poly].surface_index : goal_surface_index;
		out_waypoints[0].offmesh_type = _halo_nav_offmesh_none;
		out_waypoints[0].offmesh_index = NONE;
		if (out_reached)
			*out_reached = TRUE;
		if (out_total_distance)
			*out_total_distance = nav_dist3d(start_point, goal_point);
		pthread_mutex_unlock(&g_nav_mutex);
		return 1;
	}

	ws = g_nav.ws;
	memset(ws->state, 0, (size_t)g_nav.poly_count);
	ws->heap_size = 0;

	ws->g_score[start_poly] = 0.0f;
	ws->f_score[start_poly] = nav_dist3d(&g_nav.polys[start_poly].center, goal_point);
	ws->parent_poly[start_poly] = NONE;
	ws->parent_link[start_poly] = NONE;
	ws->state[start_poly] = 1;
	nav_heap_push(ws, start_poly);

	best_poly = start_poly;
	best_h = ws->f_score[start_poly];

	while (ws->heap_size > 0)
	{
		long curr = nav_heap_pop(ws);
		struct halo_nav_poly const *poly;
		long l;

		if (ws->state[curr] == 2)
			continue;
		ws->state[curr] = 2;

		if (curr == goal_poly)
		{
			reached = TRUE;
			best_poly = curr;
			break;
		}

		poly = &g_nav.polys[curr];
		for (l = 0; l < poly->link_count; l++)
		{
			struct halo_nav_link const *link = &poly->links[l];
			long next = link->neighbor_poly;
			struct halo_nav_poly const *next_poly;
			real tentative_g, h;

			if (next < 0 || next >= g_nav.poly_count || ws->state[next] == 2)
				continue;
			next_poly = &g_nav.polys[next];
			if (next_poly->blocked || next_poly->clearance_radius < min_clearance)
				continue;
			if (link->offmesh_type != _halo_nav_offmesh_none)
			{
				if (!allow_offmesh || agent_radius > 0.5f)
					continue;
			}
			else if (agent_radius > 0.5f && link->width < agent_radius * 1.4f)
			{
				continue;
			}

			{
				real step_cost = link->cost;
				if (agent_radius >= 1.0f)
				{
					/* Vehicle layer: penalize steep slopes and tight portals so
					   vehicles prefer wide roads, bridges, and flat canyon floors. */
					if (!next_poly->vehicle_passable)
						step_cost *= 2.4f;
					else if (next_poly->normal_z < 0.92f)
						step_cost *= (1.0f + (0.95f - next_poly->normal_z) * 2.5f);
					if (next_poly->clearance_radius < agent_radius * 1.15f)
						step_cost *= 1.35f;
				}
				tentative_g = ws->g_score[curr] + step_cost;
			}
			if (ws->state[next] == 0 || tentative_g < ws->g_score[next])
			{
				h = nav_dist3d(&next_poly->center, goal_point);
				ws->g_score[next] = tentative_g;
				ws->f_score[next] = tentative_g + h;
				ws->parent_poly[next] = curr;
				ws->parent_link[next] = (short)l;
				ws->state[next] = 1;
				nav_heap_push(ws, next);
				if (h < best_h)
				{
					best_h = h;
					best_poly = next;
				}
			}
		}
	}

	if (best_poly == start_poly && !reached)
	{
		pthread_mutex_unlock(&g_nav_mutex);
		return 0;
	}

	if (out_total_distance)
		*out_total_distance = ws->g_score[best_poly];

	/* Reconstruct polygon corridor from start_poly to best_poly */
	{
		long trace = best_poly;
		while (trace != NONE && corridor_len < NAV_MAX_CORRIDOR)
		{
			ws->corridor[corridor_len] = trace;
			ws->corridor_link[corridor_len] = ws->parent_link[trace];
			corridor_len++;
			trace = ws->parent_poly[trace];
		}
	}

	/* Build portal list in forward order (start -> best_poly) */
	ws->portal_left[0] = *start_point;
	ws->portal_right[0] = *start_point;
	ws->portal_surface[0] = start_surface_index;
	ws->portal_offmesh_type[0] = _halo_nav_offmesh_none;
	ws->portal_offmesh_index[0] = NONE;
	portal_count = 1;

	for (i = corridor_len - 2; i >= 0 && portal_count < NAV_MAX_PORTALS - 1; i--)
	{
		long prev_poly = ws->corridor[i + 1];
		long curr_poly = ws->corridor[i];
		short link_idx = ws->corridor_link[i];
		struct halo_nav_link const *link = &g_nav.polys[prev_poly].links[link_idx];

		if (link->offmesh_type != _halo_nav_offmesh_none && link->offmesh_index >= 0)
		{
			struct halo_nav_offmesh const *om = &g_nav.offmesh[link->offmesh_index];
			ws->portal_left[portal_count] = om->start_point;
			ws->portal_right[portal_count] = om->end_point;
			ws->portal_surface[portal_count] = om->start_surface;
			ws->portal_offmesh_type[portal_count] = om->type;
			ws->portal_offmesh_index[portal_count] = link->offmesh_index;
			portal_count++;
		}
		else
		{
			nav_shrink_portal(&link->left, &link->right, agent_radius * 0.8f,
				&ws->portal_left[portal_count], &ws->portal_right[portal_count]);
			ws->portal_surface[portal_count] = g_nav.polys[curr_poly].surface_index;
			ws->portal_offmesh_type[portal_count] = _halo_nav_offmesh_none;
			ws->portal_offmesh_index[portal_count] = NONE;
			portal_count++;
		}
	}

	/* Final destination portal */
	{
		real_point3d end_pt = reached ? *goal_point : g_nav.polys[best_poly].center;
		long end_surf = g_nav.polys[best_poly].surface_index != NONE ?
			g_nav.polys[best_poly].surface_index : goal_surface_index;

		ws->portal_left[portal_count] = end_pt;
		ws->portal_right[portal_count] = end_pt;
		ws->portal_surface[portal_count] = end_surf;
		ws->portal_offmesh_type[portal_count] = _halo_nav_offmesh_none;
		ws->portal_offmesh_index[portal_count] = NONE;
		portal_count++;
	}

	wp_count = nav_funnel_smooth(ws, portal_count, out_waypoints, max_waypoints);
	if (agent_radius >= 1.0f && wp_count >= 2)
		nav_smooth_vehicle_path(start_point, out_waypoints, wp_count, agent_radius);
	if (wp_count > 0)
		nav_record_debug_path(start_point, agent_radius, out_waypoints, wp_count);
	if (out_reached)
		*out_reached = reached;

	pthread_mutex_unlock(&g_nav_mutex);
	return wp_count;
}

boolean halo_navmesh_build_path_result(
	real_point3d const *start_point,
	long start_surface_index,
	real_point3d const *goal_point,
	long goal_surface_index,
	real agent_radius,
	boolean allow_offmesh,
	struct path_result *out_path,
	boolean *out_reached,
	real *out_total_distance)
{
	struct halo_nav_waypoint waypoints[32];
	boolean reached = FALSE;
	long wp_count, step_count, i;

	if (!out_path)
		return FALSE;
	csmemset(out_path, 0, sizeof(*out_path));

	wp_count = halo_navmesh_find_path(
		start_point, start_surface_index,
		goal_point, goal_surface_index,
		agent_radius, allow_offmesh,
		waypoints, 32, &reached, out_total_distance);
	if (wp_count <= 0)
		return FALSE;

	/* Stop at the first off-mesh transition so the caller walks to the
	   teleporter/ladder entrance first, then continues from its exit. */
	for (i = 0; i < wp_count; i++)
	{
		if (waypoints[i].offmesh_type != _halo_nav_offmesh_none)
		{
			wp_count = i + 1;
			reached = FALSE;
			break;
		}
	}

	step_count = MIN(wp_count, (long)MAXIMUM_SMOOTHED_PATH_STEPS);
	out_path->valid = TRUE;
	out_path->step_count = (char)step_count;
	out_path->step_index = 0;
	out_path->steps_finish_path = reached && (step_count == wp_count);
	for (i = 0; i < step_count; i++)
	{
		out_path->steps[i].point = waypoints[i].point;
		out_path->steps[i].surface_index = waypoints[i].surface_index != NONE ?
			waypoints[i].surface_index : goal_surface_index;
	}
	out_path->endpoint.point = out_path->steps[step_count - 1].point;
	out_path->endpoint.surface_index = out_path->steps[step_count - 1].surface_index;
	out_path->endpoint.target_radius = agent_radius;

	if (out_reached)
		*out_reached = reached;
	return TRUE;
}

boolean halo_navmesh_crowd_steer(
	long unit_index,
	real_point3d const *position,
	real desired_direction,
	real desired_throttle,
	real agent_radius,
	real *out_direction,
	real *out_throttle)
{
	struct object_iterator iterator;
	real vx, vy, avoid_x = 0.0f, avoid_y = 0.0f;
	long neighbors = 0;
	real steered_dir, steered_speed;

	if (out_direction)
		*out_direction = desired_direction;
	if (out_throttle)
		*out_throttle = desired_throttle;
	if (!position || desired_throttle <= 0.01f || !object_header_data)
		return FALSE;

	vx = (real)cos(desired_direction) * desired_throttle;
	vy = (real)sin(desired_direction) * desired_throttle;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while (object_iterator_next(&iterator))
	{
		struct unit_datum *other;
		real dx, dy, dz, dist, combined_r;

		if (iterator.index == unit_index)
			continue;
		other = unit_get(iterator.index);
		if (TEST_FLAG(other->object.damage_flags, _object_dead_bit) ||
			other->object.parent_object_index != NONE)
		{
			continue;
		}
		dz = (real)fabs(other->object.position.z - position->z);
		if (dz > 1.4f)
			continue;
		dx = other->object.position.x - position->x;
		dy = other->object.position.y - position->y;
		dist = (real)sqrt(dx * dx + dy * dy);
		combined_r = (agent_radius > 0.1f ? agent_radius : NAV_BIPED_RADIUS) * 2.0f + 0.55f;
		if (dist < 0.01f || dist > combined_r)
			continue;

		/* Compute RVO repulsive + right-hand tangential passing vector */
		{
			real nx = dx / dist;
			real ny = dy / dist;
			real dot = vx * nx + vy * ny;
			if (dot > -0.15f)
			{
				real weight = (combined_r - dist) / combined_r;
				/* Right-hand rule tangential component so two head-on agents
				   each step to their own right instead of oscillating */
				real tx = ny;
				real ty = -nx;
				if ((unit_index & 1) != (iterator.index & 1) && (vx * tx + vy * ty) < 0.0f)
				{
					tx = -ny;
					ty = nx;
				}
				avoid_x += (-nx * 0.65f + tx * 0.85f) * weight;
				avoid_y += (-ny * 0.65f + ty * 0.85f) * weight;
				neighbors++;
			}
		}
	}

	if (neighbors == 0)
		return FALSE;

	vx += avoid_x;
	vy += avoid_y;
	steered_speed = (real)sqrt(vx * vx + vy * vy);
	if (steered_speed < 0.05f)
		return FALSE;
	steered_dir = (real)atan2(vy, vx);

	/* Short wall feeler test: ensure RVO avoidance doesn't push into a wall */
	{
		struct collision_result hit;
		real_point3d chest = *position;
		real_vector3d feeler;
		unsigned long flags = FLAG(_collision_test_structure_bit) |
			FLAG(_collision_test_front_facing_surfaces_bit);

		chest.z += 0.55f;
		feeler.i = (real)cos(steered_dir) * 0.55f;
		feeler.j = (real)sin(steered_dir) * 0.55f;
		feeler.k = 0.0f;
		if (collision_test_vector(flags, &chest, &feeler, unit_index, &hit))
			return FALSE;
	}

	if (out_direction)
		*out_direction = steered_dir;
	if (out_throttle)
		*out_throttle = PIN(steered_speed, 0.4f, 1.0f);
	return TRUE;
}

boolean halo_navmesh_vehicle_convoy_steer(
	long vehicle_index,
	real_point3d const *position,
	real desired_direction,
	real desired_throttle,
	real vehicle_radius,
	real *out_direction,
	real *out_throttle)
{
	struct object_iterator iterator;
	real r = vehicle_radius > 0.8f ? vehicle_radius : 1.65f;
	real vx, vy, avoid_x = 0.0f, avoid_y = 0.0f;
	real throttle_scale = 1.0f;
	long adjustments = 0;
	unsigned long feeler_flags = FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_objects_bit) |
		FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit);

	if (out_direction)
		*out_direction = desired_direction;
	if (out_throttle)
		*out_throttle = desired_throttle;
	if (!position || fabs(desired_throttle) <= 0.01f || !object_header_data)
		return FALSE;

	vx = (real)cos(desired_direction);
	vy = (real)sin(desired_direction);

	/* 1. Multi-vehicle convoy spacing & friendly pedestrian avoidance */
	object_iterator_new(&iterator, _object_mask_vehicle | _object_mask_biped, 0);
	while (object_iterator_next(&iterator))
	{
		struct unit_datum *other;
		boolean is_vehicle;
		real dx, dy, dz, dist, detect_r;

		if (iterator.index == vehicle_index)
			continue;
		other = unit_get(iterator.index);
		if (TEST_FLAG(other->object.damage_flags, _object_dead_bit) ||
			other->object.parent_object_index != NONE)
		{
			continue;
		}
		is_vehicle = TEST_FLAG(_object_mask_vehicle, other->object.type);
		dz = (real)fabs(other->object.position.z - position->z);
		if (dz > 2.5f)
			continue;
		dx = other->object.position.x - position->x;
		dy = other->object.position.y - position->y;
		dist = (real)sqrt(dx * dx + dy * dy);
		detect_r = is_vehicle ? (r * 3.2f + 2.2f) : (r + 1.8f);
		if (dist < 0.05f || dist > detect_r)
			continue;

		{
			real nx = dx / dist;
			real ny = dy / dist;
			real forward_dot = vx * nx + vy * ny;
			if (forward_dot > 0.1f)
			{
				real weight = (detect_r - dist) / detect_r;
				/* Right-hand tangential passing vector */
				real tx = ny;
				real ty = -nx;
				if ((vx * tx + vy * ty) < -0.1f)
				{
					tx = -ny;
					ty = nx;
				}
				avoid_x += (-nx * 0.55f + tx * 1.10f) * weight;
				avoid_y += (-ny * 0.55f + ty * 1.10f) * weight;
				if (is_vehicle && forward_dot > 0.65f && dist < r * 2.4f + 1.2f)
				{
					/* Convoy pace matching: ease throttle behind lead vehicle */
					throttle_scale = MIN(throttle_scale, MAX(0.35f, dist / detect_r));
				}
				adjustments++;
			}
		}
	}

	/* 2. Wide Left/Right Forward Obstacle Feelers (prevent canyon & Forge wall wedging) */
	{
		struct collision_result hit_l, hit_r;
		real_point3d probe = *position;
		real feeler_len = r + 2.0f;
		real ang_l = desired_direction + 0.52f; /* +30 deg */
		real ang_r = desired_direction - 0.52f; /* -30 deg */
		real_vector3d vec_l, vec_r;
		boolean blocked_l, blocked_r;

		probe.z += 0.65f;
		vec_l.i = (real)cos(ang_l) * feeler_len;
		vec_l.j = (real)sin(ang_l) * feeler_len;
		vec_l.k = 0.0f;
		vec_r.i = (real)cos(ang_r) * feeler_len;
		vec_r.j = (real)sin(ang_r) * feeler_len;
		vec_r.k = 0.0f;

		blocked_l = collision_test_vector(feeler_flags, &probe, &vec_l, vehicle_index, &hit_l) &&
			hit_l.plane.n.k < NAV_MIN_WALKABLE_NORMAL_Z;
		blocked_r = collision_test_vector(feeler_flags, &probe, &vec_r, vehicle_index, &hit_r) &&
			hit_r.plane.n.k < NAV_MIN_WALKABLE_NORMAL_Z;

		if (blocked_l && !blocked_r)
		{
			avoid_x += (real)cos(desired_direction - 1.1f) * 0.85f;
			avoid_y += (real)sin(desired_direction - 1.1f) * 0.85f;
			adjustments++;
		}
		else if (blocked_r && !blocked_l)
		{
			avoid_x += (real)cos(desired_direction + 1.1f) * 0.85f;
			avoid_y += (real)sin(desired_direction + 1.1f) * 0.85f;
			adjustments++;
		}
		else if (blocked_l && blocked_r)
		{
			throttle_scale = MIN(throttle_scale, 0.5f);
			if (hit_l.t > hit_r.t)
			{
				avoid_x += (real)cos(desired_direction + 1.2f) * 0.95f;
				avoid_y += (real)sin(desired_direction + 1.2f) * 0.95f;
			}
			else
			{
				avoid_x += (real)cos(desired_direction - 1.2f) * 0.95f;
				avoid_y += (real)sin(desired_direction - 1.2f) * 0.95f;
			}
			adjustments++;
		}
	}

	if (adjustments == 0)
		return FALSE;

	vx += avoid_x;
	vy += avoid_y;
	if ((vx * vx + vy * vy) > 0.01f && out_direction)
		*out_direction = (real)atan2(vy, vx);
	if (out_throttle)
		*out_throttle = desired_throttle * throttle_scale;
	return TRUE;
}

long halo_navmesh_sample_points(
	real_point3d *out_points,
	long *out_surfaces,
	long max_points,
	real min_spacing)
{
	long i, j, count = 0;

	if (!out_points || max_points <= 0)
		return 0;

	pthread_mutex_lock(&g_nav_mutex);
	if (!nav_build_locked() || g_nav.poly_count <= 0)
	{
		pthread_mutex_unlock(&g_nav_mutex);
		return 0;
	}

	for (i = 0; i < g_nav.poly_count && count < max_points; i++)
	{
		struct halo_nav_poly const *poly = &g_nav.polys[i];
		boolean too_close = FALSE;

		if (poly->blocked || poly->link_count == 0)
			continue;
		for (j = 0; j < count; j++)
		{
			if (nav_dist3d(&poly->center, &out_points[j]) < min_spacing)
			{
				too_close = TRUE;
				break;
			}
		}
		if (too_close)
			continue;
		out_points[count] = poly->center;
		if (out_surfaces)
			out_surfaces[count] = poly->surface_index;
		count++;
	}

	pthread_mutex_unlock(&g_nav_mutex);
	return count;
}

/* ---------- 3D Navmesh & AI Tactical Debug Overlay */

void halo_navmesh_set_debug_overlay(boolean enabled)
{
	g_nav_debug_overlay = enabled ? TRUE : FALSE;
}

boolean halo_navmesh_get_debug_overlay(void)
{
	return g_nav_debug_overlay;
}

boolean halo_navmesh_toggle_debug_overlay(void)
{
	g_nav_debug_overlay = !g_nav_debug_overlay;
	return g_nav_debug_overlay;
}

void halo_navmesh_get_stats(struct halo_navmesh_stats *out_stats)
{
	long i, now;

	if (!out_stats)
		return;
	memset(out_stats, 0, sizeof(*out_stats));

	pthread_mutex_lock(&g_nav_mutex);
	nav_build_locked();
	out_stats->total_polys = g_nav.poly_count;
	out_stats->bsp_polys = g_nav.bsp_poly_count;
	out_stats->forge_polys = g_nav.forge_poly_count;
	out_stats->offmesh_links = g_nav.offmesh_count;
	for (i = 0; i < g_nav.poly_count; i++)
	{
		if (g_nav.polys[i].blocked)
			out_stats->blocked_polys++;
		else if (g_nav.polys[i].vehicle_passable)
			out_stats->vehicle_polys++;
	}
	now = game_time_initialized() ? game_time_get() : 0;
	for (i = 0; i < NAV_DEBUG_MAX_PATHS; i++)
	{
		if (g_nav_debug_paths[i].valid && (now - g_nav_debug_paths[i].tick) <= 180)
			out_stats->active_paths++;
	}
	pthread_mutex_unlock(&g_nav_mutex);
}

long halo_navmesh_get_offmesh_link(
	long index,
	real_point3d *out_start,
	real_point3d *out_end,
	short *out_type)
{
	long count;

	pthread_mutex_lock(&g_nav_mutex);
	count = g_nav.offmesh_count;
	if (index >= 0 && index < count)
	{
		if (out_start)
			*out_start = g_nav.offmesh[index].start_point;
		if (out_end)
			*out_end = g_nav.offmesh[index].end_point;
		if (out_type)
			*out_type = g_nav.offmesh[index].type;
	}
	pthread_mutex_unlock(&g_nav_mutex);
	return count;
}

void halo_navmesh_debug_render_pieces(
	real_point3d const *camera_pos,
	void (*draw_piece)(float const *from, float const *to, int kind))
{
	long i, c, now;
	real max_poly_dist = 36.0f;

	if (!g_nav_debug_overlay || !draw_piece)
		return;

	pthread_mutex_lock(&g_nav_mutex);
	if (!nav_build_locked() || g_nav.poly_count <= 0)
	{
		pthread_mutex_unlock(&g_nav_mutex);
		return;
	}

	/* 1. Render nearby walkable BSP polygons (Cyan = 0), Forge surface polygons
	      (Gold = 1), and Blocked obstacle polygons (Red/Orange = 2). */
	for (i = 0; i < g_nav.poly_count; i++)
	{
		struct halo_nav_poly const *poly = &g_nav.polys[i];
		int kind;
		float z_lift;

		if (poly->corner_count < 3)
			continue;
		if (camera_pos && !poly->is_forge_surface && !poly->blocked &&
			nav_dist3d(camera_pos, &poly->center) > max_poly_dist)
		{
			continue;
		}
		if (poly->blocked)
		{
			kind = 2; /* Red/Orange: blocked by Forge obstacle */
			z_lift = 0.08f;
		}
		else if (poly->is_forge_surface)
		{
			kind = 1; /* Gold: walkable Forge ramp/platform */
			z_lift = 0.07f;
		}
		else
		{
			kind = 0; /* Cyan: walkable BSP polygon */
			z_lift = 0.04f;
		}
		for (c = 0; c < poly->corner_count; c++)
		{
			long next_c = (c + 1) % poly->corner_count;
			float p0[3], p1[3];

			p0[0] = poly->corners[c].x;
			p0[1] = poly->corners[c].y;
			p0[2] = poly->corners[c].z + z_lift;
			p1[0] = poly->corners[next_c].x;
			p1[1] = poly->corners[next_c].y;
			p1[2] = poly->corners[next_c].z + z_lift;
			draw_piece(p0, p1, kind);
		}
	}

	/* 2. Render Off-Mesh Connections:
	      - Teleporters (Purple = 4)
	      - Climbable Ladders (Gold = 1)
	      - One-Way Ledge Drops (Red/Orange = 2) */
	for (i = 0; i < g_nav.offmesh_count; i++)
	{
		struct halo_nav_offmesh const *om = &g_nav.offmesh[i];
		float p0[3], p1[3];
		int kind = (om->type == _halo_nav_offmesh_teleporter) ? 4 :
			(om->type == _halo_nav_offmesh_ladder ? 1 : 2);

		if (om->type == _halo_nav_offmesh_jump_down && camera_pos &&
			nav_dist3d(camera_pos, &om->start_point) > max_poly_dist)
		{
			continue;
		}
		p0[0] = om->start_point.x;
		p0[1] = om->start_point.y;
		p0[2] = om->start_point.z + 0.15f;
		p1[0] = om->end_point.x;
		p1[1] = om->end_point.y;
		p1[2] = om->end_point.z + 0.15f;
		draw_piece(p0, p1, kind);
	}

	/* 3. Render Active Smoothed Funnel Paths:
	      - Biped AI / Bot paths (Emerald Green = 3)
	      - Vehicle AI / Bot paths (Purple = 4) */
	now = game_time_initialized() ? game_time_get() : 0;
	for (i = 0; i < NAV_DEBUG_MAX_PATHS; i++)
	{
		struct halo_nav_debug_path const *dp = &g_nav_debug_paths[i];
		int kind;
		float prev[3];

		if (!dp->valid || dp->waypoint_count <= 0 || (now - dp->tick) > 180)
			continue;
		kind = dp->agent_radius >= 1.0f ? 4 : 3;
		prev[0] = dp->start_point.x;
		prev[1] = dp->start_point.y;
		prev[2] = dp->start_point.z + 0.16f;
		for (c = 0; c < dp->waypoint_count; c++)
		{
			float curr[3];
			curr[0] = dp->waypoints[c].x;
			curr[1] = dp->waypoints[c].y;
			curr[2] = dp->waypoints[c].z + 0.16f;
			draw_piece(prev, curr, kind);
			prev[0] = curr[0];
			prev[1] = curr[1];
			prev[2] = curr[2];
		}
	}

	pthread_mutex_unlock(&g_nav_mutex);
}
