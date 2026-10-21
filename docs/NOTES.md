# vision-station: interview study sheet

Read this the day before. Each section is "what it does, why it is built that way, the question you will get".

## 1. The 60-second pitch

A Qt 6 C++ station that drives a GenICam camera over GigE Vision (Aravis) and a motion stage over TCP. One
inspection cycle is: move the stage, wait until the controller says it has settled, software-trigger the
camera, receive the frame over GVSP, measure the part's edges to sub-pixel accuracy, show the result. There's no real hardware.
The camera is Aravis's fake GigE Vision camera, which speaks the real GVCP/GVSP protocols over UDP. I replaced
its image generator so each frame shows a part where the simulated stage currently puts it, with the ground
truth written into row 0. That's what lets every measurement be scored.

## 2. Process and thread map

```
vs_rig (process)                       vs_station / vs_bench (process)                  vs_recorder (process)
  fake GigE camera  --GVCP/GVSP UDP-->   [Aravis GVSP rx thread]                           reads POSIX shm ring
  stage controller  <--TCP lines-----    [acquisition + sequencer thread]                  counts overruns
  (StageModel)                              | BoundedQueue (drop-newest, counted)
                                         [processing thread] --shm ring (overwrite)-->
                                            | coalesced queued signal
                                         [Qt GUI thread]
```

- **Why the sequencer is in the acquisition thread:** move, settle, trigger and pop are sequential and all of
  them block on I/O. Processing stays on its own thread, so measurement never delays the next trigger and the
  GUI never delays either of them.
- **Why the queue drops instead of blocking:** if the acquisition thread blocked, Aravis would run out of free
  buffers (`n_underruns`) or the socket would overflow. Those losses are silent. A drop at the queue is counted,
  and `rx_complete == processed + queue_drops` is checked at the end of every run.
- **The GUI path:** the processing thread posts at most one queued call. A flag is set until the GUI has taken
  `latest()`. If the GUI is slow, results are coalesced (counted) rather than piling up in Qt's event queue,
  which is the usual way a Qt app "freezes" under load. The GUI never holds a lock the processing thread needs
  for long. It copies a `shared_ptr` and paints from an immutable buffer.
- **Camera control from the GUI** (exposure, gain) goes straight to GVCP on the GUI thread, behind a mutex
  that it shares only with `software_trigger()`, not with frame reception.

## 3. GigE Vision facts to be ready for

- **GVCP:** UDP port 3956, request/ack, register reads and writes. The GenICam XML (the feature tree) is
  downloaded from the device. A client takes *control channel privilege* and keeps it alive with heartbeats.
- **GVSP:** a UDP stream of leader + payload packets + trailer per frame (block id = frame id). A lost packet
  means either a resend request (if the device supports it) or an incomplete buffer.
- **GenICam / SFNC:** features are nodes (Float, Integer, Enumeration, Command). The SFNC names are
  `ExposureTime` and `Gain`, but the fake camera exposes `ExposureTimeAbs` and `GainRaw`.
  `AravisCamera::resolve()` checks the node map and falls back, and `vs_bench features` prints what was
  actually found. Never assume a feature exists.
- **Software trigger:** `TriggerSelector=FrameStart`, `TriggerMode=On`, `TriggerSource=Software`, then
  execute the `TriggerSoftware` command node.

## 4. The findings (the interview stories)

1. **GVSP frame loss came from a default, and needed two conditions.** By default Aravis doesn't set
   SO_RCVBUF (`socket-buffer=FIXED`, size 0), so the kernel's `rmem_default` applies. On WSL2 that's
   208 KB, smaller than one 300 KB frame, and the default lost 438/735 and 119/138 frames. AUTO lost 4/755
   and 0/818. The fake camera doesn't implement packet resend, so a lost packet means a lost frame. On
   the CI runner, even with rmem_default forced to 208 KB, neither setting lost anything. Loss needs a small
   buffer **and** a receiver that falls behind during the burst. Sizing the buffer to the frame
   removes the dependence on how quickly the receiver gets scheduled.
2. **Settling is what makes the measurement right** (CI, 100 cycles each). With the controller DONE
   handshake, the part position error was sd 1.63 µm and max 2.02 µm, matching the ±2 µm in-position
   tolerance. A fixed 20 ms delay gave a max of 16.5 µm, with 87/100 frames exposed while the stage was
   out of position. No settle at all gave 25.7 µm and 99/100. The cost: a 125 ms cycle against 65 ms.
3. **The in-position window has to be longer than the ringing's dwell near zero.** The controller says DONE
   once it has been inside ±2 µm for a fixed window. With a 3 ms window and 60 Hz ringing, the stage spends
   more than 3 ms inside the band around each zero crossing even while its amplitude is still 2.66 µm. So
   DONE fired, and the next swing went to 2.37 µm, outside tolerance. A 5 ms window fixes it for about 9 ms
   of extra settle time. The test is `StageModel.ShortInPositionWindowDeclaresDoneTooEarly`.
4. **The fake camera's custom-image hook can't be used through the public API.** The fill callback must
   set the private `buffer->priv->received_size`, or the camera sends a zero-length payload, and there's no
   setter. `apps/rig` mirrors the head of `ArvBufferPrivate` from 0.8.30. It fails the build on another
   minor version, and at startup it checks the mirror against public getters (`layout_ok()`).
5. **Trigger latency on the fake camera is a polling artefact.** In trigger mode, the fake camera checks for
   a pending software trigger only once per `1/trigger_frequency`, on wall-clock-aligned slots (default
   25 Hz, so up to 40 ms). The rig sets it to 1 kHz. The trigger-to-exposure numbers measure that loop plus
   GVCP, not camera hardware. Say this before anyone asks.
6. **My first rig was not causal.** It rendered a frame at fill time and sampled the stage *backwards* over
   the exposure, so a 10 ms exposure "started" before the trigger. The giveaway was that 18 of 20 "settled"
   frames were flagged as exposed while not in position. Now the exposure starts when the trigger is seen,
   the rig sleeps for the exposure, and the frame can't leave earlier.

## 5. Sub-pixel edge measurement

- Average the rows of a band into a 1-D profile (noise drops by √rows), take the central-difference
  gradient, find the strongest rising and falling edges, and refine each peak with a 3-point fit.
- **Integer:** up to 0.5 px error. **Parabolic:** sub-pixel, but with a position-dependent bias.
  **Gaussian (parabola through ln g):** exact when the gradient is Gaussian-shaped, which a step blurred
  by a Gaussian PSF is. Unit tests sweep the edge across 2 px in 0.05 px steps against the renderer's exact
  truth.
- **The renderer is exact:** an ideal step convolved with N(0,σ) and integrated over each pixel has a closed
  form, σ·[G((i+1−e)/σ) − G((i−e)/σ)] with G(u) = uΦ(u) + φ(u). So the truth is known to float precision.

## 6. Calibration

- Zhang's method via `cv::calibrateCamera`: 9x6 inner corners, `findChessboardCorners` + `cornerSubPix`.
- Synthetic views are rendered by **inverse mapping**: for each (3x3 supersampled) pixel, undistort to a ray,
  intersect it with the board plane, and look up the checker colour. So the true intrinsics, distortion and
  corner positions are all known.
- Report two errors. The *training* RMS is optimistic, because it was fit on those same points. The
  *held-out* RMS uses PnP on unseen views with the fitted intrinsics. Also compare detected corners against
  the true projections.

## 7. Real-time knobs, and the experiment that was wrong twice

- `--rt`: SCHED_FIFO priority 80, with the GVSP receive thread pinned to CPU 1, acquisition to CPU 2 and
  processing to CPU 3. The Aravis receive thread is reached through the stream's
  `ARV_STREAM_CALLBACK_TYPE_INIT` callback, which runs *on* that thread.
- **Final result** (CI, 200 fps + 4 busy threads, camera simulator on its own CPU at FIFO 90):

  | Station threads | p50 | p99 | max |
  |---|---|---|---|
  | SCHED_OTHER | 2.71 ms | 5.70 ms | 25.1 ms |
  | FIFO + pinned | 2.32 ms | 2.37 ms | 2.72 ms |
  | FIFO, not pinned | 2.32 ms | 2.36 ms | 2.69 ms |
  | Pinned only (SCHED_OTHER) | 4.68 ms | 4.68 ms | 6.68 ms |

  FIFO does the work, and pinning adds nothing. Pinning without priority is *worse*, because a pinned
  thread can't migrate off a CPU a busy thread is using.
- **Confound 1:** the simulated camera shared CPUs with the stress threads. The sender was the starved
  thread, and no station setting can help it. In that run FIFO only moved about 2.4 ms from
  "packet assembly" to "wire" without lowering the total. The latency split
  (readout / send→leader / leader→pop) showed this. Fix: give the camera its own CPU, as hardware would have.
- **Confound 2:** every FIFO run used sudo, and as root Aravis silently switches to a PF_PACKET
  TPACKET_V3 ring with `tp_retire_blk_tov = 5` ms. Packets reach userspace in 5 ms blocks, so latency
  piled up at about 5 ms (p50 5.0, p99 5.0). A hypothesis I tested and rejected along the way: ksoftirqd
  starvation. Raising ksoftirqd to FIFO 95 changed nothing (5.58 → 5.59 ms). Fix: the packet socket is
  off by default. Measured on its own (root, SCHED_OTHER): 6.02 ms p50 with the ring vs 2.71 ms with
  the UDP socket.
- The lesson to say out loud: split the latency into stages before tuning, and check that the "treatment"
  is the only thing that differs between runs. Running as root differed too.

## 8. Shared-memory recorder (IPC)

- POSIX `shm_open` + `mmap`. An SPSC ring where each slot has a seqlock: odd while writing, `2*index+2`
  when complete, so the value says *which* frame the slot holds. The writer never waits. The reader detects
  being lapped (overrun) and torn copies (retry).
- The test hammers it with a writer lapping a slow reader and checks that no read reported Ok is torn.
- Caveat to state: a seqlock's racy `memcpy` is technically a data race in the C++ memory model. It's the
  standard pattern, and it's correct on x86 with the fences used, but TSan will flag it.

## 9. Error recovery

- Every stage exchange has a deadline. A socket timeout *drops the connection*, because a late reply
  would desynchronise request/response pairing.
- Faults are injected through the protocol: `FAULT stall` (the next move never arrives) and `FAULT drop` (the
  controller closes the socket). Recovery: reconnect, RESET (hold position), re-read POS, fire `Reset`, then
  continue the plan. The failed cycle is counted, not retried.
- `tests/test_uml_sync.cpp` parses `docs/uml/state_station.puml` and fails if the drawn transitions differ
  from the C++ table. That's what "the UML can't go stale" means here.

## 10. What not to claim

- No real camera, no real stage. The camera *protocol* is real (GVCP/GVSP over UDP on loopback). The scene
  and the stage dynamics are simulated, which is why the ground truth exists.
- Loopback, not a NIC, so no jumbo-frame or interrupt-coalescing story. On a real link, packet loss also
  depends on the NIC ring size and coalescing.
- Calibration is validated on synthetic images, and has never been run on a real camera.
- 1-axis stage. Mono8 only in the rig. The Windows build is CI-only: it builds and runs the unit tests, but
  the rig and recorder are Linux-only.
- Local latency numbers are from a loaded laptop VM. Quote the CI numbers (results/ci, run 39585956011).
- The RT gain was measured on a shared 4 vCPU cloud VM, not an isolated core (no isolcpus, no PREEMPT_RT).
