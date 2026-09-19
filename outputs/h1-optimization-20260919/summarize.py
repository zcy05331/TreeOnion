"""Summarize retained local screens without discarding slow samples."""
import json
import statistics
from pathlib import Path

root = Path(__file__).resolve().parent
rows = []
experiments = []
for directory, variant in [('local-256mib-ab', 'prepared'), ('local-256mib-tiled-ab', 'tiled')]:
    folder = root / directory
    manifest = json.loads((folder / 'manifest.json').read_text())
    comparison = json.loads((folder / 'comparison.json').read_text())
    assert comparison['state'] == manifest['state'] == 'complete'
    summaries = comparison['summary']
    reference = summaries['reference']
    candidate = summaries[variant]
    for key in ('wire', 'key_metadata'):
        assert reference[key] == candidate[key], key
    assert reference['gadget_metadata']['gadget_identity'] == candidate['gadget_metadata']['gadget_identity']
    experiment_rows = []
    for run in manifest['runs']:
        assert run['state'] == 'complete'
        data = json.loads((folder / (run['name'] + '.json')).read_text())
        accepted = [x for x in data['candidates'] if x.get('eligible') and x.get('samples')]
        assert len(accepted) == 1
        c = accepted[0]
        assert c['target_noise_result']['decrypts_correctly'] is True
        assert len(c['samples']) == 16
        values = [s['server_total_ms'] for s in c['samples']]
        stages = {k: statistics.fmean(s['stages_ms'][k] for s in c['samples'])
                  for k in c['samples'][0]['stages_ms']}
        row = dict(experiment=directory, run=run['name'], source=run['source'], samples=16,
                   server_ms=statistics.fmean(values), server_sd_ms=statistics.stdev(values),
                   server_median_ms=statistics.median(values), stages_ms=stages)
        rows.append(row);experiment_rows.append(row)
    pairs = []
    for round_id in (0, 1):
        a = next(r for r in experiment_rows if r['run'] == f'round{round_id}-reference')
        b = next(r for r in experiment_rows if r['run'] == f'round{round_id}-{variant}')
        pairs.append({'round': round_id,
                      'server_time_change_pct': 100 * (b['server_ms'] / a['server_ms'] - 1)})
    experiments.append({'experiment': directory, 'reference': reference['server_total_ms_mean'],
                        'candidate': candidate['server_total_ms_mean'],
                        'reference_prepare': reference['stages_ms_mean']['prepare'],
                        'candidate_prepare': candidate['stages_ms_mean']['prepare'],
                        'rounds': pairs, 'wire': reference['wire']})
result = {'scope': 'Local Apple M4, x86_64 Rosetta,256MiB,EP3/drop32,KS8,MLWE,h1/G8/P3/N0=512/b5',
          'measured_queries': sum(x['samples'] for x in rows), 'experiments': experiments, 'runs': rows,
          'decision': 'untiled rejected locally; tiled preparation improved but full-query effects need target-host validation',
          'native_avx512_executed': False}
(root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(experiments, indent=2))
