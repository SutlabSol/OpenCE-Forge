/*
FORGE_EDITOR.C (test)

Tests Forge 3D orientation math, gametype filter cycling/labels, and canonical catalog hashing.
*/

#include "harness.h"

#include <math.h>
#include <stdint.h>

#include "under_test.inc"

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("euler")
	{
		float tests[3][3] = {
			{ 0.5f, 0.25f, -0.4f },
			{ -2.1f, 0.0f, 1.2f },
			{ 1.1f, -0.7f, 0.3f }
		};
		int i;
		for (i = 0; i < 3; i++)
		{
			real_vector3d fwd, up;
			float y = 0.0f, p = 0.0f, r = 0.0f;
			euler_to_forward_up(tests[i][0], tests[i][1], tests[i][2], &fwd, &up);
			forward_up_to_euler(&fwd, &up, &y, &p, &r);
			CHECK(fabsf(up.k) > 0.0f || fabsf(tests[i][2]) > 0.0f, "valid up vector");
			CHECK(fabsf(y - tests[i][0]) < 1e-4f, "yaw round-trip failed");
			CHECK(fabsf(p - tests[i][1]) < 1e-4f, "pitch round-trip failed");
			CHECK(fabsf(r - tests[i][2]) < 1e-4f, "roll round-trip failed");
		}
	}
	else CASE("gametype")
	{
		uint16_t m = 0x0FFFu;
		m = cycle_gametype_flags(m, 1);
		CHECK(m == 0x0001u && strcmp(gametype_flags_label(m), "CTF Only") == 0, "expected CTF Only");
		m = cycle_gametype_flags(m, 1);
		CHECK(m == 0x0002u && strcmp(gametype_flags_label(m), "Slayer Only") == 0, "expected Slayer Only");
		m = cycle_gametype_flags(m, -1);
		CHECK(m == 0x0001u, "expected reverse step to CTF Only");
		CHECK(strcmp(gametype_flags_label(0), "All Modes (Default)") == 0, "0 maps to All Modes");
	}
	else CASE("hash")
	{
		uint32_t h1 = forge_compute_catalog_hash('scen', "scenery\\rocks\\BouldeR");
		uint32_t h2 = forge_compute_catalog_hash('scen', "SCENERY/Rocks/boulder");
		uint32_t h3 = forge_compute_catalog_hash('vehi', "scenery/rocks/boulder");
		CHECK(h1 != 0u && h1 == h2, "hash should normalize case and path separators");
		CHECK(h1 != h3, "hash should incorporate group_tag");
	}
	return 0;
}
