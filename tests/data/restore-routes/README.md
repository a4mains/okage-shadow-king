Fixed raw-frame pad scripts (`--buttons`, frame then hex mask) for the Restore routes, with an empty save directory.
restore-cancel: Start 809, confirm Restore 915, Cancel (0x0020) 1210 on the ready empty-slot UI.
pending-cancel: Start 809, confirm 915, Cancel 1070 while the storage check is still pending; run with the
research-only `--restore-storage-latency-ms 3000` (a lifetime regression, never a parity run).
