Fixed raw-frame pad scripts for the Retire title routes (`--buttons`, `--directions`; frame then hex mask).
restore: Start (0x0800) at 809, confirm (0x0040) at 915 — the default selection is Restore.
newgame: Start at 809, Left (0x8000, held 905..906) selects New Game, confirm at 930.
Frames are native frames; storage readiness is asynchronous, so title-relative timing varies by a few
frames between runs. Both scripts press only after the menu accepts input in every observed run.
