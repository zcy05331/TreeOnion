-- DuckDB source queries for the portable OnionPIR three-mode report.
-- The reviewed JSON is produced from the six SHA-256-pinned benchmark
-- artifacts listed in three-mode-data.json.

-- aggregate_modes
SELECT aggregate_mode.*
FROM read_json_auto(
  'outputs/merkle_baselines/three-mode-comprehensive-v2/three-mode-data.json'
) AS synthesis,
UNNEST(synthesis.aggregate_modes) AS aggregate_rows(aggregate_mode);

-- run_points
SELECT run_point.*
FROM read_json_auto(
  'outputs/merkle_baselines/three-mode-comprehensive-v2/three-mode-data.json'
) AS synthesis,
UNNEST(synthesis.run_points) AS run_rows(run_point);

-- latest_path_stats
SELECT path_stat.*
FROM read_json_auto(
  'outputs/merkle_baselines/three-mode-comprehensive-v2/three-mode-data.json'
) AS synthesis,
UNNEST(synthesis.latest_path_sample_statistics) AS stat_rows(path_stat);

-- latest_path_samples
SELECT path_sample.*
FROM read_json_auto(
  'outputs/merkle_baselines/three-mode-comprehensive-v2/three-mode-data.json'
) AS synthesis,
UNNEST(synthesis.latest_path_samples) AS sample_rows(path_sample);

-- artifact_inventory
SELECT
  row_number() OVER () AS sequence,
  CASE
    WHEN artifact.file LIKE '%abaca522%' THEN 'v1'
    ELSE 'v2'
  END AS schema,
  artifact.file,
  artifact.measured_trials,
  array_to_string(artifact.modes, ', ') AS modes,
  artifact.sha256
FROM read_json_auto(
  'outputs/merkle_baselines/three-mode-comprehensive-v2/three-mode-data.json'
) AS synthesis,
UNNEST(synthesis.included_artifacts) AS artifact_rows(artifact);
