"""Wait for other user computation to become idle; stopped jobs are left alone."""
import argparse
import json
import os
import time
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--root',required=True);args=p.parse_args()
root=Path(args.root);uid=os.getuid()
compute=('onion','python','rustc','cargo','cmake','cc1','g++','clang','seperated','pir_','benchmark')
def busy_jobs():
    result=[]
    for directory in Path('/proc').glob('[0-9]*'):
        try:
            if directory.stat().st_uid!=uid:continue
            data=(directory/'stat').read_text().split(') ',1)[1].split()
            name=(directory/'comm').read_text().strip()
            if data[0] in ('T','t','Z'):continue
            command=(directory/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
            if str(root) in command:continue
            if any(x in name.lower() for x in compute):result.append({'pid':int(directory.name),'name':name,'state':data[0]})
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
    return result

def ticks():
    return {x[0]:list(map(int,x[1:9])) for line in Path('/proc/stat').read_text().splitlines()
            if (x:=line.split()) and x[0].startswith('cpu') and x[0]!='cpu'}
quiet=0
while quiet<2:
    jobs=busy_jobs();before=ticks();time.sleep(5);after=ticks()
    activity=[]
    for cpu,a in before.items():
        b=after[cpu];delta=[y-x for x,y in zip(a,b)];total=sum(delta)
        activity.append((total-delta[3]-delta[4])/total if total else 0)
    jobs=jobs+busy_jobs();peak=max(activity)
    quiet=quiet+1 if not jobs and peak<0.20 else 0
    (root/'metadata/idle.json').write_text(json.dumps({'observed_at':time.time(),'quiet_windows':quiet,'other_jobs':jobs,'maximum_core_busy_fraction':peak},indent=2)+'\n')
    if quiet==0:time.sleep(20)
