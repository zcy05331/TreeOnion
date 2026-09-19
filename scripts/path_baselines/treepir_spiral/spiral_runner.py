from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory, mkdtemp
from typing import Iterable, Optional


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


@dataclass(frozen=True)
class SpiralPreset:
    num_expansions: str
    further_dims: str
    database_name: str
    argv4: str = "a"


DEFAULT_PRESET = SpiralPreset("9", "7", "path_bucket")


@dataclass
class BucketRun:
    label: str
    query_indices: list[int]
    record_count: int
    preset: SpiralPreset
    correct_by_query: list[bool]
    stdout: str
    stderr: str
    metrics: dict[str, list[float]]
    elapsed_ms: float
    work_dir: str
    query_ms: float
    server_ms: float
    decode_ms: float
    query_bytes: float
    response_bytes: float

    @property
    def correct_count(self) -> int:
        return sum(1 for value in self.correct_by_query if value)

    def metric_ms_at(self, name: str, query_position: int) -> float:
        values = self.metrics.get(name)
        if values is None or query_position >= len(values):
            raise RuntimeError(
                f"{self.label}: missing per-query metric {name}[{query_position}] "
                f"for {len(self.query_indices)} queries"
            )
        return values[query_position] / 1000.0


def _metric_values(path: Path) -> list[float]:
    if not path.exists():
        return []
    first_line = path.read_text(encoding="utf-8", errors="replace").splitlines()[0:1]
    if not first_line:
        return []
    return [float(part) for part in first_line[0].split()]


def _sum_metric(metrics: dict[str, list[float]], *names: str) -> float:
    return sum(sum(metrics.get(name, [])) for name in names) / 1000.0


def _first_metric(metrics: dict[str, list[float]], name: str) -> float:
    values = metrics.get(name, [])
    return values[0] if values else 0.0


def _write_bucket_database(path: Path, records: Iterable[bytes]) -> int:
    payload = [{str(i): record.hex().upper() for i, record in enumerate(records)}]
    path.write_text(json.dumps(payload, separators=(",", ":")) + "\n", encoding="utf-8")
    return len(payload[0])



def _timeout_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def parse_query_results(stdout: str) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    current: Optional[dict[str, object]] = None
    for line in stdout.splitlines():
        match = re.search(r"Using query index:\s*(-?\d+)", line)
        if match:
            if current is not None:
                results.append(current)
            current = {"index": int(match.group(1)), "valid": False, "positioned": False}
            continue
        if current is None:
            continue
        if "Hash is valid." in line:
            current["valid"] = True
        elif "Hash is invalid." in line:
            current["valid"] = False
        if "Hash is positioned correctly." in line:
            current["positioned"] = True
        elif "Hash is positioned incorrectly." in line:
            current["positioned"] = False
    if current is not None:
        results.append(current)
    return results


def load_metrics(work_dir: Path) -> dict[str, list[float]]:
    data_dir = work_dir / "Evaluation" / "Data"
    metrics: dict[str, list[float]] = {}
    if data_dir.exists():
        for path in data_dir.iterdir():
            if path.is_file() and path.name != "Meta":
                metrics[path.name] = _metric_values(path)
    return metrics


def run_spiral_bucket(
    *,
    spiral_bin: Path,
    label: str,
    records: list[bytes],
    query_indices: list[int],
    workspace_parent: Path,
    preset: SpiralPreset = DEFAULT_PRESET,
    timeout: int = 3600,
    keep_work_dir: bool = False,
) -> BucketRun:
    if not records:
        raise ValueError(f"{label}: bucket has no records")
    if any(index < 0 or index >= len(records) for index in query_indices):
        raise ValueError(f"{label}: query index outside bucket record range")
    workspace_parent.mkdir(parents=True, exist_ok=True)
    manager: Optional[TemporaryDirectory[str]] = None
    if keep_work_dir:
        work_dir = Path(mkdtemp(prefix=f"spiral-{label}.", dir=workspace_parent))
    else:
        manager = TemporaryDirectory(prefix=f"spiral-{label}.", dir=workspace_parent)
        work_dir = Path(manager.name)
    data_dir = work_dir / "Database" / "Colouring" / "Data"
    query_dir = work_dir / "Query_Storage"
    eval_dir = work_dir / "Evaluation" / "Data"
    for directory in (data_dir, query_dir, eval_dir):
        directory.mkdir(parents=True, exist_ok=True)
    database_path = data_dir / preset.database_name
    _write_bucket_database(database_path, records)
    (query_dir / f"{label}.query").write_text(
        "".join(f"{index}\n" for index in query_indices),
        encoding="utf-8",
    )

    command = [
        str(spiral_bin),
        preset.num_expansions,
        preset.further_dims,
        preset.database_name,
        preset.argv4,
    ]
    start = time.perf_counter()
    env = os.environ.copy()
    env["TREEPIR_SPIRAL_ROOT"] = str(work_dir)
    try:
        proc = subprocess.run(command, cwd=work_dir, text=True, capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired as exc:
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        stdout = ANSI_RE.sub("", _timeout_text(exc.stdout))
        stderr = ANSI_RE.sub("", _timeout_text(exc.stderr))
        raise RuntimeError(
            f"{label}: Spiral timed out after {timeout}s in {work_dir} "
            f"after {elapsed_ms:.1f} ms\nCOMMAND: {' '.join(command)}\n"
            f"STDOUT:\n{stdout[-4000:]}\nSTDERR:\n{stderr[-4000:]}"
        ) from exc
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    stdout = ANSI_RE.sub("", proc.stdout)
    stderr = ANSI_RE.sub("", proc.stderr)
    if proc.returncode != 0:
        raise RuntimeError(f"{label}: Spiral exited {proc.returncode}\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}")
    query_results = parse_query_results(stdout)
    if len(query_results) != len(query_indices):
        raise RuntimeError(
            f"{label}: parsed {len(query_results)} query result blocks, "
            f"expected {len(query_indices)}\n{stdout[-4000:]}"
        )
    parsed_indices = [int(result["index"]) for result in query_results]
    if parsed_indices != query_indices:
        raise RuntimeError(
            f"{label}: decoded query order {parsed_indices} does not match requested "
            f"{query_indices}\n{stdout[-4000:]}"
        )
    correct_by_query = [bool(result["valid"]) and bool(result["positioned"]) for result in query_results]
    if not all(correct_by_query):
        raise RuntimeError(
            f"{label}: Spiral query correctness failed {correct_by_query}\n{stdout[-4000:]}"
        )
    metrics = load_metrics(work_dir)
    for required in ("Query_Generation.client", "Answer.server", "Extract_Response.client"):
        if len(metrics.get(required, [])) != len(query_indices):
            raise RuntimeError(
                f"{label}: metric {required} has {len(metrics.get(required, []))} values, "
                f"expected {len(query_indices)} in {work_dir}"
            )
    if not metrics.get("Query_Cost.communication") or not metrics.get("Response_Cost.communication"):
        raise RuntimeError(f"{label}: missing communication metrics in {work_dir}")
    if manager is not None:
        manager.cleanup()
    return BucketRun(
        label=label,
        query_indices=query_indices,
        record_count=len(records),
        preset=preset,
        correct_by_query=correct_by_query,
        stdout=stdout,
        stderr=stderr,
        metrics=metrics,
        elapsed_ms=elapsed_ms,
        work_dir=str(work_dir),
        query_ms=_sum_metric(metrics, "Query_Generation.client"),
        server_ms=_sum_metric(metrics, "Answer.server"),
        decode_ms=_sum_metric(metrics, "Extract_Response.client"),
        query_bytes=_first_metric(metrics, "Query_Cost.communication"),
        response_bytes=_first_metric(metrics, "Response_Cost.communication"),
    )
