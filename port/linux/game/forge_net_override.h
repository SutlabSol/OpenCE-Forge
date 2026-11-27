/*
FORGE_NET_OVERRIDE.H

Declarations for Forge multiplayer session overrides and menu hooks.
*/

#ifndef FORGE_NET_OVERRIDE_H
#define FORGE_NET_OVERRIDE_H

#include "halo_port_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

struct game_variant;
struct game_variant_options;
extern long forge_get_network_version(void);
extern unsigned char forge_game_modes_kind(struct game_variant const *variant, struct game_variant_options const *options);

#ifdef __cplusplus
}
#endif

enum {
	HALO_PORT_NETWORK_VERSION_STOCK = HALO_PORT_NETWORK_VERSION,
	HALO_PORT_NETWORK_VERSION_OLDEST_STOCK = HALO_PORT_NETWORK_VERSION_OLDEST,
	HALO_PORT_NETWORK_VERSION_OLDEST_MCC_STOCK = HALO_PORT_NETWORK_VERSION_OLDEST_MCC,
	HALO_PORT_NETWORK_VERSION_ADVERTISED_STOCK = HALO_PORT_NETWORK_VERSION_ADVERTISED
};
#undef HALO_PORT_NETWORK_VERSION
#undef HALO_PORT_NETWORK_VERSION_OLDEST
#undef HALO_PORT_NETWORK_VERSION_OLDEST_MCC
#undef HALO_PORT_NETWORK_VERSION_ADVERTISED
#undef HALO_PORT_NETWORK_VERSION_PLAYS
#define FORGE_NETWORK_VERSION 0x4635
#define FORGE_GAME_KIND 0x46
#define HALO_PORT_NETWORK_VERSION (forge_get_network_version())
#define HALO_PORT_NETWORK_VERSION_OLDEST \
	((unsigned int)forge_get_network_version() == FORGE_NETWORK_VERSION ? FORGE_NETWORK_VERSION : HALO_PORT_NETWORK_VERSION_OLDEST_STOCK)
#define HALO_PORT_NETWORK_VERSION_OLDEST_MCC \
	((unsigned int)forge_get_network_version() == FORGE_NETWORK_VERSION ? FORGE_NETWORK_VERSION : HALO_PORT_NETWORK_VERSION_OLDEST_MCC_STOCK)
#define HALO_PORT_NETWORK_VERSION_ADVERTISED (forge_get_network_version())
#define HALO_PORT_NETWORK_VERSION_PLAYS(theirs, mcc) \
	((unsigned int)forge_get_network_version() == FORGE_NETWORK_VERSION \
		? ((unsigned int)(theirs) == FORGE_NETWORK_VERSION) \
		: ((unsigned int)(theirs) >= ((mcc) ? HALO_PORT_NETWORK_VERSION_OLDEST_MCC_STOCK : HALO_PORT_NETWORK_VERSION_OLDEST_STOCK) && \
		   (unsigned int)(theirs) <= HALO_PORT_NETWORK_VERSION_STOCK))
#define game_modes_kind forge_game_modes_kind

#endif /* FORGE_NET_OVERRIDE_H */
