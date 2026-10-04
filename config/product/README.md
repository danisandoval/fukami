# Product workload

`bound-workload.txt` is the run configuration of the shipped product (Fukami.app and the Linux package):
the guest's initial interrupt state, the real-time clock policy (read once from the host clock at boot), the initial
controller state, the CD policy, the persistent memory-card storage mode and the cost of each HLE patch point. It
contains no game data; it was compiled from a recorded manifest by the maintainers' tooling
(`manifest_sha256` names that manifest).

`scripts/package_fukami_app.py` and `scripts/package_fukami_linux.py` use this directory when
`local/gate3/product-live-v1` (the developer's own, ignored copy) is absent.
