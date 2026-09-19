# Respire Upstream Provenance

The Respire path adapter stages upstream Respire from:

- Repository: `https://github.com/AMACB/respire`
- Commit: `fd20d38e9e94c0298ab21af2f7699a23729eef67`
- Upstream license: the staged checkout includes Respire's original `LICENSE`.

Tracked files in this directory are limited to the path workload adapter modules, the timing-boundary patch, wrapper/staging scripts, and documentation. The upstream source tree is recreated under `.benchmarks/respire-path/respire`; staging verifies that the checkout is at the exact upstream commit before the adapter reports that commit in JSON output.
