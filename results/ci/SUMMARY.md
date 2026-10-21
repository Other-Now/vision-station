# vision-station results (runnervmxehtw, 4 CPUs)

Latencies are p50 / p99 / max in ms. Every frame is scored against the truth stamp the rig writes into row 0.

## GVSP receive buffer (100 fps, 640x480 Mono8 = 307 KB/frame, 10 s)

| host net.core.rmem_default | socket buffer | complete frames | incomplete | lost before host (seq gaps) | missing packets | processed fps |
|---|---|---|---|---|---|---|
| 1048576 | Aravis default (kernel default SO_RCVBUF) | 991 | 0 | 0 | 0 | 99.1 |
| 1048576 | socket-buffer=AUTO (sized to payload) | 992 | 0 | 0 | 0 | 99.2 |
| 212992 | Aravis default (kernel default SO_RCVBUF) | 992 | 0 | 0 | 0 | 99.2 |
| 212992 | socket-buffer=AUTO (sized to payload) | 992 | 0 | 0 | 0 | 99.2 |

## Free run: throughput, latency, drops

| run | processed fps | queue drops | incomplete | seq gaps | exposure end -> processed | exposure end -> host rx | balanced |
|---|---|---|---|---|---|---|---|
| 100 fps | 99.2 | 0 | 0 | 0 | 1.58 / 2.00 / 3.40 | 1.52 / 1.95 / 3.29 | 1 |
| 200 fps, 8 ms processing, queue 8 (overload) | 124.2 | 741 | 0 | 0 | 71.39 / 74.09 / 74.29 | 1.48 / 2.15 / 2.59 | 1 |
| 200 fps, 8 ms processing, queue 2 (overload) | 123.6 | 738 | 0 | 0 | 23.26 / 25.93 / 26.04 | 1.86 / 2.72 / 4.19 | 1 |
| 200 fps + 4 busy threads, SCHED_OTHER | 199.2 | 0 | 0 | 0 | 2.71 / 5.70 / 25.09 | 2.65 / 5.65 / 24.04 | 1 |
| 200 fps + 4 busy threads, SCHED_FIFO + pinned | 198.6 | 0 | 0 | 0 | 2.32 / 2.37 / 2.72 | 2.26 / 2.29 / 2.42 | 1 |
| 200 fps + 4 busy threads, SCHED_FIFO, not pinned | 198.8 | 0 | 0 | 0 | 2.32 / 2.36 / 2.69 | 2.27 / 2.30 / 2.40 | 1 |
| 200 fps + 4 busy threads, pinned, SCHED_OTHER | 199.6 | 0 | 0 | 0 | 4.68 / 4.68 / 6.68 | 3.65 / 3.65 / 4.65 | 1 |
| 200 fps + 4 busy threads, root, SCHED_OTHER, UDP socket | 198.2 | 0 | 3 | 3 | 2.71 / 3.86 / 7.72 | 2.65 / 3.70 / 7.66 | 1 |
| 200 fps + 4 busy threads, root, SCHED_OTHER, PF_PACKET ring (Aravis default for root) | 199.2 | 0 | 0 | 0 | 6.02 / 8.72 / 13.07 | 5.98 / 8.67 / 13.00 | 1 |

### Where the transport time goes (p50 / p99 / max ms)

| run | rig readout (render) | rig send -> GVSP leader seen by Aravis | leader -> our pop (all packets + handoff) |
|---|---|---|---|
| 100 fps | 0.35 / 0.45 / 1.02 | 0.06 / 0.07 / 0.14 | 1.12 / 1.55 / 2.61 |
| 200 fps, 8 ms processing, queue 8 (overload) | 0.35 / 0.70 / 0.81 | 0.03 / 0.04 / 1.08 | 1.09 / 1.57 / 1.87 |
| 200 fps, 8 ms processing, queue 2 (overload) | 0.35 / 0.66 / 0.76 | 0.03 / 0.04 / 0.25 | 1.47 / 2.05 / 3.51 |
| 200 fps + 4 busy threads, SCHED_OTHER | 0.70 / 0.71 / 21.93 | 1.24 / 2.32 / 3.99 | 0.71 / 2.64 / 2.98 |
| 200 fps + 4 busy threads, SCHED_FIFO + pinned | 0.70 / 0.71 / 0.87 | 0.02 / 0.04 / 0.10 | 1.54 / 1.57 / 1.64 |
| 200 fps + 4 busy threads, SCHED_FIFO, not pinned | 0.70 / 0.71 / 0.85 | 0.02 / 0.04 / 0.08 | 1.54 / 1.59 / 1.67 |
| 200 fps + 4 busy threads, pinned, SCHED_OTHER | 0.70 / 0.71 / 0.72 | 0.97 / 0.99 / 2.97 | 1.98 / 1.99 / 2.94 |
| 200 fps + 4 busy threads, root, SCHED_OTHER, UDP socket | 0.70 / 0.73 / 0.86 | 1.25 / 2.32 / 6.27 | 0.70 / 1.06 / 5.97 |
| 200 fps + 4 busy threads, root, SCHED_OTHER, PF_PACKET ring (Aravis default for root) | 0.70 / 0.71 / 0.78 | 0.27 / 2.95 / 3.17 | 5.01 / 7.70 / 9.34 |

## Inspection cycles (move -> settle -> trigger -> acquire -> measure)

| run | ok / failed / recovered | edge err max (px) | width err sd (um) | part position err sd / max (um) | exposed while not in position | cycle time p50 (ms) | trigger -> exposure p50 / p99 (ms) |
|---|---|---|---|---|---|---|---|
| settle: wait for controller DONE | 100 / 0 / 0 | 0.0107 | 0.068 | 1.63 / 2.02 | 0 | 125.0 | 0.56 / 1.05 |
| settle: fixed 20 ms | 100 / 0 / 0 | 0.0143 | 0.072 | 9.33 / 16.51 | 87 | 85.0 | 0.46 / 1.05 |
| settle: none (trigger on ARRIVED) | 100 / 0 / 0 | 0.0281 | 0.058 | 12.90 / 25.69 | 99 | 65.0 | 0.52 / 1.04 |
| controller, parabolic peak fit | 100 / 0 / 0 | 0.0444 | 0.540 | 1.65 / 2.17 | 0 | 118.0 | 0.60 / 1.05 |
| controller, integer-pixel edges | 100 / 0 / 0 | 0.5018 | 4.964 | 2.26 / 4.54 | 0 | 118.0 | 0.42 / 1.01 |
| controller, 3 injected stage faults | 27 / 3 / 3 | 0.0100 | 0.077 | 1.52 / 2.06 | 0 | 98.7 | 0.29 / 23.68 |

## Shared-memory recorder (200 fps for 8 s)

| recorder | station published | recorder read | overrun-skipped | torn retries | publish -> read p50 / p99 (ms) |
|---|---|---|---|---|---|
| keeps up | 1583 | 1583 | 0 | 0 | 0.116 / 0.364 |
| 10 ms per frame (too slow) | 1591 | 852 | 739 | 0 | 314.993 / 585.333 |

## Calibration, k3 fixed at 0 (default) (synthetic checkerboards; truth k1 -0.21, k2 0.09, k3 0)

- board found in 20/20 views; fit on 15, held out 5
- RMS reprojection error: train 0.0756 px, held-out 0.0707 px
- fx -0.033%, fy -0.043%, cx +1.11 px, cy -0.16 px vs truth
- fitted k1 -0.2087, k2 0.0762, k3 0.0000
- detected corners vs true projections: mean 0.0623 px, max 0.2873 px

## Calibration, k3 free (synthetic checkerboards; truth k1 -0.21, k2 0.09, k3 0)

- board found in 20/20 views; fit on 15, held out 5
- RMS reprojection error: train 0.0755 px, held-out 0.0705 px
- fx -0.040%, fy -0.050%, cx +1.04 px, cy -0.12 px vs truth
- fitted k1 -0.2024, k2 -0.0473, k3 0.6458
- detected corners vs true projections: mean 0.0623 px, max 0.2873 px
