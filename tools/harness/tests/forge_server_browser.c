/*
FORGE_SERVER_BROWSER.C (test)

Tests strict server browser mode isolation between Forge Mode and Standard Mode
in port/linux/game/forge_hooks.c (forge_listing_is_forge, forge_filter_lobby_listings).
*/

#include "harness.h"

#include <stdint.h>

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

#include "defs.inc"
#include "under_test.inc"

static struct p2p_listing make_listing(
	char const *name,
	char const *gametype,
	unsigned short network_version,
	unsigned char kind,
	unsigned char dedicated)
{
	struct p2p_listing g;
	memset(&g, 0, sizeof(g));
	snprintf(g.name, sizeof(g.name), "%s", name);
	snprintf(g.map, sizeof(g.map), "bloodgulch");
	snprintf(g.gametype, sizeof(g.gametype), "%s", gametype);
	g.network_version = network_version;
	g.kind = kind;
	g.dedicated = dedicated;
	g.open = 1;
	g.player_count = 1;
	g.maximum_player_count = 16;
	return g;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("forge_mode_only")
	{
		struct p2p_listing games[7];
		int count;

		games[0] = make_listing("Std P2P v44", "Slayer", 44, 0, 0);
		games[1] = make_listing("Forge Ver", "Slayer", FORGE_NETWORK_VERSION, 0, 0);
		games[2] = make_listing("Std Ded v43", "CTF Pro", 43, 0x81, 1);
		games[3] = make_listing("Forge Kind", "Custom", 44, FORGE_GAME_KIND, 0);
		games[4] = make_listing("Forge HC Kind", "Custom", 44, (unsigned char)(FORGE_GAME_KIND | 0x80), 0);
		games[5] = make_listing("Forge Name", "forge", 44, 0, 1);
		games[6] = make_listing("Std Newer v45", "Race", 45, 0, 0);

		count = forge_filter_lobby_listings(games, 7, 1);
		CHECK(count == 4, "Forge mode keeps exactly the 4 Forge games");
		CHECK(strcmp(games[0].name, "Forge Ver") == 0, "keeps Forge by network_version");
		CHECK(strcmp(games[1].name, "Forge Kind") == 0, "keeps Forge by kind");
		CHECK(strcmp(games[2].name, "Forge HC Kind") == 0, "keeps Forge by kind with high bit");
		CHECK(strcmp(games[3].name, "Forge Name") == 0, "keeps Forge by gametype name");
		CHECK(games[0].kind == FORGE_GAME_KIND && games[0].network_version == FORGE_NETWORK_VERSION,
			"normalizes Forge listing kind and version");
		CHECK(games[3].kind == FORGE_GAME_KIND && games[3].network_version == FORGE_NETWORK_VERSION,
			"normalizes dedicated Forge listing kind and version");
	}
	else CASE("standard_mode_only")
	{
		struct p2p_listing games[7];
		int count;

		games[0] = make_listing("Std P2P v44", "Slayer", 44, 2, 0);
		games[1] = make_listing("Forge Ver", "Forge", FORGE_NETWORK_VERSION, FORGE_GAME_KIND, 0);
		games[2] = make_listing("Std Ded v43", "CTF Pro", 43, 0x81, 1);
		games[3] = make_listing("Forge Kind", "Custom", 44, FORGE_GAME_KIND, 0);
		games[4] = make_listing("Forge HC Kind", "Custom", 44, (unsigned char)(FORGE_GAME_KIND | 0x80), 0);
		games[5] = make_listing("Forge Name", "Forge", 44, 0, 1);
		games[6] = make_listing("Std Newer v45", "Race", 45, 3, 0);

		count = forge_filter_lobby_listings(games, 7, 0);
		CHECK(count == 3, "Standard mode hides all Forge games and keeps 3 Standard games");
		CHECK(strcmp(games[0].name, "Std P2P v44") == 0 && games[0].kind == 2 && games[0].network_version == 44,
			"keeps Std P2P v44 unmodified");
		CHECK(strcmp(games[1].name, "Std Ded v43") == 0 && games[1].kind == 0x81 && games[1].network_version == 43,
			"keeps Std Ded v43 unmodified");
		CHECK(strcmp(games[2].name, "Std Newer v45") == 0 && games[2].kind == 3 && games[2].network_version == 45,
			"keeps Std Newer v45 unmodified");
	}
	else CASE("empty_and_null")
	{
		struct p2p_listing dummy;
		CHECK(forge_filter_lobby_listings(NULL, 4, 1) == 0, "NULL list handled safely");
		CHECK(forge_filter_lobby_listings(&dummy, 0, 0) == 0, "zero count handled safely");
	}
	return 0;
}
