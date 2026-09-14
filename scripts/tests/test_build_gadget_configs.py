"""Tests for build_gadget_configs.py with cmake and the binary copy stubbed out.

The -D names the script passes must be exactly the knobs CMakeLists.txt reads:
CMake only warns about an unused variable, so a rename on either side would
build identical default-gadget binaries and the A/B would measure nothing.
"""

import contextlib
import io
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
import build_gadget_configs as builder  # noqa: E402


def cmake_knobs():
    text = (REPO / "CMakeLists.txt").read_text()
    match = re.search(r"foreach\(PIR_KNOB([^)]*)\)", text)
    return set(match.group(1).split())


def flag_names(command):
    return {arg[2:].split("=", 1)[0] for arg in command if arg.startswith("-D")}


class FakeCMake:
    """Records every cmake invocation; `cmake --build` fails for labels in fail_builds.
    The label is read off the per-label build log the script opens for stdout."""

    def __init__(self, fail_builds=()):
        self.commands = []
        self.fail_builds = set(fail_builds)

    def run(self, command, **kwargs):
        self.commands.append(list(command))
        label = pathlib.Path(kwargs["stdout"].name).name.removesuffix("-build.log")
        failed = command[:2] == ["cmake", "--build"] and label in self.fail_builds
        return subprocess.CompletedProcess(command, 1 if failed else 0)


class BuildGadgetConfigsTest(unittest.TestCase):
    LABELS = ("ep3-s32", "ks2-s20")

    def run_main(self, root, fake):
        build_dir = root / "build"
        build_dir.mkdir(exist_ok=True)
        (build_dir / "Onion-PIR").write_bytes(b"placeholder binary")
        copies = []

        def copy2(source, target):
            copies.append((pathlib.Path(source), pathlib.Path(target)))
            pathlib.Path(target).write_bytes(pathlib.Path(source).read_bytes())

        argv = ["build_gadget_configs.py", "--build-dir", str(build_dir), "--output", str(root / "out"),
                "--binaries", str(root / "bins"), "--jobs", "4"]
        for label in self.LABELS:
            argv += ["--config", label]
        with patch.object(sys, "argv", argv), \
             patch.object(builder, "subprocess", types.SimpleNamespace(run=fake.run, STDOUT=subprocess.STDOUT)), \
             patch.object(builder, "shutil", types.SimpleNamespace(copy2=copy2)), \
             contextlib.redirect_stdout(io.StringIO()):
            code = builder.main()
        return code, copies

    def test_each_config_is_configured_built_and_copied_under_its_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            fake = FakeCMake()
            code, copies = self.run_main(root, fake)
            self.assertEqual(code, 0)
            build_dir = str(root / "build")
            self.assertEqual(fake.commands[0], ["cmake", "-S", str(REPO), "-B", build_dir,
                                                "-DPIR_EP_ROWS=3", "-DPIR_EP_LOW_BITS=32",
                                                "-DPIR_KS_ROWS=8", "-DPIR_KS_LOW_BITS=0"])
            self.assertEqual(fake.commands[1], ["cmake", "--build", build_dir, "-j", "4"])
            self.assertEqual(fake.commands[2][:5], fake.commands[0][:5])
            self.assertEqual(fake.commands[2][5:], ["-DPIR_EP_ROWS=6", "-DPIR_EP_LOW_BITS=0",
                                                    "-DPIR_KS_ROWS=2", "-DPIR_KS_LOW_BITS=20"])
            self.assertEqual(fake.commands[3], fake.commands[1])
            self.assertEqual(len(fake.commands), 4)
            self.assertEqual(flag_names(fake.commands[0]), cmake_knobs())

            manifest = json.loads((root / "out" / "builds.json").read_text())
            self.assertEqual(manifest["state"], "complete")
            self.assertEqual([row["label"] for row in manifest["builds"]], list(self.LABELS))
            self.assertEqual([row["state"] for row in manifest["builds"]], ["built", "built"])
            self.assertEqual([row["exit_code"] for row in manifest["builds"]], [0, 0])
            self.assertEqual([row["binary"] for row in manifest["builds"]],
                             [str(root / "bins" / label / "Onion-PIR") for label in self.LABELS])
            self.assertEqual([(row["EP"], row["EP_low_bits"], row["KS"], row["KS_low_bits"]) for row in manifest["builds"]],
                             [builder.CONFIGS[label] for label in self.LABELS])
            self.assertEqual([source for source, _ in copies], [root / "build" / "Onion-PIR"] * 2)
            self.assertEqual([target for _, target in copies],
                             [root / "bins" / label / "Onion-PIR" for label in self.LABELS])
            self.assertTrue(all(target.exists() for _, target in copies))
            self.assertTrue(all((root / "out" / f"{label}-build.log").exists() for label in self.LABELS))

    def test_failed_build_marks_the_row_and_the_manifest_failed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            fake = FakeCMake(fail_builds={"ks2-s20"})
            code, copies = self.run_main(root, fake)
            self.assertEqual(code, 1)
            self.assertEqual(len(fake.commands), 4)
            manifest = json.loads((root / "out" / "builds.json").read_text())
            self.assertEqual(manifest["state"], "failed")
            good, bad = manifest["builds"]
            self.assertEqual((good["state"], good["exit_code"]), ("built", 0))
            self.assertEqual((bad["state"], bad["exit_code"]), ("failed", 1))
            self.assertNotIn("binary", bad)
            self.assertEqual([target.name for _, target in copies], ["Onion-PIR"])
            self.assertEqual(copies[0][1].parent.name, "ep3-s32")

    def test_reused_output_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self.assertEqual(self.run_main(root, FakeCMake())[0], 0)
            with self.assertRaisesRegex(ValueError, "fresh output directory"):
                self.run_main(root, FakeCMake())


if __name__ == "__main__":
    unittest.main()
