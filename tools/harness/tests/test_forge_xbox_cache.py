"""Forge Xbox cache catalog filters and object tag safety validation (forge_runtime_tags.c)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure


def generated(fault=None):
    rt = read("port/linux/game/forge_runtime_tags.c")
    fe_h = read("port/linux/game/forge_engine.h")
    defs = "\n".join([
        enum_with(fe_h, "_forge_cat_weapons"),
        structure(fe_h, "forge_catalog_entry"),
        structure(rt, "forge_cache_file_tag_instance"),
    ])
    code = "\n".join([
        function(rt, "forge_float_is_finite"),
        function(rt, "is_forge_door_tag"),
        function(rt, "path_contains_any_ci"),
        function(rt, "is_excluded_scenery_or_effect_tag"),
        function(rt, "is_excluded_weapon_or_equipment_tag"),
        function(rt, "compute_catalog_entry_allowed"),
        function(rt, "forge_validate_safe_object_tag"),
        function(rt, "forge_xbox_catalog_text_is_valid"),
    ])
    if fault:
        code = mutated(code, *fault)
    return (("defs.inc", defs), ("under_test.inc", code))


@pytest.mark.parametrize("case", ["catalog_filters", "safe_tags", "effect_refs", "catalog_validity"])
def test_case(case):
    status, output = run(build("forge_xbox_cache", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    (("entry->group_tag == 'lifi'", "0"), "catalog_filters"),
    (("if (entry->source_cache_id == 3)", "if (0)"), "catalog_filters"),
    (("if (odef->type != expected_type)\n\t\treturn 0;", "if (odef->type != expected_type)\n\t\t(void)expected_type;"), "safe_tags"),
    (("pt_grp == 'actr' || pt_grp == 'actv' || pt_grp == 'bipd'", "0"), "effect_refs"),
    (("p2[1] == '0'", "1"), "catalog_validity"),
])
def test_negative_control(fault, case):
    status, output = run(build("forge_xbox_cache", generated(fault)), case)
    assert status == CHECK_FAILED, output
