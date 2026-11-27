/*
FORGE_XBOX_CACHE.C (test)

Tests catalog allow/exclude filters (no AI/lights) and loaded-map object tag safety
validation from port/linux/game/forge_runtime_tags.c.
*/

#include "harness.h"

#include <math.h>
#include <stdint.h>

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

#define FORGE_TAG_PATH_MAX 256
#define MAXIMUM_NODES_PER_MODEL 64
#define MAXIMUM_GEOMETRIES_PER_MODEL 256
#define MAXIMUM_REGIONS_PER_MODEL 32

enum
{
	_object_type_biped = 0,
	_object_type_vehicle = 1,
	_object_type_weapon = 2,
	_object_type_equipment = 3,
	_object_type_scenery = 6,
	_object_type_machine = 7,
	_object_type_control = 8,
	_object_type_light_fixture = 9
};

struct tag_block_mock { int32_t count; void *address; };
struct tag_ref_mock { uint32_t group_tag; long index; };

struct _object_definition
{
	short type;
	real bounding_radius;
	real render_bounding_radius;
	real_point3d bounding_offset;
	real_point3d origin_offset;
	struct tag_block_mock attachments;
	struct tag_block_mock widgets;
	struct tag_ref_mock model;
	struct tag_ref_mock animation_graph;
	struct tag_ref_mock creation_effect;
};

struct model
{
	struct tag_block_mock nodes;
	struct tag_block_mock geometries;
	struct tag_block_mock regions;
};

struct animation_graph
{
	struct tag_block_mock nodes;
};

struct effect_part_definition
{
	struct tag_ref_mock reference;
};

struct effect_event_definition
{
	struct tag_block_mock parts;
};

struct effect_definition
{
	struct tag_block_mock events;
};

#include "defs.inc"

static struct forge_cache_file_tag_instance g_mock_tags[16];
static long g_mock_tag_count = 0;

static void *cache_files_tag_instances(long *count)
{
	if (count)
		*count = g_mock_tag_count;
	return g_mock_tags;
}

#include "under_test.inc"

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("catalog_filters")
	{
		struct forge_catalog_entry e;
		memset(&e, 0, sizeof(e));
		e.group_tag = 'weap';
		e.category = _forge_cat_weapons;
		strcpy(e.tag_path, "weapons\\pistol\\pistol");
		CHECK(compute_catalog_entry_allowed(&e), "stock pistol allowed");

		strcpy(e.tag_path, "weapons\\flamethrower\\flamethrower");
		CHECK(!compute_catalog_entry_allowed(&e), "flamethrower excluded");

		e.group_tag = 'lifi';
		e.category = _forge_cat_scenery;
		strcpy(e.tag_path, "levels\\a50\\devices\\light_ceiling\\light_ceiling");
		CHECK(!compute_catalog_entry_allowed(&e), "lifi rejected in Vanilla Forge");

		e.group_tag = 'ctrl';
		e.category = _forge_cat_barriers_devices;
		strcpy(e.tag_path, "levels\\b30\\devices\\control\\control");
		CHECK(!compute_catalog_entry_allowed(&e), "ctrl rejected from stock catalog");

		e.group_tag = 'mach';
		e.category = _forge_cat_barriers_devices;
		strcpy(e.tag_path, "levels\\a10\\devices\\door\\door small");
		CHECK(compute_catalog_entry_allowed(&e), "door mach allowed in barriers/doors");

		e.group_tag = 'scen';
		e.category = _forge_cat_scenery;
		e.source_cache_id = 3;
		strcpy(e.tag_path, "levels\\custom\\particle_my_prop");
		CHECK(compute_catalog_entry_allowed(&e), "[MAP] source_cache_id=3 always allowed");
	}
	else CASE("safe_tags")
	{
		struct _object_definition scen_def;
		struct model mod_def;
		memset(g_mock_tags, 0, sizeof(g_mock_tags));
		memset(&scen_def, 0, sizeof(scen_def));
		memset(&mod_def, 0, sizeof(mod_def));
		mod_def.nodes.count = 2;
		mod_def.geometries.count = 1;
		mod_def.regions.count = 1;

		scen_def.type = _object_type_scenery;
		scen_def.bounding_radius = 1.5f;
		scen_def.render_bounding_radius = 3.0f;
		scen_def.model.index = 1;
		scen_def.animation_graph.index = NONE;
		scen_def.creation_effect.index = NONE;

		g_mock_tags[0].group_tag = 'scen';
		g_mock_tags[0].tag_index = 0;
		g_mock_tags[0].base_address = &scen_def;
		g_mock_tags[1].group_tag = 'mod2';
		g_mock_tags[1].tag_index = 1;
		g_mock_tags[1].base_address = &mod_def;
		g_mock_tag_count = 2;

		CHECK(forge_validate_safe_object_tag(0), "valid scenery object passes safety check");
		CHECK(!forge_validate_safe_object_tag(NONE), "NONE rejected");

		scen_def.type = _object_type_vehicle;
		CHECK(!forge_validate_safe_object_tag(0), "mismatched odef->type rejected");
		scen_def.type = _object_type_scenery;

		scen_def.bounding_radius = NAN;
		CHECK(!forge_validate_safe_object_tag(0), "NaN bounding_radius rejected");
		scen_def.bounding_radius = 1.5f;

		mod_def.nodes.count = MAXIMUM_NODES_PER_MODEL + 1;
		CHECK(!forge_validate_safe_object_tag(0), "oversized model node count rejected");
	}
	else CASE("effect_refs")
	{
		struct _object_definition obj_def;
		struct effect_definition eff_def;
		struct effect_event_definition ev;
		struct effect_part_definition pt;
		memset(g_mock_tags, 0, sizeof(g_mock_tags));
		memset(&obj_def, 0, sizeof(obj_def));
		memset(&eff_def, 0, sizeof(eff_def));
		memset(&ev, 0, sizeof(ev));
		memset(&pt, 0, sizeof(pt));

		pt.reference.group_tag = 'ptcl';
		pt.reference.index = NONE;
		ev.parts.count = 1;
		ev.parts.address = &pt;
		eff_def.events.count = 1;
		eff_def.events.address = &ev;

		obj_def.type = _object_type_weapon;
		obj_def.bounding_radius = 0.5f;
		obj_def.render_bounding_radius = 1.0f;
		obj_def.model.index = NONE;
		obj_def.animation_graph.index = NONE;
		obj_def.creation_effect.index = 1;

		g_mock_tags[0].group_tag = 'weap';
		g_mock_tags[0].tag_index = 0;
		g_mock_tags[0].base_address = &obj_def;
		g_mock_tags[1].group_tag = 'effe';
		g_mock_tags[1].tag_index = 1;
		g_mock_tags[1].base_address = &eff_def;
		g_mock_tag_count = 2;

		CHECK(forge_validate_safe_object_tag(0), "benign creation_effect allowed");
		pt.reference.group_tag = 'actv';
		CHECK(!forge_validate_safe_object_tag(0), "creation_effect referencing AI actv rejected");
	}
	else CASE("catalog_validity")
	{
		CHECK(forge_xbox_catalog_text_is_valid("weap|0|0|weapons\\pistol\\pistol\n"), "base catalog line valid without lights or wraith");
		CHECK(forge_xbox_catalog_text_is_valid("vehi|2|0|vehicles\\warthog\\mp_warthog\n"), "vehicle catalog line valid");
		CHECK(!forge_xbox_catalog_text_is_valid("weap|0|3|weapons\\pistol\\pistol\n"), "map-local |3| entry does not validate Xbox cache");
		CHECK(!forge_xbox_catalog_text_is_valid("# comment only\n"), "comment-only catalog invalid");
		CHECK(!forge_xbox_catalog_text_is_valid(""), "empty string invalid");
		CHECK(!forge_xbox_catalog_text_is_valid(NULL), "NULL invalid");
	}
	return 0;
}
