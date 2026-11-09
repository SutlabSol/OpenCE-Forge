/*
HALO_NAVMESH.H

Recast & Detour style 3D Navigation Mesh, Dynamic Forge Obstacle/Ramp
Integration, Full-Map Polygon A*, Simple Stupid Funnel String-Pulling,
Off-Mesh Connections (teleporters, ladders, ledge drops), and DetourCrowd
Reciprocal Velocity Obstacle (RVO) local steering for OpenCE.

Used by:
- Multiplayer Bots (port/linux/game/bots.c)
- MendicantCE Tactical AI & Free Movement (port/linux/game/mendicant/mendicant_move.c)
- Firefight Wave Spawner & Friendly AI (port/linux/game/firefight_spawner.c)
- Forge Runtime Object Placement (port/forge/src/forge_runtime_tags.c)
*/

#ifndef HALO_NAVMESH_H
#define HALO_NAVMESH_H

#include "cseries.h"
#include "math/real_math.h"
#include "ai/path.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	_halo_nav_offmesh_none = 0,
	_halo_nav_offmesh_teleporter = 1,
	_halo_nav_offmesh_ladder = 2,
	_halo_nav_offmesh_jump_down = 3
};

struct halo_nav_waypoint
{
	real_point3d point;
	long surface_index;
	short offmesh_type; /* _halo_nav_offmesh_* */
	short offmesh_index;
};

/* Marks the navigation mesh dirty (e.g. when Forge places, moves, or deletes
   scenery/machines) so the next query rebuilds dynamic obstacles and surfaces. */
void halo_navmesh_mark_dirty(void);

/* Frees all heap memory held by the navigation mesh on map dispose. */
void halo_navmesh_dispose(void);

/* Ensures the navigation mesh is built for the active structure BSP and
   current Forge objects. Returns TRUE if valid and ready. */
boolean halo_navmesh_ensure_built(void);

/* Finds a full-map, string-pulled 3D path from start_point to goal_point using
   Detour Polygon A* + Simple Stupid Funnel smoothing.
   - agent_radius: ~0.25f for bipeds, ~1.5f-2.2f for vehicles
   - allow_offmesh: TRUE to allow teleporters, ladders, and ledge drops
   - out_waypoints: receives smoothed 3D corner-hugging waypoints
   - max_waypoints: capacity of out_waypoints
   - out_reached: set to TRUE if the goal polygon was reached, FALSE if partial
   - out_total_distance: optional total path length in world units
   Returns the number of waypoints written (> 0 on success, 0 on failure). */
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
	real *out_total_distance);

/* Convenience wrapper that populates a Halo engine `struct path_result`
   (up to MAXIMUM_SMOOTHED_PATH_STEPS steps) from a full-map Detour + Funnel
   search. Returns TRUE if a valid path was built. */
boolean halo_navmesh_build_path_result(
	real_point3d const *start_point,
	long start_surface_index,
	real_point3d const *goal_point,
	long goal_surface_index,
	real agent_radius,
	boolean allow_offmesh,
	struct path_result *out_path,
	boolean *out_reached,
	real *out_total_distance);

/* DetourCrowd-style Reciprocal Velocity Obstacle (RVO) local steering:
   adjusts desired_direction (radians) and desired_throttle (0..1) so unit_index
   smoothly steers around nearby units and walls without doorway jamming.
   Returns TRUE if steering modified the direction. */
boolean halo_navmesh_crowd_steer(
	long unit_index,
	real_point3d const *position,
	real desired_direction,
	real desired_throttle,
	real agent_radius,
	real *out_direction,
	real *out_throttle);

/* Samples up to max_points well-spaced walkable 3D points (and their BSP
   surface indices) across the entire navmesh. Used by MendicantCE on
   multiplayer/Forge maps where scenario->ai_encounters.count == 0. */
long halo_navmesh_sample_points(
	real_point3d *out_points,
	long *out_surfaces,
	long max_points,
	real min_spacing);

#ifdef __cplusplus
}
#endif

#endif /* HALO_NAVMESH_H */
