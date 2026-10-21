# Local results: WSL2 Ubuntu 24.04 on a laptop (4 vCPUs, shared with other builds)

Only the GVSP receive-buffer comparison is quoted from this machine. Latency numbers here are
contaminated by a concurrent LLVM build (load average 4.12, 4.47, 4.58), so use results/ci for latency.

net.core.rmem_default = 212992, rmem_max = 4194304. One 640x480 Mono8 frame is 307,200 B, which is
bigger than the default receive buffer.

## GVSP receive buffer, 100 fps target, 10 s per run

| run | SO_RCVBUF | complete frames | incomplete | lost before host (seq gaps) | processed fps |
|---|---|---|---|---|---|
| gvsp_auto_buffer_run1 | AUTO | 751 | 4 | 4 | 75.1 |
| gvsp_auto_buffer_run2 | AUTO | 818 | 0 | 0 | 81.8 |
| gvsp_default_buffer_run1 | Aravis default | 297 | 438 | 434 | 29.7 |
| gvsp_default_buffer_run2 | Aravis default | 19 | 119 | 117 | 1.9 |

The first spike (20 software-triggered 512x512 frames) showed the same thing. At the default, 9 of 20
frames were complete and 724 packets were missing. After `socket-buffer=AUTO`, 20 of 20 were complete
with 0 missing.

On the CI runner the same comparison loses nothing either way, even with rmem_default forced to the
stock 212,992 B (see results/ci). Loss needs two things at once: a buffer smaller than a frame, and a
receiver that falls behind during the packet burst. Sizing the buffer to the frame removes the
dependence on receiver scheduling.
