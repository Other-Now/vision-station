#!/usr/bin/env python3
"""Turns the JSON files from bench_all.sh into results/<env>/SUMMARY.md."""
import json, os, platform, sys

o = sys.argv[1]


def j(name):
    p = os.path.join(o, name + ".json")
    return json.load(open(p)) if os.path.exists(p) else None


def lat(d, k="lat_e2e_ms"):
    x = d[k]
    return f"{x['p50']:.2f} / {x['p99']:.2f} / {x['max']:.2f}"


print(f"# vision-station results ({platform.node()}, {os.cpu_count()} CPUs)\n")
print("Latencies are p50 / p99 / max in ms. Every frame is scored against the truth stamp the rig writes into row 0.\n")

print("## GVSP receive buffer (100 fps, 640x480 Mono8 = 307 KB/frame, 10 s)\n")
print("| host net.core.rmem_default | socket buffer | complete frames | incomplete | lost before host (seq gaps) | missing packets | processed fps |")
print("|---|---|---|---|---|---|---|")
for name, label in [("gvsp_default_buffer", "Aravis default (kernel default SO_RCVBUF)"), ("gvsp_auto_buffer", "socket-buffer=AUTO (sized to payload)"),
                    ("gvsp_default_buffer_stock", "Aravis default (kernel default SO_RCVBUF)"), ("gvsp_auto_buffer_stock", "socket-buffer=AUTO (sized to payload)")]:
    d = j(name)
    if d:
        print(f"| {d.get('rmem_default', '?')} | {label} | {d['rx_complete']} | {d['rx_incomplete']} | {d['seq_gap']} | {d['missing_packets']} | {d['fps']:.1f} |")

print("\n## Free run: throughput, latency, drops\n")
print("| run | processed fps | queue drops | incomplete | seq gaps | exposure end -> processed | exposure end -> host rx | balanced |")
print("|---|---|---|---|---|---|---|---|")
runs = [("gvsp_auto_buffer", "100 fps"), ("overload", "200 fps, 8 ms processing, queue 8 (overload)"),
        ("overload_q2", "200 fps, 8 ms processing, queue 2 (overload)"),
        ("sched_normal_stress", "200 fps + 4 busy threads, SCHED_OTHER"),
        ("sched_fifo_stress", "200 fps + 4 busy threads, SCHED_FIFO + pinned"),
        ("sched_fifoonly_stress", "200 fps + 4 busy threads, SCHED_FIFO, not pinned"),
        ("sched_pinonly_stress", "200 fps + 4 busy threads, pinned, SCHED_OTHER")]
for name, label in runs:
    d = j(name)
    if d:
        print(f"| {label} | {d['fps']:.1f} | {d['queue_drops']} | {d['rx_incomplete']} | {d['seq_gap']} | "
              f"{lat(d)} | {lat(d, 'lat_transport_ms')} | {d['balanced']} |")

print("\n### Where the transport time goes (p50 / p99 / max ms)\n")
print("| run | rig readout (render) | rig send -> GVSP leader seen by Aravis | leader -> our pop (all packets + handoff) |")
print("|---|---|---|---|")
for name, label in runs:
    d = j(name)
    if d and "lat_wire_ms" in d:
        print(f"| {label} | {lat(d, 'lat_readout_ms')} | {lat(d, 'lat_wire_ms')} | {lat(d, 'lat_assembly_ms')} |")

print("\n## Inspection cycles (move -> settle -> trigger -> acquire -> measure)\n")
print("| run | ok / failed / recovered | edge err max (px) | width err sd (um) | part position err sd / max (um) | exposed while not in position | cycle time p50 (ms) | trigger -> exposure p50 / p99 (ms) |")
print("|---|---|---|---|---|---|---|---|")
for name, label in [("inspect_controller", "settle: wait for controller DONE"), ("inspect_fixed20", "settle: fixed 20 ms"),
                    ("inspect_none", "settle: none (trigger on ARRIVED)"), ("inspect_parabolic", "controller, parabolic peak fit"),
                    ("inspect_integer", "controller, integer-pixel edges"), ("inspect_faults", "controller, 3 injected stage faults")]:
    d = j(name)
    if d:
        print(f"| {label} | {d['cycles_ok']} / {d['cycles_failed']} / {d['recoveries']} | {d['edge_err_px']['max_abs']:.4f} | "
              f"{d['width_err_um']['sd']:.3f} | {d['pos_err_um']['sd']:.2f} / {d['pos_err_um']['max_abs']:.2f} | "
              f"{d['not_in_position']} | {d['lat_cycle_ms']['p50']:.1f} | {d['lat_trigger_ms']['p50']:.2f} / {d['lat_trigger_ms']['p99']:.2f} |")

print("\n## Shared-memory recorder (200 fps for 8 s)\n")
print("| recorder | station published | recorder read | overrun-skipped | torn retries | publish -> read p50 / p99 (ms) |")
print("|---|---|---|---|---|---|")
for name, label in [("recorder_fast", "keeps up"), ("recorder_slow", "10 ms per frame (too slow)")]:
    r, s = j(name), j(name + "_station")
    if r and s:
        print(f"| {label} | {s['shm_published']} | {r['read']} | {r['overrun_skipped']} | {r['torn']} | {r['lat_p50_ms']:.3f} / {r['lat_p99_ms']:.3f} |")

for cname, ctitle in [("calibration", "k3 fixed at 0 (default)"), ("calibration_free_k3", "k3 free")]:
    c = j(cname)
    if c:
        print(f"\n## Calibration, {ctitle} (synthetic checkerboards; truth k1 -0.21, k2 0.09, k3 0)\n")
        print(f"- board found in {c['detected']}/{c['views']} views; fit on {c['train']}, held out {c['holdout']}")
        print(f"- RMS reprojection error: train {c['rms_train_px']:.4f} px, held-out {c['rms_holdout_px']:.4f} px")
        print(f"- fx {c['fx_err_pct']:+.3f}%, fy {c['fy_err_pct']:+.3f}%, cx {c['cx_err_px']:+.2f} px, cy {c['cy_err_px']:+.2f} px vs truth")
        print(f"- fitted k1 {c['k1']:.4f}, k2 {c['k2']:.4f}, k3 {c['k3']:.4f}")
        print(f"- detected corners vs true projections: mean {c['corner_err_mean_px']:.4f} px, max {c['corner_err_max_px']:.4f} px")
