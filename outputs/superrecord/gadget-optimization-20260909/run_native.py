#!/usr/bin/env python3
"""Finite native validation after the local small-gadget comparison."""
import argparse, datetime, json, os, shutil, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--root',type=Path,required=True)
p.add_argument('--hexl',required=True)
p.add_argument('--baseline',required=True)
p.add_argument('--config',action='append',required=True)
a=p.parse_args();r=a.root.resolve();source=r/'source';out=r/'outputs';out.mkdir(exist_ok=True)
meta=r/'metadata';meta.mkdir(exist_ok=True)
state={'status':'running','source_commit':'5d5f6c39447fa4b4beeaa136846ba2796e25c70d','finished_stages':[]}
def save():
 state['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
 (meta/'progress.json').write_text(json.dumps(state,indent=2)+'\n')
def stage(name,cmd):
 state['current_stage']=name;save()
 with (out/(name+'.log')).open('w') as log:
  proc=subprocess.run(cmd,cwd=source,stdout=log,stderr=subprocess.STDOUT)
 state['finished_stages'].append({'name':name,'exit_code':proc.returncode});save()
 if proc.returncode:raise RuntimeError(name+' failed')
try:
 with (meta/'hardware.txt').open('w') as f:
  for cmd in [['date','-u'],['uname','-a'],['lscpu'],['free','-h'],['c++','--version']]:
   subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 stage('configure',['cmake','-S',str(source),'-B',str(r/'build'),'-DACTIVE_CONFIG=CONFIG_N2048_K1_COMP','-DHEXL_DIR='+a.hexl,'-DCMAKE_BUILD_TYPE=Benchmark','-DPIR_EP_ROWS=6','-DPIR_EP_LOW_BITS=0','-DPIR_KS_ROWS=8','-DPIR_KS_LOW_BITS=0'])
 stage('build_kernel',['cmake','--build',str(r/'build'),'-j','4'])
 kernel=r/'binaries/kernel/Onion-PIR';kernel.parent.mkdir(parents=True,exist_ok=True)
 shutil.copy2(r/'build/Onion-PIR',kernel)
 stage('regression',[str(kernel),'--test','all'])
 cmd=[sys.executable,'scripts/build_gadget_configs.py','--build-dir',str(r/'build'),'--output',str(out/'candidate_builds'),'--binaries',str(r/'binaries'),'--jobs','4']
 for label in a.config:cmd+=['--config',label]
 stage('candidate_builds',cmd)
 for h,shape,height in [(3,'3,64,1,352,9',9),(1,'1,8,3,512,7',10)]:
  cmd=['numactl','--physcpubind=0','--interleave=all',sys.executable,'scripts/benchmark_gadget_configs.py','--output',str(out/('h'+str(h))),'--experiments','50','--rounds','2','--leaf-count',str(1<<24),'--shape',shape,'--expansion-height',str(height),'--memory-gib','80','--binary','baseline='+a.baseline,'--source','baseline=a9503fb','--binary','kernel='+str(kernel),'--source','kernel='+state['source_commit']]
  for label in a.config:cmd+=['--binary',label+'='+str(r/'binaries'/label/'Onion-PIR'),'--source',label+'='+state['source_commit']]
  stage('formal_h'+str(h),cmd)
  result=json.loads((out/('h'+str(h))/'comparison.json').read_text())
  state.setdefault('comparison_states',{})['h'+str(h)]=result['state'];save()
 state['status']='complete_with_failures' if any(v!='complete' for v in state.get('comparison_states',{}).values()) else 'complete'
 state['current_stage']=None;save()
except Exception as e:
 state['status']='failed';state['error']=str(e);save();raise
