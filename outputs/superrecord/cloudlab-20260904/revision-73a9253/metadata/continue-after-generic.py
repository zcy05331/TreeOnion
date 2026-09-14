import json,os,signal,shutil,time
from datetime import datetime,timezone
from pathlib import Path
base=Path('/users/zcy05331/generic-superrecord-bench-20260904')
root=base/'revision-73a9253'
(root/'metadata/matrix.status').write_text('waiting_for_existing_generic\n')
(root/'metadata/current-job.txt').write_text('1gb generic (revision-22adeee finishing; aligned queue waiting)\n')
while True:
    try: state=Path('/proc/15162/stat').read_text().rsplit(')',1)[1].split()[0]
    except FileNotFoundError: break
    if state=='Z': break
    time.sleep(1)
# Wait until the current process wrapper has written its result. Then shut
# down the old process group before dispatching any new timing process.
try: os.killpg(14403,signal.SIGTERM)
except ProcessLookupError: pass
def group_members():
    result=[]
    for p in Path('/proc').glob('[0-9]*/stat'):
        try: fields=p.read_text().rsplit(')',1)[1].split()
        except (FileNotFoundError,ProcessLookupError): continue
        if fields[2]=='14403' and fields[0]!='Z': result.append(p.parent.name)
    return result
for _ in range(50):
    if not group_members(): break
    time.sleep(0.1)
if group_members():
    try: os.killpg(14403,signal.SIGKILL)
    except ProcessLookupError: pass
source=base/'outputs/profiling/1gb'
def archive_case(name,destination):
    destination.mkdir(parents=True,exist_ok=True)
    for suffix in ['.json','.log','-process.json']:
        p=source/(name+suffix)
        if p.exists():shutil.move(str(p),str(destination/p.name))
archive_case('cuckoo',base/'outputs/controls/cuckoo-unaligned-reference-22adeee/1gb')
proc=source/'generic-process.json'
if not proc.exists() or json.loads(proc.read_text())['exit_code']!=0:
    archive_case('generic',base/'outputs/failures/generic-revision-22adeee')
# If the old runner began the next job before the one-second handoff poll,
# keep that partial startup separately; the new queue will run it in full.
next_proc=source/'treepir-process.json'
if (source/'treepir.log').exists() and (not next_proc.exists() or json.loads(next_proc.read_text())['exit_code']!=0):
    archive_case('treepir',base/'outputs/superseded/treepir-before-aligned-handoff')
(base/'metadata/active-run-root.txt').write_text(str(root)+'\n')
(base/'metadata/active-session.txt').write_text('generic-aligned-73a9253\n')
(root/'metadata/handoff-complete-at.txt').write_text(datetime.now(timezone.utc).isoformat()+'\n')
log=os.open(root/'metadata/matrix-controller.log',os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o644)
os.dup2(log,1);os.dup2(log,2);os.close(log)
os.execv('/bin/bash',['bash',str(root/'source/scripts/run_cloudlab_profile_matrix.sh'),str(root)])
