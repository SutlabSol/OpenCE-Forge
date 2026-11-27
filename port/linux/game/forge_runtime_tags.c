/*
FORGE_RUNTIME_TAGS.C

Runtime Xbox donor tag loading, safety validation, catalog management,
and sidecar map state persistence.
*/

#ifndef HALO_RELEASE
#define HALO_RELEASE
#endif
#include "cseries.h"
#include "halo_port_limits.h" /* HALO_PORT_NETWORK_VERSION: OpenCE's protocol for standard sessions */
#include "game/game.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "objects/object_lights.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "models/models.h"
#include "models/model_definitions.h"
#include "physics/physics_definitions.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "math/real_math.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "game/players.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/rasterizer_models.h"
#include "rasterizer/rasterizer_swizzle.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "cache/cache_files.h"
#include "custom_edition_cache.h"
#include "effects/effect_definitions.h"
#include "models/model_animation_definitions.h"
#include "tag_schema.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/xbox/rasterizer_xbox_pixel_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "forge_engine.h"

boolean tag_index_is_group(long tag_index, tag group_tag);

#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef remove
#undef malloc
#undef calloc
#undef free
#undef realloc

extern unsigned long system_milliseconds(void);
extern void rasterizer_set_frustum_z(real z_near, real z_far);
extern void *cache_files_tag_instances(long *count);
extern void cache_files_set_tag_instances(void *instances, long count);
extern void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size);
extern char *main_get_multiplayer_map_name(void);
extern boolean game_engine_has_teams(void);

static int forge_float_is_finite(float x)
{
	return (x == x && x > -3.4e38f && x < 3.4e38f) ? 1 : 0;
}

static void forge_resolve_host_path(char const *path, char *out_host_path, size_t out_sz);

#undef fopen
#undef remove
#undef rename

void *forge_host_fopen(char const *path, char const *mode)
{
	char host_path[1024];
	FILE *f;
	if (!path || !path[0] || !mode)
		return NULL;
	host_path[0] = '\0';
	forge_resolve_host_path(path, host_path, sizeof(host_path));
	f = host_path[0] ? fopen(host_path, mode) : NULL;
	return f ? f : fopen(path, mode);
}

int forge_host_remove(char const *path)
{
	char host_path[1024];
	if (!path || !path[0])
		return -1;
	host_path[0] = '\0';
	forge_resolve_host_path(path, host_path, sizeof(host_path));
	if (host_path[0] && remove(host_path) == 0)
		return 0;
	return remove(path);
}

static void forge_resolve_host_path(char const *path, char *out_host_path, size_t out_sz)
{
	char norm[1024];
	char d_xbox[1024];
	size_t i;

	if (!path || !path[0] || !out_host_path || out_sz == 0)
		return;

	for (i = 0; path[i] && i + 1 < sizeof(norm); i++)
		norm[i] = (path[i] == '\\') ? '/' : path[i];
	norm[i] = '\0';

	if ((norm[0] == 'd' || norm[0] == 'D' ||
		 norm[0] == 'h' || norm[0] == 'H' ||
		 norm[0] == 'z' || norm[0] == 'Z' ||
		 norm[0] == 't' || norm[0] == 'T' ||
		 norm[0] == 'u' || norm[0] == 'U') &&
		norm[1] == ':' && norm[2] == '/')
	{
		platform_translate_path(path, out_host_path, (unsigned long)out_sz);
		return;
	}

	if (norm[0] == '/' ||
		(((norm[0] >= 'A' && norm[0] <= 'Z') || (norm[0] >= 'a' && norm[0] <= 'z')) && norm[1] == ':'))
	{
		snprintf(out_host_path, out_sz, "%s", norm);
		return;
	}

	snprintf(d_xbox, sizeof(d_xbox), "d:\\%s", norm);
	platform_translate_path(d_xbox, out_host_path, (unsigned long)out_sz);
}

int forge_host_rename(char const *old_path, char const *new_path)
{
	char old_host[1024];
	char new_host[1024];

	if (!old_path || !old_path[0] || !new_path || !new_path[0])
		return -1;

	old_host[0] = '\0';
	new_host[0] = '\0';
	forge_resolve_host_path(old_path, old_host, sizeof(old_host));
	forge_resolve_host_path(new_path, new_host, sizeof(new_host));

#ifdef _WIN32
	remove(new_host);
#endif
	if (rename(old_host, new_host) == 0)
		return 0;

#ifdef _WIN32
	remove(new_path);
#endif
	return rename(old_path, new_path);
}

char const *forge_get_active_map_name(void)
{
	static char s_active_map[256];
	struct scenario *scen = global_scenario_try_and_get();
	char const *ce_loaded;
	char const *loaded_leaf;
	char const *mp_map;
	char const *sp_map;

	if (!scen || scen->type == 2 /* _scenario_type_main_menu */)
		return "levels\\ui\\ui";

	{
		extern boolean mcc_cache_tags_loaded(void);
		extern char const *mcc_cache_loaded_level_name(void);
		if (mcc_cache_tags_loaded())
		{
			char const *mcc_level = mcc_cache_loaded_level_name();
			if (mcc_level && mcc_level[0])
				return mcc_level;
		}
	}

	ce_loaded = custom_edition_cache_tags_loaded() ? custom_edition_cache_loaded_path() : NULL;
	if (ce_loaded && ce_loaded[0])
	{
		char const *slash = strrchr(ce_loaded, '\\');
		char const *slash2 = strrchr(ce_loaded, '/');
		char const *stem;
		char clean_stem[128];
		char *dot;
		if (slash2 && (!slash || slash2 > slash))
			slash = slash2;
		stem = slash ? (slash + 1) : ce_loaded;
		snprintf(clean_stem, sizeof(clean_stem), "%s", stem);
		dot = strrchr(clean_stem, '.');
		if (dot && _stricmp(dot, ".map") == 0)
			*dot = '\0';
		if (clean_stem[0])
		{
			snprintf(s_active_map, sizeof(s_active_map), "custom_maps\\%s", clean_stem);
			return s_active_map;
		}
	}

	loaded_leaf = cache_file_loaded_map_name();
	mp_map = main_get_multiplayer_map_name();
	sp_map = main_get_map_name();

	if (loaded_leaf && loaded_leaf[0] && _stricmp(loaded_leaf, "ui") != 0)
	{
		if (mp_map && mp_map[0])
		{
			char const *ms = strrchr(mp_map, '\\');
			char const *ms2 = strrchr(mp_map, '/');
			char const *mleaf;
			if (ms2 && (!ms || ms2 > ms))
				ms = ms2;
			mleaf = ms ? (ms + 1) : mp_map;
			if (_stricmp(mleaf, loaded_leaf) == 0)
				return mp_map;
		}
		if (sp_map && sp_map[0])
		{
			char const *ss = strrchr(sp_map, '\\');
			char const *ss2 = strrchr(sp_map, '/');
			char const *sleaf;
			if (ss2 && (!ss || ss2 > ss))
				ss = ss2;
			sleaf = ss ? (ss + 1) : sp_map;
			if (_stricmp(sleaf, loaded_leaf) == 0)
				return sp_map;
		}
		return loaded_leaf;
	}

	if (scen->type == 1 /* _scenario_type_multiplayer */ && mp_map && mp_map[0])
		return mp_map;

	return (sp_map && sp_map[0]) ? sp_map : "levels\\ui\\ui";
}

void forge_snap_to_scenario_placement_origin(
	uint32_t group_tag,
	char const *tag_path,
	float *io_x,
	float *io_y,
	float *io_z)
{
	struct scenario *scen;
	short object_type;
	float best_d2 = 0.0625f; /* Within 0.25 world units */
	float best_x = 0.0f, best_y = 0.0f, best_z = 0.0f;
	int found = 0;

	if (!tag_path || !tag_path[0] || !io_x || !io_y || !io_z)
		return;
	scen = global_scenario_try_and_get();
	if (!scen)
		return;

	for (object_type = 0; object_type < NUMBER_OF_OBJECT_TYPES; object_type++)
	{
		struct object_type_definition *def = object_type_definition_get(object_type);
		struct tag_block *datums;
		struct tag_block *palette;
		long elem_size = 0;
		long i;

		if (!def || def->placement_tag_block_offset == NONE || def->palette_tag_block_offset == NONE)
			continue;
		if ((uint32_t)def->group_tag != group_tag)
			continue;

		datums = scenario_get_object_type_scenario_datums(scen, object_type, &elem_size);
		palette = scenario_get_object_type_scenario_palette(scen, object_type);
		if (!datums || !palette || datums->count <= 0 || palette->count <= 0 ||
			elem_size < (long)sizeof(struct scenario_object_datum))
			continue;

		for (i = 0; i < datums->count; i++)
		{
			struct scenario_object_datum const *sobj =
				(struct scenario_object_datum const *)tag_block_get_element_fast(datums, i, elem_size);
			struct scenario_object_palette_entry const *pal_entry;
			long ref_idx;
			char const *ref_name;
			float dx, dy, dz, d2;

			if (!sobj || sobj->palette_entry_index < 0 || sobj->palette_entry_index >= palette->count)
				continue;
			pal_entry = TAG_BLOCK_GET_ELEMENT(
				palette, sobj->palette_entry_index, struct scenario_object_palette_entry);
			if (!pal_entry)
				continue;
			ref_idx = pal_entry->reference.index;
			if (ref_idx == NONE || (uint32_t)tag_get_group_tag(ref_idx) != group_tag)
				continue;
			ref_name = tag_get_name(ref_idx);
			if (!ref_name || _stricmp(ref_name, tag_path) != 0)
				continue;

			dx = sobj->position.x - *io_x;
			dy = sobj->position.y - *io_y;
			dz = sobj->position.z - *io_z;
			d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < best_d2)
			{
				best_d2 = d2;
				best_x = sobj->position.x;
				best_y = sobj->position.y;
				best_z = sobj->position.z;
				found = 1;
			}
		}
	}

	if (found)
	{
		*io_x = best_x;
		*io_y = best_y;
		*io_z = best_z;
	}
}

#define fopen(p, m) ((FILE *)forge_host_fopen((p), (m)))
#define remove(p) forge_host_remove((p))
extern boolean custom_edition_cache_tags_loaded(void);
extern boolean mcc_cache_tags_loaded(void);
extern char const *mcc_cache_loaded_level_name(void);
extern void orig_render_model(
	long model_index,
	real level_of_detail_pixels,
	struct real_matrix4x3 const *node_matrices,
	char const *region_permutation_indices,
	union real_rgb_color const *change_colors,
	real const *function_values,
	struct render_lighting const *lighting,
	union real_point3d const *centroid,
	real radius,
	struct render_model_effect const *model_effect,
	long unique_identifier,
	short forced_shader_permutation_index,
	unsigned long flags);

struct forge_cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct forge_runtime_state g_forge;
int g_forge_session_enabled = 0;

long forge_get_network_version(void)
{
	return g_forge_session_enabled ? FORGE_NETWORK_VERSION : HALO_PORT_NETWORK_VERSION;
}

void forge_set_status(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_forge.status_message, sizeof(g_forge.status_message), fmt, ap);
	va_end(ap);
	g_forge.status_message_until_ms = (uint32_t)system_milliseconds() + 6000u;
}

void forge_draw_screen_rect(short x0, short y0, short x1, short y1, uint32_t argb)
{
	if (!global_d3d_device || x1 <= x0 || y1 <= y0)
		return;

	_rasterizer_debug_immediate_begin_screenspace();
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
	pixel_shader.final_combiner_inputs_efg = 0x1400;
	rasterizer_set_pixel_shader(&pixel_shader);

	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexDataColor(global_d3d_device, 9, argb);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x0, y0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x1, y0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x1, y1);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x0, y1);
	IDirect3DDevice8_End(global_d3d_device);
	_rasterizer_debug_immediate_end_screenspace();
}

int forge_is_multiplayer_session(void)
{
	short conn = game_connection();
	return (conn == _game_connection_network_client || conn == _game_connection_network_server) ? 1 : 0;
}

static int is_forge_door_tag(uint32_t grp, char const *tag_path)
{
	if (!tag_path || !tag_path[0])
		return 0;
	if ((grp == 0 || grp == 'mach') &&
		(strstr(tag_path, "door") != NULL || strstr(tag_path, "Door") != NULL || strstr(tag_path, "DOOR") != NULL))
	{
		if (strstr(tag_path, "doorblinker") == NULL && strstr(tag_path, "door_blast_collision") == NULL)
			return 1;
	}
	return 0;
}

static int path_contains_any_ci(char const *tag_path, char const *const *needles, size_t count)
{
	char lower_buf[260];
	size_t i, k;
	if (!tag_path || !tag_path[0])
		return 0;
	for (i = 0; tag_path[i] && i + 1 < sizeof(lower_buf); i++)
		lower_buf[i] = (tag_path[i] >= 'A' && tag_path[i] <= 'Z') ? (char)(tag_path[i] - 'A' + 'a') : tag_path[i];
	lower_buf[i] = '\0';
	for (k = 0; k < count; k++)
	{
		if (strstr(lower_buf, needles[k]) != NULL)
			return 1;
	}
	return 0;
}

static int is_excluded_scenery_or_effect_tag(char const *tag_path)
{
	static char const *const s_excluded_substrings[] = {
		"emitter", "particle", "effect", "explosion", "exhaust", "lightning",
		"point light", "lens flare", "dynamic light", "nomodel", "hologram",
		"holo control", "doorblinker", "nutblocker", "door_blast_collision",
		"countdown timer", "temp critical", "teleporter_shield", "smoke", "spark",
		"steam", "flame", "flare", "blood_pool", "floor_arrow", "cutscene_",
		"lifepod_atmosphere_entry", "\\devices\\",
		"scenery\\cortana_chip\\cortana_chip", "scenery\\baton\\baton",
		"scenery\\tubewire\\tubewire", "scenery\\debris\\rocks_smoldering\\rocks_smoldering",
		"levels\\b40\\scenery\\b40_cpanel\\b40_cpanel", "scenery\\helmet_chip\\helmet_chip",
		"scenery\\lockpick\\lockpick", "levels\\c10\\scenery\\c10_smallplant\\c10_smallplant",
		"levels\\c10\\scenery\\c10_lilypad\\c10_lilypad", "levels\\c10\\scenery\\c10_lilypadbunch\\c10_lilypadbunch",
		"levels\\c10\\scenery\\c10_lilypadcluster\\c10_lilypadcluster", "levels\\c10\\scenery\\c10_plantclump\\c10_plantclump",
		"levels\\c10\\scenery\\c10_lilypadbunchb\\c10_lilypadbunchb", "levels\\c10\\scenery\\c10_lilypadclusterb\\c10_lilypadclusterb",
		"scenery\\baton\\baton dynamic", "scenery\\index\\index", "scenery\\index_hold\\index_hold",
		"levels\\c40\\scenery\\c40_snowbushsmall_dark\\c40_snowbushsmall_dark",
		"scenery\\hilltop\\hilltop", "scenery\\captain_implant\\captain_implant", "flood_captain"
	};
	if (!tag_path || !tag_path[0])
		return 1;
	if (_strnicmp(tag_path, "cinematics\\", 11) == 0)
		return 1;
	return path_contains_any_ci(tag_path, s_excluded_substrings, sizeof(s_excluded_substrings) / sizeof(s_excluded_substrings[0]));
}

static int is_excluded_weapon_or_equipment_tag(char const *tag_path)
{
	static char const *const s_excluded_weap_eqip[] = {
		"gravity rifle", "gravity_rifle", "full-spectrum", "full spectrum", "full_spectrum",
		"plasma_cannon", "plasma cannon", "fuel rod", "fuel_rod", "double speed", "double_speed",
		"target_laser", "target laser", "energy sword", "energy_sword",
		"flamethrower", "flame_thrower", "flame thrower",
		"\\flag", "flag\\", "\\ball", "ball\\"
	};
	if (!tag_path || !tag_path[0])
		return 1;
	return path_contains_any_ci(tag_path, s_excluded_weap_eqip, sizeof(s_excluded_weap_eqip) / sizeof(s_excluded_weap_eqip[0]));
}

static int compute_catalog_entry_allowed(struct forge_catalog_entry const *entry)
{
	if (!entry)
		return 0;
	if (entry->source_cache_id == 3)
		return 1;
	/* Disable default stock bipeds and control devices */
	if (entry->category == _forge_cat_bipeds ||
	    entry->group_tag == 'bipd' ||
	    entry->group_tag == 'ctrl' ||
	    entry->group_tag == 'lifi')
	{
		return 0;
	}
	/* Doors have their own dedicated DOORS category (_forge_cat_barriers_devices) */
	if (entry->category == _forge_cat_barriers_devices)
		return is_forge_door_tag(entry->group_tag, entry->tag_path);
	if (is_forge_door_tag(entry->group_tag, entry->tag_path))
		return 0;
	if (entry->group_tag == 'mach')
		return is_forge_door_tag(entry->group_tag, entry->tag_path);
	/* Exclude gravity rifle, flamethrower, full spectrum vision, fuel rod (plasma_cannon), and double speed */
	if ((entry->group_tag == 'weap' || entry->group_tag == 'eqip' ||
	     entry->category == _forge_cat_weapons || entry->category == _forge_cat_equipment) &&
	    is_excluded_weapon_or_equipment_tag(entry->tag_path))
	{
		return 0;
	}
	/* Exclude all emitters, particle effects, point lights, lens flares, and device scenery props */
	if (entry->group_tag == 'scen' && is_excluded_scenery_or_effect_tag(entry->tag_path))
	{
		return 0;
	}
	/* Allow standard vehicles, Pelican, and Wraith in the vehicle spawn list */
	if (entry->group_tag == 'vehi' || entry->category == _forge_cat_vehicles)
	{
		static char const *const s_allowed_vehicles[] = {
			"vehicles\\warthog\\warthog",
			"vehicles\\warthog\\mp_warthog",
			"vehicles\\rwarthog\\rwarthog",
			"vehicles\\ghost\\ghost",
			"vehicles\\ghost\\ghost_mp",
			"vehicles\\banshee\\banshee",
			"vehicles\\banshee\\banshee_mp",
			"vehicles\\scorpion\\scorpion",
			"vehicles\\scorpion\\scorpion_mp",
			"vehicles\\pelican\\pelican",
			"vehicles\\wraith\\wraith",
			"vehicles\\c_dropship\\c_dropship",
			"vehicles\\c_gun_turret\\c_gun_turret",
			"vehicles\\c_gun_turret\\c_gun_turret_mp",
			"vehicles\\c gun turret\\c gun turret",
			"vehicles\\c gun turret\\c gun turret_mp"
		};
		int v, allowed = 0;
		for (v = 0; v < (int)(sizeof(s_allowed_vehicles) / sizeof(s_allowed_vehicles[0])); v++)
		{
			if (_stricmp(entry->tag_path, s_allowed_vehicles[v]) == 0)
			{
				allowed = 1;
				break;
			}
		}
		if (!allowed)
			return 0;
	}
	return 1;
}

int forge_catalog_entry_allowed(struct forge_catalog_entry const *entry)
{
	if (!entry)
		return 0;
	if (entry->pad & 0x0100u)
		return (entry->pad & 0x0001u) ? 1 : 0;
	return compute_catalog_entry_allowed(entry);
}

static void make_short_display_name(char const *tag_path, char const *prefix, char *out, size_t out_sz)
{
	char const *leaf = tag_path;
	char const *p;
	for (p = tag_path; *p; p++)
	{
		if (*p == '\\' || *p == '/')
			leaf = p + 1;
	}
	if (prefix && (_stricmp(prefix, "[c]") == 0))
		snprintf(out, out_sz, "[C] %s", leaf);
	else if (prefix && (_stricmp(prefix, "[map]") == 0))
		snprintf(out, out_sz, "[MAP] %s", leaf);
	else if (prefix && prefix[0] == '[')
		snprintf(out, out_sz, "%s", prefix);
	else if (prefix && prefix[0])
		snprintf(out, out_sz, "%s %s", prefix, leaf);
	else
		snprintf(out, out_sz, "%s", leaf);
}

static int is_equivalent_stock_vehicle_path(char const *a, char const *b)
{
	static struct { char const *x; char const *y; } const pairs[] = {
		{ "vehicles\\warthog\\warthog", "vehicles\\warthog\\mp_warthog" },
		{ "vehicles\\ghost\\ghost", "vehicles\\ghost\\ghost_mp" },
		{ "vehicles\\scorpion\\scorpion", "vehicles\\scorpion\\scorpion_mp" },
		{ "vehicles\\banshee\\banshee", "vehicles\\banshee\\banshee_mp" },
		{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" },
		{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c gun turret\\c gun turret_mp" },
		{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c gun turret\\c gun turret_mp" },
		{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" },
		{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c gun turret\\c gun turret" },
		{ "vehicles\\c_gun_turret\\c_gun_turret_mp", "vehicles\\c gun turret\\c gun turret_mp" }
	};
	size_t k;
	if (!a || !b)
		return 0;
	for (k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++)
	{
		if ((_stricmp(a, pairs[k].x) == 0 && _stricmp(b, pairs[k].y) == 0) ||
			(_stricmp(a, pairs[k].y) == 0 && _stricmp(b, pairs[k].x) == 0))
		{
			return 1;
		}
	}
	return 0;
}

static void add_catalog_entry(
	uint32_t group_tag,
	uint8_t category,
	uint8_t source_cache_id,
	char const *tag_path,
	char const *display_prefix)
{
	int i;
	struct forge_catalog_entry *e;

	if (!tag_path || !tag_path[0] || g_forge.catalog_count >= FORGE_MAX_CATALOG_ENTRIES)
		return;

	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (g_forge.catalog[i].group_tag == group_tag &&
		    (_stricmp(g_forge.catalog[i].tag_path, tag_path) == 0 ||
		     (group_tag == 'vehi' && source_cache_id == 3 &&
		      is_equivalent_stock_vehicle_path(g_forge.catalog[i].tag_path, tag_path))))
		{
			if (source_cache_id == 3 && g_forge.catalog[i].source_cache_id == 0 &&
			    !forge_catalog_entry_allowed(&g_forge.catalog[i]))
			{
				g_forge.catalog[i].source_cache_id = 3;
				g_forge.catalog[i].category = category;
				make_short_display_name(tag_path, display_prefix, g_forge.catalog[i].display_name, sizeof(g_forge.catalog[i].display_name));
				g_forge.catalog[i].pad = (uint16_t)(0x0100u | (compute_catalog_entry_allowed(&g_forge.catalog[i]) ? 1u : 0u));
			}
			return;
		}
	}

	e = &g_forge.catalog[g_forge.catalog_count++];
	e->group_tag = group_tag;
	e->category = category;
	e->source_cache_id = source_cache_id;
	e->pad = 0;
	snprintf(e->tag_path, sizeof(e->tag_path), "%s", tag_path);
	make_short_display_name(tag_path, display_prefix, e->display_name, sizeof(e->display_name));
	e->pad = (uint16_t)(0x0100u | (compute_catalog_entry_allowed(e) ? 1u : 0u));
}

int forge_validate_safe_object_tag(int32_t tag_index)
{
	long tag_count = 0;
	struct forge_cache_file_tag_instance const *instances;
	int32_t abs_idx;
	uint32_t grp;
	short expected_type = -1;
	struct _object_definition const *odef;

	if (tag_index == NONE)
		return 0;

	instances = (struct forge_cache_file_tag_instance const *)cache_files_tag_instances(&tag_count);
	if (!instances || tag_count <= 0)
		return 0;

	abs_idx = (int32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index);
	if (abs_idx < 0 || abs_idx >= (int32_t)tag_count)
		return 0;
	if (instances[abs_idx].tag_index != tag_index || !instances[abs_idx].base_address)
		return 0;

	grp = (uint32_t)instances[abs_idx].group_tag;

	if (grp == 'bipd') expected_type = _object_type_biped;
	else if (grp == 'vehi') expected_type = _object_type_vehicle;
	else if (grp == 'weap') expected_type = _object_type_weapon;
	else if (grp == 'eqip') expected_type = _object_type_equipment;
	else if (grp == 'scen') expected_type = _object_type_scenery;
	else if (grp == 'mach') expected_type = _object_type_machine;
	else if (grp == 'ctrl') expected_type = _object_type_control;
	else if (grp == 'lifi') expected_type = _object_type_light_fixture;
	else return 0;

	odef = (struct _object_definition const *)instances[abs_idx].base_address;
	if (odef->type != expected_type)
		return 0;

	if (!forge_float_is_finite(odef->bounding_radius) ||
		!forge_float_is_finite(odef->render_bounding_radius) ||
		!forge_float_is_finite(odef->bounding_offset.x) ||
		!forge_float_is_finite(odef->bounding_offset.y) ||
		!forge_float_is_finite(odef->bounding_offset.z) ||
		!forge_float_is_finite(odef->origin_offset.x) ||
		!forge_float_is_finite(odef->origin_offset.y) ||
		!forge_float_is_finite(odef->origin_offset.z))
	{
		return 0;
	}
	if (odef->bounding_radius < 0.0f || odef->bounding_radius > 500.0f ||
		odef->render_bounding_radius < 0.0f || odef->render_bounding_radius > 2000.0f)
	{
		return 0;
	}
	if (odef->attachments.count < 0 || odef->attachments.count > 64 ||
		odef->widgets.count < 0 || odef->widgets.count > 64)
	{
		return 0;
	}
	if ((odef->attachments.count > 0 && !odef->attachments.address) ||
		(odef->widgets.count > 0 && !odef->widgets.address))
	{
		return 0;
	}

	if (odef->model.index != NONE)
	{
		int32_t m_abs = (int32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(odef->model.index);
		uint32_t m_grp;
		struct model const *mod;
		if (m_abs < 0 || m_abs >= (int32_t)tag_count ||
			instances[m_abs].tag_index != odef->model.index ||
			!instances[m_abs].base_address)
		{
			return 0;
		}
		m_grp = (uint32_t)instances[m_abs].group_tag;
		if (m_grp != 'mode' && m_grp != 'mod2')
			return 0;
		mod = (struct model const *)instances[m_abs].base_address;
		if (mod->nodes.count < 0 || mod->nodes.count > MAXIMUM_NODES_PER_MODEL ||
			mod->geometries.count < 0 || mod->geometries.count > MAXIMUM_GEOMETRIES_PER_MODEL ||
			mod->regions.count < 0 || mod->regions.count > MAXIMUM_REGIONS_PER_MODEL)
		{
			return 0;
		}
	}

	if (odef->animation_graph.index != NONE)
	{
		int32_t a_abs = (int32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(odef->animation_graph.index);
		struct animation_graph const *ag;
		if (a_abs < 0 || a_abs >= (int32_t)tag_count ||
			instances[a_abs].tag_index != odef->animation_graph.index ||
			(uint32_t)instances[a_abs].group_tag != 'antr' ||
			!instances[a_abs].base_address)
		{
			return 0;
		}
		ag = (struct animation_graph const *)instances[a_abs].base_address;
		if (ag->nodes.count < 0 || ag->nodes.count > MAXIMUM_NODES_PER_MODEL)
			return 0;
	}

	if (odef->creation_effect.index != NONE)
	{
		int32_t e_abs = (int32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(odef->creation_effect.index);
		if (e_abs < 0 || e_abs >= (int32_t)tag_count ||
			instances[e_abs].tag_index != odef->creation_effect.index ||
			!instances[e_abs].base_address)
		{
			return 0;
		}
		if ((uint32_t)instances[e_abs].group_tag == 'effe')
		{
			struct effect_definition const *effe =
				(struct effect_definition const *)instances[e_abs].base_address;
			long ev_i;
			if (effe->events.count < 0 || effe->events.count > 256 ||
				(effe->events.count > 0 && !effe->events.address))
			{
				return 0;
			}
			for (ev_i = 0; ev_i < effe->events.count; ev_i++)
			{
				struct effect_event_definition const *ev =
					&((struct effect_event_definition const *)effe->events.address)[ev_i];
				long pt_i;
				if (ev->parts.count < 0 || ev->parts.count > 256 ||
					(ev->parts.count > 0 && !ev->parts.address))
				{
					return 0;
				}
				for (pt_i = 0; pt_i < ev->parts.count; pt_i++)
				{
					struct effect_part_definition const *pt =
						&((struct effect_part_definition const *)ev->parts.address)[pt_i];
					uint32_t pt_grp = (uint32_t)pt->reference.group_tag;
					if (pt_grp == 'scnr' || pt_grp == 'sbsp' || pt_grp == 'matg' ||
						pt_grp == 'actr' || pt_grp == 'actv' || pt_grp == 'bipd' ||
						pt_grp == 'udlg')
					{
						return 0;
					}
					if (pt->reference.index != NONE)
					{
						int32_t p_abs = (int32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(pt->reference.index);
						if (p_abs < 0 || p_abs >= (int32_t)tag_count ||
							instances[p_abs].tag_index != pt->reference.index ||
							!instances[p_abs].base_address)
						{
							return 0;
						}
					}
				}
			}
		}
	}

	return 1;
}

static int is_excluded_map_local_helper_tag(char const *tag_path)
{
	static char const *const s_helpers[] = {
		"nomodel",
		"hologram",
		"holo control",
		"doorblinker",
		"nutblocker",
		"door_blast_collision",
		"countdown timer",
		"temp critical",
		"teleporter_shield",
		"floor_arrow",
		"cutscene_",
		"lifepod_atmosphere_entry",
		"\\devices\\"
	};
	char lower_buf[260];
	size_t i, k;

	if (!tag_path || !tag_path[0])
		return 1;
	if (_strnicmp(tag_path, "cinematics\\", 11) == 0 ||
		_strnicmp(tag_path, "ui\\", 3) == 0 ||
		_strnicmp(tag_path, "sound\\", 6) == 0 ||
		_strnicmp(tag_path, "rasterizer\\", 11) == 0 ||
		_strnicmp(tag_path, "fx\\", 3) == 0)
	{
		return 1;
	}
	for (i = 0; tag_path[i] && i + 1 < sizeof(lower_buf); i++)
	{
		char ch = tag_path[i];
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
		lower_buf[i] = ch;
	}
	lower_buf[i] = '\0';

	for (k = 0; k < sizeof(s_helpers) / sizeof(s_helpers[0]); k++)
	{
		if (strstr(lower_buf, s_helpers[k]) != NULL)
			return 1;
	}
	return 0;
}

static void scan_loaded_map_into_catalog(void)
{
	char const *cur_map = forge_get_active_map_name();
	struct scenario *scen;
	long tag_count = 0;
	struct forge_cache_file_tag_instance *instances;
	long i;

	if (!cur_map || !cur_map[0] ||
		_stricmp(cur_map, "levels\\ui\\ui") == 0 ||
		_stricmp(cur_map, "ui") == 0)
	{
		return;
	}
	scen = global_scenario_try_and_get();
	if (!scen || scen->type == 2 /* _scenario_type_main_menu */)
		return;

	instances = (struct forge_cache_file_tag_instance *)cache_files_tag_instances(&tag_count);
	if (!instances || tag_count <= 0)
		return;

	for (i = 0; i < tag_count; i++)
	{
		struct forge_cache_file_tag_instance const *inst = &instances[i];
		uint32_t grp;
		char const *tpath;
		uint8_t cat_id;
		struct _object_definition const *odef;

		if (inst->tag_index == NONE || !inst->name || !inst->name[0] || !inst->base_address)
			continue;
		grp = (uint32_t)inst->group_tag;
		tpath = inst->name;

		if (grp != 'scen' && grp != 'vehi' && grp != 'weap' &&
			grp != 'eqip' && grp != 'mach')
		{
			continue;
		}
		if (is_excluded_map_local_helper_tag(tpath))
			continue;

		/* Skip built-in vehicle turret weapons and non-placeable pseudo-weapons */
		if (grp == 'weap' &&
			(_strnicmp(tpath, "vehicles\\", 9) == 0 ||
			 _strnicmp(tpath, "characters\\", 11) == 0 ||
			 is_excluded_weapon_or_equipment_tag(tpath)))
		{
			continue;
		}
		if (grp == 'eqip' && is_excluded_weapon_or_equipment_tag(tpath))
			continue;

		/* Require a valid 3D model on object definitions so invisible script-anchor
		   scenery or sound-only dummy objects do not clutter the palette. */
		odef = (struct _object_definition const *)inst->base_address;
		if (odef->model.index == NONE)
			continue;
		if (!forge_validate_safe_object_tag(inst->tag_index))
			continue;

		if (grp == 'weap')
			cat_id = _forge_cat_weapons;
		else if (grp == 'eqip')
			cat_id = _forge_cat_equipment;
		else if (grp == 'vehi')
			cat_id = _forge_cat_vehicles;
		else if (grp == 'mach' || is_forge_door_tag(grp, tpath))
			cat_id = _forge_cat_barriers_devices;
		else
			cat_id = _forge_cat_scenery;

		add_catalog_entry(grp, cat_id, 3, tpath, "[map]");
	}
}

void forge_runtime_reload_catalogs(void)
{
	g_forge.catalog_count = 0;

	/* Built-in Spawns & Teleporters */
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_red", "[SPAWN] Red Team Spawn");
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_blue", "[SPAWN] Blue Team Spawn");
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_ffa", "[SPAWN] FFA / Neutral Spawn");
	add_catalog_entry('tele', _forge_cat_spawns_teleporters, 0, "teleporter\\origin", "[TELEPORTER] Origin Node");
	add_catalog_entry('tele', _forge_cat_spawns_teleporters, 0, "teleporter\\destination", "[TELEPORTER] Destination Node");

	/* Built-in Netgame Objectives */
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\ctf_flag_red", "[CTF] Red Flag Base");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\ctf_flag_blue", "[CTF] Blue Flag Base");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\oddball_spawn", "[ODDBALL] Ball Spawn Marker");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\koth_hill", "[KOTH] Hill Marker");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\race_track", "[RACE] Race Marker / Checkpoint");

	/* Active-map local tags ([MAP]) only in Phase 2 (Xbox donor cache arrives in Phase 4). */
	scan_loaded_map_into_catalog();

	/* Stable-sort map-local entries ([MAP], source_cache_id == 3) before donor/stock entries (0) */
	if (g_forge.catalog_count > 10)
	{
		struct forge_catalog_entry *sorted =
			(struct forge_catalog_entry *)malloc((size_t)g_forge.catalog_count * sizeof(struct forge_catalog_entry));
		if (sorted)
		{
			int i, out_n = 0;
			for (i = 0; i < g_forge.catalog_count; i++)
			{
				if (g_forge.catalog[i].source_cache_id == 3)
					sorted[out_n++] = g_forge.catalog[i];
			}
			for (i = 0; i < g_forge.catalog_count; i++)
			{
				if (g_forge.catalog[i].source_cache_id != 3)
					sorted[out_n++] = g_forge.catalog[i];
			}
			memcpy(g_forge.catalog, sorted, (size_t)out_n * sizeof(struct forge_catalog_entry));
			free(sorted);
		}
	}
}

static void forge_delete_files_matching(char const *dir_path, char const *pattern)
{
	char search_pat[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	snprintf(search_pat, sizeof(search_pat), "%s\\%s", dir_path, pattern);
	h = FindFirstFileA(search_pat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			char full[512];
			snprintf(full, sizeof(full), "%s/%s", dir_path, fd.cFileName);
			remove(full);
		}
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);
}

static void forge_ensure_local_tag_cache_built(void);
static int s_local_tag_cache_checked = 0;

int forge_reset_stock_tag_cache(void)
{
	char z_path[512];

	s_local_tag_cache_checked = 0;
	remove("OpenCE Forge/cached_tags/xbox_catalog.txt");
	remove("OpenCE Forge/cached_tags/catalog.txt");
	remove("OpenCE Forge/credits.md");
	forge_delete_files_matching("OpenCE Forge/cached_tags/xbox_cache", "*.map");
	RemoveDirectoryA("OpenCE Forge/cached_tags/xbox_cache");
	forge_delete_files_matching("OpenCE Forge/cached_tags/markers", "m_*.*");

	/* Remove only z:\cache_donor.map; preserve z:\cache000..005.map (active ui.map / engine caches) */
	platform_translate_path("z:\\cache_donor.map", z_path, sizeof(z_path));
	remove(z_path);

	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	forge_ensure_local_tag_cache_built();
	forge_runtime_reload_catalogs();
	forge_set_status("Stock Tag Cache Rebuilt!");
	return 1;
}

static void forge_ensure_local_tag_cache_built(void)
{
	if (s_local_tag_cache_checked)
		return;
	s_local_tag_cache_checked = 1;

	/* Clean up any legacy duplicate Xbox .map cache folder from older versions */
	forge_delete_files_matching("OpenCE Forge/cached_tags/xbox_cache", "*.map");
	RemoveDirectoryA("OpenCE Forge/cached_tags/xbox_cache");

	/* Active-map [MAP] tags only in Phase 3 (no Xbox maps/ ripping to cached_tags). */
	CreateDirectoryA("OpenCE Forge", NULL);
}

void forge_runtime_init(void)
{
	if (g_forge.initialized)
	{
		if (g_forge_session_enabled && !s_local_tag_cache_checked)
			forge_ensure_local_tag_cache_built();
		return;
	}
	memset(&g_forge, 0, sizeof(g_forge));
	g_forge.initialized = 1;
	g_forge.mode = _forge_mode_play;
	g_forge.active_modal = _forge_modal_none;
	g_forge.current_category = _forge_cat_vehicles;
	g_forge.active_weld_group = 1;
	g_forge.snap_mode = _forge_snap_15_deg;
	g_forge.cam_speed = 12.0f;
	g_forge.grabbed_object_index = NONE;
	g_forge.grabbed_live_index = -1;
	g_forge.hovered_object_index = NONE;
	g_forge.hovered_live_index = -1;
	g_forge.grab_distance = 5.0f;
	snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "forge_custom_arena");
	g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);

	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	forge_delete_files_matching("OpenCE Forge/cached_tags/xbox_cache", "*.map");
	RemoveDirectoryA("OpenCE Forge/cached_tags/xbox_cache");
	RemoveDirectoryA("OpenCE Forge/custom_tags");
	RemoveDirectoryA("custom_tags");
	if (g_forge_session_enabled)
		forge_ensure_local_tag_cache_built();
	forge_runtime_reload_catalogs();
}

/* ---------- NATIVE XBOX & CUSTOM EDITION MAP TAG CACHE LOADER — Tags are loaded natively from extracted map tag bundles and Xbox map caches — (using OpenCE's exact tag schema & hardware vertex/triangle/texture registration). */

#define FORGE_MAX_HW_PARTS       48
#define FORGE_MAX_DYNAMIC_TAGS   1024

struct forge_hw_part
{
	struct vertex_buffer vertex_buffer;
	struct triangle_buffer triangle_buffer;
	char shader_hint[128];
	float u_scale;
	float v_scale;
};

struct forge_dynamic_tag_entry
{
	uint32_t group_tag;
	char tag_path[256];
	long definition_index;
	float bounding_radius;
	int part_count;
	struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
};

static struct
{
	void *saved_base_instances;
	long saved_base_count;
	void *allocated_instances;
	int count;
	struct forge_dynamic_tag_entry entries[FORGE_MAX_DYNAMIC_TAGS];
} s_dyn_tags;

static long s_monitor_bipd_handle = NONE;
static int s_monitor_resolved = 0;
static int s_local_monitor_drawn_this_frame = 0;
static int s_last_preview_chosen_idx = -1;
static unsigned long s_last_preview_cursor_ms = 0;

void forge_runtime_on_map_unload(void)
{
	int i;

	s_monitor_bipd_handle = NONE;
	s_monitor_resolved = 0;
	s_local_monitor_drawn_this_frame = 0;
	s_last_preview_chosen_idx = -1;
	s_last_preview_cursor_ms = 0;

	if (s_dyn_tags.saved_base_instances)
	{
		cache_files_set_tag_instances(s_dyn_tags.saved_base_instances, s_dyn_tags.saved_base_count);
		s_dyn_tags.saved_base_instances = NULL;
		s_dyn_tags.saved_base_count = 0;
	}
	if (s_dyn_tags.allocated_instances)
	{
		free(s_dyn_tags.allocated_instances);
		s_dyn_tags.allocated_instances = NULL;
	}
	s_dyn_tags.count = 0;
	if (g_forge.initialized && g_forge.catalog_count > 0)
	{
		int out_c = 0;
		for (i = 0; i < g_forge.catalog_count; i++)
		{
			if (g_forge.catalog[i].source_cache_id != 3 &&
				g_forge.catalog[i].source_cache_id != 4)
				g_forge.catalog[out_c++] = g_forge.catalog[i];
		}
		g_forge.catalog_count = out_c;
	}
}

static void build_hw_part_from_arrays(
	struct forge_hw_part *dst_part,
	uint8_t *uncomp_verts,
	int32_t vtx_count,
	uint8_t const *idx_bytes,
	int32_t tri_count,
	char const *shader_hint,
	float scale_factor,
	float z_offset)
{
	uint32_t uncomp_sz;
	uint32_t comp_sz;
	uint32_t idx_sz;
	void *hw_verts;
	void *hw_tris;
	int v, c;

	/* Strip trailing 0xFFFF padding indices (from .gbxmodel 3-index model_triangle blocks) */
	if (idx_bytes && vtx_count >= 3)
	{
		uint16_t const *raw_idx = (uint16_t const *)idx_bytes;
		while (tri_count > 1 && raw_idx[tri_count + 1] >= (uint16_t)vtx_count)
			tri_count--;
	}
	if (vtx_count < 3 || tri_count < 1)
		return;

	uncomp_sz = (uint32_t)vtx_count * 68u;
	comp_sz = (uint32_t)vtx_count * 32u;
	idx_sz = ((uint32_t)tri_count + 2u) * 2u;

	for (v = 0; v < vtx_count; v++)
	{
		uint8_t *vp = uncomp_verts + (uint32_t)v * 68u;
		if (scale_factor != 1.0f || z_offset != 0.0f)
		{
			*(float *)(vp + 0) *= scale_factor;
			*(float *)(vp + 4) *= scale_factor;
			*(float *)(vp + 8) = (*(float *)(vp + 8) * scale_factor) + z_offset;
		}
		for (c = 0; c < 9; c++)
		{
			float val = *(float *)(vp + 12 + c * 4);
			if (!(val >= -1.0f && val <= 1.0f))
				*(float *)(vp + 12 + c * 4) = (c % 3 == 0) ? 1.0f : 0.0f;
		}
		*(int16_t *)(vp + 56) = 0;
		*(int16_t *)(vp + 58) = 0;
		*(float *)(vp + 60) = 1.0f;
		*(float *)(vp + 64) = 0.0f;
	}

	hw_verts = XPhysicalAlloc(comp_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!hw_verts)
		hw_verts = malloc(comp_sz);
	hw_tris = XPhysicalAlloc(idx_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!hw_tris)
		hw_tris = malloc(idx_sz);
	if (!hw_verts || !hw_tris)
		return;

	memset(dst_part, 0, sizeof(*dst_part));
	dst_part->u_scale = 1.0f;
	dst_part->v_scale = 1.0f;
	if (shader_hint && shader_hint[0])
	{
		char const *bar1 = strchr(shader_hint, '|');
		char const *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
		if (bar2)
		{
			float us = 1.0f, vs = 1.0f;
			size_t copy_len = (size_t)(bar2 - shader_hint);
			if (copy_len >= sizeof(dst_part->shader_hint))
				copy_len = sizeof(dst_part->shader_hint) - 1;
			memcpy(dst_part->shader_hint, shader_hint, copy_len);
			dst_part->shader_hint[copy_len] = '\0';
			if (sscanf(bar2 + 1, "%f,%f", &us, &vs) == 2)
			{
				if (us > 0.0001f && us < 10000.0f)
					dst_part->u_scale = us;
				if (vs > 0.0001f && vs < 10000.0f)
					dst_part->v_scale = vs;
			}
		}
		else
		{
			snprintf(dst_part->shader_hint, sizeof(dst_part->shader_hint), "%s", shader_hint);
		}
	}

	rasterizer_geometry_compress_vertices(
		_rasterizer_vertex_type_model_uncompressed,
		vtx_count,
		hw_verts,
		(long)comp_sz,
		uncomp_verts,
		(long)uncomp_sz);
	memcpy(hw_tris, idx_bytes, idx_sz);
	{
		uint16_t *tris_u16 = (uint16_t *)hw_tris;
		int32_t total_idx = tri_count + 2;
		int32_t k;
		for (k = 0; k < total_idx; k++)
		{
			if (tris_u16[k] >= (uint16_t)vtx_count)
				tris_u16[k] = (k > 0 && tris_u16[k - 1] < (uint16_t)vtx_count) ? tris_u16[k - 1] : 0u;
		}
	}

	rasterizer_vertex_buffer_new(
		&dst_part->vertex_buffer,
		_rasterizer_vertex_type_model_compressed,
		vtx_count,
		hw_verts,
		(long)comp_sz);

	rasterizer_triangle_buffer_new(
		&dst_part->triangle_buffer,
		_triangle_buffer_type_precompiled_strip,
		tri_count,
		hw_tris);
}

static long append_runtime_tag_instance(
	long group_tag,
	long parent0,
	long parent1,
	char const *tag_path,
	void *base_address)
{
	long existing_count = 0;
	struct forge_cache_file_tag_instance *instances;
	struct forge_cache_file_tag_instance *grown;
	long max_salt = 0xE174;
	long i, new_abs_idx, new_tag_index;
	char *name_copy;

	instances = (struct forge_cache_file_tag_instance *)cache_files_tag_instances(&existing_count);
	if (!instances || existing_count <= 0 || existing_count >= 65000)
		return NONE;

	if (!s_dyn_tags.saved_base_instances)
	{
		s_dyn_tags.saved_base_instances = instances;
		s_dyn_tags.saved_base_count = existing_count;
	}

	grown = (struct forge_cache_file_tag_instance *)malloc((size_t)(existing_count + 1) * sizeof(*grown));
	name_copy = (char *)malloc(strlen(tag_path) + 1);
	if (!grown || !name_copy)
	{
		free(grown);
		free(name_copy);
		return NONE;
	}
	memcpy(grown, instances, (size_t)existing_count * sizeof(*grown));
	strcpy(name_copy, tag_path);

	for (i = 0; i < existing_count; i++)
	{
		if (instances[i].tag_index != NONE)
		{
			long s = (long)(((unsigned long)instances[i].tag_index >> 16) & 0x7FFFu);
			if (s > max_salt)
				max_salt = s;
		}
	}

	new_abs_idx = existing_count;
	new_tag_index = ((max_salt + 1) << 16) | (new_abs_idx & 0xFFFF);

	if (parent0 == NONE)
	{
		switch (group_tag)
		{
			case 'vehi':
			case 'bipd':
				parent0 = 'unit';
				parent1 = 'obje';
				break;
			case 'weap':
			case 'eqip':
			case 'garb':
				parent0 = 'item';
				parent1 = 'obje';
				break;
			case 'mach':
			case 'ctrl':
			case 'lifi':
				parent0 = 'devi';
				parent1 = 'obje';
				break;
			case 'scen':
			case 'proj':
			case 'ssce':
			case 'plac':
				parent0 = 'obje';
				parent1 = NONE;
				break;
			case 'soso':
			case 'senv':
			case 'schi':
			case 'scex':
			case 'sotr':
			case 'sgla':
			case 'smet':
			case 'swat':
			case 'spla':
				parent0 = 'shdr';
				parent1 = NONE;
				break;
			default:
				break;
		}
	}
	memset(&grown[new_abs_idx], 0, sizeof(grown[new_abs_idx]));
	grown[new_abs_idx].group_tag = group_tag;
	grown[new_abs_idx].parent_group_tags[0] = parent0;
	grown[new_abs_idx].parent_group_tags[1] = parent1;
	grown[new_abs_idx].tag_index = new_tag_index;
	grown[new_abs_idx].name = name_copy;
	grown[new_abs_idx].base_address = base_address;

	cache_files_set_tag_instances(grown, existing_count + 1);
	if (s_dyn_tags.allocated_instances)
		free(s_dyn_tags.allocated_instances);
	s_dyn_tags.allocated_instances = grown;

	return new_tag_index;
}

uint32_t forge_get_base_map_tag_count(void)
{
	long existing_count = 0;
	if (s_dyn_tags.saved_base_count > 0)
		return (uint32_t)s_dyn_tags.saved_base_count;
	if (cache_files_tag_instances(&existing_count) && existing_count > 0)
		return (uint32_t)existing_count;
	return 0;
}

static int is_valid_cyborg_driver_seat_label(char const *label)
{
	if (!label || !label[0])
		return 0;
	if (_stricmp(label, "W-driver") == 0 ||
		_stricmp(label, "G-driver") == 0 ||
		_stricmp(label, "B-driver") == 0 ||
		_stricmp(label, "S-driver") == 0 ||
		_stricmp(label, "R-driver") == 0 ||
		_stricmp(label, "scorpion-driver") == 0 ||
		_stricmp(label, "CT-driver") == 0)
		return 1;
	return 0;
}

static void patch_vehicle_tag_for_player_driving(long vehi_tag_idx, char const *tag_path)
{
	struct unit_definition *ud;
	int is_wraith = 0;

	if (vehi_tag_idx == NONE)
		return;

	ud = (struct unit_definition *)tag_get('vehi', vehi_tag_idx);
	if (ud)
	{
		if ((tag_path && (strstr(tag_path, "wraith") || strstr(tag_path, "Wraith"))) ||
			(ud->object.collision_model.name && strstr(ud->object.collision_model.name, "wraith")))
		{
			is_wraith = 1;
		}
		ud->unit.flags |= FLAG(_unit_has_entrance_points_inside_bounding_sphere_bit);
		if (ud->unit.seats.count > 0 && ud->unit.seats.address)
		{
			struct unit_seat *seat0 = (struct unit_seat *)ud->unit.seats.address;
			seat0->flags &= ~FLAG(_unit_seat_invisible_bit);
			seat0->flags |= FLAG(_unit_seat_driver_bit) |
							FLAG(_unit_seat_third_person_camera_bit) |
							FLAG(_unit_seat_third_person_on_enter_bit);
			if (is_wraith)
				seat0->flags |= FLAG(_unit_seat_gunner_bit);
			if (!is_valid_cyborg_driver_seat_label(seat0->label))
				snprintf(seat0->label, sizeof(seat0->label), "W-driver");
		}
		if (is_wraith && ud->object.collision_model.index != NONE)
		{
			uint8_t *cb = (uint8_t *)tag_get('coll', ud->object.collision_model.index);
			if (cb)
			{
				int32_t m_cnt = *(int32_t const *)(cb + 0x220u);
				uint8_t *m_ptr = *(uint8_t **)(cb + 0x224u);
				int32_t r_cnt = *(int32_t const *)(cb + 0x22Cu);
				uint8_t *r_ptr = *(uint8_t **)(cb + 0x230u);
				int32_t k;

				*(float *)(cb + 0x04u) = 0.0f; /* maximum_body_vitality = 0 (cannot take body damage) */
				*(float *)(cb + 0x08u) = 0.0f; /* body_system_shock = 0 */
				*(float *)(cb + 0x4Cu) = 0.0f; /* body_damaged_threshold = 0 */
				*(int32_t *)(cb + 0x60u) = NONE; /* body_depleted_effect.index = NONE */
				*(float *)(cb + 0x64u) = 0.0f; /* body_destroyed_threshold = 0 */
				*(int32_t *)(cb + 0x74u) = NONE; /* body_destroyed_effect.index = NONE */
				*(float *)(cb + 0x78u) = 0.0f; /* maximum_shield_vitality = 0 */

				if (m_ptr && m_cnt > 0 && m_cnt <= 64)
				{
					for (k = 0; k < m_cnt; k++)
					{
						uint8_t *mp = m_ptr + (uint32_t)k * 0x48u;
						*(float *)(mp + 0x2Cu) = 0.0f; /* shield_damage_multiplier */
						*(float *)(mp + 0x3Cu) = 0.0f; /* body_damage_multiplier */
					}
				}
				if (r_ptr && r_cnt > 0 && r_cnt <= 64)
				{
					for (k = 0; k < r_cnt; k++)
					{
						uint8_t *rp = r_ptr + (uint32_t)k * 0x54u;
						*(uint32_t *)(rp + 0x20u) &= ~0x0Eu; /* clear forces_object_to_die / dies_when_object_dies / dies_in_water */
						*(float *)(rp + 0x28u) = 0.0f; /* damage_threshold = 0 (cannot be destroyed) */
						*(int32_t *)(rp + 0x50u) = NONE; /* destroyed_effect.index = NONE */
					}
				}
			}
		}
	}
}

static void patch_forge_device_tag_runtime(long tag_idx, uint32_t grp, char const *tag_path)
{
	uint8_t *obuf;
	int32_t model_idx;

	(void)tag_path;
	if (tag_idx == NONE)
		return;
	if (grp != 'scen' && grp != 'mach' && grp != 'ctrl' &&
		grp != 'weap' && grp != 'eqip' && grp != 'vehi' && grp != 'bipd')
		return;

	obuf = (uint8_t *)tag_get((long)grp, tag_idx);
	if (!obuf)
		return;

	/* Ensure scenery, devices, weapons, and equipment have valid bounding and render radii so Forge raycasting
	   and long-distance frustum/LOD visibility never cull custom items early */
	if (grp == 'weap' || grp == 'eqip')
	{
		if (*(float const *)(obuf + 0x04u) < 0.6f)
			*(float *)(obuf + 0x04u) = 0.6f;
		if (*(float const *)(obuf + 0x104u) < 4.0f)
			*(float *)(obuf + 0x104u) = 4.0f;
		if (grp == 'weap')
		{
			char *wlabel = (char *)(obuf + 0x30Cu);
			unsigned w_j;
			/* Repair weapon.flags (+0x308) if an earlier build wrote "ar" (0x7261) at +0x308 instead of +0x30C */
			if ((*(uint32_t const *)(obuf + 0x308u) & 0xFFFFu) == 0x7261u)
				*(uint32_t *)(obuf + 0x308u) = 0u;
			/* Normalize MCC weapon HUD interface enum values (17..18 -> 15..16) */
			for (w_j = 0; w_j < 4u; w_j++)
			{
				uint16_t val = *(uint16_t const *)(obuf + 0x330u + w_j * 2u);
				if (val == 17u || val == 18u)
					*(uint16_t *)(obuf + 0x330u + w_j * 2u) = (uint16_t)(val - 2u);
			}
			/* Ensure an empty weapon.label (+0x30C) gets a valid default cyborg animation class */
			if (wlabel[0] == '\0')
			{
				strcpy(wlabel, "ar");
			}
		}
	}
	else if (grp == 'scen' || grp == 'mach' || grp == 'ctrl')
	{
		if (*(float const *)(obuf + 0x04u) < 1.5f)
			*(float *)(obuf + 0x04u) = 1.5f;
		if (*(float const *)(obuf + 0x104u) < 8.0f)
			*(float *)(obuf + 0x104u) = 8.0f;
	}
	if (*(float const *)(obuf + 0x104u) < *(float const *)(obuf + 0x04u) * 2.0f)
		*(float *)(obuf + 0x104u) = *(float const *)(obuf + 0x04u) * 2.0f;

	/* Patch the object's model ('mode'/'mod2') LOD cutoffs and permutation geometry_indices[0..4]
	   so custom items with missing low-LOD geometries or positive super_low pixel cutoffs
	   render across the entire map just like base-game objects! */
	model_idx = *(int32_t const *)(obuf + 0x34u);
	if (model_idx != NONE)
	{
		uint8_t *mbuf = NULL;
		if (tag_index_is_group(model_idx, 'mode'))
			mbuf = (uint8_t *)tag_get('mode', model_idx);
		else if (tag_index_is_group(model_idx, 'mod2'))
			mbuf = (uint8_t *)tag_get('mod2', model_idx);
		if (mbuf)
		{
			float *cutoffs = (float *)(mbuf + 0x08u);
			int32_t model_node_cnt = *(int32_t const *)(mbuf + 0xB8u);
			int32_t antr_idx = *(int32_t const *)(obuf + 0x44u);
			int32_t reg_cnt = *(int32_t const *)(mbuf + 0xC4u);
			uint8_t *reg_base = *(uint8_t *const *)(mbuf + 0xC8u);
			int32_t geom_cnt = *(int32_t const *)(mbuf + 0xD0u);
			uint8_t *geom_base = *(uint8_t *const *)(mbuf + 0xD4u);
			int32_t r, p, lv;

			if (antr_idx != NONE && (grp == 'scen' || grp == 'eqip' || grp == 'weap'))
			{
				uint8_t const *abuf = tag_index_is_group(antr_idx, 'antr') ?
					(uint8_t const *)tag_get('antr', antr_idx) : NULL;
				if (!abuf)
				{
					*(int32_t *)(obuf + 0x44u) = NONE;
				}
				else
				{
					int32_t antr_node_cnt = *(int32_t const *)(abuf + 0x68u);
					int32_t antr_anim_cnt = *(int32_t const *)(abuf + 0x74u);
					if (antr_anim_cnt <= 0 ||
						(grp == 'scen' && antr_node_cnt > 0 && model_node_cnt > 0 && antr_node_cnt != model_node_cnt))
					{
						*(int32_t *)(obuf + 0x44u) = NONE;
					}
				}
			}

			if (cutoffs[0] > cutoffs[4] && cutoffs[4] >= 0.0f)
			{
				float t0 = cutoffs[0], t1 = cutoffs[1];
				cutoffs[0] = cutoffs[4];
				cutoffs[1] = cutoffs[3];
				cutoffs[3] = t1;
				cutoffs[4] = t0;
			}
			cutoffs[0] = 0.0f; /* Never cull at super_low distance (models.c:1000) */

			if (reg_cnt > 0 && reg_cnt <= 32 && reg_base && geom_cnt > 0 && geom_base)
			{
				for (r = 0; r < reg_cnt; r++)
				{
					uint8_t *reg = reg_base + (size_t)r * 76u;
					int32_t perm_cnt = *(int32_t const *)(reg + 0x40u);
					uint8_t *perm_base = *(uint8_t *const *)(reg + 0x44u);
					if (perm_cnt <= 0 || perm_cnt > 32 || !perm_base)
						continue;
					for (p = 0; p < perm_cnt; p++)
					{
						uint8_t *perm = perm_base + (size_t)p * 88u;
						int16_t *g_indices = (int16_t *)(perm + 0x40u);
						int16_t best_g = -1;
						for (lv = 4; lv >= 0; lv--)
						{
							int16_t gi = g_indices[lv];
							if (gi >= 0 && gi < (int16_t)geom_cnt)
							{
								int32_t part_cnt = *(int32_t const *)(geom_base + (size_t)gi * 48u + 36u);
								if (part_cnt > 0)
								{
									best_g = gi;
									break;
								}
							}
						}
						if (best_g >= 0)
						{
							for (lv = 0; lv < 5; lv++)
							{
								int16_t gi = g_indices[lv];
								if (gi < 0 || gi >= (int16_t)geom_cnt ||
									*(int32_t const *)(geom_base + (size_t)gi * 48u + 36u) <= 0)
								{
									g_indices[lv] = best_g;
								}
							}
						}
					}
				}
			}
		}
	}

	/* Configure 'mach' doors to open/close automatically when players or vehicles approach */
	if (grp == 'mach')
	{
		int16_t mtype = *(int16_t const *)(obuf + 0x290u);
		if (mtype == 0 /* _machine_type_door */ ||
			(tag_path && (strstr(tag_path, "door") != NULL || strstr(tag_path, "Door") != NULL || strstr(tag_path, "DOOR") != NULL)))
		{
			*(int16_t *)(obuf + 0x290u) = 0;    /* _machine_type_door */
			*(float *)(obuf + 0x21Cu) = 2.5f;   /* device.automatic_activation_radius */
			*(uint16_t *)(obuf + 0x292u) |= 0x0003u; /* _machine_is_pathfinding_obstacle_bit | _machine_is_not_pathfinding_obstacle_when_open_bit */
			*(float *)(obuf + 0x294u) = 3.0f;   /* machine.door_open_time */
			*(int32_t *)(obuf + 0x320u) = 90;   /* machine.runtime_door_open_ticks (3.0f * 30) */
		}
	}
}

static long find_native_or_mp_variant_tag(uint32_t group_tag, char const *tag_path)
{
	long idx;
	long i;

	idx = tag_loaded((long)group_tag, tag_path);
	if (idx != NONE)
	{
		if (group_tag == 'vehi')
			patch_vehicle_tag_for_player_driving(idx, tag_path);
		patch_forge_device_tag_runtime(idx, group_tag, tag_path);
		return idx;
	}

	/* Map between SP and MP vehicle/weapon names natively present in MP maps */
	if (group_tag == 'vehi')
	{
		static struct { char const *a; char const *b; } const vehi_pairs[] = {
			{ "vehicles\\warthog\\warthog", "vehicles\\warthog\\mp_warthog" },
			{ "vehicles\\ghost\\ghost", "vehicles\\ghost\\ghost_mp" },
			{ "vehicles\\scorpion\\scorpion", "vehicles\\scorpion\\scorpion_mp" },
			{ "vehicles\\banshee\\banshee", "vehicles\\banshee\\banshee_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" }
		};
		for (i = 0; i < (long)(sizeof(vehi_pairs) / sizeof(vehi_pairs[0])); i++)
		{
			if (_stricmp(tag_path, vehi_pairs[i].a) == 0)
			{
				idx = tag_loaded('vehi', vehi_pairs[i].b);
				if (idx != NONE) return idx;
			}
			else if (_stricmp(tag_path, vehi_pairs[i].b) == 0)
			{
				idx = tag_loaded('vehi', vehi_pairs[i].a);
				if (idx != NONE) return idx;
			}
		}
	}
	return NONE;
}

int forge_find_credits_tag_by_hash(uint32_t hash, uint32_t *out_group_tag, char *out_tag_path, size_t out_path_sz)
{
	(void)hash;
	if (out_group_tag)
		*out_group_tag = 0;
	if (out_tag_path && out_path_sz > 0)
		out_tag_path[0] = '\0';
	return 0;
}

static long forge_try_get_loaded_engine_tag(
	uint32_t group_tag,
	char const *tag_path,
	uint32_t *out_actual_group,
	int *out_known_missing)
{
	long idx;
	int i;

	if (out_actual_group)
		*out_actual_group = group_tag;
	if (out_known_missing)
		*out_known_missing = 0;
	if (!tag_path || !tag_path[0])
		return NONE;

	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].group_tag == group_tag &&
			_stricmp(s_dyn_tags.entries[i].tag_path, tag_path) == 0)
		{
			if (s_dyn_tags.entries[i].definition_index == NONE)
			{
				if (out_known_missing)
					*out_known_missing = 1;
				return NONE;
			}
			if (out_actual_group)
				*out_actual_group = (uint32_t)tag_get_group_tag(s_dyn_tags.entries[i].definition_index);
			return s_dyn_tags.entries[i].definition_index;
		}
	}

	idx = find_native_or_mp_variant_tag(group_tag, tag_path);
	if (idx != NONE)
	{
		if (!forge_validate_safe_object_tag(idx))
			return NONE;
		if (group_tag == 'vehi')
			patch_vehicle_tag_for_player_driving(idx, tag_path);
		return idx;
	}
	return NONE;
}

long forge_resolve_engine_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	long idx;
	int known_missing = 0;

	idx = forge_try_get_loaded_engine_tag(group_tag, tag_path, out_actual_group, &known_missing);
	if (idx != NONE || known_missing)
		return idx;

	if (!tag_path || !tag_path[0])
		return NONE;

	return NONE;
}

extern void network_game_server_port_refresh_map_version(void);
extern int halo_server_mode(void);

static int s_forge_server_dirty_txt = 0;
static int s_forge_server_dirty_map = 0;
static unsigned long s_forge_server_last_edit_ms = 0;
static int s_forge_server_prev_player_count = 0;
static char s_pending_sidecar_leaf[64] = "";
static char s_active_sidecar_base_map[128] = "";

void forge_set_pending_sidecar_leaf(char const *leaf)
{
	if (leaf && leaf[0])
	{
		snprintf(s_pending_sidecar_leaf, sizeof(s_pending_sidecar_leaf), "%s", leaf);
	}
	else
	{
		s_pending_sidecar_leaf[0] = '\0';
		s_active_sidecar_base_map[0] = '\0';
	}
}

int forge_has_pending_or_active_sidecar(void)
{
	return (s_pending_sidecar_leaf[0] != '\0' || g_forge.live_instance_count > 0) ? 1 : 0;
}

static int forge_write_sidecar_state_file(char const *leaf);

static int does_custom_map_file_exist(char const *leaf)
{
	char d_map[160], host_map[512];
	FILE *f;

	if (!leaf || !leaf[0])
		return 0;

	snprintf(d_map, sizeof(d_map), "d:\\custom_maps\\%s.map", leaf);
	host_map[0] = '\0';
	platform_translate_path(d_map, host_map, sizeof(host_map));
	f = host_map[0] ? fopen(host_map, "rb") : NULL;
	if (!f)
	{
		snprintf(host_map, sizeof(host_map), "custom_maps/%s.map", leaf);
		f = fopen(host_map, "rb");
	}
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

int forge_ui_save_copy_as(char const *new_map_name)
{
	char clean_name[64];
	int was_already_saved = g_forge.has_saved_map_once;
	int ok;

	if (!new_map_name || !new_map_name[0])
	{
		char const *cur = forge_get_active_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		char const *slash2 = cur ? strrchr(cur, '/') : NULL;
		if (slash2 && (!slash || slash2 > slash))
			slash = slash2;
		if (halo_server_mode())
			snprintf(clean_name, sizeof(clean_name), "%s", slash ? (slash + 1) : (cur && cur[0] ? cur : "bloodgulch"));
		else
			snprintf(clean_name, sizeof(clean_name), "%s_forge", slash ? (slash + 1) : (cur && cur[0] ? cur : "bloodgulch"));
	}
	else
	{
		snprintf(clean_name, sizeof(clean_name), "%s", new_map_name);
	}

	{
		char d_dir[512];
		d_dir[0] = '\0';
		platform_translate_path("d:\\custom_maps", d_dir, sizeof(d_dir));
		if (d_dir[0])
			CreateDirectoryA(d_dir, NULL);
	}
	CreateDirectoryA("d:\\custom_maps", NULL);
	CreateDirectoryA("custom_maps", NULL);

	if (does_custom_map_file_exist(clean_name) && !halo_server_mode())
	{
		/* Never overwrite or shadow a non-Forge .map in custom_maps/ */
		char suffixed[64];
		snprintf(suffixed, sizeof(suffixed), "%s_forge", clean_name);
		snprintf(clean_name, sizeof(clean_name), "%s", suffixed);
	}
	ok = forge_write_sidecar_state_file(clean_name);

	if (ok)
	{
		char const *saved_ext = "txt";
		s_forge_server_dirty_txt = 0;
		s_forge_server_dirty_map = 0;
		forge_menu_custom_edition_maps_look_again();
		network_game_server_port_refresh_map_version();
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", clean_name);
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
		g_forge.has_saved_map_once = 1;

		if (was_already_saved)
		{
			forge_set_status("Updated saved map 'custom_maps/%s.%s' (%d placed, %d default edits)!",
				clean_name,
				saved_ext,
				(int)g_forge.edit_state.placed_object_count,
				(int)g_forge.edit_state.default_edit_count);
		}
		else
		{
			forge_set_status("Initial save complete: 'custom_maps/%s.%s'! Press F5 anytime to quick-update.",
				clean_name,
				saved_ext);
		}
		return 1;
	}
	forge_set_status("ERROR: Failed to export custom_maps/%s.txt", clean_name);
	return 0;
}

void forge_trigger_save_map_action(void)
{
	/* If connected as a client in a multiplayer Forge game, also tell a dedicated host to flush its map file */
	if (game_connection() == _game_connection_network_client)
	{
		extern void forge_send_packet_to_host(char const *ascii_payload);
		forge_send_packet_to_host("!FW");
	}

	/* If an initial save has already been done in this session (or loaded from Edit Maps), quick-update immediately! */
	if (g_forge.has_saved_map_once && g_forge.save_map_name[0])
	{
		g_forge.active_modal = _forge_modal_none;
		forge_ui_save_copy_as(g_forge.save_map_name);
		return;
	}

	/* Otherwise, open the initial save prompt so the user can name their custom map */
	if (!g_forge.save_map_name[0])
	{
		char const *cur = forge_get_active_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		char const *slash2 = cur ? strrchr(cur, '/') : NULL;
		if (slash2 && (!slash || slash2 > slash))
			slash = slash2;
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s_forge", slash ? (slash + 1) : (cur && cur[0] ? cur : "custom"));
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
	}
	g_forge.active_modal = _forge_modal_save_copy_as;
}

void forge_trigger_save_as_new_map_action(void)
{
	if (!g_forge.save_map_name[0])
	{
		char const *cur = forge_get_active_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		char const *slash2 = cur ? strrchr(cur, '/') : NULL;
		if (slash2 && (!slash || slash2 > slash))
			slash = slash2;
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s_forge", slash ? (slash + 1) : (cur && cur[0] ? cur : "custom"));
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
	}
	g_forge.active_modal = _forge_modal_save_copy_as;
}

extern void sync_live_player_spawns_to_scenario(void);
extern void sync_live_netgame_flags_to_scenario(void);

static void parse_forge_state_text_buffer(char const *text_buf, char const *leaf)
{
	char const *cur = text_buf;
	char line[512];
	int in_forge_state = 0;
	int cleared_default_spawns = 0;
	int cleared_default_flags = 0;
	int32_t last_object_li = -1;
	int32_t last_object_po = -1;
	int32_t last_default_li = -1;
	int32_t last_default_de = -1;
	int16_t placed_to_li[FORGE_MAX_PLACED_OBJECTS];
	int16_t def_to_li[FORGE_MAX_DEFAULT_OBJECT_EDITS];
	int map_i;

	if (!cur)
		return;

	for (map_i = 0; map_i < FORGE_MAX_PLACED_OBJECTS; map_i++)
		placed_to_li[map_i] = -1;
	for (map_i = 0; map_i < FORGE_MAX_DEFAULT_OBJECT_EDITS; map_i++)
		def_to_li[map_i] = -1;

	while (*cur)
	{
		size_t len = 0;
		while (cur[len] && cur[len] != '\r' && cur[len] != '\n')
			len++;
		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, cur, len);
		line[len] = 0;
		cur += len;
		while (*cur == '\r' || *cur == '\n')
			cur++;

		if (strcmp(line, "[OpenCE Forge Map]") == 0)
		{
			/* This map is already a saved Forge map: enable quick-update on F5! */
			snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", leaf);
			g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
			g_forge.has_saved_map_once = 1;
		}
		else if (strcmp(line, "[ForgeState]") == 0)
		{
			in_forge_state = 1;
		}
		else if (in_forge_state && strncmp(line, "OBJECT|", 7) == 0)
		{
			unsigned int grp = 0;
			int src_cache = 0, baked = 0, team = 0;
			float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0;
			char tpath[256];
			tpath[0] = 0;
			(void)leaf;
			(void)baked;
			if (sscanf(line + 7, "%u|%d|%d|%f|%f|%f|%f|%f|%f|%d|%255[^\r\n]",
					&grp, &src_cache, &baked, &x, &y, &z, &yaw, &pitch, &roll, &team, tpath) == 11 && tpath[0])
			{
				int dup_li = -1;
				int32_t chk_li;
				for (chk_li = 0; chk_li < g_forge.live_instance_count; ++chk_li)
				{
					struct forge_live_instance const *ci = &g_forge.live_instances[chk_li];
					float dx, dy, dz;
					if (!ci->active || ci->is_default_scenario_object)
						continue;
					if (ci->group_tag != (uint32_t)grp || strcmp(ci->tag_path, tpath) != 0)
						continue;
					dx = ci->x - x;
					dy = ci->y - y;
					dz = ci->z - z;
					if (dx * dx + dy * dy + dz * dz < 0.0025f)
					{
						dup_li = (int)chk_li;
						break;
					}
				}
				if (dup_li >= 0)
				{
					last_object_li = dup_li;
					last_object_po = g_forge.live_instances[dup_li].edit_entry_index;
				}
				else
				{
					last_object_li = forge_3d_spawn_object_at((uint32_t)grp, tpath, (uint8_t)src_cache, x, y, z, yaw, pitch, roll, (int16_t)team, 0);
					last_object_po = (last_object_li >= 0) ? g_forge.live_instances[last_object_li].edit_entry_index : -1;
					if (last_object_po >= 0 && last_object_po < FORGE_MAX_PLACED_OBJECTS)
						placed_to_li[last_object_po] = (int16_t)last_object_li;
				}
			}
		}
		else if (in_forge_state && strncmp(line, "DEFAULT_EDIT|", 13) == 0)
		{
			unsigned int grp = 0;
			int deleted = 0;
			float ox = 0, oy = 0, oz = 0, nx = 0, ny = 0, nz = 0, nyaw = 0, npitch = 0, nroll = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 13, "%u|%d|%f|%f|%f|%f|%f|%f|%f|%f|%f|%255[^\r\n]",
					&grp, &deleted, &ox, &oy, &oz, &nx, &ny, &nz, &nyaw, &npitch, &nroll, tpath) == 12 &&
				g_forge.edit_state.default_edit_count < FORGE_MAX_DEFAULT_OBJECT_EDITS)
			{
				int32_t de_idx = g_forge.edit_state.default_edit_count++;
				struct forge_default_object_edit *de = &g_forge.edit_state.default_edits[de_idx];
				struct object_iterator iter;
				struct object_datum *obj;
				long found_oidx = NONE;
				float best_d2 = 36.0f;

				memset(de, 0, sizeof(*de));
				de->active = 1;
				de->group_tag = (uint32_t)grp;
				de->deleted = (uint8_t)deleted;
				de->orig_x = ox; de->orig_y = oy; de->orig_z = oz;
				de->new_x = nx; de->new_y = ny; de->new_z = nz;
				de->new_yaw = nyaw; de->new_pitch = npitch; de->new_roll = nroll;
				snprintf(de->tag_path, sizeof(de->tag_path), "%s", tpath);

				/* Also update scenario placement blocks in RAM */
				{
					struct scenario *scen_ram = global_scenario_try_and_get();
					if (scen_ram)
					{
						long si;
						for (si = 0; si < scen_ram->netgame_equipment.count; si++)
						{
							struct scenario_netgame_equipment *eq = TAG_BLOCK_GET_ELEMENT(
								&scen_ram->netgame_equipment, si, struct scenario_netgame_equipment);
							float dx = eq->position.x - ox;
							float dy = eq->position.y - oy;
							float dz = eq->position.z - oz;
							if (dx * dx + dy * dy + dz * dz < 16.0f)
							{
								if (deleted)
								{
									eq->item_collection.index = NONE;
									eq->position.x = -99999.0f;
									eq->position.y = -99999.0f;
									eq->position.z = -99999.0f;
								}
								else
								{
									eq->position.x = nx;
									eq->position.y = ny;
									eq->position.z = nz;
									eq->facing = nyaw;
								}
								break;
							}
						}
						for (si = 0; si < scen_ram->vehicles.count; si++)
						{
							struct scenario_object_datum *sv = (struct scenario_object_datum *)tag_block_get_element_fast(
								&scen_ram->vehicles, si, 120);
							if (sv)
							{
								float dx = sv->position.x - ox;
								float dy = sv->position.y - oy;
								float dz = sv->position.z - oz;
								if (dx * dx + dy * dy < 16.0f && (float)fabs((double)dz) < 12.0f)
								{
									if (deleted)
									{
										sv->palette_entry_index = NONE;
										sv->position.x = -99999.0f;
										sv->position.y = -99999.0f;
										sv->position.z = -99999.0f;
									}
									else
									{
										sv->position.x = nx;
										sv->position.y = ny;
										sv->position.z = nz;
										sv->rotation.yaw = nyaw;
										sv->rotation.pitch = npitch;
										sv->rotation.roll = nroll;
									}
									break;
								}
							}
						}
					}
				}

				/* Apply default edit live if the scenario object is still at its unedited origin */
				{
					int li;
					int32_t matched_li = -1;
					float max_xy_d2 = (grp == 'scen' || grp == 'mach' || grp == 'lifi' || grp == 'ctrl') ? 0.04f : 16.0f;
					float best_li_d2 = max_xy_d2;

					for (li = 0; li < g_forge.live_instance_count; li++)
					{
						struct forge_live_instance *inst = &g_forge.live_instances[li];
						float dx, dy, dz, d2;
						if (!inst->active || !inst->is_default_scenario_object || inst->edit_entry_index >= 0)
							continue;
						if (inst->group_tag != (uint32_t)grp || _stricmp(inst->tag_path, tpath) != 0)
							continue;
						dx = inst->orig_x - ox;
						dy = inst->orig_y - oy;
						dz = inst->orig_z - oz;
						d2 = dx * dx + dy * dy + dz * dz * 0.25f;
						if (dx * dx + dy * dy <= max_xy_d2 && (float)fabs((double)dz) <= 12.0f && d2 < best_li_d2)
						{
							best_li_d2 = d2;
							matched_li = li;
							found_oidx = inst->object_index;
						}
					}

					if (matched_li < 0)
					{
						best_d2 = max_xy_d2;
						object_iterator_new(&iter, (unsigned long)-1, 0);
						while ((obj = object_iterator_next(&iter)) != NULL)
						{
							float dx = obj->object.position.x - ox;
							float dy = obj->object.position.y - oy;
							float dz = obj->object.position.z - oz;
							float d2 = dx * dx + dy * dy + dz * dz * 0.25f;
							int claimed = 0;
							if (obj->object.parent_object_index != NONE)
								continue;
							if (obj->object.type == _object_type_biped &&
								((struct unit_datum *)obj)->unit.player_index != NONE)
								continue;
							if (obj->definition_index != NONE)
							{
								char const *oname = tag_get_name(obj->definition_index);
								if (oname && oname[0] && _stricmp(oname, tpath) != 0)
									continue;
							}
							for (li = 0; li < g_forge.live_instance_count; li++)
							{
								if (g_forge.live_instances[li].active &&
									g_forge.live_instances[li].object_index == iter.index &&
									(!g_forge.live_instances[li].is_default_scenario_object ||
									 g_forge.live_instances[li].edit_entry_index >= 0))
								{
									claimed = 1;
									break;
								}
							}
							if (claimed)
								continue;
							if (dx * dx + dy * dy <= max_xy_d2 && (float)fabs((double)dz) <= 12.0f && d2 < best_d2)
							{
								best_d2 = d2;
								found_oidx = iter.index;
							}
						}
					}
				}
				if (found_oidx != NONE)
				{
					if (deleted)
					{
						int li, recorded_del = 0;
						for (li = 0; li < g_forge.live_instance_count; li++)
						{
							if (g_forge.live_instances[li].active &&
								g_forge.live_instances[li].object_index == found_oidx)
							{
								g_forge.live_instances[li].active = 0;
								g_forge.live_instances[li].object_index = NONE;
								g_forge.live_instances[li].edit_entry_index = de_idx;
								recorded_del = 1;
							}
						}
						if (!recorded_del && g_forge.live_instance_count < FORGE_MAX_LIVE_INSTANCES)
						{
							struct forge_live_instance *dinst = &g_forge.live_instances[g_forge.live_instance_count++];
							memset(dinst, 0, sizeof(*dinst));
							dinst->active = 0;
							dinst->object_index = NONE;
							dinst->edit_entry_index = de_idx;
							dinst->group_tag = (uint32_t)grp;
							dinst->is_default_scenario_object = 1;
							dinst->death_tick = -1;
							dinst->orig_x = ox;
							dinst->orig_y = oy;
							dinst->orig_z = oz;
							snprintf(dinst->tag_path, sizeof(dinst->tag_path), "%s", tpath);
						}
						object_delete(found_oidx);
					}
					else
					{
						int32_t li_idx = forge_3d_adopt_or_find_object_instance(found_oidx);
						last_default_de = de_idx;
						last_default_li = li_idx;
						if (de_idx >= 0 && de_idx < FORGE_MAX_DEFAULT_OBJECT_EDITS)
							def_to_li[de_idx] = (int16_t)li_idx;
						if (li_idx >= 0)
						{
							g_forge.live_instances[li_idx].edit_entry_index = de_idx;
							forge_3d_move_instance(li_idx, nx, ny, nz, nyaw, npitch, nroll, 0);
						}
					}
				}
			}
		}
		else if (in_forge_state && strncmp(line, "DEFAULT_GAMETYPE|", 17) == 0)
		{
			int didx = -1;
			unsigned int gflags = 0;
			int nread = sscanf(line + 17, "%d|%u", &didx, &gflags);
			if (nread == 1)
			{
				gflags = (unsigned int)didx;
				didx = last_default_de;
			}
			if (nread >= 1)
			{
				int32_t target_li = (didx >= 0 && didx < FORGE_MAX_DEFAULT_OBJECT_EDITS && def_to_li[didx] >= 0) ? def_to_li[didx] : last_default_li;
				if (didx >= 0 && didx < g_forge.edit_state.default_edit_count)
					g_forge.edit_state.default_edits[didx].gametype_flags = (uint16_t)gflags;
				if (target_li >= 0 && target_li < g_forge.live_instance_count)
					g_forge.live_instances[target_li].gametype_flags = (uint16_t)gflags;
			}
		}
		else if (in_forge_state && strncmp(line, "DEFAULT_PROPS|", 14) == 0)
		{
			int didx = -1, rsec = 0;
			unsigned int pmode = 0, gid = 0;
			if (sscanf(line + 14, "%d|%u|%d|%u",
					&didx, &pmode, &rsec, &gid) >= 4)
			{
				int32_t target_de = (didx >= 0 && didx < g_forge.edit_state.default_edit_count) ? didx : last_default_de;
				int32_t target_li = (target_de >= 0 && target_de < FORGE_MAX_DEFAULT_OBJECT_EDITS && def_to_li[target_de] >= 0)
					? def_to_li[target_de] : last_default_li;

				if (pmode >= NUMBER_OF_FORGE_PHYSICS_MODES) pmode = 0;
				if (gid > FORGE_MAX_WELD_GROUPS) gid = 0;
				if (rsec < 0) rsec = 0;

				if (target_de >= 0 && target_de < g_forge.edit_state.default_edit_count)
				{
					struct forge_default_object_edit *de = &g_forge.edit_state.default_edits[target_de];
					de->physics_mode = (uint8_t)pmode;
					de->respawn_timer_sec = (int16_t)rsec;
					de->group_id = (uint8_t)gid;
				}
				if (target_li >= 0 && target_li < g_forge.live_instance_count)
				{
					struct forge_live_instance *inst = &g_forge.live_instances[target_li];
					inst->physics_mode = (uint8_t)pmode;
					inst->respawn_timer_sec = (int16_t)rsec;
					inst->group_id = (uint8_t)gid;
					forge_3d_move_instance_ext(target_li, inst->x, inst->y, inst->z,
						inst->yaw, inst->pitch, inst->roll, 0, 0);
				}
			}
		}
		else if (in_forge_state && strncmp(line, "OBJECT_GAMETYPE|", 16) == 0)
		{
			unsigned int gflags = 0;
			if (sscanf(line + 16, "%u", &gflags) == 1)
			{
				if (last_object_po >= 0 && last_object_po < g_forge.edit_state.placed_object_count)
					g_forge.edit_state.placed_objects[last_object_po].gametype_flags = (uint16_t)gflags;
				else if (g_forge.edit_state.placed_object_count > 0)
					g_forge.edit_state.placed_objects[g_forge.edit_state.placed_object_count - 1].gametype_flags = (uint16_t)gflags;
				if (last_object_li >= 0 && last_object_li < g_forge.live_instance_count)
					g_forge.live_instances[last_object_li].gametype_flags = (uint16_t)gflags;
				else if (g_forge.live_instance_count > 0)
					g_forge.live_instances[g_forge.live_instance_count - 1].gametype_flags = (uint16_t)gflags;
			}
		}
		else if (in_forge_state && strncmp(line, "OBJECT_PROPS|", 13) == 0)
		{
			int pidx = -1, rsec = 0;
			unsigned int pmode = 0, gid = 0;
			int parsed = 0;
			if (sscanf(line + 13, "%d|%u|%d|%u",
					&pidx, &pmode, &rsec, &gid) == 4)
			{
				parsed = 1;
			}
			else if (sscanf(line + 13, "%u|%d|%u",
					&pmode, &rsec, &gid) >= 3)
			{
				pidx = last_object_po;
				parsed = 1;
			}
			if (parsed)
			{
				int32_t target_po = (pidx >= 0 && pidx < g_forge.edit_state.placed_object_count) ? pidx : last_object_po;
				int32_t target_li = (target_po >= 0 && target_po < FORGE_MAX_PLACED_OBJECTS && placed_to_li[target_po] >= 0)
					? placed_to_li[target_po] : last_object_li;

				if (pmode >= NUMBER_OF_FORGE_PHYSICS_MODES) pmode = 0;
				if (gid > FORGE_MAX_WELD_GROUPS) gid = 0;
				if (rsec < 0) rsec = 0;

				if (target_po >= 0 && target_po < g_forge.edit_state.placed_object_count)
				{
					struct forge_placed_object_entry *po = &g_forge.edit_state.placed_objects[target_po];
					po->physics_mode = (uint8_t)pmode;
					po->respawn_timer_sec = (int16_t)rsec;
					po->group_id = (uint8_t)gid;
				}
				if (target_li >= 0 && target_li < g_forge.live_instance_count)
				{
					struct forge_live_instance *inst = &g_forge.live_instances[target_li];
					inst->physics_mode = (uint8_t)pmode;
					inst->respawn_timer_sec = (int16_t)rsec;
					inst->group_id = (uint8_t)gid;
					forge_3d_move_instance_ext(target_li, inst->x, inst->y, inst->z,
						inst->yaw, inst->pitch, inst->roll, 0, 0);
				}
			}
		}
		else if (in_forge_state && strncmp(line, "SPAWN|", 6) == 0)
		{
			float sx = 0, sy = 0, sz = 0, syaw = 0;
			int steam = 0;
			unsigned int sgflags = 0;
			if (sscanf(line + 6, "%f|%f|%f|%f|%d|%u", &sx, &sy, &sz, &syaw, &steam, &sgflags) >= 5)
			{
				if (!cleared_default_spawns)
				{
					int li;
					cleared_default_spawns = 1;
					g_forge.edit_state.replace_player_spawns = 1;
					g_forge.edit_state.player_spawn_count = 0;
					for (li = 0; li < g_forge.live_instance_count; li++)
					{
						if (g_forge.live_instances[li].group_tag == 'spwn')
							g_forge.live_instances[li].active = 0;
					}
				}
				if (g_forge.live_instance_count < FORGE_MAX_LIVE_INSTANCES)
				{
					int32_t li_idx = g_forge.live_instance_count++;
					struct forge_live_instance *inst = &g_forge.live_instances[li_idx];
					memset(inst, 0, sizeof(*inst));
					inst->active = 1;
					inst->object_index = NONE;
					inst->edit_entry_index = -1;
					inst->group_tag = 'spwn';
					inst->team_designator = (int16_t)steam;
					inst->gametype_flags = (uint16_t)sgflags;
					inst->netgame_flag_type = (steam == 2) ? 13 : 12;
					inst->x = sx; inst->y = sy; inst->z = sz;
					inst->yaw = syaw;
					snprintf(inst->tag_path, sizeof(inst->tag_path), "%s",
						steam == 1 ? "spawn\\player_blue" : (steam == 2 ? "spawn\\player_ffa" : "spawn\\player_red"));
				}
			}
		}
		else if (in_forge_state && strncmp(line, "NETGAME_FLAG|", 13) == 0)
		{
			float fx = 0, fy = 0, fz = 0, fyaw = 0;
			int ftype = 0, fusage = 0;
			if (sscanf(line + 13, "%f|%f|%f|%f|%d|%d", &fx, &fy, &fz, &fyaw, &ftype, &fusage) >= 6)
			{
				if (!cleared_default_flags)
				{
					int li;
					cleared_default_flags = 1;
					g_forge.edit_state.replace_netgame_flags = 1;
					g_forge.edit_state.netgame_flag_count = 0;
					for (li = 0; li < g_forge.live_instance_count; li++)
					{
						if (g_forge.live_instances[li].group_tag == 'tele' ||
							g_forge.live_instances[li].group_tag == 'flag')
						{
							g_forge.live_instances[li].active = 0;
						}
					}
				}
				if (g_forge.live_instance_count < FORGE_MAX_LIVE_INSTANCES)
				{
					int32_t li_idx = g_forge.live_instance_count++;
					struct forge_live_instance *inst = &g_forge.live_instances[li_idx];
					memset(inst, 0, sizeof(*inst));
					inst->active = 1;
					inst->object_index = NONE;
					inst->edit_entry_index = -1;
					inst->group_tag = (ftype == 6 || ftype == 7) ? 'tele' : 'flag';
					inst->netgame_flag_type = (int16_t)ftype;
					inst->team_designator = (int16_t)fusage;
					inst->x = fx; inst->y = fy; inst->z = fz;
					inst->yaw = fyaw;
					if (ftype == 6)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "teleporter\\origin");
					else if (ftype == 7)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "teleporter\\destination");
					else if (ftype == 2)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "objective\\oddball_spawn");
					else if (ftype == 3)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "objective\\race_track");
					else if (ftype == 4)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "objective\\race_vehicle");
					else if (ftype == 5)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "objective\\vegas_bank");
					else if (ftype == 8)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "objective\\koth_hill");
					else if (ftype == 1)
						snprintf(inst->tag_path, sizeof(inst->tag_path), "%s",
							fusage == 1 ? "objective\\ctf_vehicle_blue" : "objective\\ctf_vehicle_red");
					else
						snprintf(inst->tag_path, sizeof(inst->tag_path), "%s",
							fusage == 1 ? "objective\\ctf_flag_blue" : "objective\\ctf_flag_red");
				}
			}
		}
	}
	if (cleared_default_spawns)
	{
		sync_live_player_spawns_to_scenario();
		if (game_connection() != _game_connection_network_client &&
			g_forge.edit_state.player_spawn_count > 0 && player_data)
		{
			long p_idx;
			int sp_cursor = 0;
			boolean teams = game_engine_has_teams();
			for (p_idx = data_next_index(player_data, NONE);
				 p_idx != NONE;
				 p_idx = data_next_index(player_data, p_idx))
			{
				struct player_datum *player = (struct player_datum *)datum_get(player_data, p_idx);
				if (player && player->unit_index != NONE)
				{
					int chosen_sp = -1;
					int s;
					for (s = 0; s < g_forge.edit_state.player_spawn_count; s++)
					{
						int idx = (sp_cursor + s) % g_forge.edit_state.player_spawn_count;
						int16_t td = g_forge.edit_state.player_spawns[idx].team_index;
						if (!teams || td < 0 || td == player->team_index)
						{
							chosen_sp = idx;
							break;
						}
					}
					if (chosen_sp < 0)
						chosen_sp = sp_cursor % g_forge.edit_state.player_spawn_count;
					sp_cursor++;
					{
						struct forge_player_spawn_entry const *sp = &g_forge.edit_state.player_spawns[chosen_sp];
						real_point3d pos;
						real_vector3d fwd, up;
						pos.x = sp->x;
						pos.y = sp->y;
						pos.z = sp->z + 0.05f;
						fwd.i = (float)cosine(sp->yaw);
						fwd.j = (float)sine(sp->yaw);
						fwd.k = 0.0f;
						up.i = 0.0f;
						up.j = 0.0f;
						up.k = 1.0f;
						object_set_position(player->unit_index, &pos, &fwd, &up);
					}
				}
			}
		}
	}
	if (cleared_default_flags)
		sync_live_netgame_flags_to_scenario();
}

void forge_load_saved_map_state_if_present(char const *map_name)
{
	char const *slash;
	char leaf[64];
	char txt_path[512];
	FILE *f;

	if (!map_name || !map_name[0])
		return;

	forge_undo_clear();

	slash = strrchr(map_name, '\\');
	if (!slash)
		slash = strrchr(map_name, '/');
	if (_stricmp(slash ? (slash + 1) : map_name, "ui") == 0)
	{
		s_pending_sidecar_leaf[0] = '\0';
		s_active_sidecar_base_map[0] = '\0';
		return;
	}
	{
		int has_pending_sidecar = (s_pending_sidecar_leaf[0] != '\0');
		if (has_pending_sidecar)
		{
			snprintf(leaf, sizeof(leaf), "%s", s_pending_sidecar_leaf);
			s_pending_sidecar_leaf[0] = '\0';
			snprintf(s_active_sidecar_base_map, sizeof(s_active_sidecar_base_map), "%s", forge_get_active_map_name());
		}
		else
		{
			snprintf(leaf, sizeof(leaf), "%s", slash ? (slash + 1) : map_name);
			s_active_sidecar_base_map[0] = '\0';
		}

		/* Patch any vehicles and custom scenery/devices/lights in the loaded map so vehicle seats and long-distance LOD rendering work */
		if (g_forge_session_enabled || has_pending_sidecar ||
			halo_server_mode() || forge_is_multiplayer_session())
		{
			long tag_cnt = 0, ti;
			struct forge_cache_file_tag_instance *instances =
				(struct forge_cache_file_tag_instance *)cache_files_tag_instances(&tag_cnt);
			if (instances && tag_cnt > 0)
			{
				for (ti = 0; ti < tag_cnt; ti++)
				{
					uint32_t g = (uint32_t)instances[ti].group_tag;
					if (!instances[ti].base_address)
						continue;
					if (g == 'vehi')
						patch_vehicle_tag_for_player_driving(instances[ti].tag_index, instances[ti].name);
					else if (g == 'scen' || g == 'mach' || g == 'ctrl')
						patch_forge_device_tag_runtime(instances[ti].tag_index, g, instances[ti].name);
				}
			}
			if (object_header_data)
			{
				unsigned long scen_mask =
					_object_mask_scenery |
					_object_mask_machine |
					_object_mask_control |
					_object_mask_light_fixture;
				long o_idx;
				for (o_idx = data_next_index(object_header_data, NONE);
					 o_idx != NONE;
					 o_idx = data_next_index(object_header_data, o_idx))
				{
					struct object_datum *obj = (struct object_datum *)object_try_and_get_and_verify_type(o_idx, scen_mask);
					if (obj && obj->definition_index != NONE)
					{
						uint8_t const *obuf = (uint8_t const *)object_definition_get(obj->definition_index);
						if (obuf)
						{
							float def_br = *(float const *)(obuf + 0x04u);
							if (obj->object.bounding_sphere_radius < def_br)
								obj->object.bounding_sphere_radius = def_br;
						}
					}
				}
			}
		}
		if (g_forge.initialized)
			forge_runtime_reload_catalogs();

		/* Clients receive the live Forge object state from the Host via !FH: / !FM: / !FD: packets */
		if (game_connection() == _game_connection_network_client)
			return;

		s_forge_server_dirty_txt = 0;
		s_forge_server_dirty_map = 0;
		s_forge_server_prev_player_count = 0;

		if (halo_server_mode())
		{
			forge_runtime_init();
			snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", leaf);
			g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
			g_forge.has_saved_map_once = 1;
		}

		/* 0. If a sidecar custom_maps/<leaf>.txt exists (either explicitly selected via Edit Maps / Forge Maps
		      or on a dedicated server), load it now.
		      When launching a stock map directly without selecting a custom sidecar, do NOT auto-load a sidecar. */
		f = NULL;
		if (has_pending_sidecar || halo_server_mode())
		{
			char d_txt[160];
			snprintf(d_txt, sizeof(d_txt), "d:\\custom_maps\\%s.txt", leaf);
			txt_path[0] = '\0';
			platform_translate_path(d_txt, txt_path, sizeof(txt_path));
			f = txt_path[0] ? fopen(txt_path, "rb") : NULL;
			if (!f)
			{
				snprintf(txt_path, sizeof(txt_path), "custom_maps/%s.txt", leaf);
				f = fopen(txt_path, "rb");
			}
		}
		if (f)
		{
			long sz;
			fseek(f, 0, SEEK_END);
			sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (sz > 0 && sz < 4 * 1024 * 1024)
			{
				char *buf = (char *)malloc((size_t)sz + 1u);
				if (buf)
				{
					size_t rd = fread(buf, 1, (size_t)sz, f);
					buf[rd] = 0;
					fclose(f);
					forge_runtime_init();
					snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", leaf);
					g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
					if (halo_server_mode() || has_pending_sidecar)
						g_forge.has_saved_map_once = 1;
					parse_forge_state_text_buffer(buf, leaf);
					free(buf);
					if (halo_server_mode())
						forge_ui_save_copy_as(leaf);
					return;
				}
			}
			fclose(f);
		}
	}
}

static int forge_write_sidecar_state_file(char const *leaf)
{
	char txt_path[512];
	char tmp_path[520];
	FILE *f;
	int i;
	int def_emit_idx = 0;
	int placed_emit_idx = 0;

	if (!leaf || !leaf[0])
		return 0;

	{
		char d_dir[512];
		d_dir[0] = '\0';
		platform_translate_path("d:\\custom_maps", d_dir, sizeof(d_dir));
		if (d_dir[0])
			CreateDirectoryA(d_dir, NULL);
	}
	CreateDirectoryA("d:\\custom_maps", NULL);
	CreateDirectoryA("custom_maps", NULL);

	{
		char d_txt[160];
		snprintf(d_txt, sizeof(d_txt), "d:\\custom_maps\\%s.txt", leaf);
		txt_path[0] = '\0';
		platform_translate_path(d_txt, txt_path, sizeof(txt_path));
		if (!txt_path[0])
			snprintf(txt_path, sizeof(txt_path), "custom_maps/%s.txt", leaf);
	}
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", txt_path);
	f = fopen(tmp_path, "wb");
	if (!f)
	{
		snprintf(txt_path, sizeof(txt_path), "custom_maps/%s.txt", leaf);
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", txt_path);
		f = fopen(tmp_path, "wb");
	}
	if (!f)
		return 0;

	{
		char base_map_str[128];
		int is_baked_val = 0;

		if (s_active_sidecar_base_map[0] != '\0')
			snprintf(base_map_str, sizeof(base_map_str), "%s", s_active_sidecar_base_map);
		else
			snprintf(base_map_str, sizeof(base_map_str), "%s", forge_get_active_map_name());

		/* Ensure any network-replicated or client-streamed live instances (including on a Joiner)
		   each have a distinct, up-to-date entry in edit_state before saving, and deduplicate
		   only exact overlapping placed_objects entries. */
		{
			int32_t li_idx;
			int p_i, p_j;
			uint8_t used_po[FORGE_MAX_PLACED_OBJECTS];
			memset(used_po, 0, sizeof(used_po));
			for (li_idx = 0; li_idx < g_forge.live_instance_count; ++li_idx)
			{
				struct forge_live_instance *inst = &g_forge.live_instances[li_idx];
				if (!inst->active || inst->is_default_scenario_object ||
					!inst->tag_path[0] || strncmp(inst->tag_path, "streaming\\", 10) == 0 ||
					inst->x < -90000.0f)
				{
					continue;
				}
				if (inst->group_tag != 'spwn' && inst->group_tag != 'tele' && inst->group_tag != 'flag')
				{
					int32_t po_idx = inst->edit_entry_index;
					if (po_idx < 0 || po_idx >= g_forge.edit_state.placed_object_count || used_po[po_idx])
					{
						if (g_forge.edit_state.placed_object_count < FORGE_MAX_PLACED_OBJECTS)
						{
							po_idx = g_forge.edit_state.placed_object_count++;
							inst->edit_entry_index = po_idx;
						}
						else
						{
							po_idx = -1;
						}
					}
					if (po_idx >= 0 && po_idx < FORGE_MAX_PLACED_OBJECTS)
					{
						struct forge_placed_object_entry *po = &g_forge.edit_state.placed_objects[po_idx];
						used_po[po_idx] = 1u;
						po->active = 1;
						po->source_cache_id = inst->source_cache_id;
						po->group_tag = inst->group_tag;
						po->gametype_flags = inst->gametype_flags;
						po->respawn_timer_sec = inst->respawn_timer_sec;
						po->physics_mode = inst->physics_mode;
						po->group_id = inst->group_id;
						snprintf(po->tag_path, sizeof(po->tag_path), "%s", inst->tag_path);
						po->x = inst->x;
						po->y = inst->y;
						po->z = inst->z;
						po->yaw = inst->yaw;
						po->pitch = inst->pitch;
						po->roll = inst->roll;
						po->team_index = inst->team_designator;
					}
				}
			}
			for (p_i = 0; p_i < g_forge.edit_state.placed_object_count; ++p_i)
			{
				struct forge_placed_object_entry *a = &g_forge.edit_state.placed_objects[p_i];
				if (!a->active || !a->tag_path[0] || a->x < -90000.0f)
					continue;
				for (p_j = p_i + 1; p_j < g_forge.edit_state.placed_object_count; ++p_j)
				{
					struct forge_placed_object_entry *b = &g_forge.edit_state.placed_objects[p_j];
					float dx, dy, dz;
					if (!b->active || b->group_tag != a->group_tag || strcmp(b->tag_path, a->tag_path) != 0)
						continue;
					dx = a->x - b->x;
					dy = a->y - b->y;
					dz = a->z - b->z;
					if (dx * dx + dy * dy + dz * dz < 0.0001f &&
						!used_po[p_j])
					{
						b->active = 0;
					}
				}
			}
		}

		fprintf(f,
			"[OpenCE Forge Map]\r\n"
			"ForgeSource=UserCreated\r\n"
			"Map Name: %s\r\n"
			"Base Map: %s\r\n"
			"Author: OpenCE : Forge Creator\r\n"
			"Objects Placed: %d\r\n"
			"[ForgeState]\r\n",
			leaf,
			base_map_str,
			(int)g_forge.edit_state.placed_object_count);

		for (i = 0; i < g_forge.edit_state.default_edit_count; i++)
		{
			struct forge_default_object_edit const *de = &g_forge.edit_state.default_edits[i];
			if (!de->active)
				continue;
			fprintf(f,
				"DEFAULT_EDIT|%u|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%s\r\n",
				(unsigned)de->group_tag, (int)de->deleted,
				de->orig_x, de->orig_y, de->orig_z,
				de->new_x, de->new_y, de->new_z,
				de->new_yaw, de->new_pitch, de->new_roll,
				de->tag_path);
			if (!de->deleted && de->gametype_flags != 0 && de->gametype_flags != 0x0FFFu)
			{
				fprintf(f, "DEFAULT_GAMETYPE|%d|%u\r\n", def_emit_idx, (unsigned)de->gametype_flags);
			}
			if (!de->deleted &&
				(de->physics_mode != 0 || de->respawn_timer_sec > 0 || de->group_id != 0))
			{
				fprintf(f,
					"DEFAULT_PROPS|%d|%u|%d|%u\r\n",
					def_emit_idx,
					(unsigned)de->physics_mode,
					(int)de->respawn_timer_sec,
					(unsigned)de->group_id);
			}
			def_emit_idx++;
		}
		for (i = 0; i < g_forge.edit_state.placed_object_count; i++)
		{
			struct forge_placed_object_entry const *po = &g_forge.edit_state.placed_objects[i];
			if (!po->active || !po->tag_path[0] || po->x < -90000.0f)
				continue;
			fprintf(f,
				"OBJECT|%u|%d|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%d|%s\r\n",
				(unsigned)po->group_tag,
				(int)po->source_cache_id,
				is_baked_val,
				po->x, po->y, po->z,
				po->yaw, po->pitch, po->roll,
				(int)po->team_index,
				po->tag_path);
		if (po->gametype_flags != 0 && po->gametype_flags != 0x0FFFu)
		{
			fprintf(f, "OBJECT_GAMETYPE|%u\r\n", (unsigned)po->gametype_flags);
		}
		if (po->physics_mode != 0 || po->respawn_timer_sec > 0 || po->group_id != 0)
		{
			fprintf(f,
				"OBJECT_PROPS|%d|%u|%d|%u\r\n",
				placed_emit_idx,
				(unsigned)po->physics_mode,
				(int)po->respawn_timer_sec,
				(unsigned)po->group_id);
		}
		placed_emit_idx++;
	}
	}
	if (g_forge.edit_state.replace_player_spawns)
	{
		for (i = 0; i < g_forge.edit_state.player_spawn_count; i++)
		{
			struct forge_player_spawn_entry const *sp = &g_forge.edit_state.player_spawns[i];
			int steam = (sp->type0 == 13) ? 2 : (int)sp->team_index;
			int smode = (sp->type0 == 13) ? (int)sp->type1 : (int)sp->type0;
			unsigned int sgflags =
				(smode == 1) ? 0x0001u :
				(smode == 2) ? 0x0002u :
				(smode == 3) ? 0x0004u :
				(smode == 4) ? 0x0008u :
				(smode == 5) ? 0x0010u : 0u;
			fprintf(f,
				"SPAWN|%.4f|%.4f|%.4f|%.4f|%d|%u\r\n",
				sp->x, sp->y, sp->z, sp->yaw,
				steam,
				sgflags);
		}
	}
	if (g_forge.edit_state.replace_netgame_flags)
	{
		for (i = 0; i < g_forge.edit_state.netgame_flag_count; i++)
		{
			struct forge_netgame_flag_entry const *nf = &g_forge.edit_state.netgame_flags[i];
			fprintf(f,
				"NETGAME_FLAG|%.4f|%.4f|%.4f|%.4f|%d|%d\r\n",
				nf->x, nf->y, nf->z, nf->yaw,
				(int)nf->type,
				(int)nf->usage_id);
		}
	}

	fclose(f);
	if (forge_host_rename(tmp_path, txt_path) != 0)
	{
		remove(tmp_path);
		return 0;
	}
	return 1;
}

void forge_server_mark_dirty(void)
{
	s_forge_server_dirty_txt = 1;
	s_forge_server_dirty_map = 1;
	s_forge_server_last_edit_ms = GetTickCount() | 1u;
}

int forge_server_save_map_if_dirty(int force_full_export)
{
	char leaf[64];

	if (!g_forge_session_enabled || game_connection() == _game_connection_network_client)
		return 0;
	if (!halo_server_mode() && !g_forge.has_saved_map_once)
		return 0;

	if (g_forge.save_map_name[0])
	{
		snprintf(leaf, sizeof(leaf), "%s", g_forge.save_map_name);
	}
	else
	{
		char const *cur = forge_get_active_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		char const *slash2 = cur ? strrchr(cur, '/') : NULL;
		if (slash2 && (!slash || slash2 > slash))
			slash = slash2;
		snprintf(leaf, sizeof(leaf), "%s", slash ? (slash + 1) : (cur ? cur : ""));
	}

	if (!leaf[0] || _stricmp(leaf, "ui") == 0)
		return 0;

	if (force_full_export)
	{
		if (!s_forge_server_dirty_map)
			return 0;
		return forge_ui_save_copy_as(leaf);
	}

	if (s_forge_server_dirty_txt)
	{
		if (forge_write_sidecar_state_file(leaf))
		{
			s_forge_server_dirty_txt = 0;
			return 1;
		}
	}
	return 0;
}

void forge_server_autosave_tick(void)
{
	unsigned long now;
	int player_cnt = 0;

	if (!halo_server_mode() || !g_forge_session_enabled || game_connection() == _game_connection_network_client)
		return;

	if (player_data)
	{
		long p_idx;
		for (p_idx = data_next_index(player_data, NONE); p_idx != NONE; p_idx = data_next_index(player_data, p_idx))
			player_cnt++;
	}

	now = GetTickCount() | 1u;

	/* 1. Flush lightweight crash-recovery sidecar (<4 KB) 5 seconds after the last edit while players are online */
	if (s_forge_server_dirty_txt && s_forge_server_last_edit_ms && (now - s_forge_server_last_edit_ms) >= 5000u)
	{
		forge_server_save_map_if_dirty(0);
	}

	/* 2. When the last player leaves the server after making edits, overwrite custom_maps/<leaf>.map in full! */
	if (s_forge_server_prev_player_count > 0 && player_cnt == 0 && s_forge_server_dirty_map)
	{
		forge_server_save_map_if_dirty(1);
	}

	s_forge_server_prev_player_count = player_cnt;
}

/* ---------- MONITOR ("characters\monitor\monitor") & NATIVE MODEL RENDERER — - Renders 343 Guilty Spark ("characters\monitor\monitor") and 3D Spawn Menu — previews directly through OpenCE's native orig_render_model! */

static int draw_native_object_model_at(
	long obj_def_idx,
	long unique_id,
	void const *lighting_ptr,
	union real_point3d const *pos,
	union real_vector3d const *forward,
	union real_vector3d const *up,
	int is_monitor,
	int is_preview,
	unsigned long flags)
{
	uint8_t const *obj_def;
	float radius;
	long model_idx;
	struct model const *mdef;
	struct real_matrix4x3 node_matrices[64];
	static struct render_lighting studio_lighting;
	struct render_lighting const *use_lighting;
	static union real_rgb_color change_colors_mon[4];
	static union real_rgb_color change_colors_std[4];
	static real function_values[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	union real_rgb_color *change_colors = is_monitor ? change_colors_mon : change_colors_std;
	union real_point3d draw_centroid = *pos;
	int node_count, n;

	if (obj_def_idx == NONE)
		return 0;
	obj_def = (uint8_t const *)tag_get(tag_get_group_tag(obj_def_idx), obj_def_idx);
	if (!obj_def)
		return 0;

	radius = *(float const *)(obj_def + 0x04u);
	if (!(radius > 0.1f && radius < 100.0f))
		radius = 1.5f;

	model_idx = *(int32_t const *)(obj_def + 0x34u);
	if (model_idx == NONE)
		return 0;
	mdef = (struct model const *)(tag_index_is_group(model_idx, 'mode') ?
		tag_get('mode', model_idx) :
		(tag_index_is_group(model_idx, 'mod2') ? tag_get('mod2', model_idx) : NULL));
	if (!mdef)
		return 0;

	node_count = (int)mdef->nodes.count;
	if (node_count <= 0)
		return 0;
	if (node_count > 64)
		node_count = 64;

	memset(node_matrices, 0, sizeof(node_matrices));
	if (is_preview)
	{
		float preview_scale = 0.52f / ((radius > 0.15f) ? radius : 0.52f);
		struct real_matrix4x3 scale_pos_mat;
		if (preview_scale < 0.035f) preview_scale = 0.035f;
		if (preview_scale > 1.35f) preview_scale = 1.35f;

		model_get_node_matrices(mdef, node_matrices, global_origin3d, forward, up);
		matrix4x3_from_point_and_vectors(&scale_pos_mat, pos, global_forward3d, global_up3d);
		scale_pos_mat.scale = preview_scale;
		for (n = 0; n < node_count; n++)
		{
			struct real_matrix4x3 tmp = node_matrices[n];
			matrix4x3_multiply(&scale_pos_mat, &tmp, &node_matrices[n]);
		}
		radius *= preview_scale;
	}
	else if (is_monitor)
	{
		union real_point3d mpos = *pos;
		struct real_matrix4x3 scale_pos_mat;
		mpos.z += 0.55f;
		draw_centroid = mpos;
		model_get_node_matrices(mdef, node_matrices, global_origin3d, forward, up);
		matrix4x3_from_point_and_vectors(&scale_pos_mat, &mpos, global_forward3d, global_up3d);
		scale_pos_mat.scale = 1.35f;
		for (n = 0; n < node_count; n++)
		{
			struct real_matrix4x3 tmp = node_matrices[n];
			matrix4x3_multiply(&scale_pos_mat, &tmp, &node_matrices[n]);
		}
		radius *= 1.35f;
	}
	else
	{
		model_get_node_matrices(mdef, node_matrices, pos, forward, up);
	}

	if (is_monitor)
	{
		change_colors[0].red = 0.28f;
		change_colors[0].green = 0.72f;
		change_colors[0].blue = 1.0f;
		change_colors[1].red = 0.65f;
		change_colors[1].green = 0.90f;
		change_colors[1].blue = 1.0f;
	}
	else
	{
		change_colors[0].red = 0.85f;
		change_colors[0].green = 0.85f;
		change_colors[0].blue = 0.85f;
		change_colors[1].red = 0.75f;
		change_colors[1].green = 0.75f;
		change_colors[1].blue = 0.75f;
	}
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[1];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	if (lighting_ptr)
	{
		use_lighting = (struct render_lighting const *)lighting_ptr;
	}
	else
	{
		memset(&studio_lighting, 0, sizeof(studio_lighting));
		studio_lighting.ambient_color.red = 0.72f;
		studio_lighting.ambient_color.green = 0.74f;
		studio_lighting.ambient_color.blue = 0.78f;
		studio_lighting.distant_light_count = 1;
		studio_lighting.distant_lights[0].color.red = 0.95f;
		studio_lighting.distant_lights[0].color.green = 0.95f;
		studio_lighting.distant_lights[0].color.blue = 0.92f;
		studio_lighting.distant_lights[0].direction.i = -0.577f;
		studio_lighting.distant_lights[0].direction.j = -0.577f;
		studio_lighting.distant_lights[0].direction.k = -0.577f;
		use_lighting = &studio_lighting;
	}

	orig_render_model(
		model_idx,
		10000.0f,
		node_matrices,
		NULL,
		change_colors,
		function_values,
		use_lighting,
		&draw_centroid,
		radius,
		NULL,
		(is_monitor || is_preview) ? NONE : unique_id,
		0,
		is_preview ? (flags | (1u << 3)) /* _render_model_first_person_bit */ : flags);
	return 1;
}

enum forge_marker_color_preset
{
	_marker_color_ffa_yellow = 0,
	_marker_color_red_team,
	_marker_color_blue_team,
	_marker_color_tele_green,
	_marker_color_race_orange,
	_marker_color_oddball_purple,
	_marker_color_koth_gold,
	NUMBER_OF_MARKER_COLOR_PRESETS
};

enum forge_marker_mesh_kind
{
	_marker_mesh_player_spawn = 0,
	_marker_mesh_tele_origin,
	_marker_mesh_tele_dest,
	_marker_mesh_race,
	_marker_mesh_oddball,
	_marker_mesh_ctf_flag,
	_marker_mesh_koth,
	_marker_mesh_koth_beam,
	NUMBER_OF_MARKER_MESH_KINDS
};

static struct
{
	int loaded;
	struct
	{
		int valid;
		int part_count;
		float radius;
		struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
	} meshes[NUMBER_OF_MARKER_MESH_KINDS];
} s_marker_meshes = { 0 };

static long get_or_create_translucent_marker_shader(int color_preset, int is_highlighted, int is_z_prime)
{
	static uint32_t const s_preset_rgb[NUMBER_OF_MARKER_COLOR_PRESETS] = {
		0x00FFE618u, /* FFA Player Spawn     - Translucent Yellow */
		0x00FF2424u, /* Red Team Spawn/Flag  - Translucent Red    */
		0x002488FFu, /* Blue Team Spawn/Flag - Translucent Blue   */
		0x0024FF54u, /* Teleporter In/Out    - Translucent Green  */
		0x00FF8C14u, /* Race Marker          - Translucent Orange */
		0x00BC38FFu, /* Oddball Marker       - Translucent Purple */
		0x00FFC824u  /* KOTH Hill Marker     - Translucent Gold   */
	};
	char shdr_name[96];
	char bitm_name[96];
	long shdr_idx, tmpl_soso_idx, bitm_idx;
	struct tag_iterator iter;
	void *tmpl_ptr;
	struct bitmap_data *bmp;
	struct bitmap_group *bgrp;
	uint8_t *soso_def;
	uint32_t rgb, alpha_byte, argb;
	uint32_t *pix;
	float r_f, g_f, b_f;
	int i;

	if (color_preset < 0 || color_preset >= NUMBER_OF_MARKER_COLOR_PRESETS)
		color_preset = _marker_color_ffa_yellow;

	snprintf(shdr_name, sizeof(shdr_name), "forge\\markers\\shdr_%d_%d_%d",
		color_preset, is_highlighted ? 1 : 0, is_z_prime ? 1 : 0);
	shdr_idx = tag_loaded('soso', shdr_name);
	if (shdr_idx != NONE)
		return shdr_idx;

	rgb = s_preset_rgb[color_preset];
	alpha_byte = is_z_prime ? 0xFFu : (is_highlighted ? 0xB8u : 0x8Cu);
	argb = (alpha_byte << 24) | rgb;
	r_f = (float)((rgb >> 16) & 0xFFu) / 255.0f;
	g_f = (float)((rgb >> 8) & 0xFFu) / 255.0f;
	b_f = (float)(rgb & 0xFFu) / 255.0f;

	snprintf(bitm_name, sizeof(bitm_name), "forge\\markers\\bitm_%d_%d_%d",
		color_preset, is_highlighted ? 1 : 0, is_z_prime ? 1 : 0);
	bitm_idx = tag_loaded('bitm', bitm_name);
	if (bitm_idx == NONE)
	{
		/* Format 11 = _bitmap_format_a8r8g8b8 (32-bit ARGB) */
		bmp = bitmap_2d_new(16, 16, 0, 11);
		if (!bmp || !bmp->base_address)
			return NONE;
		pix = (uint32_t *)bmp->base_address;
		for (i = 0; i < 16 * 16; i++)
			pix[i] = argb;
		bitmap_rebuild(bmp);

		bgrp = (struct bitmap_group *)calloc(1, sizeof(struct bitmap_group));
		if (!bgrp)
			return NONE;
		bgrp->bitmaps.count = 1;
		bgrp->bitmaps.address = bmp;
		bitm_idx = append_runtime_tag_instance('bitm', NONE, NONE, bitm_name, bgrp);
		if (bitm_idx == NONE)
			return NONE;
	}

	tag_iterator_new(&iter, 'soso');
	tmpl_soso_idx = tag_iterator_next(&iter);
	if (tmpl_soso_idx == NONE)
		return NONE;
	tmpl_ptr = shader_definition_get(tmpl_soso_idx);
	if (!tmpl_ptr)
		return NONE;

	soso_def = (uint8_t *)calloc(1, 512);
	if (!soso_def)
		return NONE;
	memcpy(soso_def, tmpl_ptr, 440);

	*(int16_t *)(soso_def + 0x24) = 4; /* _shader_type_model */
	/* Bit 1 = _shader_model_two_sided_bit, Bit 2 = _shader_model_not_alpha_tested_bit, Bit 3 = _shader_model_alpha_blended_decal_bit */
	*(uint16_t *)(soso_def + 0x28) = is_z_prime ? ((1u << 1) | (1u << 2)) : ((1u << 1) | (1u << 2) | (1u << 3));
	*(float *)(soso_def + 0x38) = is_highlighted ? 0.72f : 0.55f;
	*(int16_t *)(soso_def + 0x4C) = 0;
	*(uint16_t *)(soso_def + 0x6C) = 1u; /* _shader_model_self_illumination_no_random_phase_bit */
	*(int16_t *)(soso_def + 0x70) = 0;
	*(int16_t *)(soso_def + 0x72) = 0;
	*(float *)(soso_def + 0x74) = 1.0f;
	*(float *)(soso_def + 0x78) = r_f * 0.34f;
	*(float *)(soso_def + 0x7C) = g_f * 0.34f;
	*(float *)(soso_def + 0x80) = b_f * 0.34f;
	*(float *)(soso_def + 0x84) = r_f * 0.34f;
	*(float *)(soso_def + 0x88) = g_f * 0.34f;
	*(float *)(soso_def + 0x8C) = b_f * 0.34f;
	*(float *)(soso_def + 0x9C) = 1.0f;
	*(float *)(soso_def + 0xA0) = 1.0f;
	*(int32_t *)(soso_def + 0xA4) = 'bitm';
	*(int32_t *)(soso_def + 0xA8) = 0;
	*(int32_t *)(soso_def + 0xAC) = 0;
	*(int32_t *)(soso_def + 0xB0) = bitm_idx;
	*(int32_t *)(soso_def + 0xC8) = NONE;
	*(int16_t *)(soso_def + 0xD4) = 0;
	*(int16_t *)(soso_def + 0xD6) = 0;
	*(float *)(soso_def + 0xD8) = 1.0f;
	*(int32_t *)(soso_def + 0xE8) = NONE;
	*(float *)(soso_def + 0xEC) = 1.0f;
	*(float *)(soso_def + 0x144) = 0.0f;
	*(float *)(soso_def + 0x154) = 0.0f;
	*(int32_t *)(soso_def + 0x170) = NONE;

	return append_runtime_tag_instance('soso', 'shdr', NONE, shdr_name, soso_def);
}

struct forge_proc_mesh_builder
{
	uint8_t *verts;
	int32_t vtx_count;
	int32_t vtx_cap;
	uint16_t *strip;
	int32_t idx_count;
	int32_t idx_cap;
};

static uint16_t proc_mesh_add_vtx(
	struct forge_proc_mesh_builder *b,
	float x, float y, float z,
	float nx, float ny, float nz)
{
	uint8_t *vp;
	uint16_t idx;

	if (!b || b->vtx_count >= b->vtx_cap)
		return 0;
	idx = (uint16_t)b->vtx_count++;
	vp = b->verts + (uint32_t)idx * 68u;
	memset(vp, 0, 68u);
	*(float *)(vp + 0) = x;
	*(float *)(vp + 4) = y;
	*(float *)(vp + 8) = z;
	*(float *)(vp + 12) = nx;
	*(float *)(vp + 16) = ny;
	*(float *)(vp + 20) = nz;
	*(float *)(vp + 24) = 0.0f;
	*(float *)(vp + 28) = 1.0f;
	*(float *)(vp + 32) = 0.0f;
	*(float *)(vp + 36) = 1.0f;
	*(float *)(vp + 40) = 0.0f;
	*(float *)(vp + 44) = 0.0f;
	*(float *)(vp + 48) = 0.5f;
	*(float *)(vp + 52) = 0.5f;
	*(int16_t *)(vp + 56) = 0;
	*(int16_t *)(vp + 58) = 0;
	*(float *)(vp + 60) = 1.0f;
	*(float *)(vp + 64) = 0.0f;
	return idx;
}

static void proc_mesh_add_tri(
	struct forge_proc_mesh_builder *b,
	uint16_t i0, uint16_t i1, uint16_t i2)
{
	if (!b || b->idx_count + 6 > b->idx_cap)
		return;
	if (b->idx_count == 0)
	{
		b->strip[b->idx_count++] = i0;
		b->strip[b->idx_count++] = i1;
		b->strip[b->idx_count++] = i2;
	}
	else
	{
		uint16_t last = b->strip[b->idx_count - 1];
		b->strip[b->idx_count++] = last;
		b->strip[b->idx_count++] = last;
		b->strip[b->idx_count++] = i0;
		b->strip[b->idx_count++] = i0;
		b->strip[b->idx_count++] = i1;
		b->strip[b->idx_count++] = i2;
	}
}

static void proc_mesh_add_quad(
	struct forge_proc_mesh_builder *b,
	uint16_t i0, uint16_t i1, uint16_t i2, uint16_t i3)
{
	proc_mesh_add_tri(b, i0, i1, i2);
	proc_mesh_add_tri(b, i0, i2, i3);
}

static void proc_mesh_add_box(
	struct forge_proc_mesh_builder *b,
	float x0, float y0, float z0,
	float x1, float y1, float z1)
{
	uint16_t v0, v1, v2, v3;

	/* Top (+Z) */
	v0 = proc_mesh_add_vtx(b, x0, y0, z1, 0.0f, 0.0f, 1.0f);
	v1 = proc_mesh_add_vtx(b, x1, y0, z1, 0.0f, 0.0f, 1.0f);
	v2 = proc_mesh_add_vtx(b, x1, y1, z1, 0.0f, 0.0f, 1.0f);
	v3 = proc_mesh_add_vtx(b, x0, y1, z1, 0.0f, 0.0f, 1.0f);
	proc_mesh_add_quad(b, v0, v1, v2, v3);

	/* Bottom (-Z) */
	v0 = proc_mesh_add_vtx(b, x0, y1, z0, 0.0f, 0.0f, -1.0f);
	v1 = proc_mesh_add_vtx(b, x1, y1, z0, 0.0f, 0.0f, -1.0f);
	v2 = proc_mesh_add_vtx(b, x1, y0, z0, 0.0f, 0.0f, -1.0f);
	v3 = proc_mesh_add_vtx(b, x0, y0, z0, 0.0f, 0.0f, -1.0f);
	proc_mesh_add_quad(b, v0, v1, v2, v3);

	/* Front (+X) */
	v0 = proc_mesh_add_vtx(b, x1, y0, z0, 1.0f, 0.0f, 0.0f);
	v1 = proc_mesh_add_vtx(b, x1, y1, z0, 1.0f, 0.0f, 0.0f);
	v2 = proc_mesh_add_vtx(b, x1, y1, z1, 1.0f, 0.0f, 0.0f);
	v3 = proc_mesh_add_vtx(b, x1, y0, z1, 1.0f, 0.0f, 0.0f);
	proc_mesh_add_quad(b, v0, v1, v2, v3);

	/* Back (-X) */
	v0 = proc_mesh_add_vtx(b, x0, y1, z0, -1.0f, 0.0f, 0.0f);
	v1 = proc_mesh_add_vtx(b, x0, y0, z0, -1.0f, 0.0f, 0.0f);
	v2 = proc_mesh_add_vtx(b, x0, y0, z1, -1.0f, 0.0f, 0.0f);
	v3 = proc_mesh_add_vtx(b, x0, y1, z1, -1.0f, 0.0f, 0.0f);
	proc_mesh_add_quad(b, v0, v1, v2, v3);

	/* Left (+Y) */
	v0 = proc_mesh_add_vtx(b, x1, y1, z0, 0.0f, 1.0f, 0.0f);
	v1 = proc_mesh_add_vtx(b, x0, y1, z0, 0.0f, 1.0f, 0.0f);
	v2 = proc_mesh_add_vtx(b, x0, y1, z1, 0.0f, 1.0f, 0.0f);
	v3 = proc_mesh_add_vtx(b, x1, y1, z1, 0.0f, 1.0f, 0.0f);
	proc_mesh_add_quad(b, v0, v1, v2, v3);

	/* Right (-Y) */
	v0 = proc_mesh_add_vtx(b, x0, y0, z0, 0.0f, -1.0f, 0.0f);
	v1 = proc_mesh_add_vtx(b, x1, y0, z0, 0.0f, -1.0f, 0.0f);
	v2 = proc_mesh_add_vtx(b, x1, y0, z1, 0.0f, -1.0f, 0.0f);
	v3 = proc_mesh_add_vtx(b, x0, y0, z1, 0.0f, -1.0f, 0.0f);
	proc_mesh_add_quad(b, v0, v1, v2, v3);
}

static void proc_mesh_add_frustum_z(
	struct forge_proc_mesh_builder *b,
	float cx, float cy,
	float z0, float r0,
	float z1, float r1,
	int segs,
	int cap_bottom,
	int cap_top)
{
	int s;
	uint16_t bot_center = 0, top_center = 0;

	if (segs < 3)
		segs = 3;
	if (cap_bottom && r0 > 0.001f)
		bot_center = proc_mesh_add_vtx(b, cx, cy, z0, 0.0f, 0.0f, -1.0f);
	if (cap_top && r1 > 0.001f)
		top_center = proc_mesh_add_vtx(b, cx, cy, z1, 0.0f, 0.0f, 1.0f);

	for (s = 0; s < segs; s++)
	{
		float a0 = (2.0f * _pi * (float)s) / (float)segs;
		float a1 = (2.0f * _pi * (float)(s + 1)) / (float)segs;
		float c0 = cosine(a0), s0 = sine(a0);
		float c1 = cosine(a1), s1 = sine(a1);
		uint16_t b0 = proc_mesh_add_vtx(b, cx + r0 * c0, cy + r0 * s0, z0, c0, s0, 0.25f);
		uint16_t b1 = proc_mesh_add_vtx(b, cx + r0 * c1, cy + r0 * s1, z0, c1, s1, 0.25f);
		uint16_t t1 = proc_mesh_add_vtx(b, cx + r1 * c1, cy + r1 * s1, z1, c1, s1, 0.25f);
		uint16_t t0 = proc_mesh_add_vtx(b, cx + r1 * c0, cy + r1 * s0, z1, c0, s0, 0.25f);
		proc_mesh_add_quad(b, b0, b1, t1, t0);
		if (cap_bottom && r0 > 0.001f)
			proc_mesh_add_tri(b, bot_center, b1, b0);
		if (cap_top && r1 > 0.001f)
			proc_mesh_add_tri(b, top_center, t0, t1);
	}
}

static void proc_mesh_add_arrow_x(
	struct forge_proc_mesh_builder *b,
	float x_tail, float x_neck, float x_tip,
	float half_shaft_w, float half_head_w,
	float z0, float z1)
{
	uint16_t t_tip, t_hl, t_hr, b_tip, b_hl, b_hr;

	proc_mesh_add_box(b, x_tail, -half_shaft_w, z0, x_neck, half_shaft_w, z1);

	t_tip = proc_mesh_add_vtx(b, x_tip, 0.0f, z1, 0.0f, 0.0f, 1.0f);
	t_hl  = proc_mesh_add_vtx(b, x_neck, half_head_w, z1, 0.0f, 0.0f, 1.0f);
	t_hr  = proc_mesh_add_vtx(b, x_neck, -half_head_w, z1, 0.0f, 0.0f, 1.0f);
	proc_mesh_add_tri(b, t_hr, t_tip, t_hl);

	b_tip = proc_mesh_add_vtx(b, x_tip, 0.0f, z0, 0.0f, 0.0f, -1.0f);
	b_hl  = proc_mesh_add_vtx(b, x_neck, half_head_w, z0, 0.0f, 0.0f, -1.0f);
	b_hr  = proc_mesh_add_vtx(b, x_neck, -half_head_w, z0, 0.0f, 0.0f, -1.0f);
	proc_mesh_add_tri(b, b_hl, b_tip, b_hr);

	proc_mesh_add_quad(b, b_hr, b_tip, t_tip, t_hr);
	proc_mesh_add_quad(b, b_tip, b_hl, t_hl, t_tip);
	proc_mesh_add_quad(b, b_hl, b_hr, t_hr, t_hl);
}

static void proc_mesh_add_torus_yz(
	struct forge_proc_mesh_builder *b,
	float cx, float cy, float cz,
	float major_r, float minor_r,
	int maj_segs, int min_segs,
	int half_arch_only)
{
	int i, j;
	float max_angle = half_arch_only ? _pi : (2.0f * _pi);

	for (i = 0; i < maj_segs; i++)
	{
		float u0 = (max_angle * (float)i) / (float)maj_segs;
		float u1 = (max_angle * (float)(i + 1)) / (float)maj_segs;
		float cu0 = cosine(u0), su0 = sine(u0);
		float cu1 = cosine(u1), su1 = sine(u1);

		for (j = 0; j < min_segs; j++)
		{
			float v0 = (2.0f * _pi * (float)j) / (float)min_segs;
			float v1 = (2.0f * _pi * (float)(j + 1)) / (float)min_segs;
			float cv0 = cosine(v0), sv0 = sine(v0);
			float cv1 = cosine(v1), sv1 = sine(v1);

			uint16_t p00 = proc_mesh_add_vtx(b,
				cx + minor_r * sv0,
				cy + (major_r + minor_r * cv0) * cu0,
				cz + (major_r + minor_r * cv0) * su0,
				sv0, cv0 * cu0, cv0 * su0);
			uint16_t p10 = proc_mesh_add_vtx(b,
				cx + minor_r * sv0,
				cy + (major_r + minor_r * cv0) * cu1,
				cz + (major_r + minor_r * cv0) * su1,
				sv0, cv0 * cu1, cv0 * su1);
			uint16_t p11 = proc_mesh_add_vtx(b,
				cx + minor_r * sv1,
				cy + (major_r + minor_r * cv1) * cu1,
				cz + (major_r + minor_r * cv1) * su1,
				sv1, cv1 * cu1, cv1 * su1);
			uint16_t p01 = proc_mesh_add_vtx(b,
				cx + minor_r * sv1,
				cy + (major_r + minor_r * cv1) * cu0,
				cz + (major_r + minor_r * cv1) * su0,
				sv1, cv1 * cu0, cv1 * su0);
			proc_mesh_add_quad(b, p00, p10, p11, p01);
		}
	}
}

static void ensure_objective_marker_meshes_loaded(void)
{
	int k, all_valid = 1;

	if (s_marker_meshes.loaded)
	{
		for (k = 0; k < NUMBER_OF_MARKER_MESH_KINDS; k++)
			if (!s_marker_meshes.meshes[k].valid) { all_valid = 0; break; }
		if (all_valid)
			return;
	}
	s_marker_meshes.loaded = 1;

	for (k = 0; k < NUMBER_OF_MARKER_MESH_KINDS; k++)
	{
		struct forge_proc_mesh_builder mb;

		if (s_marker_meshes.meshes[k].valid && s_marker_meshes.meshes[k].part_count > 0)
			continue;
		s_marker_meshes.meshes[k].valid = 0;
		s_marker_meshes.meshes[k].part_count = 0;
		s_marker_meshes.meshes[k].radius = 0.45f;

		memset(&mb, 0, sizeof(mb));
		mb.vtx_cap = 2048;
		mb.idx_cap = 16384;
		mb.verts = (uint8_t *)malloc((size_t)mb.vtx_cap * 68u);
		mb.strip = (uint16_t *)malloc((size_t)mb.idx_cap * sizeof(uint16_t));
		if (!mb.verts || !mb.strip)
		{
			free(mb.verts);
			free(mb.strip);
			continue;
		}

		switch (k)
		{
		case _marker_mesh_player_spawn:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.032f, 0.156f, 20, 1, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.032f, 0.122f, 0.042f, 0.114f, 20, 0, 1);
			proc_mesh_add_arrow_x(&mb, -0.07f, 0.11f, 0.234f, 0.034f, 0.076f, 0.032f, 0.059f);
			s_marker_meshes.meshes[k].radius = 0.45f;
			break;

		case _marker_mesh_tele_origin:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.034f, 0.156f, 20, 1, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, -0.115f, 0.034f, 0.022f, 0.084f, 0.020f, 10, 0, 0);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.115f, 0.034f, 0.022f, 0.084f, 0.020f, 10, 0, 0);
			proc_mesh_add_torus_yz(&mb, 0.0f, 0.0f, 0.084f, 0.115f, 0.020f, 14, 8, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.068f, 0.004f, 0.110f, 0.036f, 8, 0, 0);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.110f, 0.036f, 0.152f, 0.004f, 8, 0, 0);
			proc_mesh_add_arrow_x(&mb, -0.04f, 0.09f, 0.180f, 0.028f, 0.064f, 0.034f, 0.056f);
			s_marker_meshes.meshes[k].radius = 0.45f;
			break;

		case _marker_mesh_tele_dest:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.034f, 0.156f, 20, 1, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, -0.115f, 0.034f, 0.022f, 0.084f, 0.020f, 10, 0, 0);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.115f, 0.034f, 0.022f, 0.084f, 0.020f, 10, 0, 0);
			proc_mesh_add_torus_yz(&mb, 0.0f, 0.0f, 0.084f, 0.115f, 0.020f, 14, 8, 1);
			proc_mesh_add_arrow_x(&mb, -0.02f, 0.11f, 0.220f, 0.034f, 0.076f, 0.034f, 0.060f);
			s_marker_meshes.meshes[k].radius = 0.45f;
			break;

		case _marker_mesh_race:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.034f, 0.156f, 20, 1, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.034f, 0.028f, 0.082f, 0.022f, 10, 0, 0);
			proc_mesh_add_torus_yz(&mb, 0.0f, 0.0f, 0.195f, 0.102f, 0.022f, 18, 8, 0);
			proc_mesh_add_arrow_x(&mb, -0.05f, 0.09f, 0.195f, 0.028f, 0.064f, 0.034f, 0.055f);
			s_marker_meshes.meshes[k].radius = 0.45f;
			break;

		case _marker_mesh_oddball:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.034f, 0.156f, 20, 1, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.034f, 0.064f, 0.130f, 0.026f, 12, 0, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.165f, 0.025f, 0.210f, 0.075f, 14, 1, 0);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.210f, 0.075f, 0.265f, 0.086f, 14, 0, 0);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.265f, 0.086f, 0.315f, 0.062f, 14, 0, 0);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.315f, 0.062f, 0.339f, 0.020f, 14, 0, 1);
			s_marker_meshes.meshes[k].radius = 0.45f;
			break;

		case _marker_mesh_ctf_flag:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.034f, 0.156f, 20, 1, 1);
			proc_mesh_add_arrow_x(&mb, 0.0f, 0.11f, 0.234f, 0.032f, 0.072f, 0.034f, 0.056f);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.034f, 0.016f, 0.545f, 0.014f, 10, 0, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.545f, 0.024f, 0.565f, 0.006f, 10, 1, 1);
			proc_mesh_add_box(&mb, -0.220f, -0.008f, 0.385f, 0.0f, 0.008f, 0.535f);
			s_marker_meshes.meshes[k].radius = 0.60f;
			break;

		case _marker_mesh_koth:
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.012f, 0.164f, 0.034f, 0.156f, 20, 1, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.034f, 0.044f, 0.215f, 0.028f, 12, 0, 1);
			proc_mesh_add_frustum_z(&mb, 0.0f, 0.0f, 0.195f, 0.078f, 0.263f, 0.108f, 12, 1, 1);
			s_marker_meshes.meshes[k].radius = 0.45f;
			break;

		case _marker_mesh_koth_beam:
		default:
			proc_mesh_add_box(&mb, 0.0f, -0.014f, 0.025f, 1.0f, 0.014f, 0.053f);
			s_marker_meshes.meshes[k].radius = 1.00f;
			break;
		}

		if (mb.vtx_count >= 3 && mb.idx_count >= 3)
		{
			build_hw_part_from_arrays(
				&s_marker_meshes.meshes[k].parts[0],
				mb.verts,
				mb.vtx_count,
				(uint8_t const *)mb.strip,
				mb.idx_count - 2,
				"",
				1.0f,
				0.0f);
			s_marker_meshes.meshes[k].part_count = 1;
			s_marker_meshes.meshes[k].valid = 1;
		}

		free(mb.verts);
		free(mb.strip);
	}
}

static void draw_translucent_marker_hw_parts(
	union real_point3d const *pos,
	union real_vector3d const *forward,
	union real_vector3d const *up,
	float radius,
	float scale,
	struct forge_hw_part *parts,
	int part_count,
	int color_preset,
	int is_highlighted,
	int is_preview)
{
	struct rasterizer_model_begin_parameters model_parameters;
	static union real_rgb_color change_colors[4];
	static real function_values[4];
	struct real_matrix4x3 node_matrix;
	long col_shdr_idx;
	struct shader *col_shdr_def;
	int p;

	if (!parts || part_count <= 0)
		return;

	col_shdr_idx = get_or_create_translucent_marker_shader(color_preset, is_highlighted, 0);
	if (col_shdr_idx == NONE)
		return;
	col_shdr_def = shader_definition_get(col_shdr_idx);
	if (!col_shdr_def)
		return;

	matrix4x3_from_point_and_vectors(&node_matrix, pos, forward, up);
	if (scale > 0.001f && scale != 1.0f)
		node_matrix.scale = scale;

	change_colors[0].red = 1.0f;
	change_colors[0].green = 1.0f;
	change_colors[0].blue = 1.0f;
	change_colors[1] = change_colors[0];
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[0];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	memset(&model_parameters, 0, sizeof(model_parameters));
	model_parameters.unique_identifier = NONE;
	model_parameters.lighting.ambient_color.red = is_highlighted ? 0.88f : 0.72f;
	model_parameters.lighting.ambient_color.green = is_highlighted ? 0.88f : 0.72f;
	model_parameters.lighting.ambient_color.blue = is_highlighted ? 0.88f : 0.72f;
	model_parameters.lighting.distant_light_count = 1;
	model_parameters.lighting.distant_lights[0].color.red = 0.95f;
	model_parameters.lighting.distant_lights[0].color.green = 0.95f;
	model_parameters.lighting.distant_lights[0].color.blue = 0.95f;
	model_parameters.lighting.distant_lights[0].direction.i = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.j = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.k = -0.77f;
	model_parameters.centroid = *pos;
	model_parameters.radius = (radius > 0.1f) ? (radius * ((scale > 0.001f) ? scale : 1.0f)) : 0.6f;
	model_parameters.animation.colors = change_colors;
	model_parameters.animation.values = function_values;
	model_parameters.skinning.node_matrices = &node_matrix;
	model_parameters.skinning.node_matrix_count = 1;
	model_parameters.geometry_flags = (1u << 2) | (is_preview ? (1u << 4) : 0u); /* _rasterizer_geometry_no_fog_bit */
	model_parameters.base_map_scale.i = 1.0f;
	model_parameters.base_map_scale.j = 1.0f;

	rasterizer_model_begin(&model_parameters, FALSE);

	for (p = 0; p < part_count; p++)
	{
		rasterizer_model_draw(
			col_shdr_def,
			0,
			&parts[p].triangle_buffer,
			NONE,
			parts[p].triangle_buffer.count,
			&parts[p].vertex_buffer,
			NONE);
	}
	rasterizer_model_end();
	if (is_preview)
	{
		rasterizer_set_stencil_mode(0);
		rasterizer_set_frustum_z(0.0f, 0.0f);
	}
}

static int resolve_marker_kind_and_color(
	uint32_t group_tag,
	char const *tag_path,
	int16_t netgame_flag_type,
	int16_t team_designator,
	int *out_mesh_kind,
	int *out_color_preset)
{
	if (group_tag == 'spwn' || (tag_path && strncmp(tag_path, "spawn\\", 6) == 0))
	{
		*out_mesh_kind = _marker_mesh_player_spawn;
		if (team_designator == 1 || (tag_path && strstr(tag_path, "blue")))
			*out_color_preset = _marker_color_blue_team;
		else if (team_designator == 2 || (tag_path && strstr(tag_path, "ffa")))
			*out_color_preset = _marker_color_ffa_yellow;
		else
			*out_color_preset = _marker_color_red_team;
		return 1;
	}
	if (group_tag == 'tele' || (tag_path && strncmp(tag_path, "teleporter\\", 11) == 0))
	{
		int is_sender = (netgame_flag_type == 6 /* _netgame_flag_teleporter_source */ ||
			(tag_path && (strstr(tag_path, "origin") || strstr(tag_path, "sender"))));
		*out_mesh_kind = is_sender ? _marker_mesh_tele_origin : _marker_mesh_tele_dest;
		*out_color_preset = _marker_color_tele_green;
		return 1;
	}
	if (group_tag == 'flag' || (tag_path && strncmp(tag_path, "objective\\", 10) == 0))
	{
		if (netgame_flag_type == 2 /* _netgame_flag_oddball_ball_spawn */ ||
			(tag_path && strstr(tag_path, "oddball")))
		{
			*out_mesh_kind = _marker_mesh_oddball;
			*out_color_preset = _marker_color_oddball_purple;
			return 1;
		}
		if (netgame_flag_type == 3 /* _netgame_flag_race_track */ ||
			netgame_flag_type == 4 /* _netgame_flag_race_vehicle */ ||
			(tag_path && strstr(tag_path, "race")))
		{
			*out_mesh_kind = _marker_mesh_race;
			*out_color_preset = _marker_color_race_orange;
			return 1;
		}
		if (netgame_flag_type == 0 /* _netgame_flag_ctf_flag */ ||
			netgame_flag_type == 1 /* _netgame_flag_ctf_vehicle */ ||
			(tag_path && strstr(tag_path, "ctf")))
		{
			*out_mesh_kind = _marker_mesh_ctf_flag;
			*out_color_preset = (team_designator == 1 || (tag_path && strstr(tag_path, "blue")))
				? _marker_color_blue_team
				: _marker_color_red_team;
			return 1;
		}
		*out_mesh_kind = _marker_mesh_koth;
		*out_color_preset = _marker_color_koth_gold;
		return 1;
	}
	return 0;
}

static void draw_translucent_koth_beam_3d(
	union real_point3d const *p0,
	union real_point3d const *p1)
{
	struct rasterizer_model_begin_parameters model_parameters;
	union real_rgb_color change_colors[4];
	real function_values[4];
	struct real_matrix4x3 node_matrix;
	long col_shdr_idx;
	struct shader *col_shdr_def;
	float dx, dy, dz, len_xy, len_3d;
	int p;

	if (!s_marker_meshes.meshes[_marker_mesh_koth_beam].valid ||
		s_marker_meshes.meshes[_marker_mesh_koth_beam].part_count <= 0)
		return;

	dx = p1->x - p0->x;
	dy = p1->y - p0->y;
	dz = p1->z - p0->z;
	len_xy = square_root(dx * dx + dy * dy);
	len_3d = square_root(dx * dx + dy * dy + dz * dz);
	if (len_3d < 0.01f)
		return;

	col_shdr_idx = get_or_create_translucent_marker_shader(_marker_color_koth_gold, 1, 0);
	if (col_shdr_idx == NONE)
		return;
	col_shdr_def = shader_definition_get(col_shdr_idx);
	if (!col_shdr_def)
		return;

	/*
	   Stretch the unit 3D beam (x in [0..1], y in [-0.015..+0.015], z in [0.025..0.055])
	   along forward = (p1 - p0) while keeping left and up unit-length so cross-section
	   thickness stays constant regardless of segment length!
	 */
	memset(&node_matrix, 0, sizeof(node_matrix));
	node_matrix.scale = 1.0f;
	node_matrix.position = *p0;
	node_matrix.forward.i = dx;
	node_matrix.forward.j = dy;
	node_matrix.forward.k = dz;
	if (len_xy > 0.001f)
	{
		node_matrix.left.i = -dy / len_xy;
		node_matrix.left.j = dx / len_xy;
		node_matrix.left.k = 0.0f;
	}
	else
	{
		node_matrix.left.i = 0.0f;
		node_matrix.left.j = 1.0f;
		node_matrix.left.k = 0.0f;
	}
	node_matrix.up.i = 0.0f;
	node_matrix.up.j = 0.0f;
	node_matrix.up.k = 1.0f;

	change_colors[0].red = 1.0f;
	change_colors[0].green = 1.0f;
	change_colors[0].blue = 1.0f;
	change_colors[1] = change_colors[0];
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[0];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	memset(&model_parameters, 0, sizeof(model_parameters));
	model_parameters.unique_identifier = NONE;
	model_parameters.lighting.ambient_color.red = 0.90f;
	model_parameters.lighting.ambient_color.green = 0.90f;
	model_parameters.lighting.ambient_color.blue = 0.90f;
	model_parameters.lighting.distant_light_count = 1;
	model_parameters.lighting.distant_lights[0].color.red = 0.95f;
	model_parameters.lighting.distant_lights[0].color.green = 0.95f;
	model_parameters.lighting.distant_lights[0].color.blue = 0.95f;
	model_parameters.lighting.distant_lights[0].direction.i = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.j = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.k = -0.77f;
	model_parameters.centroid.x = 0.5f * (p0->x + p1->x);
	model_parameters.centroid.y = 0.5f * (p0->y + p1->y);
	model_parameters.centroid.z = 0.5f * (p0->z + p1->z);
	model_parameters.radius = 0.5f * len_3d + 0.5f;
	model_parameters.animation.colors = change_colors;
	model_parameters.animation.values = function_values;
	model_parameters.skinning.node_matrices = &node_matrix;
	model_parameters.skinning.node_matrix_count = 1;
	model_parameters.geometry_flags = (1u << 2); /* _rasterizer_geometry_no_fog_bit */
	model_parameters.base_map_scale.i = 1.0f;
	model_parameters.base_map_scale.j = 1.0f;

	rasterizer_model_begin(&model_parameters, FALSE);
	for (p = 0; p < s_marker_meshes.meshes[_marker_mesh_koth_beam].part_count; p++)
	{
		rasterizer_model_draw(
			col_shdr_def,
			0,
			&s_marker_meshes.meshes[_marker_mesh_koth_beam].parts[p].triangle_buffer,
			NONE,
			s_marker_meshes.meshes[_marker_mesh_koth_beam].parts[p].triangle_buffer.count,
			&s_marker_meshes.meshes[_marker_mesh_koth_beam].parts[p].vertex_buffer,
			NONE);
	}
	rasterizer_model_end();
}

static void render_world_objective_and_spawn_markers(void)
{
	int i, hill_id;

	/* Objective markers, spawn markers, teleporter markers, and stream proxy markers are only visible in 3D Monitor Mode */
	if (!g_forge_session_enabled || g_forge.mode != _forge_mode_3d_monitor)
		return;

	ensure_objective_marker_meshes_loaded();

	for (i = 0; i < g_forge.live_instance_count; i++)
	{
		struct forge_live_instance const *inst = &g_forge.live_instances[i];
		int mesh_kind = 0, color_preset = 0;
		int is_hi;
		int is_stream_proxy = 0;
		float mesh_scale = 1.0f;
		float dx, dy, dz, d2;
		union real_point3d pos;
		union real_vector3d fwd, up;

		if (!inst->active)
			continue;
		if ((inst->pad_inst & 0x40u) != 0 && inst->object_index == NONE &&
			inst->group_tag != 'spwn' && inst->group_tag != 'tele' && inst->group_tag != 'flag')
		{
			is_stream_proxy = 1;
			mesh_kind = _marker_mesh_tele_origin;
			color_preset = _marker_color_blue_team;
			mesh_scale = 0.85f;
		}
		else
		{
			if (inst->group_tag != 'spwn' && inst->group_tag != 'tele' && inst->group_tag != 'flag')
				continue;
			if (!resolve_marker_kind_and_color(
					inst->group_tag,
					inst->tag_path,
					inst->netgame_flag_type,
					inst->team_designator,
					&mesh_kind,
					&color_preset))
				continue;
		}
		if (mesh_kind < 0 || mesh_kind >= NUMBER_OF_MARKER_MESH_KINDS ||
			!s_marker_meshes.meshes[mesh_kind].valid)
			continue;

		/* Player spawns, CTF flags, KOTH, Oddball, and Race markers are only visible while Forge Mode is active */
		if (!is_stream_proxy && g_forge.mode != _forge_mode_3d_monitor &&
			(mesh_kind == _marker_mesh_player_spawn ||
			 mesh_kind == _marker_mesh_ctf_flag ||
			 mesh_kind == _marker_mesh_koth ||
			 mesh_kind == _marker_mesh_oddball ||
			 mesh_kind == _marker_mesh_race))
		{
			continue;
		}

		if (mesh_kind == _marker_mesh_koth)
		{
			/* KOTH markers only highlight when actively clicked/grabbed (or during 10-node placement), never on hover/proximity */
			is_hi = (i == g_forge.grabbed_live_index) ||
					(g_forge.koth_group_placing && inst->team_designator == g_forge.koth_group_hill_id);
		}
		else
		{
			is_hi = (i == g_forge.grabbed_live_index || i == g_forge.hovered_live_index);
		}

		dx = inst->x - render.camera.position.x;
		dy = inst->y - render.camera.position.y;
		dz = inst->z - render.camera.position.z;
		d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > 300.0f * 300.0f && !is_hi)
			continue;

		pos.x = inst->x;
		pos.y = inst->y;
		pos.z = inst->z;
		fwd.i = (float)cosine(inst->yaw);
		fwd.j = (float)sine(inst->yaw);
		fwd.k = 0.0f;
		up.i = 0.0f;
		up.j = 0.0f;
		up.k = 1.0f;

		draw_translucent_marker_hw_parts(
			&pos,
			&fwd,
			&up,
			s_marker_meshes.meshes[mesh_kind].radius * mesh_scale,
			mesh_scale,
			s_marker_meshes.meshes[mesh_kind].parts,
			s_marker_meshes.meshes[mesh_kind].part_count,
			color_preset,
			is_hi,
			0);
	}

	/* Render 3D translucent KOTH boundary beams ONLY in Forge mode when a node of that hill is actually clicked/grabbed
	   (or actively being placed as a 10-point hill), connecting the exact 3D marker positions. */
	if (g_forge.mode != _forge_mode_3d_monitor)
		return;
	for (hill_id = 0; hill_id < 16; hill_id++)
	{
		int node_indices[32];
		float node_angles[32];
		int ncount = 0;
		int hill_grabbed = (g_forge.koth_group_placing && g_forge.koth_group_hill_id == hill_id) ? 1 : 0;
		float cx_h = 0.0f, cy_h = 0.0f;
		int a, b;

		for (i = 0; i < g_forge.live_instance_count && ncount < 32; i++)
		{
			struct forge_live_instance const *inst = &g_forge.live_instances[i];
			if (!inst->active || inst->group_tag != 'flag')
				continue;
			if (inst->netgame_flag_type != 8 /* _netgame_flag_hill */ && strstr(inst->tag_path, "koth") == NULL)
				continue;
			if (inst->team_designator != hill_id)
				continue;
			node_indices[ncount++] = i;
			cx_h += inst->x;
			cy_h += inst->y;
			if (i == g_forge.grabbed_live_index)
				hill_grabbed = 1;
		}

		if (ncount >= 2 && hill_grabbed)
		{
			cx_h /= (float)ncount;
			cy_h /= (float)ncount;

			for (a = 0; a < ncount; a++)
			{
				struct forge_live_instance const *na = &g_forge.live_instances[node_indices[a]];
				node_angles[a] = (float)atan2((double)(na->y - cy_h), (double)(na->x - cx_h));
			}
			for (a = 0; a < ncount - 1; a++)
			{
				for (b = a + 1; b < ncount; b++)
				{
					if (node_angles[b] < node_angles[a])
					{
						float ta = node_angles[a];
						int ti = node_indices[a];
						node_angles[a] = node_angles[b];
						node_indices[a] = node_indices[b];
						node_angles[b] = ta;
						node_indices[b] = ti;
					}
				}
			}

			for (a = 0; a < ncount; a++)
			{
				int nb = (a + 1) % ncount;
				struct forge_live_instance const *inst0 = &g_forge.live_instances[node_indices[a]];
				struct forge_live_instance const *inst1 = &g_forge.live_instances[node_indices[nb]];
				union real_point3d p0, p1;
				p0.x = inst0->x;
				p0.y = inst0->y;
				p0.z = inst0->z;
				p1.x = inst1->x;
				p1.y = inst1->y;
				p1.z = inst1->z;
				draw_translucent_koth_beam_3d(&p0, &p1);
			}
		}
	}
}

static void render_spawn_palette_3d_preview(long object_index, void const *lighting_ptr)
{
	int i, match_count = 0, chosen_idx = -1;
	struct forge_catalog_entry const *entry;
	union real_point3d preview_pos;
	union real_vector3d preview_fwd, preview_up, cam_right;
	float dist, angle;
	int marker_kind = -1, marker_color = 0;
	uint32_t actual_grp = 0;
	long obj_def_idx;

	if (g_forge.active_modal != _forge_modal_spawn_palette || g_forge.h3_menu_stage != 1)
		return;

	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (g_forge.catalog[i].category == g_forge.current_category &&
			forge_catalog_entry_allowed(&g_forge.catalog[i]))
		{
			if (match_count == g_forge.palette_cursor)
			{
				chosen_idx = i;
				break;
			}
			match_count++;
		}
	}
	if (chosen_idx < 0)
		return;

	entry = &g_forge.catalog[chosen_idx];

	/* Check if this catalog entry is one of our 3D Translucent Spawn/Teleporter/Objective Markers! */
	if (resolve_marker_kind_and_color(entry->group_tag, entry->tag_path, -1, -1, &marker_kind, &marker_color))
	{
		ensure_objective_marker_meshes_loaded();
		if (marker_kind >= 0 && marker_kind < NUMBER_OF_MARKER_MESH_KINDS &&
			s_marker_meshes.meshes[marker_kind].valid)
		{
			dist = 1.45f;
			cross_product3d(&render.camera.forward, &render.camera.up, &cam_right);
			preview_pos.x = render.camera.position.x + render.camera.forward.i * dist + cam_right.i * 0.42f - render.camera.up.i * 0.18f;
			preview_pos.y = render.camera.position.y + render.camera.forward.j * dist + cam_right.j * 0.42f - render.camera.up.j * 0.18f;
			preview_pos.z = render.camera.position.z + render.camera.forward.k * dist + cam_right.k * 0.42f - render.camera.up.k * 0.18f;

			angle = (float)(system_milliseconds() % 6000u) * (6.2831853f / 6000.0f);
			preview_fwd.i = (float)cosine(angle);
			preview_fwd.j = (float)sine(angle);
			preview_fwd.k = 0.0f;
			preview_up.i = 0.0f;
			preview_up.j = 0.0f;
			preview_up.k = 1.0f;

			draw_translucent_marker_hw_parts(
				&preview_pos,
				&preview_fwd,
				&preview_up,
				s_marker_meshes.meshes[marker_kind].radius,
				1.15f,
				s_marker_meshes.meshes[marker_kind].parts,
				s_marker_meshes.meshes[marker_kind].part_count,
				marker_color,
				1,
				1);
			return;
		}
	}

	{
		int known_missing = 0;
		obj_def_idx = forge_try_get_loaded_engine_tag(
			entry->group_tag,
			entry->tag_path,
			&actual_grp,
			&known_missing);
		if (obj_def_idx == NONE && !known_missing)
		{
			unsigned long now_ms = system_milliseconds();
			if (chosen_idx != s_last_preview_chosen_idx)
			{
				s_last_preview_chosen_idx = chosen_idx;
				s_last_preview_cursor_ms = now_ms;
				return;
			}
			if ((unsigned long)(now_ms - s_last_preview_cursor_ms) < 120u)
				return;
			obj_def_idx = forge_resolve_engine_tag(entry->group_tag, entry->tag_path, &actual_grp);
		}
		else
		{
			s_last_preview_chosen_idx = chosen_idx;
		}
	}
	if (obj_def_idx == NONE)
		return;

	dist = 1.55f;
	cross_product3d(&render.camera.forward, &render.camera.up, &cam_right);
	preview_pos.x = render.camera.position.x + render.camera.forward.i * dist + cam_right.i * 0.42f - render.camera.up.i * 0.14f;
	preview_pos.y = render.camera.position.y + render.camera.forward.j * dist + cam_right.j * 0.42f - render.camera.up.j * 0.14f;
	preview_pos.z = render.camera.position.z + render.camera.forward.k * dist + cam_right.k * 0.42f - render.camera.up.k * 0.14f;

	angle = (float)(system_milliseconds() % 6000u) * (6.2831853f / 6000.0f);
	preview_fwd.i = (float)cosine(angle);
	preview_fwd.j = (float)sine(angle);
	preview_fwd.k = 0.0f;
	preview_up.i = 0.0f;
	preview_up.j = 0.0f;
	preview_up.k = 1.0f;

	draw_native_object_model_at(
		obj_def_idx,
		object_index,
		lighting_ptr,
		&preview_pos,
		&preview_fwd,
		&preview_up,
		0,
		1,
		0);
}

void forge_render_world_3d_overlays(void const *lighting_ptr)
{
	if (!g_forge.initialized || (!g_forge_session_enabled && g_forge.live_instance_count <= 0))
		return;
	if (g_forge_session_enabled && g_forge.mode == _forge_mode_3d_monitor && g_forge.third_person_view && !s_local_monitor_drawn_this_frame)
	{
		long local_unit = player_control_get_unit_index(0);
		if (local_unit != NONE)
			forge_monitor_render_player_unit(local_unit, lighting_ptr, 0);
	}
	s_local_monitor_drawn_this_frame = 0;
	render_world_objective_and_spawn_markers();
	if (g_forge_session_enabled)
		render_spawn_palette_3d_preview(NONE, lighting_ptr);
}

int forge_monitor_render_player_unit(long object_index, void const *lighting_ptr, unsigned long flags)
{
	struct unit_datum *unit;

	if (!g_forge_session_enabled || object_index == NONE)
		return 0;

	/* Check if this object is a player biped in Forge Monitor mode (per-player in MP!) */
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);
	if (!unit || unit->object.type != _object_type_biped || unit->unit.player_index == NONE)
		return 0;

	if (!forge_is_unit_in_monitor_mode(object_index))
		return 0;

	if (flags & (1u << 1)) /* Skip shadow pass for Monitor */
		return 1;

	if (!s_monitor_resolved || s_monitor_bipd_handle == NONE)
	{
		uint32_t actual_grp = 'bipd';
		s_monitor_resolved = 1;
		s_monitor_bipd_handle = forge_resolve_engine_tag('bipd', "characters\\monitor\\monitor", &actual_grp);
	}
	if (s_monitor_bipd_handle == NONE)
		return 0;

	if (object_index == player_control_get_unit_index(0))
	{
		union real_point3d pos;
		union real_vector3d fwd, up;
		float cy, sy, cp, sp;

		if (!g_forge.third_person_view)
			return 1;
		s_local_monitor_drawn_this_frame = 1;

		/* Place local Monitor at exact live camera anchor (draw_native_object_model_at adds +0.55f to Z) */
		pos.x = g_forge.cam_x;
		pos.y = g_forge.cam_y;
		pos.z = g_forge.cam_z - 0.55f;

		cy = (float)cosine(g_forge.cam_yaw);
		sy = (float)sine(g_forge.cam_yaw);
		cp = (float)cosine(g_forge.cam_pitch);
		sp = (float)sine(g_forge.cam_pitch);
		fwd.i = cy * cp;
		fwd.j = sy * cp;
		fwd.k = sp;
		up.i = -cy * sp;
		up.j = -sy * sp;
		up.k = cp;

		return draw_native_object_model_at(
			s_monitor_bipd_handle,
			NONE,
			lighting_ptr,
			&pos,
			&fwd,
			&up,
			1,
			0,
			flags);
	}

	{
		union real_point3d pos = unit->object.position;
		union real_vector3d fwd = unit->object.forward;
		union real_vector3d up = unit->object.up;
		int p_abs = (int)((uint32_t)unit->unit.player_index & 0xFFFFu);

		if (p_abs >= 0 && p_abs < 128 && g_forge.player_has_monitor_pos[p_abs])
		{
			float rx = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pos[p_abs][0]
				: g_forge.player_monitor_pos[p_abs][0];
			float ry = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pos[p_abs][1]
				: g_forge.player_monitor_pos[p_abs][1];
			float rz = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pos[p_abs][2]
				: g_forge.player_monitor_pos[p_abs][2];
			float ryaw = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_yaw[p_abs]
				: g_forge.player_monitor_yaw[p_abs];
			float rpitch = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pitch[p_abs]
				: g_forge.player_monitor_pitch[p_abs];
			float cy = (float)cosine(ryaw);
			float sy = (float)sine(ryaw);
			float cp = (float)cosine(rpitch);
			float sp = (float)sine(rpitch);

			pos.x = rx;
			pos.y = ry;
			pos.z = rz - 0.55f;
			fwd.i = cy * cp;
			fwd.j = sy * cp;
			fwd.k = sp;
			up.i = -cy * sp;
			up.j = -sy * sp;
			up.k = cp;
		}

		return draw_native_object_model_at(
			s_monitor_bipd_handle,
			NONE,
			lighting_ptr,
			&pos,
			&fwd,
			&up,
			1,
			0,
			flags);
	}
}


/* ---------- FORGE TAG OPTIONS: ADD TAGS (MAP TAG BROWSER), INSTALLED TAGS & 3D PREVIEW */

#define FORGE_BROWSER_NUM_CATS 5

static int browser_group_matches_category(uint32_t grp, int category_tab)
{
	switch (category_tab)
	{
	case 0: /* SCENERY */
		return grp == 'scen';
	case 1: /* VEHICLES */
		return grp == 'vehi';
	case 2: /* DEVICES */
		return grp == 'mach' || grp == 'ctrl';
	case 3: /* WEAPONS */
		return grp == 'weap';
	case 4: /* EQUIPMENT */
		return grp == 'eqip';
	default:
		return 0;
	}
}

static char const *browser_group_type_label(uint32_t grp)
{
	switch (grp)
	{
	case 'scen': return "SCENERY (scen)";
	case 'vehi': return "VEHICLE (vehi)";
	case 'mach': return "DEVICE MACHINE (mach)";
	case 'ctrl': return "DEVICE CONTROL (ctrl)";
	case 'weap': return "WEAPON (weap)";
	case 'eqip': return "EQUIPMENT (eqip)";
	default:     return "OBJECT TAG";
	}
}

static void format_tag_byte_size(int32_t bytes, char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return;
	if (bytes <= 0)
		snprintf(out, out_sz, "N/A");
	else if (bytes >= 1024 * 1024)
		snprintf(out, out_sz, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024)
		snprintf(out, out_sz, "%.1f KB", (double)bytes / 1024.0);
	else
		snprintf(out, out_sz, "%d B", (int)bytes);
}

static int catalog_entry_matches_installed_tab(struct forge_catalog_entry const *e, int category_tab)
{
	if (!e || e->source_cache_id == 3 || e->group_tag == 'spwn' || e->group_tag == 'tele' || e->group_tag == 'flag')
		return 0;
	return browser_group_matches_category(e->group_tag, category_tab);
}

static int get_installed_catalog_index(int category_tab, int list_index)
{
	int i, found = 0;
	if (category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS || list_index < 0)
		return -1;
	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (catalog_entry_matches_installed_tab(&g_forge.catalog[i], category_tab))
		{
			if (found == list_index)
				return i;
			found++;
		}
	}
	return -1;
}

int forge_ui_installed_tag_count(int category_tab)
{
	int i, count = 0;
	if (category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS)
		return 0;
	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (catalog_entry_matches_installed_tab(&g_forge.catalog[i], category_tab))
			count++;
	}
	return count;
}

int forge_ui_installed_tag_get_info(
	int category_tab,
	int list_index,
	char *out_display_name,
	size_t disp_sz,
	char *out_full_path,
	size_t full_sz,
	char *out_type_str,
	size_t type_sz,
	char *out_size_str,
	size_t size_sz,
	int *out_is_custom)
{
	int cat_idx = get_installed_catalog_index(category_tab, list_index);
	struct forge_catalog_entry const *e;

	if (cat_idx < 0 || cat_idx >= g_forge.catalog_count)
		return 0;
	e = &g_forge.catalog[cat_idx];

	if (out_display_name && disp_sz > 0)
		snprintf(out_display_name, disp_sz, "%s", e->display_name);
	if (out_full_path && full_sz > 0)
		snprintf(out_full_path, full_sz, "%s", e->tag_path);
	if (out_type_str && type_sz > 0)
		snprintf(out_type_str, type_sz, "%s", browser_group_type_label(e->group_tag));
	if (out_is_custom)
		*out_is_custom = 0;
	if (out_size_str && size_sz > 0)
		format_tag_byte_size(0, out_size_str, size_sz);
	return 1;
}

int forge_ui_installed_tag_delete(int category_tab, int list_index)
{
	int cat_idx = get_installed_catalog_index(category_tab, list_index);

	if (cat_idx < 0 || cat_idx >= g_forge.catalog_count)
		return 0;
	forge_runtime_reload_catalogs();
	return 1;
}

uint32_t forge_compute_catalog_hash(uint32_t group_tag, char const *tag_path)
{
	uint32_t h = 2166136261u;
	char const *p;

	h = (h ^ ((group_tag >> 24) & 0xFFu)) * 16777619u;
	h = (h ^ ((group_tag >> 16) & 0xFFu)) * 16777619u;
	h = (h ^ ((group_tag >> 8) & 0xFFu)) * 16777619u;
	h = (h ^ (group_tag & 0xFFu)) * 16777619u;

	if (tag_path)
	{
		for (p = tag_path; *p; p++)
		{
			unsigned char c = (unsigned char)*p;
			if (c == '/')
				c = '\\';
			else if (c >= 'A' && c <= 'Z')
				c = (unsigned char)(c - 'A' + 'a');
			h = (h ^ c) * 16777619u;
		}
	}
	return h ? h : 1u;
}
