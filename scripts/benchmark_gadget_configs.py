#!/usr/bin/env python3
"""Run fixed-layout Generic gadget-configuration benchmark binaries."""
import argparse
import json
import os
from pathlib import Path
from statistics import fmean

from read_process_peak_memory import run_measured
from summarize_cloudlab_profiles import finite_ms, require_count, require_leaf_count


FIXED_LAYOUT_KEYS = (
    "entry_bits",
    "leaf_count",
    "bundle_arity",
    "record_coeffs",
    "plane_count",
    "records_per_plaintext",
    "N0",
    "beta_bits",
    "expansion_height",
)
SCHEME_LOCK_KEYS = ("ring_degree", "plaintext_modulus", "rns_modulus_bits", "rns_moduli", "response_modulus")


def parse_label_path(value):
    label, sep, path = value.partition("=")
    if not sep or not label or not path:
        raise argparse.ArgumentTypeError("expected label=path")
    return label, Path(path)


def parse_label_source(value):
    label, sep, source = value.partition("=")
    if not sep or not label or not source:
        raise argparse.ArgumentTypeError("expected label=source")
    return label, source


def parse_label_packing(value):
    label, sep, packing = value.partition("=")
    if not sep or not label or packing not in ("projection", "mlwe"):
        raise argparse.ArgumentTypeError("expected label=projection or label=mlwe")
    return label, packing


def parse_shape(value):
    try:
        parts = tuple(int(part) for part in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("--shape must be h,G,P,N0,b") from error
    if len(parts) != 5 or any(part <= 0 for part in parts[:4]) or parts[4] < 0:
        raise argparse.ArgumentTypeError("--shape must be h,G,P,N0,b with positive h/G/P/N0")
    return parts


def finite_stage_sum(sample, keys, path):
    stages = sample.get("stages_ms")
    if not isinstance(stages, dict):
        raise ValueError(f"{path}: sample missing stages_ms")
    total = 0.0
    for key in keys:
        if key not in stages:
            raise ValueError(f"{path}: sample missing stages_ms.{key}")
        value = stages[key]
        finite_ms(value, path, f"stages_ms.{key}")
        total += value
    return total


def response_processing_ms(sample, packing, path):
    total = finite_stage_sum(sample, ("rotate", "project", "pack"), path)
    stages = sample.get("stages_ms", {})
    if "merge" in stages:
        finite_ms(stages["merge"], path, "stages_ms.merge")
        total += stages["merge"]
    elif packing == "mlwe":
        raise ValueError(f"{path}: sample missing stages_ms.merge")
    return total


def merge_key_count(sample, packing, path):
    if "response_merge_keyswitches" in sample:
        value = sample["response_merge_keyswitches"]
        if not isinstance(value, (int, float)) or value < 0:
            raise ValueError(f"{path}: invalid response_merge_keyswitches")
        return value
    if packing == "mlwe":
        raise ValueError(f"{path}: sample missing response_merge_keyswitches")
    return 0


def validate_profile(data, path, trials, leaves, shape, expansion_height, requested_packing=None):
    require_leaf_count(data, path, leaves)
    require_count(data.get("measured_trials"), trials, path, "measured trials")
    if data.get("entry_bits") != 256 or data.get("warmup_count") != 3:
        raise ValueError(f"{path}: expected 32-byte entries and three warmups")
    candidates = [c for c in data.get("candidates", []) if c.get("eligible") and c.get("samples")]
    if len(candidates) != 1:
        raise ValueError(f"{path}: expected one measured Generic candidate")
    candidate = candidates[0]
    h, G, P, N0, b = shape
    expected = {
        "bundle_arity": h,
        "record_coeffs": G,
        "plane_count": P,
        "N0": N0,
        "beta_bits": b,
        "expansion_height": expansion_height,
    }
    actual_shape = candidate.get("shape")
    if not isinstance(actual_shape, dict):
        raise ValueError(f"{path}: measured candidate missing shape")
    changed = {key: (actual_shape.get(key), value) for key, value in expected.items() if actual_shape.get(key) != value}
    if changed:
        raise ValueError(f"{path}: calibrated fixed layout changed: {changed}")
    require_count(len(candidate["samples"]), trials, path, "candidate samples")
    gate = candidate.get("target_noise_result")
    if not isinstance(gate, dict) or gate.get("decrypts_correctly") is not True:
        raise ValueError(f"{path}: measured candidate decryption failed")
    if gate.get("observed_key_coverage_matches_tag") is not True:
        raise ValueError(f"{path}: measured candidate key coverage failed")
    actual_packing = data.get("response_packing")
    if requested_packing is not None:
        if actual_packing != requested_packing:
            raise ValueError(f"{path}: response packing {actual_packing!r} does not match requested {requested_packing!r}")
    if actual_packing is None:
        actual_packing = requested_packing or "projection"
    noise_packing = gate.get("response_packing")
    if noise_packing is not None and noise_packing != actual_packing:
        raise ValueError(f"{path}: noise result response packing {noise_packing!r} does not match top-level {actual_packing!r}")
    for sample in candidate["samples"]:
        for key in ("server_total_ms", "client_query_ms", "client_decode_ms", "response_serialize_ms"):
            finite_ms(sample.get(key), path, key)
        finite_stage_sum(sample, ("rotate", "project"), path)
        response_processing_ms(sample, actual_packing, path)
        merge_key_count(sample, actual_packing, path)
    candidate["_driver_response_packing"] = actual_packing
    return candidate


def identity_subset(candidate):
    identity = candidate.get("scheme_fingerprint", {}).get("identity", {})
    missing = [key for key in SCHEME_LOCK_KEYS if key not in identity]
    if missing:
        raise ValueError(f"{candidate.get('label', 'candidate')}: missing scheme identity lock fields: {missing}")
    return {key: identity.get(key) for key in SCHEME_LOCK_KEYS if key in identity}


def comparable_layout(candidate):
    shape = candidate.get("shape", {})
    return {key: shape.get(key) for key in FIXED_LAYOUT_KEYS if key in shape}


def schedule(candidate):
    return [(sample.get("block_id"), sample.get("leaf")) for sample in candidate.get("samples", [])]


def check_comparable(reference, candidate, label):
    if comparable_layout(candidate) != comparable_layout(reference):
        raise ValueError(f"{label}: fixed layout differs from first completed candidate")
    if identity_subset(candidate) != identity_subset(reference):
        raise ValueError(f"{label}: ring/q/t scheme fields differ from first completed candidate")
    if candidate.get("level_plans") != reference.get("level_plans"):
        raise ValueError(f"{label}: level plans changed")
    if schedule(candidate) != schedule(reference):
        raise ValueError(f"{label}: query schedules differ")


def candidate_metadata(candidate):
    shape = candidate.get("shape", {})
    metrics = candidate.get("candidate_metrics", {})
    identity = candidate.get("scheme_fingerprint", {}).get("identity", {})
    return {
        "label": candidate.get("label"),
        "response_packing": candidate.get("_driver_response_packing", "projection"),
        "gadget_identity": {
            key: identity.get(key)
            for key in (
                "selector_gadget_rows",
                "selector_base_log2",
                "selector_low_bits",
                "completion_key_gadget_rows",
                "completion_key_base_log2",
                "bv_ks_rows",
                "bv_ks_base_log2",
                "bv_ks_low_bits",
            )
            if key in identity
        },
        "query_width": shape.get("query_width"),
        "eval_key_bytes": {
            "modeled_per_client_eval_key_bytes": metrics.get("modeled_per_client_eval_key_bytes"),
            "eval_key_coefficient_payload_bytes_estimate": metrics.get("eval_key_coefficient_payload_bytes_estimate"),
        },
        "wire": candidate.get("wire", {}),
        "key_metadata": {
            "required_key_height": shape.get("required_key_height"),
            "registered_key_height": shape.get("registered_key_height"),
            "scheme_registered_session_key_height": candidate.get("scheme_fingerprint", {}).get("registered_session_key_height"),
        },
    }


def summarize_candidate(candidate, processes):
    samples = candidate["samples"]
    packing = candidate.get("_driver_response_packing", "projection")
    stage_names = sorted({stage for sample in samples for stage in sample.get("stages_ms", {})})
    for idx, sample in enumerate(samples):
        stages = sample.get("stages_ms", {})
        for stage in stage_names:
            if stage not in stages:
                raise ValueError(f"{candidate.get('label', 'candidate')}: sample {idx} missing stages_ms.{stage}")
    return {
        "completed_samples": len(samples),
        "server_total_ms_mean": fmean(sample["server_total_ms"] for sample in samples),
        "rotation_projection_ms_mean": fmean(finite_stage_sum(sample, ("rotate", "project"), "summary") for sample in samples),
        "response_processing_ms_mean": fmean(response_processing_ms(sample, packing, "summary") for sample in samples),
        "stages_ms_mean": {stage: fmean(sample["stages_ms"][stage] for sample in samples) for stage in stage_names},
        "packing": packing,
        "counter_metadata": {
            "response_merge_keyswitches_mean": fmean(merge_key_count(sample, packing, "summary") for sample in samples),
        },
        "wire": candidate.get("wire", {}),
        "key_metadata": candidate_metadata(candidate)["key_metadata"],
        "gadget_metadata": candidate_metadata(candidate),
        "process_records": processes,
    }


def run_order(labels, rounds):
    if rounds == 1:
        return [labels]
    return [labels, list(reversed(labels))]


def round_command_and_environment(args, binary, label, sources, packings, raw, expansion_height):
    h, G, P, N0, b = args.shape
    env = os.environ.copy()
    for key in ("SUPERRECORD_PLAN_ONLY", "SUPERRECORD_CANDIDATE_INDEX"):
        env.pop(key, None)
    env.update(
        {
            "SUPERRECORD_ENTRY_BITS": "256",
            "SUPERRECORD_BUNDLE_ARITIES": str(h),
            "SUPERRECORD_MAX_ARITY": str(h),
            "SUPERRECORD_SKIP_LEGACY": "1",
            "SUPERRECORD_MAX_FRONTIER": "0",
            "SUPERRECORD_H1_FRONTIER": "1" if h == 1 else "0",
            "SUPERRECORD_EXPANSION_HEIGHTS": str(expansion_height),
            "SUPERRECORD_ALPHA_ALIGNMENTS": "16",
            "SUPERRECORD_MAX_RESPONSES": "1",
            "SUPERRECORD_FIXED_KEY_HEIGHT": "11",
            "SUPERRECORD_MEMORY_BYTES": str(args.memory_gib * 2**30),
            "SUPERRECORD_MIN_NOISE_MARGIN": "0",
            "SUPERRECORD_PIN_SHAPE": f"{h},{G},{P},{N0},{b}",
            "SUPERRECORD_OUTPUT_JSON": str(raw),
        }
    )
    if label in sources:
        env["SUPERRECORD_COMMIT"] = sources[label]
    if label in packings:
        env["SUPERRECORD_RESPONSE_PACKING"] = packings[label]
    command = [
        str(binary.resolve()),
        "--test",
        "tree_superrecord_bench",
        "--leaf-count",
        str(args.leaf_count),
        "--experiments",
        str(args.experiments),
        "--warmup",
        "3",
        "--trial-seed",
        "7",
    ]
    return command, env


def run_candidate_round(args, binaries, sources, packings, out, expansion_height,
                        manifest, save, round_id, label):
    """Persist one round and return its accepted candidate and process record."""
    name = f"round{round_id}-{label}"
    raw = out / f"{name}.json"
    command, env = round_command_and_environment(
        args, binaries[label], label, sources, packings, raw, expansion_height
    )
    row = {
        "name": name,
        "label": label,
        "state": "running",
        "command": command,
        "source": sources.get(label),
        "environment": {key: value for key, value in env.items() if key.startswith("SUPERRECORD_")},
    }
    manifest["runs"].append(row)
    save()
    process = run_measured(command, log_path=out / f"{name}.log", env=env)
    process_path = out / f"{name}-process.json"
    process_path.write_text(json.dumps(process, indent=2) + "\n")
    process_record = {**process, "path": str(process_path), "raw": str(raw)}
    if process["exit_code"] != 0:
        row["state"] = "failed"
        row["error"] = "benchmark process returned nonzero exit"
        row["process"] = process
        save()
        return None, process_record
    try:
        data = json.loads(raw.read_text())
        candidate = validate_profile(
            data,
            raw,
            args.experiments,
            args.leaf_count,
            args.shape,
            expansion_height,
            packings.get(label),
        )
    except Exception as error:
        row["state"] = "failed"
        row["error"] = str(error)
        save()
        return None, process_record
    row["state"] = "complete"
    row["mean_server_ms"] = fmean(sample["server_total_ms"] for sample in candidate["samples"])
    row["mean_rotation_projection_ms"] = fmean(finite_stage_sum(sample, ("rotate", "project"), raw) for sample in candidate["samples"])
    row["mean_response_processing_ms"] = fmean(
        response_processing_ms(sample, candidate["_driver_response_packing"], raw)
        for sample in candidate["samples"]
    )
    row["response_packing"] = candidate["_driver_response_packing"]
    save()
    print(json.dumps(row), flush=True)
    return candidate, process_record


def compare_completed_rounds(binaries, rounds, completed, process_records, label_failures, runs):
    """Reject incomplete or mismatched labels before pooling their samples."""
    rejected_labels = dict(label_failures)
    for label in binaries:
        if label not in completed:
            rejected_labels.setdefault(label, "no completed rounds")
        elif len(completed[label]) != rounds:
            rejected_labels.setdefault(label, "incomplete completed rounds")
    valid_labels = [label for label in binaries if label in completed and label not in rejected_labels]
    reference = completed[valid_labels[0]][0] if valid_labels else None
    if reference is not None:
        for label in list(valid_labels):
            for candidate in completed[label]:
                try:
                    check_comparable(reference, candidate, label)
                except Exception as error:
                    rejected_labels[label] = str(error)
                    break
    valid_labels = [label for label in binaries if label in completed and label not in rejected_labels]
    if not valid_labels:
        raise RuntimeError("no valid benchmark results completed")
    summary = {}
    for label in valid_labels:
        candidates = completed[label]
        merged = copy_candidate_with_samples(candidates)
        summary[label] = summarize_candidate(merged, process_records.get(label, []))
    state = "complete_with_failures" if rejected_labels else "complete"
    comparison = {
        "state": state,
        "completed_labels": valid_labels,
        "failed_runs": [run for run in runs if run["state"] == "failed"],
        "rejected_labels": rejected_labels,
        "layout": comparable_layout(completed[valid_labels[0]][0]),
        "summary": summary,
    }
    return comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="append", type=parse_label_path, required=True)
    parser.add_argument("--source", action="append", type=parse_label_source, default=[])
    parser.add_argument("--packing", action="append", type=parse_label_packing, default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--experiments", type=int, default=100)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--leaf-count", type=int, default=1 << 22)
    parser.add_argument("--shape", type=parse_shape, default=parse_shape("3,64,1,352,7"))
    parser.add_argument("--memory-gib", type=int, default=4)
    parser.add_argument("--expansion-height", type=int)
    args = parser.parse_args()
    if args.experiments <= 0:
        parser.error("--experiments must be positive")
    if args.rounds <= 0 or args.rounds > 2:
        parser.error("--rounds must be 1 or 2")
    if args.memory_gib <= 0:
        parser.error("--memory-gib must be positive")
    binaries = dict(args.binary)
    if len(binaries) != len(args.binary):
        parser.error("--binary labels must be unique")
    h, G, P, N0, b = args.shape
    expansion_height = args.expansion_height if args.expansion_height is not None else (10 if h == 1 else 9)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / "manifest.json"
    if manifest_path.exists():
        raise ValueError("choose a fresh output directory")
    sources = dict(args.source)
    packings = dict(args.packing)
    if len(packings) != len(args.packing):
        parser.error("--packing labels must be unique")
    unknown_packings = sorted(label for label in packings if label not in binaries)
    if unknown_packings:
        parser.error(f"--packing label has no matching --binary: {unknown_packings}")
    manifest = {
        "state": "running",
        "runs": [],
        "leaf_count": args.leaf_count,
        "samples_per_run": args.experiments,
        "warmups_per_run": 3,
        "rounds": args.rounds,
        "memory_gib": args.memory_gib,
        "shape": {"bundle_arity": h, "record_coeffs": G, "plane_count": P, "N0": N0, "beta_bits": b},
        "expansion_height": expansion_height,
    }

    def save():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    save()
    completed = {}
    process_records = {}
    label_failures = {}
    try:
        for round_id, order in enumerate(run_order(list(binaries.keys()), args.rounds)):
            for label in order:
                candidate, process_record = run_candidate_round(
                    args, binaries, sources, packings, out, expansion_height,
                    manifest, save, round_id, label,
                )
                process_records.setdefault(label, []).append(process_record)
                if candidate is None:
                    label_failures.setdefault(label, "one or more rounds failed")
                else:
                    completed.setdefault(label, []).append(candidate)
        comparison = compare_completed_rounds(
            binaries, args.rounds, completed, process_records, label_failures, manifest["runs"]
        )
        (out / "comparison.json").write_text(json.dumps(comparison, indent=2) + "\n")
        manifest["state"] = comparison["state"]
        save()
    except Exception as error:
        manifest["state"] = "failed"
        manifest["error"] = str(error)
        save()
        raise


def copy_candidate_with_samples(candidates):
    merged = dict(candidates[0])
    samples = []
    for candidate in candidates:
        samples.extend(candidate["samples"])
    merged["samples"] = samples
    return merged


if __name__ == "__main__":
    main()
