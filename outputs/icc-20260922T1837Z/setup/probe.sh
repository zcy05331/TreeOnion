#!/usr/bin/env bash
# Light, read-only probe on the login node: no builds, no benchmarks here.
echo "== host"; hostname; head -2 /etc/os-release
echo "== login cpu"; lscpu | grep -E "Model name|^CPU\(s\)|NUMA node\(s\)|Socket"; grep -o -w 'avx512f\|avx512dq\|avx512ifma\|avx2' /proc/cpuinfo | sort -u | tr '\n' ' '; echo
free -g | head -2
echo "== associations (account|partition|qos)"; sacctmgr -nP show assoc user=$USER format=account,partition,qos 2>&1 | head -40
echo "== partitions (name avail timelimit nodes cpus mem features)"; sinfo -h -o "%P|%a|%l|%D|%c|%m|%f" 2>&1 | sort -u | head -80
echo "== partition access"; scontrol show partition 2>&1 | grep -E "^PartitionName|MaxTime|AllowAccounts|AllowQos|DefaultTime" | paste - - - - - 2>/dev/null | head -60
echo "== my jobs"; squeue -u $USER 2>&1 | head
echo "== tools"; for t in gcc g++ cmake python3 numactl taskset git; do printf '%s: ' $t; command -v $t || echo missing; done
gcc --version 2>&1 | head -1; cmake --version 2>&1 | head -1; python3 --version 2>&1
echo "== modules"; (module -t avail 2>&1 || true) | grep -i -E "^(gcc|cmake|python|numactl|intel|git|anaconda)" | sort -u | head -60
echo "== storage"; echo HOME=$HOME; df -h $HOME 2>/dev/null | tail -1; for d in /scratch/users/$USER /scratch/$USER /projects /scratch; do [ -e $d ] && { echo "exists: $d"; df -h $d 2>/dev/null | tail -1; }; done
(quota -s 2>/dev/null || true) | head -8
echo "== internet from login"; curl -s -o /dev/null -w "%{http_code}\n" --max-time 10 https://github.com
