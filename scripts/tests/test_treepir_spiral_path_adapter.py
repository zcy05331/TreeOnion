import json
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
ADAPTER = REPO / "scripts" / "path_baselines" / "treepir_spiral" / "treepir_spiral_path_adapter.py"
BUILD_SPIRAL = REPO / "scripts" / "path_baselines" / "treepir_spiral" / "build_spiral_cpu.py"

sys.path.insert(0, str(REPO / "scripts" / "path_baselines"))
from treepir_spiral.build_spiral_cpu import patch_spiral_root
from treepir_spiral.subcsa import balanced_color_sizes, color_path_positions, local_csa_indices
from treepir_spiral.synthetic_tree import node_bytes, path_indices
from treepir_spiral.treepir_spiral_path_adapter import canonical_bucket_records


def write_fake_spiral(path: pathlib.Path) -> None:
    path.write_text(textwrap.dedent(
        """\
        #!/usr/bin/env python3
        import json
        import os
        import pathlib
        import sys

        root = pathlib.Path(os.environ["TREEPIR_SPIRAL_ROOT"])
        assert root != pathlib.Path("/tmp/Spiral")
        assert sys.argv[3] == "path_bucket"
        database = json.loads((root / "Database" / "Colouring" / "Data" / sys.argv[3]).read_text())[0]
        queries = []
        query_files = sorted((root / "Query_Storage").glob("*.query"))
        assert len(query_files) == 1
        for query_file in query_files:
            queries.extend(int(line.strip()) for line in query_file.read_text().splitlines() if line.strip())
        for index in queries:
            assert str(index) in database, index
            assert len(database[str(index)]) == 64
            print(f"[Input] Using query index: {index}")
            print("[Check] Hash is valid.")
            print("[Check] Hash is positioned correctly.")
        metrics = root / "Evaluation" / "Data"
        metrics.mkdir(parents=True, exist_ok=True)
        count = len(queries)
        (metrics / "Query_Generation.client").write_text(" ".join(["1000"] * count) + "\\n1000\\n1000\\n")
        (metrics / "Answer.server").write_text(" ".join(["2000"] * count) + "\\n2000\\n2000\\n")
        (metrics / "Extract_Response.client").write_text(" ".join(["3000"] * count) + "\\n3000\\n3000\\n")
        (metrics / "Query_Cost.communication").write_text("11\\n11\\n11\\n")
        (metrics / "Response_Cost.communication").write_text("22\\n22\\n22\\n")
        """
    ), encoding="utf-8")
    path.chmod(0o755)


class TreePirSpiralPathAdapterTests(unittest.TestCase):
    def test_subcsa_port_matches_h10_golden_fixture_prefix(self):
        self.assertEqual(
            local_csa_indices(10, 496),
            [0, 198, 98, 0, 95, 92, 51, 77, 90, 90],
        )


    def test_h8_canonical_bucket_data_is_schedule_independent_and_complete(self):
        first = canonical_bucket_records(8)
        second = canonical_bucket_records(8)
        self.assertEqual(first, second)
        self.assertEqual([len(bucket) for bucket in first], balanced_color_sizes(8))

        all_records = [record for bucket in first for record in bucket]
        expected = [
            node_bytes(level, local)
            for level in range(1, 9)
            for local in range(1 << level)
        ]
        self.assertCountEqual(all_records, expected)
        self.assertEqual(len(all_records), len(expected))

    def test_canonical_bucket_data_matches_subcsa_indices_for_h8_and_h10(self):
        for height in (8, 10):
            with self.subTest(height=height):
                records = canonical_bucket_records(height)
                for level in range(1, height + 1):
                    leaf_shift = height - level
                    for local in range(1 << level):
                        descendant_leaf = local << leaf_shift
                        color_positions = color_path_positions(height, descendant_leaf)
                        csa_indices = local_csa_indices(height, descendant_leaf)
                        color = color_positions.index(level)
                        local_index = csa_indices[color]
                        self.assertEqual(records[color][local_index], node_bytes(level, local))

    def test_build_helper_rejects_mismatched_source_version_before_build(self):
        with tempfile.TemporaryDirectory(prefix="treepir-spiral-build-provenance.") as tmp:
            tmpdir = pathlib.Path(tmp)
            stage_root = tmpdir / "stage"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_SPIRAL),
                    "--height",
                    "8",
                    "--source-version",
                    "0" * 40,
                    "--stage-root",
                    str(stage_root),
                    "--cxx",
                    "/usr/bin/c++",
                ],
                cwd=REPO,
                text=True,
                capture_output=True,
            )

            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("does not match upstream Git HEAD", proc.stderr)
            self.assertFalse(stage_root.exists(), proc.stderr)

    def test_build_helper_patches_avx512_load_buffer_to_64_byte_alignment(self):
        simd_body = """        void useSIMDParser(const std::filesystem::path& jsonFile) {
            simdjson::ondemand::parser simdParser;
            auto jsonHash = simdjson::padded_string::load(jsonFile.c_str());
            auto& hashStoreRef = const_cast<HashStore&>(hashStore(true));
            for (simdjson::ondemand::object container : simdParser.iterate(jsonHash)) {
                for (auto element : container) {
                    simdjson::ondemand::value hash = element.value();
                    hashStoreRef.get<Container::Sequence>().emplace_back(
                            static_cast<std::string_view>(hash.get_string())
                    );
                }
            }
        }
"""
        with tempfile.TemporaryDirectory(prefix="treepir-spiral-source-patch.") as tmp:
            source = pathlib.Path(tmp)
            (source / "src").mkdir()
            (source / "include").mkdir()
            (source / "src" / "spiral.cpp").write_text(
                '#include "spiral.h"\n'
                'void snippets() {\n'
                'const std::filesystem::path base("/tmp/Spiral");\n'
                'const std::filesystem::path metricsPath("/tmp/Spiral/Evaluation/Data");\n'
                'sched_getcpu();\n'
                '(hashExistsInFile ? "correctly." : "incorrectly.");\n'
                'assert(readHeadEnd < databaseCapacity);\n'
                '}\n'
                + simd_body,
                encoding="utf-8",
            )
            (source / "include" / "spiral.h").write_text(
                "reoriented_ciphertexts = (uint64_t *)aligned_alloc(32, L_small_size_bytes * n1_padded / n1);\n",
                encoding="utf-8",
            )

            patch_spiral_root(source)

            header = (source / "include" / "spiral.h").read_text(encoding="utf-8")
            self.assertIn("aligned_alloc(64, L_small_size_bytes * n1_padded / n1)", header)
            self.assertNotIn("aligned_alloc(32, L_small_size_bytes * n1_padded / n1)", header)

    def test_h8_fake_spiral_full_path_includes_encrypted_root_bucket(self):
        with tempfile.TemporaryDirectory(prefix="treepir-spiral-adapter.") as tmp:
            tmpdir = pathlib.Path(tmp)
            fake_spiral = tmpdir / "fake_spiral.py"
            write_fake_spiral(fake_spiral)
            leaves = tmpdir / "leaves.txt"
            leaves.write_text("0\n17\n129\n", encoding="utf-8")
            output = tmpdir / "out.json"
            work_root = tmpdir / "work"

            subprocess.run(
                [
                    sys.executable,
                    str(ADAPTER),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaves),
                    "--warmup",
                    "1",
                    "--trials",
                    "2",
                    "--output",
                    str(output),
                    "--spiral-bin",
                    str(fake_spiral),
                    "--work-root",
                    str(work_root),
                ],
                cwd=REPO,
                check=True,
                text=True,
                capture_output=True,
            )

            data = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(data["schema_version"], "path-baseline-v1")
        self.assertEqual(data["scheme"], "treepir_spiral")
        self.assertEqual(data["path_mode"], "ancestor_root_included")
        self.assertEqual(data["height"], 8)
        self.assertEqual(data["record_bytes"], 32)
        self.assertEqual(data["warmups"], 1)
        self.assertEqual(data["trials"], 2)
        self.assertEqual(data["leaf_schedule"]["measured"], [17, 129])
        self.assertEqual(data["root_handling"]["mode"], "encrypted_constant_bucket")
        self.assertIsNone(data["setup"]["preprocess_ms"])
        self.assertIsNone(data["setup"]["keygen_ms"])
        self.assertIsNone(data["setup"]["helper_bytes"])
        self.assertEqual(len(data["bucket_runs"]), 9)
        self.assertEqual(len(data["warmup_samples"]), 1)
        self.assertEqual(len(data["samples"]), 2)

        for sample in data["samples"]:
            leaf = sample["leaf"]
            self.assertTrue(sample["correct"], sample)
            self.assertEqual(sample["correct_entries"], 9)
            self.assertEqual(sample["path_entries"], 9)
            self.assertEqual(sample["path_indices"], path_indices(8, leaf))
            self.assertEqual(sample["decoded_path_indices"], path_indices(8, leaf))
            self.assertEqual(sample["query_ms"], 9.0)
            self.assertEqual(sample["server_ms"], 18.0)
            self.assertEqual(sample["decode_ms"], 27.0)
            self.assertEqual(sample["query_bytes"], 99)
            self.assertEqual(sample["response_bytes"], 198)
            self.assertEqual(sample["path_records"][0]["bucket"], "root")
            self.assertTrue(sample["path_records"][0]["pir_verified"])
            self.assertEqual(sample["path_records"][0]["expected_hex"], node_bytes(0, 0).hex())
            for record in sample["path_records"]:
                self.assertTrue(record["pir_verified"])


if __name__ == "__main__":
    unittest.main()
