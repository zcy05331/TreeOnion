# Scheduled Clemson matrix

User instruction supersedes the earlier idle observation: another project is
currently running. DO NOT start remote compilation, dependency installation or
benchmarks before2026-09-20 03:00 America/Chicago (08:00UTC). At that time check
live processes and CPU activity again; wait if busy. Never stop the other job.
A one-time task heartbeat `clemson-1-4-gib` is scheduled for03:00.

## Machine

SSH zcy05331@clnode273.clemson.cloudlab.us
Observed hostname node0.zcy05331-317033.merkle-pg0.clemson.cloudlab.us.
Xeon Platinum8360Y2.40GHz,2sockets,36cores/socket,144threads,251GiB RAM,
2NUMA nodes,AVX512F/DQ/IFMA. CPU0 is onNUMA0; even CPU IDs are node0.
Root disk had57GiB available at initial read-only inspection. This is a fresh
machine, so do not assume old Wisconsin dependency paths exist.
User agrees to a new1GiB/4GiB matrix because hardware differs. Old numbers are
historical context only. Preferred benchmark policy: singleCPU0, memorypreferred
NUMA0 (fallback allowed for larger protocols), captureactualNUMAallocation.

## Frozen sources

This worktree/branch: codex/clemson-273-matrix-20260919.
Core base refactored main2261bbd + retained scan optimizationse83e3d7.
H1 reference: e83e3d736be0453476c7fd759916cbce742b27ff.
H1 tiled query-layout candidate:806c73aa179c52a2e142e26811655b1247cc2213.
Local experiment archive38c3b68: default57/57+CLI pass,128measuredqueriescorrect.
Untiledf87da4c regressed4.8%; tiledprepareimproved but fulltimewasnoisy.
Nativecandidatecomparison is required before promotion.
External six adapters are restored ONLY from committedd4d55bb, never copied
from the dirty path-baselines worktree. Top-L/A3 work remains unfinished and
must not enter this campaign silently.
No remote uploads or remote builds were performed during initial preparation.

## Repetitions and workload

Original logical DB1GiB/4GiB,32B entries,2^24/2^26leaves,seed7,3warmups.
Typical100 measurements→50. Typical16→8: Cuckoo tuned, TreePIR, Flat,
and TreePIR+Spiral. Retain current path semantics and explicit useful-node counts.
Standard OnionPIR is a single-block reference, not a full-path protocol.
Core legacy sibling paths omitroot; Generic/g32 and external adapters includeroot.
Recordserverlatency,stageprofiles,query/responsebytekind,setup,memory,correctness.

## Execution order and remaining preparation

1. Prepare a finite serial runner and an idle gate locally. At/after03:00, upload
   only code/scripts/source manifests (exclude docs,papers,private backups).
2. Build native HEXL1.2.6 and core defaultEP6 + referenceEP3/drop32 + tiledEP3/drop32.
   Full default regressions must pass. EP3 fullsuite has9 known tests assuming
   EP6 defaults; comparefailurelist and run actual h1complete-query gates.
3. h1fixed-layout A/B first: G8/P3/N0=512,b7 at1GiB,b9 at4GiB,expansion10,
   exactKS8 andMLWE. Two reverse-order rounds of25 produce50samples/arm/size.
   Existing scripts/benchmark_gadget_configs.py supports this; numactl on the
   parentrunner propagates CPU/memorypolicy to children.
4. Reprofile Layerwise and Cuckoo tuning on this machine, then measure matrix:
   Generich1(reference and candidate),Generich3 control,Layerwise tuned,Treeg32,
   Cuckoo tuned,TreePIR existing backends,Flat,Standard single-block reference.
   Reuse scripts/run_cloudlab_profile_matrix.sh and existing validators rather
   than inventing incompatible report schemas. Half-countpilotchoices explicit.
5. ExternalSPIRIT/SPIRIT+RC/VBPIR+PBC/InsPIRe/TreePIR+Spiral/Respire:
   build/read adapter READMEs, nativeH8correctness gates beforelargepoints.
   Existing finite six-mode script is available from d4d55bb at
   outputs/path-baselines-20260914/run_native.py, but it is Wisconsin/H24specific:
   adapt paths,NUMA,countpolicy and4GiB support rather than run it unchanged.
   Respire currently dispatches onlyH8/H24; H26 needs a supported public shape
   andcompile/crypto validation. Other adapters' height limits also requirecheck.
   SPIRIT must use its bundled SEAL fork (offline/online APIs), and retainHEXL.
   Spiral must keep the64-byteAVX512alignment fix andquery-independent fullDB.
   VBPIR256/512pilots failedonold125GiBhost: preservefailures; identify real
   error onnewhost rather than label aninvalid point as completed.
6. Preserve allrawJSON/processlogs andgenerate complete/pending/failedmatrix.
   Do not useH8smokes or predicted costs as1/4GiB measurements.

## Upstream pins

TreePIR/VBPIR/Spiral:930063c5aefc441244abb4890fdf35383f3aa956
SPIRIT:7e4c114807d5a83ae5cbad17c514df1dbc32dfc3
Poulpy/InsPIRe:533081a74301c8ba6ddd5e1dfc0c9daa6e3e75ef (thirdparty)
Respire:fd20d38e9e94c0298ab21af2f7699a23729eef67

No periodic status automation was requested. The scheduled start can launch a
finite idle-gatedqueue; notify only meaningful start/failure/completion events.
