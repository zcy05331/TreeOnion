#!/usr/bin/env python3
import csv,json,math,statistics,sys
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts'))
from summarize_cloudlab_profiles import validate_profile
OUT=Path(__file__).resolve().parent
SOURCES={
 'Clemson':ROOT/'outputs/superrecord/cloudlab-20260904/profiling',
 'Wisconsin':ROOT/'outputs/superrecord/wisconsin-20260905/profiling',
}
CORE=['Generic best h3','Generic fastest h1','Layerwise tuned','Tree g=32','Cuckoo tuned','TreePIR','Flat']
NAMES={'layerwise_tuned':'Layerwise tuned','tree_g32':'Tree g=32','cuckoo_tuned':'Cuckoo tuned','treepir':'TreePIR','flat':'Flat','layerwise':'Layerwise legacy','cuckoo':'Cuckoo legacy','tree_compressed':'Tree compressed'}
rows=[]; stages=[]; candidate_rows=[]
def add(host,scale,name,p,samples,query=None,response=None,query_kind='modeled',extra=None):
 vals=[float(v) for v in samples]
 assert vals and all(math.isfinite(v) and v>0 for v in vals)
 mean=statistics.fmean(vals); gib={'1gb':1,'4gb':4,'8gb':8}[scale]
 row=dict(host=host,scale=scale,scheme=name,count=len(vals),mean_server_ms=mean,
          p95_server_ms=sorted(vals)[math.ceil(len(vals)*.95)-1],sample_sd_ms=statistics.stdev(vals),
          logical_MiB_s=gib*1024*1000/mean,path_queries_s=1000/mean,
          nominal_database_bytes=gib*2**30,entry_bytes=32,query_bytes=query,query_kind=query_kind,
          response_bytes=response,response_kind='actual',source=str(p),**(extra or {}))
 rows.append(row); return row
for host,root in SOURCES.items():
 for scale in ('1gb','4gb','8gb'):
  folder=root/scale
  for basename,name in [('generic','Generic'),*NAMES.items()]:
   p=folder/(basename+'.json'); proc=folder/(basename+'-process.json')
   if not p.exists() or not proc.exists(): continue
   data=json.loads(p.read_text()); process=json.loads(proc.read_text())
   assert process['exit_code']==0, p
   n=16 if basename=='flat' or (host=='Wisconsin' and basename in ('cuckoo_tuned','treepir')) else 100
   leaves=2**{'1gb':24,'4gb':26,'8gb':27}[scale]
   family=validate_profile(data,p,n,leaves)
   if family=='generic':
    assert data['entry_bits']==256 and not data.get('plan_only')
    candidates=[c for c in data['candidates'] if c.get('eligible') and len(c.get('samples',[]))==n and c.get('is_generic')]
    for c in candidates:
     h=c['shape']['bundle_arity']; vals=[x['server_total_ms'] for x in c['samples']]
     candidate_rows.append(dict(host=host,scale=scale,label=c['label'],h=h,G=c['shape']['record_coeffs'],P=c['shape']['plane_count'],N0=c['shape']['N0'],b=c['shape']['beta_bits'],count=n,mean_server_ms=statistics.fmean(vals),logical_MiB_s={'1gb':1024,'4gb':4096,'8gb':8192}[scale]*1000/statistics.fmean(vals)))
    for h,label in [(3,'Generic best h3'),(1,'Generic fastest h1')]:
     pool=[c for c in candidates if c['shape']['bundle_arity']==h]
     assert pool,(host,scale,h)
     c=min(pool,key=lambda c:statistics.fmean(x['server_total_ms'] for x in c['samples']))
     shape={k:c['shape'][k] for k in ('bundle_arity','record_coeffs','plane_count','N0','beta_bits')}
     add(host,scale,label,p,[x['server_total_ms'] for x in c['samples']],c['wire']['modeled_seeded_query_bytes'],c['wire']['actual_response_ciphertext_payload_bytes'],extra={'shape':json.dumps(shape,sort_keys=True)})
     for key in c['samples'][0]['stages_ms']:
      stages.append(dict(host=host,scale=scale,scheme=label,stage=key,mean_ms=statistics.fmean(x['stages_ms'][key] for x in c['samples']),count=n))
   elif family=='merkle':
    assert data['workload']['node_bytes']==32
    target='merkle_flat' if basename=='flat' else 'merkle_layerwise'
    matches=[case for case in data['cases'] if case['name']==target]
    assert len(matches)==1,(p,target)
    c=matches[0]
    add(host,scale,name,p,c['server_compute_samples_ms'],c['online_query_bytes_modeled'],c['online_response_bytes_actual'])
    for key in c['pipeline_profile_samples_ms'][0]['stages']:
     stages.append(dict(host=host,scale=scale,scheme=name,stage=key,mean_ms=statistics.fmean(x['stages'][key] for x in c['pipeline_profile_samples_ms']),count=n))
   elif family=='sampled':
    assert data['node_entry_bits']==256
    comm=data['communication']
    add(host,scale,name,p,[x['server_total_ms'] for x in data['samples']],comm.get('modeled_query_bytes',comm.get('modeled_query_bytes_per_trial')),comm.get('actual_response_bytes',comm.get('actual_response_bytes_per_trial')))
    for key in data['samples'][0]['stages_ms']:
     stages.append(dict(host=host,scale=scale,scheme=name,stage=key,mean_ms=statistics.fmean(x['stages_ms'][key] for x in data['samples']),count=n))
   else:
    assert data['entry_size_bytes']==32 and not data.get('validate_index_only')
    samples=[x for x in data['results'] if x['phase']=='measured']
    add(host,scale,name,p,[x['server_ms'] for x in samples],statistics.fmean(x['actual_query_payload_bytes_unseeded'] for x in samples),statistics.fmean(x['actual_response_payload_bytes'] for x in samples),'actual_unseeded',extra={'configuration':data.get('backend_label') or ('vbpir_fd'+str(data.get('layout',{}).get('first_two_dimensions','unknown')))})
    for key in samples[0]:
     if key.endswith('_ms') and key!='server_ms':
      stages.append(dict(host=host,scale=scale,scheme=name,stage=key,mean_ms=statistics.fmean(x[key] for x in samples),count=n))

pilots=[]
for host,root in SOURCES.items():
 for scale in ('1gb','4gb','8gb'):
  for p in sorted((root/scale/'tuning/cuckoo').glob('h[0-9]*.json')):
   if '-process' in p.name: continue
   proc=p.with_name(p.stem+'-process.json')
   if not proc.exists(): continue
   d=json.loads(p.read_text()); assert json.loads(proc.read_text())['exit_code']==0
   validate_profile(d,p,16,2**{'1gb':24,'4gb':26,'8gb':27}[scale])
   mean=statistics.fmean(x['server_total_ms'] for x in d['samples'])
   pilots.append(dict(host=host,scale=scale,measurement_kind='pilot',scheme='Cuckoo',
      expansion_height=d['requested_expansion_height'],N0=d['bucket_layouts'][0]['layout']['N0'],
      count=16,mean_server_ms=mean,logical_MiB_s={'1gb':1024,'4gb':4096,'8gb':8192}[scale]*1000/mean,source=str(p)))

single_blocks=[]
for host,root in SOURCES.items():
 for scale in ('1gb','4gb','8gb'):
  p=root/scale/'standard.json'; proc=root/scale/'standard-process.json'
  if not p.exists() or not proc.exists(): continue
  d=json.loads(p.read_text()); assert json.loads(proc.read_text())['exit_code']==0
  validate_profile(d,p,100,2**{'1gb':24,'4gb':26,'8gb':27}[scale])
  c=next(x for x in d['cases'] if x['name']=='standard_onionpir')
  mean=statistics.fmean(c['server_compute_samples_ms'])
  single_blocks.append(dict(host=host,scale=scale,workload='single_contiguous_plaintext_block',
    count=len(c['server_compute_samples_ms']),mean_server_ms=mean,
    logical_MiB_s={'1gb':1024,'4gb':4096,'8gb':8192}[scale]*1000/mean,
    returned_block_bytes=c['useful_response_bytes'],query_bytes=c['online_query_bytes_modeled'],
    response_bytes=c['online_response_bytes_actual'],helper_key_bytes=c['helper_key_bytes_modeled'],source=str(p)))

def csvwrite(name,data):
 keys=list(dict.fromkeys(k for row in data for k in row))
 with (OUT/name).open('w',newline='') as f:
  writer=csv.DictWriter(f,fieldnames=keys);writer.writeheader();writer.writerows(data)
csvwrite('single-block-reference.csv',single_blocks); csvwrite('cuckoo-pilots.csv',pilots); csvwrite('formal-throughput.csv',rows); csvwrite('stage-times.csv',stages);csvwrite('generic-candidates.csv',candidate_rows)
(OUT/'summary.json').write_text(json.dumps({'generated_utc':datetime.now(timezone.utc).isoformat(),'metric':'nominal logical tier MiB / mean server seconds','rows':rows,'generic_candidates':candidate_rows,'cuckoo_pilots':pilots,'single_block_reference':single_blocks},indent=2)+'\n')
text='# 已完成实验吞吐量快照\n\n生成时间：'+datetime.now(timezone.utc).isoformat()+'\n\n'
text+='口径：档位逻辑容量 / 平均服务器耗时，MiB/s；1 GiB=1024 MiB。每次服务完整路径；不含离线预处理、密钥生成、客户端解码和网络传输时间。吞吐量按容量除以平均耗时计算。\n\n'
text+='仅纳入已生成结果、进程退出码为0、样本数与正确性验证通过的正式实验。空格表示尚无完成结果。\n\n'
text+='| 方案 | Wisconsin 1 GiB | Wisconsin 4 GiB | Clemson 1 GiB | Clemson 8 GiB |\n|---|---:|---:|---:|---:|\n'
lookup={(x['host'],x['scale'],x['scheme']):x for x in rows}
for name in CORE:
 cells=[]
 for host,scale in [('Wisconsin','1gb'),('Wisconsin','4gb'),('Clemson','1gb'),('Clemson','8gb')]:
  x=lookup.get((host,scale,name));cells.append(f"{x['logical_MiB_s']:.2f} ({x['count']}次)" if x else '—')
 text+='| '+name+' | '+' | '.join(cells)+' |\n'
text+='\n## 耗时与通信\n\n| 服务器/档位 | 方案 | 平均服务器秒 | 95分位秒 | 查询B | 响应B |\n|---|---|---:|---:|---:|---:|\n'
for x in rows:
 if x['scheme'] not in CORE:continue
 text+=f"| {x['host']} {x['scale']} | {x['scheme']} | {x['mean_server_ms']/1000:.6f} | {x['p95_server_ms']/1000:.6f} | {x['query_bytes']:.0f} | {x['response_bytes']:.0f} |\n"
text+='\n查询除TreePIR为实际unseeded序列化外均为模型估算；响应为实际序列化。TreePIR的Clemson1GiB为旧VBPIR默认首维128；Wisconsin1GiB已完成VBPIR首维256的独立16次正式复测。4GiB后端正式复测仍在运行。\n\n'
text+='## 历史对照附表\n\n| 服务器/档位 | 旧对照 | MiB/s | 次数 |\n|---|---|---:|---:|\n'
for x in rows:
 if x['scheme'] not in CORE:text+=f"| {x['host']} {x['scale']} | {x['scheme']} | {x['logical_MiB_s']:.2f} | {x['count']} |\n"
text+='\n## 单块OnionPIRv2参考\n\n此处每次返回一个3KiB连续明文块，不完成完整路径，因此单独列示。\n\n| 服务器/档位 | 平均服务器秒 | MiB/s | 返回块B | 次数 |\n|---|---:|---:|---:|---:|\n'
for x in single_blocks:
 text+=f"| {x['host']} {x['scale']} | {x['mean_server_ms']/1000:.6f} | {x['logical_MiB_s']:.2f} | {x['returned_block_bytes']} | {x['count']} |\n"
text+='\n## Cuckoo选参附表（pilot）\n\n| 服务器/档位 | 展开高度 | N0 | MiB/s | 次数 |\n|---|---:|---:|---:|---:|\n'
for x in pilots:
 text+=f"| {x['host']} {x['scale']} | {x['expansion_height']} | {x['N0']} | {x['logical_MiB_s']:.2f} | {x['count']} |\n"
text+='\n## 范围\n\nGeneric/g32返回含根路径，Layerwise/Cuckoo/TreePIR/Flat为非根层路径。保留既有语义。Standard单次查找和g1的12bit标量数据不进入32B路径吞吐量对照。旧node0历史运行也未并入当前两台服务器的表格。各方案在独立进程执行，跨方案倍数为描述性比较。\n\n'
text+='Generic完整候选及形状见generic-candidates.csv。阶段均值见stage-times.csv；原始路径、样本数、标准差、通信口径见formal-throughput.csv。选参pilot与正式试验分开，主表不采用pilot。\n'
(OUT/'REPORT.md').write_text(text)
print(text.split('## 耗时')[0])
