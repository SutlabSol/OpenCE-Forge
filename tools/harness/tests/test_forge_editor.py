"""Forge 3D editor math, gametype filters, and catalog hashing (forge_3d_mode.c, forge_runtime_tags.c)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run


def generated(fault=None):
    m3d = read("port/linux/game/forge_3d_mode.c")
    rt = read("port/linux/game/forge_runtime_tags.c")
    code = "\n".join([
        function(m3d, "euler_to_forward_up"),
        function(m3d, "forward_up_to_euler"),
        function(m3d, "gametype_flags_label"),
        function(m3d, "cycle_gametype_flags"),
        function(m3d, "forge_compute_vehicle_copy_candidate_offset"),
        function(m3d, "forge_should_suppress_editor_input"),
        function(rt, "forge_compute_catalog_hash"),
    ])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", ["euler", "gametype", "hash", "vehicle_copy_offset", "input_suppress"])
def test_case(case):
    status, output = run(build("forge_editor", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    (('up->k = cp * cr;', 'up->k = -cp * cr;'), 'euler'),
    (('cur = (cur + 6 + dir) % 6;', 'cur = (cur + 6) % 6;'), 'gametype'),
    (('if (c == \'/\')\n\t\t\t\tc = \'\\\\\';', 'if (0)\n\t\t\t\tc = \'\\\\\';'), 'hash'),
    (('horiz_sep = r * 2.15f + 0.45f;', 'horiz_sep = 0.0f;'), 'vehicle_copy_offset'),
    (('(ui_active || chat_typing || console_active)', '(ui_active || console_active)'), 'input_suppress'),
])
def test_negative_control(fault, case):
    status, output = run(build("forge_editor", generated(fault)), case)
    assert status == CHECK_FAILED, output
