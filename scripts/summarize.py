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

print("## GVSP receive buffer (100 fps, 640x480 Mono8, 10 s)\n")
print("| socket buffer | complete frames | incomplete | lost before host (seq gaps) | processed fps |")
print("|---|---|---|---|---|")
for name, label in [("gvsp_default_buffer", "Aravis default (kernel default SO_RCVBUF)"), ("gvsp_auto_buffer", "socket-buffer=AUTO (sized to payload)")]:
    d = j(name)
    if d:
        print(f"| {label} | {d['rx_complete']} | {d['rx_incomplete']} | {d['seq_gap']} | {d['fps']:.1f} |")

print("\n## Free run: throughput, latency, drops\n")
print("| run | processed fps | queue drops | incomplete | seq gaps | exposure end -> processed | exposure end -> host rx | balanced |")
print("|---|---|---|---|---|---|---|---|")
for name, label in [("gvsp_auto_buffer", "100 fps"), ("overload", "200 fps, 8 ms processing (overload)"),
                    ("sched_normal_stress", "200 fps + 4 busy threads, SCHED_OTHER"),
                    ("sched_fifo_stress", "200 fps + 4 busy threads, SCHED_FIFO + pinned")]:
    d = j(name)
    if d:
        print(f"| {label} | {d['fps']:.1f} | {d['queue_drops']} | {d['rx_incomplete']} | {d['seq_gap']} | "
              f"{lat(d)} | {lat(d, 'lat_transport_ms')} | {d['balanced']} |")

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

c = j("calibration")
if c:
    print("\n## Calibration (synthetic checkerboards, known intrinsics)\n")
    print(f"- board found in {c['detected']}/{c['views']} views; fit on {c['train']}, held out {c['holdout']}")
    print(f"- RMS reprojection error: train {c['rms_train_px']:.4f} px, held-out {c['rms_holdout_px']:.4f} px")
    print(f"- fx {c['fx_err_pct']:+.3f}%, fy {c['fy_err_pct']:+.3f}%, cx {c['cx_err_px']:+.2f} px, cy {c['cy_err_px']:+.2f} px vs truth")
    print(f"- detected corners vs true projections: mean {c['corner_err_mean_px']:.4f} px, max {c['corner_err_max_px']:.4f} px")
