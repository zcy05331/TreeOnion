# Scheduled Clemson matrix

## Latest algorithm contract (2026-09-20)

The final primary algorithm fixes h=1 and restores the OLD (gamma,alpha,beta)
indexing, with variable power-of-two g and plane_count P>=1. Tree g32 is a
parameter point, not a fixed-g restriction. Choose P=ceil(entry_bits/(c*g)),
c=floor(log2(t)); rho=n/g. Every plane uses node=p+u*R_level and coefficient
u+digit*rho, sharing the same alpha/beta/gamma query. Do not substitute Generic
G32/P1: Generic assigns nodes to polynomials differently. Do not schedule the
old Generic G8/P3 point as the new primary merely because h=1 matches.

The new old-layout multi-plane implementation is now in src/tree_multiplane_*
with test entries tree_multiplane and tree_multiplane_bench. Local default
58/58 C++ regressions and the full CLI chain passed. EP3/drop32/exactKS8 passed
the new crypto suite and14 configurations/34 measured queries plus14 warmups
across16/32/48/64/128B, Projection/MLWE and both query-buffer layouts. Details
and raw artifacts are in multiplane-local/. These are small Rosetta correctness
and profiling-contract checks; native1/4GiB tuning and measurement are pending.
Select g/P and feasible N0/b by complete measured server time on this machine.
Keep EP3/drop32/exactKS8 and MLWE as separately validated computational options.
Do not apply previous Generic speed claims to the restored layout. Original
Tree g32 EP6/Projection stays an unchanged baseline; h3 is historical only.

Earlier immediate-run override: run ONLY the Standard OnionPIRv2 1GiB speed baseline
immediately. It has been submitted under
`/users/zcy05331/standard-onion-1gb-20260920T0132Z`, guarded by idle checks.
The other project processes15294/20293 were stopped(T) with zero CPU-tick growth
when inspected; do not resume or terminate them. Check metadata/status and reuse
the validated50-trial result at the scheduled matrix start; never submit a duplicate.

The rest of the matrix remains scheduled for2026-09-20 06:00America/Chicago
(11:00UTC). At that time check live processes again and wait if busy. The one-time
heartbeat `clemson-1-4-gib` was updated with the immediateStandard run location.

## Standard baseline completed

The immediate Standard1GiB run completed:50/50correct,906.011ms mean,
1130.230MiB/s original-data throughput,11264Bactualresponsepayload.
Native57/57regressions passed. Resultsidecars are in `standard-now/`;
remote metadata/status is `complete`. At06:00validate/reuse it, then proceed
to the new old-layout h1 validation/tuning and the remainingmatrix; do not rerun it solely because step2below
describes the initial command.

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
Initial preparation was local-only. The subsequent explicit "run now" request
authorized the separate Standard upload/build/run described above.

## Repetitions and workload

Original logical DB1GiB/4GiB,32B entries,2^24/2^26leaves,seed7,3warmups.
Typical100 measurements→50. Typical16→8: Cuckoo tuned, TreePIR, Flat,
and TreePIR+Spiral. Retain current path semantics and explicit useful-node counts.
Standard OnionPIR is a single-block reference, not a full-path protocol.
Core legacy sibling paths omitroot; Generic/g32 and external adapters includeroot.
Recordserverlatency,stageprofiles,query/responsebytekind,setup,memory,correctness.

## Execution order and remaining preparation

1. Prepare a finite serial runner and an idle gate locally. At/after06:00, upload
   only code/scripts/source manifests (exclude docs,papers,private backups).
2. FIRST PRIORITY per latest user instruction: build native HEXL1.2.6 and the
   defaultEP6 core, verify correctness, then run Standard OnionPIRv2 1GiB
   before h1 tuning or external-backend preparation. Use50 measured queries,
   3warmups,seed7. Preserve rawJSON,log,process exit andpeakRSS; validate and
   report this speed baseline promptly. This changes order, not the06:00/idle
   restriction. Exact case invocation after the time and idle checks:

   ```sh
   python3 scripts/read_process_peak_memory.py --log "$OUT/standard.log" \
     --output "$OUT/standard-process.json" -- \
     numactl --physcpubind=0 --preferred=0 "$DEFAULT_BIN" \
     --test merkle_benchmarks --benchmark-case standard_onionpir \
     --leaf-count 16777216 --experiments 50 --warmup 3 --trial-seed 7 \
     --benchmark-json "$OUT/standard.json"
   python3 scripts/summarize_cloudlab_profiles.py \
     --validate "$OUT/standard.json" --trials 50 --leaf-count 16777216
   ```

   OUT is a fresh1GiB output directory. Report fullserverlatency,logicalMiB/s,
   stageprofile,query/responsebytekind and50/50correctness. Standard retrieves
   one3KiB plaintext block, so it is a speed reference rather than pathworkload.
3. Rebuild and validate old-layout h1 multi-plane support on the native host
   before scheduling the primary point. Local P=1 legacy equivalence, P=2/P=3
   complete decryption, EP3/drop32/exactKS8 and MLWE checks passed as recorded
   above. Native actual-query gates remain required; the EP3 fullsuite has9 known legacy tests
   assuming EP6 defaults, so compare failure identities rather than ignore errors.
   Tune g/P and N0/b on this machine, then run50 primary measurements per size.
   Use scripts/benchmark_tree_multiplane.py with explicit g/N0 candidates;
   it invokes tree_multiplane_bench, validates correct full responses and matched
   query schedules, and ranks complete points by mean full server time.
   Any prepared-query A/B must use the SAME new old-layout parameters; the
   previously planned Generic G8/P3/N0=512 A/B is superseded, not evidence for it.
4. Reprofile Layerwise and Cuckoo tuning on this machine, then measure matrix:
   old-layout h1(variable g/P),Layerwise tuned,original Treeg32,
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
