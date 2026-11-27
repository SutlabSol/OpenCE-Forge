"""Forge vs Standard mode server browser listing isolation (port/linux/game/forge_hooks.c)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure


def generated(fault=None):
    p2p_h = read("port/linux/src/p2p.h")
    fe_h = read("port/linux/game/forge_engine.h")
    hooks_c = read("port/linux/game/forge_hooks.c")
    net_ver = re.search(r"#define FORGE_NETWORK_VERSION\s+\S+", fe_h).group(0)
    game_kind = re.search(r"#define FORGE_GAME_KIND\s+\S+", fe_h).group(0)
    defs = "\n".join([
        net_ver,
        game_kind,
        enum_with(p2p_h, "P2P_LISTING_NAME_SIZE = 32"),
        structure(p2p_h, "p2p_listing"),
    ])
    code = "\n".join([
        function(hooks_c, "forge_listing_is_forge"),
        function(hooks_c, "forge_filter_lobby_listings"),
    ])
    if fault:
        code = mutated(code, *fault)
    return (("defs.inc", defs), ("under_test.inc", code))


@pytest.mark.parametrize("case", ["forge_mode_only", "standard_mode_only", "empty_and_null"])
def test_case(case):
    status, output = run(build("forge_server_browser", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    (("if (!is_forge)\n\t\t\t\tcontinue;", "if (0)\n\t\t\t\tcontinue;"), "forge_mode_only"),
    (("if (is_forge)\n\t\t\t\tcontinue;", "if (0)\n\t\t\t\tcontinue;"), "standard_mode_only"),
])
def test_negative_control(fault, case):
    status, output = run(build("forge_server_browser", generated(fault)), case)
    assert status == CHECK_FAILED, output
