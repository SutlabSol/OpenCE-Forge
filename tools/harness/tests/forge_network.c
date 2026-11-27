/*
FORGE_NETWORK.C (test)

Tests distributed Forge wire message definitions (108/109) and packet helpers
from network_distributed.h and port/linux/game/forge_network.c.
*/

#include "harness.h"

#include <math.h>
#include <stdint.h>

#define FORGE_TAG_PATH_MAX 256

#include "defs.inc"
#include "under_test.inc"

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("wire")
	{
		CHECK(_distributed_message_forge_up == 108, "client->host message ID must be 108");
		CHECK(_distributed_message_forge_down == 109, "host->client message ID must be 109");
		CHECK(DISTRIBUTED_FORGE_PAYLOAD_SIZE == 64, "payload size must be 64");
		CHECK(sizeof(struct distributed_forge_message) == 68, "wire struct must be 68 bytes");
	}
	else CASE("coords")
	{
		CHECK(forge_net_coords_valid(10.0f, -25.5f, 3.0f, 157, -50, 0), "normal coordinates accepted");
		CHECK(!forge_net_coords_valid(NAN, 0.0f, 0.0f, 0, 0, 0), "NaN coordinate rejected");
		CHECK(!forge_net_coords_valid(60000.0f, 0.0f, 0.0f, 0, 0, 0), "out-of-range coordinate rejected");
		CHECK(!forge_net_coords_valid(0.0f, 0.0f, 0.0f, 200000, 0, 0), "out-of-range yaw rejected");
	}
	else CASE("props")
	{
		struct forge_live_instance inst;
		char buf[64];
		memset(&inst, 0, sizeof(inst));
		inst.team_designator = 1;
		inst.gametype_flags = 0x0002u;
		inst.physics_mode = 1;
		inst.respawn_timer_sec = 45;
		inst.group_id = 3;
		forge_format_props_packet(buf, sizeof(buf), 12, &inst);
		CHECK(strcmp(buf, "!FP:C:1:2:1:45:3") == 0,
			"expected compact 6-field !FP packet without AI or light fields");
	}
	else CASE("hash")
	{
		uint32_t a = forge_catalog_entry_hash('weap', "weapons/assault rifle/Assault Rifle");
		uint32_t b = forge_catalog_entry_hash('weap', "WEAPONS\\assault rifle\\assault rifle");
		CHECK(a != 0u && a == b, "network catalog hash must normalize slashes and ASCII case");
	}
	return 0;
}
