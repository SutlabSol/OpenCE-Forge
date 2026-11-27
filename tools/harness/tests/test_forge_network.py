"""Forge distributed wire structs (108/109) and packet formatting (network_distributed.h, forge_network.c)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure


def generated(fault=None):
    nd_h = read("port/linux/game/network_distributed.h")
    fe_h = read("port/linux/game/forge_engine.h")
    fn_c = read("port/linux/game/forge_network.c")
    defs = "\n".join([
        enum_with(nd_h, "_distributed_message_forge_up"),
        enum_with(nd_h, "DISTRIBUTED_FORGE_PAYLOAD_SIZE"),
        structure(nd_h, "distributed_forge_message"),
        structure(fe_h, "forge_live_instance"),
    ])
    code = "\n".join([
        function(fn_c, "forge_catalog_entry_hash"),
        function(fn_c, "forge_net_coords_valid"),
        function(fn_c, "forge_format_props_packet"),
    ])
    if fault:
        code = mutated(code, *fault)
    return (("defs.inc", defs), ("under_test.inc", code))


@pytest.mark.parametrize("case", ["wire", "coords", "props", "hash"])
def test_case(case):
    status, output = run(build("forge_network", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    (('if (!(x == x && y == y && z == z))', 'if (0)'), 'coords'),
    (('x < -50000.0f || x > 50000.0f', 'x < -500000.0f || x > 500000.0f'), 'coords'),
    (('(unsigned int)inst->group_id', '0u'), 'props'),
    (('if (c == \'/\')', 'if (0)'), 'hash'),
])
def test_negative_control(fault, case):
    status, output = run(build("forge_network", generated(fault)), case)
    assert status == CHECK_FAILED, output
