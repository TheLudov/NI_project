# Tests for the replica-selection part (ring)

Responsible: Atieh.

Not part of the submission; keep them in scripts/ or outside the archive.

- `test_ring.c` — unit tests, linked without libgen (a fake gen_alloc that can be told to fail):
  lookup vs a brute-force reference (ties, 0, UINT32_MAX), random event sequences (incremental ring ==
  full rebuild == the copy's ring, never a non-active replica, ids), allocation failures, balance and
  movement for v = 1/10/100/1000 with the LB's CRC32C hash, lookup and event timings.

      gcc -std=c11 -O1 -g -msse4.2 -DHAVE_IDS -fsanitize=address,undefined -Isrc -Igen \
          tests_ring/test_ring.c src/ring.c -o /tmp/test_ring && /tmp/test_ring        # correctness
      gcc -std=c11 -O2 -msse4.2 -DHAVE_IDS -Isrc -Igen tests_ring/test_ring.c src/ring.c -o /tmp/test_ring \
          && taskset -c 1 /tmp/test_ring                                                  # E3 numbers

- `e2e_main.c` — a test-only LB (stand-in flow table + IP-in-IP) that drives the ring through the real
  harness. Copy it over lb/main.c in a SCRATCH copy of the project, `make`, then `tests/run.sh build`.
  Params: ring_v, drain_aware=1, syn_rule=0.
- `leave_during_drain.scn`, `leave_after_scaleout.scn` — LB churn while replicas drain / after a scale-out.
