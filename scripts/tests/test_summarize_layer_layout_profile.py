"""Tests for summarize_layer_layout_profile.py against the tracked fixtures.

The script re-derives the legacy layout from the profile with its own 4-key
tie-break and marks the profiled planner's choice; both are cross-checked
here against the layouts the C++ binary recorded in the merkle A/B reports
for the same H=22 workload (ab_legacy_1.json / ab_profiled_1.json).
"""

import contextlib
import io
import pathlib
import re
import sys
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures" / "layer_layout"
PROFILE = FIXTURES / "layer-layout-256mb.json"
AB_LEGACY = FIXTURES / "ab_legacy_1.json"
AB_PROFILED = FIXTURES / "ab_profiled_1.json"
sys.path.insert(0, str(SCRIPTS))
import summarize_layer_layout_profile as layout  # noqa: E402


def capture(function, *args):
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        result = function(*args)
    return result, buffer.getvalue()


def table_rows(output):
    """Parse the `level h N0 Nrest num_pt subs intt cmux median_ms role` table."""
    lines = output.splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("level  h ")) + 1
    rows = []
    for line in lines[start:]:
        if not line.strip():
            break
        parts = line.split()
        rows.append({
            "level": int(parts[0]),
            "h": int(parts[1]),
            "median_ms": float(parts[8]),
            "role": parts[9].split(",") if len(parts) > 9 else [],
        })
    return rows


def layer_heights(report):
    case = layout.layerwise_case(report)
    return {layer["level"]: layer["expansion_height"] for layer in case["layers"]}


class SummarizeLayerLayoutProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = layout.load(PROFILE)
        cls.legacy_heights = layer_heights(layout.load(AB_LEGACY))
        cls.profiled_heights = layer_heights(layout.load(AB_PROFILED))
        _, cls.output = capture(layout.summarize_profile, cls.profile)
        cls.rows = table_rows(cls.output)
        cls.levels = sorted({m["level"] for m in cls.profile["measurements"]})

    def measurements(self, level):
        return [m for m in self.profile["measurements"] if m["level"] == level]

    def marked(self, level, role):
        return [row for row in self.rows if row["level"] == level and role in row["role"]]

    def test_legacy_row_is_the_layout_the_cpp_legacy_policy_used(self):
        # Regression: the Python re-derivation (min padded_plaintexts, then
        # useful_expanded_ciphertexts, other_dim_count, expansion_height) mislabelling a
        # tie level would mark a height the legacy benchmark never ran.
        for level in self.levels:
            legacy = self.marked(level, "legacy")
            self.assertEqual(len(legacy), 1, f"level {level}: {legacy}")
            self.assertEqual(legacy[0]["h"], self.legacy_heights[level], f"level {level}")
        ties = [level for level in self.levels
                if len({m["padded_plaintexts"] for m in self.measurements(level)}) < len(self.measurements(level))]
        self.assertTrue(ties, "fixture no longer contains a padded_plaintexts tie; the tie-break is untested")

    def test_selected_row_is_the_profiled_planner_choice(self):
        selected_heights = self.profile["selected_expansion_heights"]
        for level in self.levels:
            selected = self.marked(level, "selected")
            self.assertEqual(len(selected), 1, f"level {level}: {selected}")
            self.assertEqual(selected[0]["h"], self.profiled_heights[level], f"level {level}")
            self.assertEqual(selected[0]["h"], selected_heights[level - 1], f"level {level}")

    def test_attribution_lines_cover_exactly_the_levels_where_the_layouts_differ(self):
        differing = {level for level in self.levels
                     if self.legacy_heights[level] != self.profiled_heights[level]}
        self.assertTrue(differing, "fixture has no level where legacy and profiled differ")
        attributed = {
            int(match.group(1)): (int(match.group(2)), int(match.group(3)))
            for match in re.finditer(r"^level (\d+): h (\d+)->(\d+):", self.output, re.MULTILINE)
        }
        self.assertEqual(set(attributed), differing)
        for level, heights in attributed.items():
            self.assertEqual(heights, (self.legacy_heights[level], self.profiled_heights[level]))

    def test_predicted_server_ms_sums_exactly_the_marked_rows(self):
        def total(heights):
            return sum(m["median_server_ms"] for m in self.profile["measurements"]
                       if m["expansion_height"] == heights[m["level"]])

        match = re.search(r"^predicted PIR-level server ms: legacy ([0-9.]+) -> selected ([0-9.]+) ",
                          self.output, re.MULTILINE)
        self.assertIsNotNone(match, self.output)
        self.assertEqual(match.group(1), f"{total(self.legacy_heights):.2f}")
        self.assertEqual(match.group(2), f"{total(self.profiled_heights):.2f}")
        self.assertAlmostEqual(sum(row["median_ms"] for row in self.rows if "legacy" in row["role"]),
                               float(match.group(1)), delta=0.01)
        self.assertAlmostEqual(sum(row["median_ms"] for row in self.rows if "selected" in row["role"]),
                               float(match.group(2)), delta=0.01)

    def test_ab_report_names_the_same_differing_levels(self):
        code, output = capture(layout.main, ["summarize_layer_layout_profile.py",
                                             str(PROFILE), str(AB_LEGACY), str(AB_PROFILED)])
        self.assertEqual(code, 0)
        ab_section = output.split("A/B layerwise", 1)[1]
        ab_levels = {int(match.group(1)) for match in re.finditer(r"^  level (\d+): h ", ab_section, re.MULTILINE)}
        self.assertEqual(ab_levels, {level for level in self.legacy_heights
                                     if self.legacy_heights[level] != self.profiled_heights[level]})
        self.assertIn("communication unchanged: True", ab_section)


if __name__ == "__main__":
    unittest.main()
