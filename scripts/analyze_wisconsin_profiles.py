#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Recompute Wisconsin stage profiles from completed raw results; no reruns."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path
from analyze_superrecord_sweep import normalize_run, paired_bootstrap
from summarize_cloudlab_profiles import validate_profile

GROUPS = ['frontend','scan','finalize','fold','rotate_project','pack_switch','other']
LABELS = {'frontend':'查询展开/转换/准备','scan':'首维扫描','finalize':'结果恢复/逆NTT','fold':'折叠',
          'rotate_project':'旋转/投影','pack_switch':'打包/模切','other':'其余开销'}
ORDER = ['Standard','Generic h3','Generic h1','Layerwise tuned','Tree g32','Cuckoo tuned','Flat']

def mean(values): return statistics.fmean(values)
def load(p): return json.loads(p.read_text())
def write_csv(p, rows):
    keys = list(dict.fromkeys(k for row in rows for k in row))
    with p.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=keys); w.writeheader();w.writerows(rows)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();root=args.root.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    rows=[];fine=[];native=[];shapes=[];deltas=[];comparisons={};sources=[]
    for scale,H,MiB in [('1gb',24,1024),('4gb',26,4096)]:
        folder=root/scale
        for basename in ['generic','standard','layerwise_tuned','tree_g32','cuckoo_tuned','flat','treepir']:
            p=folder/(basename+'.json');d=load(p);proc=load(folder/(basename+'-process.json'))
            assert proc['exit_code']==0,p
            trials=16 if basename in ['cuckoo_tuned','flat','treepir'] else 100
            validate_profile(d,p,trials,1<<H);sources.append(str(p))
            items=[]
            if basename=='generic':
                normalize_run(d)  # validates shared leaf/block schedules
                chosen={}
                for h in (3,1):
                    candidates=[c for c in d['candidates'] if c.get('eligible') and len(c.get('samples',[]))==trials and c['shape']['bundle_arity']==h]
                    c=min(candidates,key=lambda x:mean(s['server_total_ms'] for s in x['samples']))
                    chosen[h]=c;sh=c['shape'];m=c['candidate_metrics'];wire=c['wire']
                    shapes.append(dict(scale=scale,scheme='Generic h'+str(h),h=h,G=sh['record_coeffs'],P=sh['plane_count'],N0=sh['N0'],b=sh['beta_bits'],rho=sh['records_per_plaintext'],query_width=sh['query_width'],expansion_capacity=sh['expansion_capacity'],scan_GiB=m['physical_scan_bytes']/2**30,steady_GiB=m['optimized_steady_state_bytes_estimate']/2**30,fold_count=m['fold_cmux_count'],rotate_count=m['rotate_cmux_count'],project_count=m['projection_keyswitch_count'],alpha_ct=m['alpha_pyramid_ciphertexts'],payload_utilization=h*256/(sh['plane_count']*sh['record_coeffs']*12)))
                    items.append(('Generic h'+str(h),c['samples'],'server_total_ms','stages_ms',wire['modeled_seeded_query_bytes'],wire['actual_response_ciphertext_payload_bytes'],m['modeled_per_client_eval_key_bytes'],'generic'))
                a=sorted(chosen[3]['samples'],key=lambda s:s['block_id']);b=sorted(chosen[1]['samples'],key=lambda s:s['block_id'])
                assert [(s['block_id'],s['leaf']) for s in a]==[(s['block_id'],s['leaf']) for s in b]
                comparisons[scale]=paired_bootstrap([s['server_total_ms'] for s in a],[s['server_total_ms'] for s in b],seed=20260906,resamples=10000)
            elif 'cases' in d:
                target='standard_onionpir' if basename=='standard' else 'merkle_flat' if basename=='flat' else 'merkle_layerwise'
                c=next(c for c in d['cases'] if c['name']==target)
                samples=[dict(server_total_ms=t,stages_ms=s['stages'],client_query_ms=c['client_query_samples_ms'][i],client_decode_ms=c['response_load_decrypt_extract_samples_ms'][i],response_serialize_ms=c['response_serialize_samples_ms'][i]) for i,(t,s) in enumerate(zip(c['server_compute_samples_ms'],c['pipeline_profile_samples_ms']))]
                name={'standard':'Standard','layerwise_tuned':'Layerwise tuned','flat':'Flat'}[basename]
                items.append((name,samples,'server_total_ms','stages_ms',c['online_query_bytes_modeled'],c['online_response_bytes_actual'],c['helper_key_bytes_modeled'],'onion'))
            elif basename!='treepir':
                co=d['communication'];name='Tree g32' if basename=='tree_g32' else 'Cuckoo tuned'
                items.append((name,d['samples'],'server_total_ms','stages_ms',co.get('modeled_query_bytes',co.get('modeled_query_bytes_per_trial')),co.get('actual_response_bytes',co.get('actual_response_bytes_per_trial')),co.get('modeled_helper_key_bytes',co.get('modeled_helper_key_bytes')), 'tree' if basename=='tree_g32' else 'onion'))
            else:
                samples=[s for s in d['results'] if s['phase']=='measured'];total=mean(s['server_ms'] for s in samples)
                keys=['server_first_dimension_ms','server_second_dimension_ms','server_last_dimension_ms','server_merge_ms'] if scale=='1gb' else ['server_expansion_ms','server_query_ntt_ms','server_database_ntt_ms','server_multiply_accumulate_ms','server_inverse_ntt_ms','server_recursive_decomposition_modswitch_ms','server_modswitch_ms']
                timed=0
                for key in keys:
                    value=mean(s[key] for s in samples);timed+=value
                    native.append(dict(scale=scale,backend=d['backend_label'],stage=key,mean_ms=value,percent=100*value/total))
                native.append(dict(scale=scale,backend=d['backend_label'],stage='other',mean_ms=total-timed,percent=100*(total-timed)/total))
                rows.append(dict(scale=scale,scheme='TreePIR',n=trials,total_ms=total,MiB_s=MiB*1000/total,query_ms=mean(s['query_ms'] for s in samples),decode_ms=mean(s['decode_ms'] for s in samples),serialize_ms=mean(s.get('query_serialize_ms',0)+s.get('response_serialize_ms',0) for s in samples),query_B=mean(s['actual_query_payload_bytes_unseeded'] for s in samples),response_B=mean(s['actual_response_payload_bytes'] for s in samples),keys_B=d['actual_helper_key_bytes_uncompressed'],query_kind='actual_unseeded',keys_kind='actual',peak_GiB=proc['process_peak_rss_bytes']/2**30,backend=d['backend_label'],source=str(p)))
            for name,samples,totalkey,stagekey,qbytes,rbytes,kbytes,kind in items:
                st={k:mean(s[stagekey][k] for s in samples) for k in samples[0][stagekey]}
                for k,v in st.items():fine.append(dict(scale=scale,scheme=name,stage=k,mean_ms=v))
                if kind=='generic':g=dict(frontend=st['expand']+st['complete']+st['prepare'],scan=st['scan'],finalize=st['finalize'],fold=st['fold'],rotate_project=st['rotate']+st['project'],pack_switch=st['pack']+st['switch'])
                elif kind=='tree':g=dict(frontend=st['query_expand']+st['selector_completion']+st['pyramid'],scan=st['scan_core'],finalize=st['scan_finalize'],fold=st['fold'],rotate_project=st['rotate']+st['project'],pack_switch=st['pack']+st['switch'])
                else:g=dict(frontend=st['expand']+st['convert']+st['first_dim_query_ntt']+st['first_dim_query_pack'],scan=st['first_dim_core'],finalize=st['first_dim_finalize'],fold=st['other_dim'],rotate_project=0.0,pack_switch=st['mod_switch'])
                total=mean(s[totalkey] for s in samples);g['other']=total-sum(g.values())
                assert g['other']>=-0.01,(name,scale,total,g)
                row=dict(scale=scale,scheme=name,n=trials,total_ms=total,MiB_s=MiB*1000/total,**g,query_ms=mean(s['client_query_ms'] for s in samples),decode_ms=mean(s['client_decode_ms'] for s in samples),serialize_ms=mean(s.get('response_serialize_ms',0) for s in samples),query_B=qbytes,response_B=rbytes,keys_B=kbytes,query_kind='modeled_seeded',keys_kind='modeled',peak_GiB=proc['process_peak_rss_bytes']/2**30,source=str(p))
                rows.append(row)
        h1=next(r for r in rows if r['scale']==scale and r['scheme']=='Generic h1');h3=next(r for r in rows if r['scale']==scale and r['scheme']=='Generic h3')
        for group in GROUPS:deltas.append(dict(scale=scale,stage=group,h1_ms=h1[group],h3_ms=h3[group],saved_ms=h1[group]-h3[group]))
        assert abs(sum(x['saved_ms'] for x in deltas if x['scale']==scale)-(h1['total_ms']-h3['total_ms']))<1e-6
    write_csv(out/'stages.csv',rows);write_csv(out/'native-stages.csv',fine);write_csv(out/'treepir-stages.csv',native);write_csv(out/'generic-shapes.csv',shapes);write_csv(out/'h3-savings.csv',deltas)
    (out/'analysis.json').write_text(json.dumps(dict(rows=rows,generic_shapes=shapes,h3_vs_h1=comparisons,stage_savings=deltas,treepir=native,sources=sources),indent=2,ensure_ascii=False)+'\n')
    lines=['# Wisconsin 分阶段性能分析','', '口径：32 B entry，固定原生二进制；100次测量（Cuckoo/TreePIR/Flat为16次），均有3次预热。单位ms，吞吐量用逻辑档位容量除以平均服务器耗时。数据来自已完成的JSON及exit0进程记录；本次没有重跑或修改C++计时。','', '## h3 为什么比 h1 快','', 'h=3合并的是**同一层内三个相邻entry**，并非三个树层。h1的G8/P3给一个256-bit entry分配288bit，槽位利用率88.89%；h3的G64/P1或G32/P2给三个entry分配768bit，利用率100%。理想容量下降11.11%，考虑h3矩阵padding后，实际扫描量下降约8.33%。','']
    for scale in ('1gb','4gb'):
        cp=comparisons[scale]
        lines.append(f"- {scale}: h3耗时减少 **{cp['server_time_reduction_percent']:.3f}%**；吞吐提升 **{(cp['speedup']-1)*100:.3f}%**。配对block bootstrap 95%加速比区间 **{cp['speedup_ci95'][0]:.5f}–{cp['speedup_ci95'][1]:.5f}**（10000次重采样）。")
    lines+=['','1GiB中，旋转/投影合计节省115.658ms，扫描节省81.855ms，查询前端节省39.992ms；折叠和恢复多花57.237ms。4GiB中扫描节省426.648ms是主要收益，但折叠和恢复多花230.055ms，抵消了部分收益。','', '扫描时间下降并不完全等同于扫描字节下降：N0与矩阵形状同时改变，观测到有效扫描带宽也不同；本次未用硬件计数器隔离缓存/频率因素，不能把全部差值归因于容量。','', '## 共同阶段表','', '前端包含展开、selector转换与查询/alpha准备；恢复包含CRT重建/逆NTT；其余是总计时减去已命名阶段的余项。标准/Layerwise/Cuckoo/Flat没有私有旋转与投影阶段，表中为0。TreePIR使用原生阶段表另列，避免把其复合首维阶段当作Onion的纯扫描内核。','']
    for scale in ('1gb','4gb'):
        lines += [f'### {scale}','', '| 方案 | 前端 | 扫描 | 恢复 | 折叠 | 旋转/投影 | 打包/模切 | 其余 | 总计 |','|---|---:|---:|---:|---:|---:|---:|---:|---:|']
        for name in ORDER:
            r=next(r for r in rows if r['scale']==scale and r['scheme']==name)
            lines.append('| '+name+' | '+' | '.join(f"{r[k]:.3f}" for k in GROUPS+["total_ms"])+' |')
        lines+=['']
    lines += ['## h1/h3 布局与操作量','', '| 档位/方案 | h/G/P | N0/b | 展开容量W | 扫描GiB | Fold次数 | 旋转/投影次数 | alpha密文数 |','|---|---|---|---:|---:|---:|---:|---:|']
    for sh in shapes:
        lines.append(f"| {sh['scale']} / {sh['scheme']} | {sh['h']}/{sh['G']}/{sh['P']} | {sh['N0']}/{sh['b']} | {sh['expansion_capacity']} | {sh['scan_GiB']:.6f} | {sh['fold_count']} | {sh['rotate_count']}/{sh['project_count']} | {sh['alpha_ct']} |")
    lines += ['', '1GiB实测3个h1和3个h3布局；4GiB受共同驻留内存预算限制，实测1个h1和3个h3布局。“fastest/best”均指本次已测候选。配对CI针对选定配置，不包含参数选择、硬件或数据库变化的不确定性。','']
    lines += ['## TreePIR 原生阶段','', '| 档位/后端 | 阶段 | ms | 占服务器总时长 |','|---|---|---:|---:|']
    for r in native:lines.append(f"| {r['scale']} / {r['backend']} | {r['stage']} | {r['mean_ms']:.3f} | {r['percent']:.2f}% |")
    lines += ['', '1GiB为VBPIR首维256；4GiB为SealPIR二维。VBPIR阶段较粗；SealPIR用逐颜色数据库执行、按路径求和的方式统计。该不同执行方式和HE参数使跨后端逐列归因需要谨慎。','', '## 客户端、通信与进程峰值','', '| 档位/方案 | 查询ms | 解码ms | 序列化ms | 查询B | 响应B | 密钥B | 进程峰值GiB |','|---|---:|---:|---:|---:|---:|---:|---:|']
    for r in rows:lines.append(f"| {r['scale']} / {r['scheme']} | {r['query_ms']:.3f} | {r['decode_ms']:.3f} | {r['serialize_ms']:.3f} | {r['query_B']:.0f} | {r['response_B']:.0f} | {r['keys_B']:.0f} | {r['peak_GiB']:.3f} |")
    lines += ['', 'Onion查询/密钥为模型估算，TreePIR为实际unseeded查询与uncompressed密钥；响应均为实际序列化。Onion序列化列只有响应，TreePIR列为查询+响应，因此不直接比较该列。Generic进程峰值属于同时驻留的全部候选，共享在h1/h3行中，不能当作某一个候选单独占用。Standard每次返回一个3KiB连续块；其余为路径。Generic/g32含根，其他路径基线不含根。','', '## 计时代码与新增产物','', '本轮使用既有src/tests/test_tree_superrecord_benchmark.cpp、src/merkle_benchmark.cpp相关计时、Cuckoo与TreePIR adapter的结构化输出；未增加C++ instrumentation。新增scripts/analyze_wisconsin_profiles.py负责校验、统计和归一化，scripts/plot_wisconsin_profiles.R负责独立图表。CSV/JSON保留未四舍五入数据。','', '后续如要把N0形状影响与容量收益严格分离，需要固定布局的内核消融；当前报告只对已有实测作归因，不增补未经运行的硬件指标。']
    (out/'REPORT.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps(comparisons,indent=2))

if __name__=='__main__':main()
