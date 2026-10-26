#!/usr/bin/env python3
"""Compare opposed-style estimates on synthetic clipping of bracketed raw pairs."""

import os

for _name in (
    "OPENBLAS_NUM_THREADS",
    "OMP_NUM_THREADS",
    "MKL_NUM_THREADS",
    "NUMEXPR_NUM_THREADS",
    "VECLIB_MAXIMUM_THREADS",
):
    os.environ[_name] = "1"

import argparse
import json
import pathlib
import sys
import time
import traceback
import warnings
from functools import lru_cache

sys.dont_write_bytecode = True

import cv2
import numpy as np
import rawpy
from scipy import ndimage

HERE = pathlib.Path(__file__).resolve().parent
RUNS = pathlib.Path("/tmp/hl-rules-study/runs")
INCLUDED = pathlib.Path("/tmp/hl-rules-study/scratch/included.txt")
CAMDATA_DIR = pathlib.Path("/tmp/hl-rules-study/scripts")
PREDICT_JSON = pathlib.Path("/tmp/hl-rules-study/tasks/2026-10-24-lreg-offline/predict.json")
warnings.filterwarnings("ignore", message='"Matplotlib" related API features are not available')
sys.path.insert(0, str(CAMDATA_DIR))
import camdata  # noqa: E402

camdata.model = lru_cache(maxsize=8)(camdata.model)
cv2.setNumThreads(1)

CLIP_FRACTION = 0.98
DARKER_FRAME_EXCLUSION = 0.98
DOWNSAMPLE_TRIGGER_QUADS = 3_000_000
DOWNSAMPLE_TRIGGER_CLIPPED_QUADS = 200_000
LOCAL_RADIUS = 3.0
LOCAL_MIN_SAMPLES = 30
# ring3.py: ring2.py with per-region offsets combined with the iteration; ring2.py is ring.py with the iteration restricted to partly clipped quads (fully clipped quads keep
# the first estimate: with all three channels clipped, every pass adds the offsets again and the
# estimate grows without bound), at several pass counts, to check convergence
PREDICTORS = (
    "OPP",
    "ITERP-10",
    "CBRT-ITERP-10",
    "LOCAL-ITERP-10",
    "CBRT-LOCAL-ITERP-10",
    "CBRT-LOCAL",
)
CLASSES = ("partial", "one", "two", "two_rg", "two_rb", "two_gb", "full")
REGION_CONNECTIVITY = np.ones((3, 3), dtype=bool)


def json_write_atomic(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def scene_kind(ref_path):
    parts = pathlib.Path(ref_path).parts
    try:
        return parts[parts.index("bracketed") + 1]
    except (ValueError, IndexError):
        return "unknown"


def bayer_pattern(path):
    """Return a reason for rejecting a raw unless its visible CFA is a Bayer 2x2."""
    with rawpy.imread(str(path)) as raw:
        pattern = raw.raw_pattern
        if pattern is None or np.asarray(pattern).shape != (2, 2):
            return False, "CFA pattern is not 2x2"
        colors = raw.raw_colors_visible
        h = (colors.shape[0] // 2) * 2
        w = (colors.shape[1] // 2) * 2
        block = colors[:h, :w].reshape(h // 2, 2, w // 2, 2)
        mapped = np.where(block == 3, 1, block)
        if not np.all(np.sum(mapped == 0, axis=(1, 3)) == 1):
            return False, "2x2 blocks do not contain one red photosite"
        if not np.all(np.sum(mapped == 1, axis=(1, 3)) == 2):
            return False, "2x2 blocks do not contain two green photosites"
        if not np.all(np.sum(mapped == 2, axis=(1, 3)) == 1):
            return False, "2x2 blocks do not contain one blue photosite"
        return True, None


def darker_raw_clipped_quads(path):
    """Flag 2x2 quads with any darker-frame photosite at or above 98% of white."""
    camera = camdata.model(str(path))
    with rawpy.imread(str(path)) as raw:
        samples = raw.raw_image_visible.astype(np.float32)
        colors = raw.raw_colors_visible
        black = np.asarray(raw.black_level_per_channel, dtype=np.float32)
        white = camdata.white_level(camera, raw)
        denominator = white - black[colors]
        normalized = (samples - black[colors]) / denominator
        h = (normalized.shape[0] // 2) * 2
        w = (normalized.shape[1] // 2) * 2
        clipped = normalized[:h, :w] >= DARKER_FRAME_EXCLUSION
        return clipped.reshape(h // 2, 2, w // 2, 2).any(axis=(1, 3)), (h // 2, w // 2)


def downsample_quads(truth, dark_clipped, factor):
    if factor == 1:
        return truth, dark_clipped, 0
    h, w = truth.shape[:2]
    h2, w2 = h // 2, w // 2
    dropped = h * w - (2 * h2) * (2 * w2)
    truth = truth[: 2 * h2, : 2 * w2]
    dark_clipped = dark_clipped[: 2 * h2, : 2 * w2]
    truth = truth.reshape(h2, 2, w2, 2, 3).mean(axis=(1, 3), dtype=np.float32)
    dark_clipped = dark_clipped.reshape(h2, 2, w2, 2).any(axis=(1, 3))
    return truth, dark_clipped, dropped


def observed_values(truth, clipped, lower):
    observed = truth.copy()
    for channel in range(3):
        observed[..., channel][clipped[..., channel]] = lower[channel]
    return observed


def _reference_linear(values, channel):
    other = [index for index in range(3) if index != channel]
    roots = np.cbrt(np.maximum(values[..., other], 0.0))
    return ((roots[..., 0] + roots[..., 1]) * 0.5) ** 3


def _reference_cbrt(values, channel):
    other = [index for index in range(3) if index != channel]
    roots = np.cbrt(np.maximum(values[..., other], 0.0))
    return (roots[..., 0] + roots[..., 1]) * 0.5


def _reference_log(values, channel):
    other = [index for index in range(3) if index != channel]
    return np.log(np.maximum(values[..., other], 1.0e-12)).mean(axis=2)


def _global_offsets(truth, observed, clipped, usable, thresholds, space):
    offsets = []
    sample_counts = []
    for channel in range(3):
        if space == "linear":
            reference = _reference_linear(observed, channel)
            target = truth[..., channel]
            delta = target - reference
            eligible_values = target
        elif space == "cbrt":
            reference = _reference_cbrt(observed, channel)
            target = np.cbrt(np.maximum(truth[..., channel], 0.0))
            delta = target - reference
            eligible_values = truth[..., channel]
        else:
            reference = _reference_log(observed, channel)
            target = np.log(np.maximum(truth[..., channel], 1.0e-12))
            delta = target - reference
            eligible_values = truth[..., channel]

        if clipped[..., channel].any():
            near = ndimage.distance_transform_edt(~clipped[..., channel]) <= LOCAL_RADIUS
        else:
            near = np.zeros(clipped.shape[:2], dtype=bool)
        eligible = (
            usable
            & ~clipped[..., channel]
            & (eligible_values > 0.2 * thresholds[channel])
            & (eligible_values < CLIP_FRACTION * thresholds[channel])
            & near
        )
        offsets.append(float(np.mean(delta[eligible])) if eligible.any() else 0.0)
        sample_counts.append(int(eligible.sum()))
    return np.asarray(offsets, dtype=np.float64), sample_counts


def _offset_at(offsets, channel, target):
    # a global offset per channel (shape 3) or a per-quad map (h, w, 3)
    offsets = np.asarray(offsets)
    return offsets[channel] if offsets.ndim == 1 else offsets[..., channel][target]


def _apply_prediction(values, clipped, usable, lower, offsets, space):
    estimate = values.copy()
    for channel in range(3):
        target = clipped[..., channel] & usable
        if not target.any():
            continue
        if space == "linear":
            reference = _reference_linear(values, channel)
            prediction = reference[target] + _offset_at(offsets, channel, target)
        elif space == "cbrt":
            reference = _reference_cbrt(values, channel)
            prediction = np.maximum(reference[target] + _offset_at(offsets, channel, target), 0.0) ** 3
        else:
            reference = _reference_log(values, channel)
            prediction = np.exp(np.clip(reference[target] + _offset_at(offsets, channel, target), -80.0, 80.0))
        estimate[..., channel][target] = np.maximum(prediction, lower[channel])
    return estimate


def _iterated_prediction(observed, clipped, usable, lower, offsets, space, passes=3):
    state = observed.copy()
    estimate = _apply_prediction(state, clipped, usable, lower, offsets, space)
    first = estimate.copy()
    full = clipped.all(axis=2)
    for _ in range(passes):
        state = observed.copy()
        for channel in range(3):
            target = clipped[..., channel] & usable
            state[..., channel][target] = estimate[..., channel][target]
        estimate = _apply_prediction(state, clipped, usable, lower, offsets, space)
        estimate[full] = first[full]
    return estimate


def _local_offsets(truth, observed, clipped, usable, thresholds, space, global_offsets):
    regions = clipped.any(axis=2) & usable
    labels, nregions = ndimage.label(regions, structure=REGION_CONNECTIVITY)
    if nregions == 0:
        return global_offsets.copy(), {"regions": 0, "global_fallbacks": 0, "local_samples": 0}

    objects = ndimage.find_objects(labels)
    offsets_by_region = np.tile(global_offsets, (nregions + 1, 1))
    global_fallbacks = 0
    local_samples = 0
    for label, bounds in enumerate(objects, start=1):
        if bounds is None:
            continue
        y0 = max(0, bounds[0].start - 3)
        y1 = min(labels.shape[0], bounds[0].stop + 3)
        x0 = max(0, bounds[1].start - 3)
        x1 = min(labels.shape[1], bounds[1].stop + 3)
        crop = (slice(y0, y1), slice(x0, x1))
        in_region = labels[crop] == label
        expanded = ndimage.distance_transform_edt(~in_region) <= LOCAL_RADIUS
        for channel in range(3):
            if space == "linear":
                reference = _reference_linear(observed[crop], channel)
                target = truth[crop][..., channel]
                eligible_values = target
                delta = target - reference
            elif space == "cbrt":
                reference = _reference_cbrt(observed[crop], channel)
                target = np.cbrt(np.maximum(truth[crop][..., channel], 0.0))
                eligible_values = truth[crop][..., channel]
                delta = target - reference
            else:
                reference = _reference_log(observed[crop], channel)
                target = np.log(np.maximum(truth[crop][..., channel], 1.0e-12))
                eligible_values = truth[crop][..., channel]
                delta = target - reference

            eligible = (
                usable[crop]
                & ~clipped[crop][..., channel]
                & (eligible_values > 0.2 * thresholds[channel])
                & (eligible_values < CLIP_FRACTION * thresholds[channel])
                & expanded
            )
            count = int(eligible.sum())
            local_samples += count
            if count < LOCAL_MIN_SAMPLES:
                global_fallbacks += 1
                continue
            offsets_by_region[label, channel] = float(np.mean(delta[eligible]))
    return offsets_by_region, {
        "regions": int(nregions),
        "global_fallbacks": global_fallbacks,
        "local_samples": local_samples,
    }


def _apply_local_prediction(truth, observed, clipped, usable, lower,
                            offsets_by_region, space):
    regions = clipped.any(axis=2) & usable
    labels, _ = ndimage.label(regions, structure=REGION_CONNECTIVITY)
    estimate = truth.copy()
    for channel in range(3):
        target = clipped[..., channel] & usable
        if not target.any():
            continue
        if space == "linear":
            reference = _reference_linear(observed, channel)
        elif space == "cbrt":
            reference = _reference_cbrt(observed, channel)
        else:
            reference = _reference_log(observed, channel)
        target_labels = labels[target]
        regional_offset = offsets_by_region[target_labels, channel]
        if space == "linear":
            prediction = reference[target] + regional_offset
        elif space == "cbrt":
            prediction = np.maximum(reference[target] + regional_offset, 0.0) ** 3
        else:
            prediction = np.exp(np.clip(reference[target] + regional_offset, -80.0, 80.0))
        estimate[..., channel][target] = np.maximum(prediction, lower[channel])
    return estimate


def _class_masks(clipped, usable):
    count = clipped.sum(axis=2)
    two = usable & (count == 2)
    return {
        "partial": usable & ((count == 1) | (count == 2)),
        "one": usable & (count == 1),
        "two": two,
        "two_rg": two & clipped[..., 0] & clipped[..., 1],
        "two_rb": two & clipped[..., 0] & clipped[..., 2],
        "two_gb": two & clipped[..., 1] & clipped[..., 2],
        "full": usable & (count == 3),
    }


def _metrics_for_estimate(truth, clipped, estimate, masks):
    result = {}
    flat_truth = truth.reshape(-1, 3).astype(np.float64, copy=False)
    flat_estimate = estimate.reshape(-1, 3).astype(np.float64, copy=False)
    flat_clipped = clipped.reshape(-1, 3)
    for name, mask in masks.items():
        indices = np.flatnonzero(mask.ravel())
        if indices.size == 0:
            result[name] = None
            continue
        channel_mask = flat_clipped[indices]
        logs = np.log2(flat_estimate[indices][channel_mask] / flat_truth[indices][channel_mask])
        if name == "full":
            angle = None
        else:
            true_vectors = flat_truth[indices]
            estimated_vectors = flat_estimate[indices]
            dot = np.sum(true_vectors * estimated_vectors, axis=1)
            norm = np.linalg.norm(true_vectors, axis=1) * np.linalg.norm(estimated_vectors, axis=1)
            cosine = np.divide(dot, norm, out=np.ones_like(dot), where=norm > 0.0)
            angle = float(np.mean(np.degrees(np.arccos(np.clip(cosine, -1.0, 1.0)))))
        result[name] = {
            "n_quads": int(indices.size),
            "n_clipped_values": int(channel_mask.sum()),
            "bias_ev": float(np.median(logs)),
            "abs_ev_mean": float(np.mean(np.abs(logs))),
            "angle_deg_mean": angle,
        }
    return result


def _evaluate_predictor(truth, clipped, usable, observed, thresholds, lower, masks,
                        predictor, global_offsets, details):
    space = "cbrt" if predictor.startswith("CBRT") else "linear"
    if predictor == "OPP":
        estimate = _apply_prediction(observed, clipped, usable, lower, global_offsets["linear"], "linear")
    elif "LOCAL" in predictor:
        by_region, local_details = _local_offsets(truth, observed, clipped, usable, thresholds, space,
                                                  global_offsets[space])
        details[predictor] = local_details
        labels, _ = ndimage.label(clipped.any(axis=2) & usable, structure=REGION_CONNECTIVITY)
        offset_map = by_region[labels]
        passes = int(predictor.rsplit("-", 1)[1]) if "ITERP" in predictor else 0
        estimate = _iterated_prediction(observed, clipped, usable, lower, offset_map, space, passes=passes)
    elif "ITERP-" in predictor:
        estimate = _iterated_prediction(observed, clipped, usable, lower, global_offsets[space], space,
                                        passes=int(predictor.rsplit("-", 1)[1]))
    else:
        raise ValueError(f"unknown predictor: {predictor}")
    metrics = _metrics_for_estimate(truth, clipped, estimate, masks)
    del estimate
    return metrics


def process_pair(pair_id, scores):
    pair_start = time.perf_counter()
    ref = pathlib.Path(scores["ref"])
    okay, reason = bayer_pattern(ref)
    if not okay:
        return {"pair": pair_id, "scene": scene_kind(ref), "status": "skipped",
                "skip_reason": reason}, 0.0

    load_start = time.perf_counter()
    dark_clipped, expected_shape = darker_raw_clipped_quads(ref)
    gain = 2.0 ** float(scores["k"])
    truth, thresholds = camdata.quads(str(ref), gain, False)
    if truth.shape[:2] != expected_shape or dark_clipped.shape != truth.shape[:2]:
        raise RuntimeError(f"raw quad shape mismatch: quads={truth.shape[:2]}, mask={dark_clipped.shape}")
    original_shape = truth.shape[:2]
    original_clip = truth >= (CLIP_FRACTION * thresholds.reshape(1, 1, 3))
    original_clipped_quads = int((original_clip.any(axis=2) & ~dark_clipped).sum())
    downsample_factor = (
        2 if (truth.shape[0] * truth.shape[1] > DOWNSAMPLE_TRIGGER_QUADS
              or original_clipped_quads > DOWNSAMPLE_TRIGGER_CLIPPED_QUADS)
        else 1
    )
    truth, dark_clipped, dropped_quads = downsample_quads(truth, dark_clipped, downsample_factor)
    usable = ~dark_clipped
    lower = CLIP_FRACTION * thresholds
    clipped = truth >= lower.reshape(1, 1, 3)
    masks = _class_masks(clipped, usable)
    observed = observed_values(truth, clipped, lower)
    load_seconds = time.perf_counter() - load_start

    offsets = {}
    samples = {}
    for space in ("linear", "cbrt", "log"):
        offsets[space], samples[space] = _global_offsets(
            truth, observed, clipped, usable, thresholds, space
        )
    details = {
        "global_offsets": {space: [float(x) for x in values] for space, values in offsets.items()},
        "global_offset_samples": samples,
        "ratio_log_floor": 1.0e-12,
        "local_radius_quads": LOCAL_RADIUS,
        "local_minimum_samples": LOCAL_MIN_SAMPLES,
    }
    predictor_metrics = {}
    for predictor in PREDICTORS:
        start = time.perf_counter()
        predictor_metrics[predictor] = _evaluate_predictor(
            truth, clipped, usable, observed, thresholds, lower, masks,
            predictor, offsets, details
        )
        details.setdefault("runtime_seconds", {})[predictor] = time.perf_counter() - start

    nclip = clipped.sum(axis=2)
    class_quads = {name: int(mask.sum()) for name, mask in masks.items()}
    counts = {
        "input_quads": int(original_shape[0] * original_shape[1]),
        "analyzed_quads": int(truth.shape[0] * truth.shape[1]),
        "darker_frame_clipped_quads": int(dark_clipped.sum()),
        "truth_quads": int(usable.sum()),
        "class_quads": class_quads,
        "one_channel_clipped_quads": int((usable & (nclip == 1)).sum()),
        "two_channels_clipped_quads": int((usable & (nclip == 2)).sum()),
        "fully_clipped_quads": int((usable & (nclip == 3)).sum()),
        "clip_free_truth_quads": int((usable & (nclip == 0)).sum()),
        "dropped_edge_quads_before_downsampling": dropped_quads,
    }
    record = {
        "pair": pair_id,
        "scene": scene_kind(ref),
        "status": "ok",
        "ref": str(ref),
        "k_ev": float(scores["k"]),
        "gain": gain,
        "wb_thresholds": [float(x) for x in thresholds],
        "grid": {
            "input_shape_quads": list(original_shape),
            "analyzed_shape_quads": list(truth.shape[:2]),
            "downsample_factor": downsample_factor,
            "downsample_rule": "average 2x2 quads before synthetic clipping" if downsample_factor == 2 else None,
            "input_clipped_quads_before_downsampling": original_clipped_quads,
        },
        "counts": counts,
        "evaluation_share_of_truth_quads": float(masks["partial"].sum() / usable.sum()) if usable.any() else 0.0,
        "predictors": predictor_metrics,
        "predictor_details": details,
        "runtime_seconds": {
            **details["runtime_seconds"],
            "load_truth_seconds": load_seconds,
            "pair_total_seconds": time.perf_counter() - pair_start,
        },
    }
    return record, time.perf_counter() - pair_start


def _comparison(values, baseline, metric, tie):
    comparisons = []
    for result in values:
        current = result.get(metric)
        base = baseline.get(result["pair"], {}).get(metric)
        if current is None or base is None:
            continue
        difference = current - base
        if abs(difference) <= tie:
            comparisons.append("tie")
        elif difference < 0:
            comparisons.append("win")
        else:
            comparisons.append("loss")
    return {
        "wins": comparisons.count("win"),
        "ties": comparisons.count("tie"),
        "losses": comparisons.count("loss"),
        "n_pairs": len(comparisons),
    }


def aggregate(records, group_by_scene=False):
    good = [record for record in records if record.get("status") == "ok"]
    groups = {"overall": good}
    if group_by_scene:
        groups = {}
        for record in good:
            groups.setdefault(record.get("scene", "unknown"), []).append(record)

    summary = {}
    for group, group_records in groups.items():
        summary[group] = {}
        for class_name in CLASSES:
            rows = {}
            for predictor in PREDICTORS:
                per_pair = []
                for record in group_records:
                    metric = record["predictors"][predictor][class_name]
                    if metric is not None:
                        per_pair.append({"pair": record["pair"], **metric})
                if not per_pair:
                    rows[predictor] = {
                        "n_pairs": 0, "median_bias_ev": None,
                        "median_abs_ev_mean": None, "median_angle_deg_mean": None,
                        "vs_OPP_abs": None, "vs_OPP_angle": None,
                    }
                    continue
                rows[predictor] = {
                    "n_pairs": len(per_pair),
                    "median_bias_ev": float(np.median([row["bias_ev"] for row in per_pair])),
                    "median_abs_ev_mean": float(np.median([row["abs_ev_mean"] for row in per_pair])),
                    "median_angle_deg_mean": (
                        float(np.median([row["angle_deg_mean"] for row in per_pair
                                         if row["angle_deg_mean"] is not None]))
                        if any(row["angle_deg_mean"] is not None for row in per_pair) else None
                    ),
                    "vs_OPP_abs": _comparison(per_pair, {
                        record["pair"]: record["predictors"]["OPP"][class_name]
                        for record in group_records
                        if record["predictors"]["OPP"][class_name] is not None
                    }, "abs_ev_mean", 0.02),
                    "vs_OPP_angle": _comparison(per_pair, {
                        record["pair"]: record["predictors"]["OPP"][class_name]
                        for record in group_records
                        if record["predictors"]["OPP"][class_name] is not None
                    }, "angle_deg_mean", 0.1),
                }
            summary[group][class_name] = rows
    return summary


def _fmt(value, digits=3):
    return "-" if value is None else f"{value:.{digits}f}"


def _wtl(value):
    if value is None or value["n_pairs"] == 0:
        return "-"
    return f"{value['wins']}/{value['ties']}/{value['losses']}"


def _append_table(lines, summary, group, class_name):
    rows = summary.get(group, {}).get(class_name, {})
    if not rows or not any(row["n_pairs"] for row in rows.values()):
        return
    lines.extend([
        "| Predictor | Pairs | Bias (EV) | Abs (EV) | Angle (deg) | Abs W/T/L | Angle W/T/L |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ])
    for predictor in PREDICTORS:
        row = rows[predictor]
        if row["n_pairs"] == 0:
            continue
        lines.append(
            f"| {predictor} | {row['n_pairs']} | { _fmt(row['median_bias_ev']) } | "
            f"{ _fmt(row['median_abs_ev_mean']) } | { _fmt(row['median_angle_deg_mean']) } | "
            f"{_wtl(row['vs_OPP_abs'])} | {_wtl(row['vs_OPP_angle'])} |"
        )
    lines.append("")


def write_report(path, output):
    records = output.get("pairs", [])
    good = [item for item in records if item.get("status") == "ok"]
    skipped = [item for item in records if item.get("status") == "skipped"]
    errors = [item for item in records if item.get("status") == "error"]
    downsampled = [item for item in good if item["grid"]["downsample_factor"] > 1]
    summary = aggregate(records)
    by_scene = aggregate(records, group_by_scene=True)
    total_pair_seconds = sum(item["runtime_seconds"]["pair_total_seconds"] for item in good)
    lines = [
        "# Opposed-style brightness predictors on synthetic clipping",
        "",
        "The darker raw in each bracket pair is scaled to the brighter exposure and synthetically clipped at 0.98 of each white-balance threshold. Quads that were already clipped in the darker raw are excluded from training and evaluation.",
        "",
        "## Methods and counts",
        "",
        f"Processed {len(records)} of {output['config']['included_pairs']} included pairs: {len(good)} Bayer pairs, {len(skipped)} non-Bayer skips, and {len(errors)} errors. This report is {'complete' if output.get('complete') else 'a partial checkpoint'}.",
        f"Averaged 2x2 quads before clipping for {len(downsampled)} pairs when the input exceeded {DOWNSAMPLE_TRIGGER_QUADS:,} quads or {DOWNSAMPLE_TRIGGER_CLIPPED_QUADS:,} valid clipped quads.",
        f"Pair processing time summed to {total_pair_seconds:.1f} seconds; recorded full-run wall time is {output.get('runtime_seconds', {}).get('batch_wall_seconds', 0.0):.1f} seconds. It used one Python process, one OpenCV thread, and one BLAS/OpenMP thread with `nice -n 19`.",
        "OPP uses the cube-root mean of the other two observed channels, cubed, plus a global per-channel linear offset. CBRT measures and applies that offset in cube-root space. ITER variants run three further passes, updating references from the previous estimates while holding offsets fixed. LOCAL variants use per-region offsets fitted from unclipped quads within 3 quads of the 8-connected clipped region; they fall back to the global offset with fewer than 30 samples. RATIO-REF applies a global offset in log space, with input values floored at 1e-12.",
        "The local fit keeps OPP's 0.2-to-0.98 threshold value window and selects training quads by distance to the region. All predictions are clamped to at least 0.98 of the channel threshold. The region labels and evaluated quads exclude darker-frame-clipped quads.",
        "Bias is the median signed log2(estimate/truth) over clipped channel values; negative values are too dark. Abs is the mean absolute log2 error. Angle is the mean RGB-vector angle over quads and is shown only for partly clipped classes. Table values are medians of the per-pair metrics. Wins/ties/losses compare per-pair Abs or angle with OPP; ties are within 0.02 EV or 0.1 degree respectively.",
        "",
        "## All pairs",
        "",
    ]
    class_titles = {
        "partial": "All partly clipped quads",
        "one": "One clipped channel",
        "two": "Two clipped channels",
        "two_rg": "Two clipped channels: red and green",
        "two_rb": "Two clipped channels: red and blue",
        "two_gb": "Two clipped channels: green and blue",
        "full": "Fully clipped quads",
    }
    for class_name in CLASSES:
        lines.extend([f"### {class_titles[class_name]}", ""])
        _append_table(lines, summary, "overall", class_name)

    lines.extend(["## By scene kind", ""])
    if by_scene:
        for scene in sorted(by_scene):
            lines.extend([f"### {scene}", ""])
            for class_name in ("two", "full"):
                lines.extend([f"#### {class_titles[class_name]}", ""])
                _append_table(lines, by_scene, scene, class_name)

    candidates = [item for item in good
                  if item["predictors"]["OPP"]["two"] is not None]
    candidates.sort(key=lambda item: item["predictors"]["OPP"]["two"]["bias_ev"])
    lines.extend([
        "## Ten most negative OPP biases in the two-channel class",
        "",
        "Bias columns are per-pair medians in EV over the clipped values in two-channel quads.",
        "",
        "| Pair | Scene | Count | OPP | OPP-CBRT | OPP-ITER | OPP-CBRT-ITER | OPP-LOCAL | OPP-CBRT-LOCAL | RATIO-REF |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for item in candidates[:10]:
        biases = [item["predictors"][predictor]["two"]["bias_ev"] for predictor in PREDICTORS]
        lines.append(
            f"| {item['pair'].split('/')[-1]} | {item['scene']} | "
            f"{item['counts']['class_quads']['two']} | " + " | ".join(_fmt(value) for value in biases) + " |"
        )
    if not candidates:
        lines.append("No evaluated two-channel pairs are available yet.")
    lines.extend(["", "## Skips, errors, and limits", ""])
    if skipped:
        lines.append("Skipped non-Bayer pairs: " + ", ".join(
            f"`{item['pair']}` ({item['skip_reason']})" for item in skipped
        ) + ".")
    else:
        lines.append("Skipped non-Bayer pairs: none.")
    if errors:
        lines.append("Pair errors: " + ", ".join(
            f"`{item['pair']}` ({item['error']})" for item in errors
        ) + ".")
    if downsampled:
        lines.append("Downsampled pairs: " + ", ".join(
            f"`{item['pair'].split('/')[-1]}`" for item in downsampled
        ) + ".")
    else:
        lines.append("Downsampled pairs: none.")
    lines.extend([
        "This is a synthetic-clipping comparison using 2x2 Bayer quad means, not per-photosite estimates or rendered images. It does not test X-Trans, OpenCL, demosaicing, registration errors, or scene motion. RATIO-REF is included only as a reference comparison.",
        "",
        "Per-pair class counts, metrics, offsets, and runtimes are in `ring.json`.",
        "",
    ])
    path.write_text("\n".join(lines))


def _select_smoke_pairs(included):
    required = (
        "g294-corridor-wall-lamp-0122-0120",
        "g065-sunset-field-0347-0345",
    )
    selected = []
    for name in required:
        match = next((pair for pair in included if pair.split("/")[-1] == name), None)
        if match is None:
            raise ValueError(f"required smoke pair is absent from included.txt: {name}")
        selected.append(match)
    selected.append(next(pair for pair in included if pair not in selected))
    return selected


def _compare_predict_json(records):
    if not PREDICT_JSON.exists():
        return {"available": False, "path": str(PREDICT_JSON), "comparisons": []}
    baseline = json.loads(PREDICT_JSON.read_text())
    by_pair = {item.get("pair", "").split("/")[-1]: item for item in baseline.get("pairs", [])}
    comparisons = []
    for record in records:
        if record.get("status") != "ok":
            continue
        old = by_pair.get(record["pair"].split("/")[-1])
        if not old or old.get("status") != "ok":
            continue
        old_metric = old.get("predictors", {}).get("OPP", {}).get("all")
        new_metric = record["predictors"]["OPP"]["partial"]
        if not old_metric or new_metric is None:
            continue
        comparisons.append({
            "pair": record["pair"],
            "ring_log2_abs_mean": new_metric["abs_ev_mean"],
            "predict_log2_abs_mean": old_metric["log2_abs_mean"],
            "log2_abs_difference": new_metric["abs_ev_mean"] - old_metric["log2_abs_mean"],
            "ring_angle_deg_mean": new_metric["angle_deg_mean"],
            "predict_angle_deg_mean": old_metric["angle_deg_mean"],
            "angle_difference_deg": new_metric["angle_deg_mean"] - old_metric["angle_deg_mean"],
        })
    return {"available": True, "path": str(PREDICT_JSON), "comparisons": comparisons}


def run(smoke=False, resume=False, redo_pairs=None):
    included = [line.strip() for line in INCLUDED.read_text().splitlines() if line.strip()]
    config = {
        "included_pairs": len(included),
        "included_list": included,
        "dark_frame_photosite_clip_fraction": DARKER_FRAME_EXCLUSION,
        "synthetic_clip_fraction": CLIP_FRACTION,
        "downsample_trigger_quads": DOWNSAMPLE_TRIGGER_QUADS,
        "downsample_trigger_clipped_quads": DOWNSAMPLE_TRIGGER_CLIPPED_QUADS,
        "python_version": sys.version.split()[0],
    }
    redo_pairs = set(redo_pairs or [])
    records = []
    prior_pair_seconds = 0.0
    output_path = HERE / "ring3.json"
    if redo_pairs and not output_path.exists():
        raise ValueError("--redo-pair requires an existing ring.json checkpoint")
    if resume or output_path.exists():
        if not output_path.exists():
            raise FileNotFoundError("--resume needs an existing ring.json checkpoint")
        previous = json.loads(output_path.read_text())
        if previous.get("config", {}).get("included_list") != included:
            raise ValueError("ring.json checkpoint does not match the current included.txt")
        if previous.get("complete") and not redo_pairs:
            if smoke:
                raise ValueError("the current ring.json is complete; --smoke is only for the initial check")
        records = [item for item in previous.get("pairs", [])
                   if item.get("pair") not in redo_pairs and item.get("status") != "error"]
        prior_pair_seconds = sum(
            item.get("runtime_seconds", {}).get("pair_total_seconds", 0.0)
            for item in records if item.get("status") == "ok"
        )
        if not redo_pairs and previous.get("complete"):
            previous["runtime_seconds"]["pair_processing_seconds"] = sum(
                item.get("runtime_seconds", {}).get("pair_total_seconds", 0.0)
                for item in records if item.get("status") == "ok"
            )
            previous["predict_json_check"] = _compare_predict_json(records)
            json_write_atomic(output_path, previous)
            write_report(HERE / "ring3.md", previous)
            print("ring.json is complete; refreshed ring.md", flush=True)
            return

    known = {item.get("pair") for item in records}
    if not redo_pairs.issubset(set(included)):
        raise ValueError("--redo-pair contains a pair outside included.txt")
    if redo_pairs and not redo_pairs.issubset({item.get("pair") for item in previous.get("pairs", [])}):
        raise ValueError("--redo-pair must name a pair already present in ring.json")

    selected = _select_smoke_pairs(included) if smoke else included
    start = time.perf_counter()
    for index, pair_id in enumerate(selected, 1):
        if pair_id in known:
            continue
        workdir = RUNS / pair_id
        try:
            scores = json.loads((workdir / "scores.json").read_text())
            record, elapsed = process_pair(pair_id, scores)
            records.append(record)
            status = record["status"]
            count_text = ""
            if status == "ok":
                count_text = (f" partial={record['counts']['class_quads']['partial']}"
                              f" full={record['counts']['class_quads']['full']}"
                              f" factor={record['grid']['downsample_factor']}")
            print(f"[{index}/{len(selected)}] {pair_id}: {status}{count_text} {elapsed:.1f}s", flush=True)
        except Exception as error:
            record = {"pair": pair_id, "scene": "unknown", "status": "error",
                      "error": f"{type(error).__name__}: {error}"}
            records.append(record)
            print(f"[{index}/{len(selected)}] {pair_id}: ERROR {type(error).__name__}: {error}",
                  file=sys.stderr, flush=True)
            traceback.print_exc()
        known.add(pair_id)
        records.sort(key=lambda item: included.index(item["pair"]))
        partial_output = {
            "config": config,
            "pairs": records,
            "runtime_seconds": {
                "pair_processing_seconds": sum(
                    item.get("runtime_seconds", {}).get("pair_total_seconds", 0.0)
                    for item in records if item.get("status") == "ok"
                ),
                "batch_wall_seconds": prior_pair_seconds + time.perf_counter() - start,
            },
            "complete": False,
        }
        partial_output["predict_json_check"] = _compare_predict_json(records)
        json_write_atomic(output_path, partial_output)

    records.sort(key=lambda item: included.index(item["pair"]))
    complete = len(records) == len(included)
    output = {
        "config": config,
        "pairs": records,
        "runtime_seconds": {
            "pair_processing_seconds": sum(
                item.get("runtime_seconds", {}).get("pair_total_seconds", 0.0)
                for item in records if item.get("status") == "ok"
            ),
            "batch_wall_seconds": prior_pair_seconds + time.perf_counter() - start,
        },
        "predict_json_check": _compare_predict_json(records),
        "complete": complete,
    }
    json_write_atomic(output_path, output)
    write_report(HERE / "ring3.md", output)
    print(f"Wrote {HERE / 'ring.md'} and {output_path}; complete={complete}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", action="store_true",
                        help="run the two required pairs plus the first included pair")
    parser.add_argument("--resume", action="store_true",
                        help="continue from the existing ring.json checkpoint")
    parser.add_argument("--redo-pair", action="append", default=[],
                        help="recompute a named included pair; may be repeated")
    parser.add_argument("--report-only", action="store_true",
                        help="regenerate ring.md from ring.json")
    arguments = parser.parse_args()
    if arguments.report_only:
        write_report(HERE / "ring3.md", json.loads((HERE / "ring3.json").read_text()))
        print(f"Wrote {HERE / 'ring.md'}", flush=True)
    else:
        run(arguments.smoke, arguments.resume, arguments.redo_pair)
