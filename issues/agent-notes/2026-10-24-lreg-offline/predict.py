#!/usr/bin/env python3
"""Compare camera-space predictors on synthetic clipping of bracketed raw pairs."""

import os

# Clamp numerical libraries before importing NumPy so the one Python process stays single-threaded.
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
import math
import pathlib
import sys
import time
import traceback
import warnings
from functools import lru_cache

sys.dont_write_bytecode = True

import cv2
import numpy as np
import pyamg
import rawpy
from scipy import ndimage, sparse, special

HERE = pathlib.Path(__file__).resolve().parent
RUNS = pathlib.Path("/tmp/hl-rules-study/runs")
INCLUDED = pathlib.Path("/tmp/hl-rules-study/scratch/included.txt")
CAMDATA_DIR = pathlib.Path("/tmp/hl-rules-study/scripts")
warnings.filterwarnings("ignore", message='"Matplotlib" related API features are not available')
sys.path.insert(0, str(CAMDATA_DIR))
import camdata  # noqa: E402
camdata.model = lru_cache(maxsize=8)(camdata.model)

cv2.setNumThreads(1)

CLIP_FRACTION = 0.98
DARKER_FRAME_EXCLUSION = 0.98
DOWNSAMPLE_TRIGGER_QUADS = 3_000_000
DOWNSAMPLE_TRIGGER_CLIPPED_QUADS = 200_000
MIN_EFFECTIVE_SAMPLES = 32.0
MIN_WEIGHT_SUM = 1.0
SLOPE_RIDGE = 1.0e-4
SIGMA_FLOOR_RELATIVE = 1.0e-6
LAPLACE_TOL = 1.0e-7
LAPLACE_MAXITER = 100
FOUR = np.array([[0, 1, 0], [1, 1, 1], [0, 1, 0]], dtype=bool)
NEIGHBORS = ((-1, 0), (1, 0), (0, -1), (0, 1))


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
        # Check every Bayer quad because a cropped or unusual CFA phase must not silently
        # enter the 2x2 reduction used by camdata.quads().
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


def laplace_system(unknown, boundary):
    """Build the 4-neighbor Dirichlet system for unknown components touching boundary."""
    height, width = unknown.shape
    labels, nlabels = ndimage.label(unknown, structure=FOUR)
    if nlabels == 0:
        return None, np.zeros_like(unknown), np.zeros_like(unknown), None

    adjacent_boundary = unknown & ndimage.binary_dilation(boundary, structure=FOUR)
    anchored_labels = np.zeros(nlabels + 1, dtype=bool)
    if adjacent_boundary.any():
        anchored_labels[np.unique(labels[adjacent_boundary])] = True
    anchored_labels[0] = False
    solve_mask = unknown & anchored_labels[labels]
    fallback_mask = unknown & ~solve_mask
    flat_unknown = np.flatnonzero(solve_mask.ravel())
    if flat_unknown.size == 0:
        return None, solve_mask, fallback_mask, None

    n = flat_unknown.size
    flat_to_row = np.full(height * width, -1, dtype=np.int32)
    flat_to_row[flat_unknown] = np.arange(n, dtype=np.int32)
    rows = [np.arange(n, dtype=np.int32)]
    cols = [np.arange(n, dtype=np.int32)]
    values = []
    degree = np.zeros(n, dtype=np.float64)
    boundary_edges = []
    y = flat_unknown // width
    x = flat_unknown % width
    solve_flat = solve_mask.ravel()
    boundary_flat = boundary.ravel()

    for dy, dx in NEIGHBORS:
        yy = y + dy
        xx = x + dx
        in_bounds = (yy >= 0) & (yy < height) & (xx >= 0) & (xx < width)
        candidate_rows = np.flatnonzero(in_bounds)
        neighbor_flat = yy[in_bounds] * width + xx[in_bounds]
        neighbor_unknown = solve_flat[neighbor_flat]
        neighbor_boundary = boundary_flat[neighbor_flat]
        active = neighbor_unknown | neighbor_boundary
        active_rows = candidate_rows[active]
        active_neighbors = neighbor_flat[active]
        degree[active_rows] += 1.0

        unknown_edges = neighbor_unknown[active]
        if unknown_edges.any():
            rows.append(active_rows[unknown_edges].astype(np.int32, copy=False))
            cols.append(flat_to_row[active_neighbors[unknown_edges]])
            values.append(np.full(int(unknown_edges.sum()), -1.0, dtype=np.float64))
        boundary_edges.append((active_rows[neighbor_boundary[active]].astype(np.int32, copy=False),
                               active_neighbors[neighbor_boundary[active]]))

    values.insert(0, degree)
    matrix = sparse.coo_matrix(
        (np.concatenate(values), (np.concatenate(rows), np.concatenate(cols))),
        shape=(n, n),
    ).tocsr()
    return (matrix, boundary_edges, flat_unknown), solve_mask, fallback_mask, flat_to_row


def solve_laplace(matrix_info, boundary_values, fallback_values, roi_lookup=None, solver=None):
    """Solve all coefficient or hue fields with one pyamg hierarchy."""
    if matrix_info is None:
        return np.empty((0, boundary_values.shape[-1]), dtype=np.float64), 0.0
    matrix, boundary_edges, flat_unknown = matrix_info
    if matrix.shape[0] == 0:
        return np.empty((0, boundary_values.shape[-1]), dtype=np.float64), 0.0
    if roi_lookup is None:
        fixed = boundary_values.reshape(-1, boundary_values.shape[-1])
        boundary_rois = None
        field_count = fixed.shape[-1]
    else:
        fixed = boundary_values
        boundary_rois = roi_lookup
        field_count = fixed.shape[0]

    rhs = np.zeros((matrix.shape[0], field_count), dtype=np.float64)
    for edge_rows, edge_neighbors in boundary_edges:
        if edge_rows.size == 0:
            continue
        if boundary_rois is None:
            rhs[edge_rows] += fixed[edge_neighbors]
        else:
            anchor_indices = boundary_rois[edge_neighbors]
            rhs[edge_rows] += fixed[:, anchor_indices].T

    if solver is None:
        solver = pyamg.smoothed_aggregation_solver(matrix)
    # pyamg's hierarchy accepts one right-hand side per solve, so reuse it for each field.
    solution = np.column_stack([
        solver.solve(rhs[:, channel], tol=LAPLACE_TOL, maxiter=LAPLACE_MAXITER)
        for channel in range(rhs.shape[1])
    ])
    residual = matrix @ solution - rhs
    denominator = np.maximum(np.linalg.norm(rhs, axis=0), 1.0e-30)
    relative_residual = float(np.max(np.linalg.norm(residual, axis=0) / denominator))
    return solution, relative_residual


def hue_field(truth, clipped, usable):
    any_clipped = clipped.any(axis=2) & usable
    border = (~clipped.any(axis=2)) & usable & ndimage.binary_dilation(any_clipped, structure=FOUR)
    sums = truth.sum(axis=2)
    bad_sum = border & (~np.isfinite(sums) | (sums <= 1.0e-12))
    rho = np.full_like(truth, 1.0 / 3.0, dtype=np.float64)
    good = border & ~bad_sum
    rho[good] = truth[good] / sums[good, None]

    system, solve_mask, fallback_mask, _ = laplace_system(any_clipped, border)
    max_residual = 0.0
    if system is not None and solve_mask.any():
        matrix_info = system
        filled, max_residual = solve_laplace(matrix_info, rho, None)
        unknown_idx = np.flatnonzero(solve_mask.ravel())
        rho.reshape(-1, 3)[unknown_idx] = filled

    if fallback_mask.any():
        if border.any():
            fallback = np.mean(rho[border], axis=0)
        else:
            fallback = np.full(3, 1.0 / 3.0)
        rho[fallback_mask] = fallback
    return rho.astype(np.float32), border, int(bad_sum.sum()), int(fallback_mask.sum()), max_residual


def observed_values(truth, clipped, lower):
    observed = truth.copy()
    for c in range(3):
        observed[..., c][clipped[..., c]] = lower[c]
    return observed


def opposed_estimates(truth, observed, clipped, usable, thresholds):
    h, w = truth.shape[:2]
    cube_roots = np.cbrt(np.maximum(observed, 0.0))
    offsets = []
    nclip = clipped.sum(axis=2)
    partial = usable & ((nclip == 1) | (nclip == 2))
    partial_flat = np.flatnonzero(partial.ravel())
    estimates = truth.reshape(-1, 3)[partial_flat].copy()
    eval_map = np.full(h * w, -1, dtype=np.int32)
    eval_map[partial_flat] = np.arange(partial_flat.size, dtype=np.int32)

    for c in range(3):
        other = [s for s in range(3) if s != c]
        ref = ((cube_roots[..., other[0]] + cube_roots[..., other[1]]) * 0.5) ** 3
        if clipped[..., c].any():
            distance = ndimage.distance_transform_edt(~clipped[..., c])
            near = distance <= 3.0
        else:
            near = np.zeros((h, w), dtype=bool)
        eligible = (
            usable
            & ~clipped[..., c]
            & (truth[..., c] > 0.2 * thresholds[c])
            & (truth[..., c] < CLIP_FRACTION * thresholds[c])
            & near
        )
        offset = float(np.mean(truth[..., c][eligible] - ref[eligible])) if eligible.any() else 0.0
        offsets.append({"channel": c, "value": offset, "samples": int(eligible.sum())})
        target = partial & clipped[..., c]
        rows = eval_map[np.flatnonzero(target.ravel())]
        if rows.size:
            estimates[rows, c] = np.maximum(ref.ravel()[np.flatnonzero(target.ravel())] + offset,
                                            CLIP_FRACTION * thresholds[c])
    return estimates, offsets, partial, partial_flat, eval_map


def hue_estimates(truth, observed, clipped, rho, partial, partial_flat, eval_map, thresholds):
    estimates = truth.reshape(-1, 3)[partial_flat].copy()
    flat_truth = truth.reshape(-1, 3)
    flat_clipped = clipped.reshape(-1, 3)
    flat_rho = rho.reshape(-1, 3)
    lower = CLIP_FRACTION * thresholds
    nclip = flat_clipped.sum(axis=1)
    for c in range(3):
        target_flat = np.flatnonzero(partial.ravel() & clipped[..., c].ravel())
        if target_flat.size == 0:
            continue
        rows = eval_map[target_flat]
        measured = [s for s in range(3) if s != c]
        value_sum = np.zeros(target_flat.size, dtype=np.float64)
        count = np.zeros(target_flat.size, dtype=np.float64)
        for s in measured:
            valid = (~flat_clipped[target_flat, s]) & (np.abs(flat_rho[target_flat, s]) > 1.0e-12)
            ratio = np.zeros(target_flat.size, dtype=np.float64)
            np.divide(flat_rho[target_flat, c], flat_rho[target_flat, s], out=ratio, where=valid)
            contribution = flat_truth[target_flat, s] * ratio
            value_sum[valid] += contribution[valid]
            count[valid] += 1.0
        prediction = np.divide(value_sum, count, out=np.full_like(value_sum, lower[c]), where=count > 0)
        estimates[rows, c] = np.maximum(prediction, lower[c])
    return estimates


def clipped_partials(clipped, usable):
    count = clipped.sum(axis=2)
    return usable & ((count == 1) | (count == 2)), count


def _filter_moments(truth, train, max_normalized, p, radius, roi_indices):
    height, width = train.shape
    weights = np.zeros((height, width), dtype=np.float64)
    if p == 0:
        weights[train] = 1.0
    else:
        weights[train] = np.power(max_normalized[train], p, dtype=np.float64)

    kernel = 2 * radius + 1
    sums = np.empty((11, roi_indices.size), dtype=np.float64)
    temporary = np.empty_like(weights)
    filtered = np.empty_like(weights)
    moment_specs = [
        ("w", 0, 0),
        ("ww", 0, 0),
        ("wx", 0, 0),
        ("wx", 1, 0),
        ("wx", 2, 0),
        ("wxx", 0, 0),
        ("wxx", 1, 1),
        ("wxx", 2, 2),
        ("wxx", 0, 1),
        ("wxx", 0, 2),
        ("wxx", 1, 2),
    ]
    flat_truth = truth.reshape(-1, 3)

    for index, (kind, a, b) in enumerate(moment_specs):
        if kind == "w":
            source = weights
        else:
            if kind == "ww":
                np.multiply(weights, weights, out=temporary)
            elif kind == "wx":
                np.multiply(weights, flat_truth[:, a].reshape(height, width), out=temporary)
            else:
                np.multiply(flat_truth[:, a].reshape(height, width),
                            flat_truth[:, b].reshape(height, width), out=temporary)
                temporary *= weights
            source = temporary
        cv2.boxFilter(source, cv2.CV_64F, (kernel, kernel), dst=filtered,
                      normalize=False, borderType=cv2.BORDER_CONSTANT)
        sums[index] = filtered.ravel()[roi_indices]

    return sums, weights


def _fit_local(sums, target, predictors, minimum_valid):
    sw = sums[0]
    sw2 = sums[1]
    # OpenCV's large-window accumulator can leave round-off near zero at empty
    # windows. Keep those placeholders finite; their support gate is false below.
    safe_sw = np.where(sw > 1.0e-12, sw, 1.0)
    sy = sums[2 + target]
    syy = sums[5 + target]
    mean_y = sy / safe_sw
    n_eff = sw * sw / np.maximum(sw2, 1.0e-300)
    valid = (n_eff >= MIN_EFFECTIVE_SAMPLES) & (sw >= MIN_WEIGHT_SUM)

    sx = [sums[2 + s] for s in predictors]
    sxy = [sums[_pair_moment(target, s)] for s in predictors]
    cxy = [sxy_i - sx_i * sy / safe_sw for sx_i, sxy_i in zip(sx, sxy)]
    variances = [np.maximum(sums[_diag_moment(s)] - sx_i * sx_i / safe_sw, 0.0)
                 for s, sx_i in zip(predictors, sx)]

    if len(predictors) == 1:
        denominator = variances[0] * (1.0 + SLOPE_RIDGE)
        slope = np.divide(cxy[0], denominator, out=np.zeros_like(denominator),
                          where=denominator > 1.0e-24)
        intercept = mean_y - slope * sx[0] / safe_sw
        slopes = [slope]
    else:
        c12 = sums[_pair_moment(predictors[0], predictors[1])] - sx[0] * sx[1] / safe_sw
        m11 = variances[0] * (1.0 + SLOPE_RIDGE)
        m22 = variances[1] * (1.0 + SLOPE_RIDGE)
        determinant = m11 * m22 - c12 * c12
        determinant_floor = 1.0e-14 * np.maximum(m11 * m22, 1.0)
        good = determinant > determinant_floor
        b1 = np.divide(cxy[0] * m22 - cxy[1] * c12, determinant,
                       out=np.zeros_like(determinant), where=good)
        b2 = np.divide(cxy[1] * m11 - cxy[0] * c12, determinant,
                       out=np.zeros_like(determinant), where=good)
        intercept = mean_y - (b1 * sx[0] + b2 * sx[1]) / safe_sw
        slopes = [b1, b2]

    explained = intercept * sy
    for slope, cross in zip(slopes, sxy):
        explained += slope * cross
    sse = np.maximum(syy - explained, 0.0)
    parameter_count = len(predictors) + 1
    correction = np.divide(n_eff, np.maximum(n_eff - parameter_count, 1.0))
    sigma = np.sqrt(np.maximum(sse / safe_sw * correction, 0.0))
    coefficients = np.vstack([intercept, *slopes, np.log(np.maximum(sigma, 1.0e-12))])
    finite = np.all(np.isfinite(coefficients), axis=0)
    return coefficients, valid & finite


def _pair_moment(a, b):
    if a == b:
        return _diag_moment(a)
    lo, hi = sorted((a, b))
    return {(0, 1): 8, (0, 2): 9, (1, 2): 10}[(lo, hi)]


def _diag_moment(channel):
    return 5 + channel


class CoefficientTransport:
    """Harmonically extend fit fields only across clipped quads with invalid windows."""

    def __init__(self, clipped_region, local_valid, roi_indices):
        self.height, self.width = clipped_region.shape
        self.roi_lookup = np.full(self.height * self.width, -1, dtype=np.int32)
        self.roi_lookup[roi_indices] = np.arange(roi_indices.size, dtype=np.int32)
        self.valid_full = np.zeros(self.height * self.width, dtype=bool)
        self.valid_full[roi_indices] = local_valid
        self.valid_full = self.valid_full.reshape(self.height, self.width)
        unknown = clipped_region & ~self.valid_full
        self.matrix_info, self.solve_mask, self.fallback_mask = self._make_system(unknown)
        self.unknown_count = int(unknown.sum())
        self.unanchored_count = int(self.fallback_mask.sum())
        self.solver = (
            pyamg.smoothed_aggregation_solver(self.matrix_info[0])
            if self.matrix_info is not None and self.solve_mask.any()
            else None
        )

    def _make_system(self, unknown):
        built, solve_mask, fallback_mask, _ = laplace_system(unknown, self.valid_full)
        return built, solve_mask, fallback_mask

    def fill(self, coefficients, fallback):
        filled = coefficients.copy()
        if self.fallback_mask.any():
            roi_ids = self.roi_lookup[np.flatnonzero(self.fallback_mask.ravel())]
            filled[:, roi_ids] = fallback[:, None]
        if self.matrix_info is not None and self.solve_mask.any():
            solution, residual = solve_laplace(self.matrix_info, filled, fallback,
                                               roi_lookup=self.roi_lookup, solver=self.solver)
            unknown_flat = self.matrix_info[2]
            roi_ids = self.roi_lookup[unknown_flat]
            filled[:, roi_ids] = solution.T
        else:
            residual = 0.0
        return filled, residual


def _global_fallback(truth, weights, target, predictors):
    total = float(weights.sum())
    if total <= 1.0e-20:
        return np.array([0.0] + [0.0] * len(predictors) + [math.log(1.0e-6)], dtype=np.float64)
    values = truth[..., target].astype(np.float64)
    mean = float(np.sum(weights * values) / total)
    variance = float(np.sum(weights * (values - mean) ** 2) / total)
    fields = [mean] + [0.0] * len(predictors) + [math.log(max(math.sqrt(max(variance, 0.0)), 1.0e-8))]
    return np.asarray(fields, dtype=np.float64)


def _active_models(clipped, partial):
    count = clipped.sum(axis=2)
    models = []
    for target in range(3):
        others = tuple(s for s in range(3) if s != target)
        mask = partial & clipped[..., target] & (count == 1)
        if mask.any():
            models.append((target, others, np.flatnonzero(mask.ravel())))
        for survivor in others:
            mask = partial & clipped[..., target] & (count == 2) & ~clipped[..., survivor]
            if mask.any():
                models.append((target, (survivor,), np.flatnonzero(mask.ravel())))
    return models


def local_regression(truth, clipped, usable, thresholds, radius, power, collect_censored):
    start = time.perf_counter()
    partial, counts = clipped_partials(clipped, usable)
    partial_flat = np.flatnonzero(partial.ravel())
    eval_map = np.full(partial.size, -1, dtype=np.int32)
    eval_map[partial_flat] = np.arange(partial_flat.size, dtype=np.int32)
    base_estimate = truth.reshape(-1, 3)[partial_flat].copy()
    censored_estimate = base_estimate.copy() if collect_censored else None
    models = _active_models(clipped, partial)
    if not models:
        elapsed = time.perf_counter() - start
        if collect_censored:
            return base_estimate, censored_estimate, 0, 0.0, elapsed, 0.0
        return base_estimate, censored_estimate, 0, 0.0, elapsed

    clipped_region = clipped.any(axis=2) & usable
    roi_mask = ndimage.binary_dilation(clipped_region, structure=FOUR)
    roi_indices = np.flatnonzero(roi_mask.ravel())
    maximum = np.max(truth / thresholds.reshape(1, 1, 3), axis=2)
    train = usable & ~clipped.any(axis=2) & (maximum > 0.2)
    moments, weights = _filter_moments(truth, train, maximum, power, radius, roi_indices)
    sw = moments[0]
    neff = sw * sw / np.maximum(moments[1], 1.0e-300)
    local_valid = (neff >= MIN_EFFECTIVE_SAMPLES) & (sw >= MIN_WEIGHT_SUM)
    transport = CoefficientTransport(clipped_region, local_valid, roi_indices)
    roi_lookup = transport.roi_lookup
    flat_truth = truth.reshape(-1, 3)
    max_residual = 0.0
    censored_extra = 0.0
    sigma_floor = max(float(np.max(thresholds)) * SIGMA_FLOOR_RELATIVE, 1.0e-8)

    for target, predictors, target_indices in models:
        coefficients, valid = _fit_local(moments, target, predictors, local_valid)
        fallback = _global_fallback(truth, weights, target, predictors)
        coefficients[:, ~np.all(np.isfinite(coefficients), axis=0)] = fallback[:, None]
        # The window support gate is shared by every channel model. A non-finite fit is
        # replaced before transport so it cannot poison a Dirichlet boundary value.
        filled, residual = transport.fill(coefficients, fallback)
        max_residual = max(max_residual, residual)

        roi_rows = roi_lookup[target_indices]
        if np.any(roi_rows < 0):
            raise RuntimeError("a clipped evaluation quad is missing from the regression ROI")
        mu = filled[0, roi_rows].copy()
        for index, channel in enumerate(predictors):
            mu += filled[1 + index, roi_rows] * flat_truth[target_indices, channel]
        output_rows = eval_map[target_indices]
        base_estimate[output_rows, target] = np.maximum(mu, CLIP_FRACTION * thresholds[target])

        if collect_censored:
            extra_start = time.perf_counter()
            sigma = np.maximum(np.exp(filled[-1, roi_rows]), sigma_floor)
            alpha = np.clip((CLIP_FRACTION * thresholds[target] - mu) / sigma, -50.0, 50.0)
            mills = np.sqrt(2.0 / np.pi) / special.erfcx(alpha / np.sqrt(2.0))
            censored_estimate[output_rows, target] = np.maximum(
                mu + sigma * mills, CLIP_FRACTION * thresholds[target]
            )
            censored_extra += time.perf_counter() - extra_start

    elapsed = time.perf_counter() - start
    if collect_censored:
        return (base_estimate, censored_estimate, transport.unanchored_count, max_residual,
                elapsed, censored_extra)
    return base_estimate, censored_estimate, transport.unanchored_count, max_residual, elapsed


def weighted_global_fit(truth, target, predictors, sample_mask, weights, thresholds):
    flat_truth = truth.reshape(-1, 3)
    indices = np.flatnonzero(sample_mask.ravel())
    if indices.size == 0:
        return np.array([CLIP_FRACTION * thresholds[target]] + [0.0] * len(predictors)), 0
    w = weights.ravel()[indices].astype(np.float64)
    x = flat_truth[indices][:, predictors].astype(np.float64)
    y = flat_truth[indices, target].astype(np.float64)
    total = float(w.sum())
    if total <= 1.0e-20:
        return np.array([CLIP_FRACTION * thresholds[target]] + [0.0] * len(predictors)), int(indices.size)
    mean_x = np.sum(w[:, None] * x, axis=0) / total
    mean_y = float(np.sum(w * y) / total)
    xc = x - mean_x
    yc = y - mean_y
    covariance = (xc * w[:, None]).T @ xc
    cross = (xc * w[:, None]).T @ yc
    ridge = np.diag(np.maximum(np.diag(covariance), 0.0) * SLOPE_RIDGE)
    try:
        slopes = np.linalg.solve(covariance + ridge, cross)
    except np.linalg.LinAlgError:
        slopes = np.zeros(len(predictors), dtype=np.float64)
    intercept = mean_y - float(mean_x @ slopes)
    return np.concatenate(([intercept], slopes)), int(indices.size)


def global_regression(truth, clipped, usable, thresholds):
    start = time.perf_counter()
    partial, counts = clipped_partials(clipped, usable)
    partial_flat = np.flatnonzero(partial.ravel())
    estimate = truth.reshape(-1, 3)[partial_flat].copy()
    eval_map = np.full(partial.size, -1, dtype=np.int32)
    eval_map[partial_flat] = np.arange(partial_flat.size, dtype=np.int32)
    if not partial_flat.size:
        return estimate, 0, time.perf_counter() - start

    clipped_region = clipped.any(axis=2) & usable
    if clipped_region.any():
        near = ndimage.distance_transform_edt(~clipped_region) <= 3.0
    else:
        near = np.zeros(usable.shape, dtype=bool)
    maximum = np.max(truth / thresholds.reshape(1, 1, 3), axis=2)
    train = usable & ~clipped.any(axis=2) & (maximum > 0.2) & near
    weights = np.zeros(usable.shape, dtype=np.float64)
    weights[train] = np.power(maximum[train], 4, dtype=np.float64)
    model_count = 0
    flat_truth = truth.reshape(-1, 3)
    for target, predictors, target_indices in _active_models(clipped, partial):
        coefficients, model_count = weighted_global_fit(
            truth, target, predictors, train, weights, thresholds
        )
        mu = np.full(target_indices.size, coefficients[0], dtype=np.float64)
        for index, channel in enumerate(predictors):
            mu += coefficients[1 + index] * flat_truth[target_indices, channel]
        estimate[eval_map[target_indices], target] = np.maximum(
            mu, CLIP_FRACTION * thresholds[target]
        )
    return estimate, model_count, time.perf_counter() - start


def error_metrics(truth, clipped, partial_flat, estimate):
    true_eval = truth.reshape(-1, 3)[partial_flat].astype(np.float64)
    clip_eval = clipped.reshape(-1, 3)[partial_flat]
    estimate = estimate.astype(np.float64)
    count = clip_eval.sum(axis=1)
    result = {}
    for label, select in (
        ("all", np.ones(count.shape, dtype=bool)),
        ("one", count == 1),
        ("two", count == 2),
    ):
        chosen = np.flatnonzero(select)
        if chosen.size == 0:
            result[label] = None
            continue
        clipped_selected = clip_eval[chosen]
        log_errors = np.abs(np.log2(estimate[chosen][clipped_selected] /
                                    true_eval[chosen][clipped_selected]))
        true_vectors = true_eval[chosen]
        estimate_vectors = estimate[chosen]
        dot = np.sum(true_vectors * estimate_vectors, axis=1)
        norm = np.linalg.norm(true_vectors, axis=1) * np.linalg.norm(estimate_vectors, axis=1)
        cosine = np.divide(dot, norm, out=np.ones_like(dot), where=norm > 0.0)
        angles = np.degrees(np.arccos(np.clip(cosine, -1.0, 1.0)))
        result[label] = {
            "n_quads": int(chosen.size),
            "n_clipped_channels": int(clipped_selected.sum()),
            "log2_abs_mean": float(np.mean(log_errors)),
            "log2_abs_median": float(np.median(log_errors)),
            "angle_deg_mean": float(np.mean(angles)),
            "angle_deg_median": float(np.median(angles)),
        }
    return result


def process_pair(pair_id, workdir, scores):
    pair_start = time.perf_counter()
    ref = pathlib.Path(scores["ref"])
    okay, reason = bayer_pattern(ref)
    if not okay:
        return {"pair": pair_id, "scene": scene_kind(ref), "status": "skipped",
                "skip_reason": reason}, {}, 0.0

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
    clipped = truth >= (CLIP_FRACTION * thresholds.reshape(1, 1, 3))
    partial, clip_counts = clipped_partials(clipped, usable)
    full = usable & (clip_counts == 3)
    partial_flat = np.flatnonzero(partial.ravel())
    lower = CLIP_FRACTION * thresholds
    observed = observed_values(truth, clipped, lower)
    load_seconds = time.perf_counter() - load_start

    predictor_times = {}
    predictor_details = {}
    estimates = {}

    start = time.perf_counter()
    opp_estimate, offsets, _, _, _ = opposed_estimates(
        truth, observed, clipped, usable, thresholds
    )
    predictor_times["OPP"] = time.perf_counter() - start
    estimates["OPP"] = opp_estimate
    predictor_details["OPP"] = {"offsets": offsets}

    start = time.perf_counter()
    rho, border, bad_rho, hue_fallback, hue_residual = hue_field(truth, clipped, usable)
    hue_estimate = hue_estimates(
        truth, observed, clipped, rho, partial, partial_flat,
        _eval_map_from_partial(partial_flat, truth.size // 3), thresholds
    )
    predictor_times["HUE"] = time.perf_counter() - start
    estimates["HUE"] = hue_estimate
    predictor_details["HUE"] = {
        "border_quads": int(border.sum()),
        "bad_border_rho_fallbacks": bad_rho,
        "unanchored_hue_quads": hue_fallback,
        "laplace_relative_residual": hue_residual,
    }

    start = time.perf_counter()
    greg_estimate, greg_training_count, greg_seconds = global_regression(
        truth, clipped, usable, thresholds
    )
    predictor_times["GREG"] = greg_seconds
    estimates["GREG"] = greg_estimate
    predictor_details["GREG"] = {"training_samples": greg_training_count}

    local_residuals = {}
    local_unanchored = {}
    for radius in (8, 32, 128):
        for power in (0, 4):
            key = f"LREG(R={radius},p={power})"
            collect_censored = radius == 32 and power == 4
            result = local_regression(
                truth, clipped, usable, thresholds, radius, power, collect_censored
            )
            if collect_censored:
                base, censored, no_anchor, residual, elapsed, extra = result
                predictor_times[key] = max(0.0, elapsed - extra)
                predictor_times["LREG-T"] = elapsed
                estimates[key] = base
                estimates["LREG-T"] = censored
                local_residuals[key] = residual
                local_unanchored[key] = no_anchor
                predictor_details[key] = {"unanchored_coefficient_quads": no_anchor,
                                           "laplace_relative_residual": residual}
                predictor_details["LREG-T"] = {
                    "fit": "shared LREG(R=32,p=4)",
                    "unanchored_coefficient_quads": no_anchor,
                    "laplace_relative_residual": residual,
                }
            else:
                base, _, no_anchor, residual, elapsed = result
                predictor_times[key] = elapsed
                estimates[key] = base
                local_residuals[key] = residual
                local_unanchored[key] = no_anchor
                predictor_details[key] = {"unanchored_coefficient_quads": no_anchor,
                                           "laplace_relative_residual": residual}

    predictor_metrics = {
        name: error_metrics(truth, clipped, partial_flat, estimate)
        for name, estimate in estimates.items()
    }
    total_valid = int(usable.sum())
    counts = {
        "input_quads": int(original_shape[0] * original_shape[1]),
        "analyzed_quads": int(truth.shape[0] * truth.shape[1]),
        "darker_frame_clipped_quads": int(dark_clipped.sum()),
        "truth_quads": total_valid,
        "partly_clipped_quads": int(partial.sum()),
        "one_channel_clipped_quads": int((usable & (clip_counts == 1)).sum()),
        "two_channels_clipped_quads": int((usable & (clip_counts == 2)).sum()),
        "fully_clipped_quads": int(full.sum()),
        "clip_free_truth_quads": int((usable & (clip_counts == 0)).sum()),
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
        "evaluation_share_of_truth_quads": (
            float(partial.sum() / total_valid) if total_valid else 0.0
        ),
        "predictors": predictor_metrics,
        "predictor_details": predictor_details,
        "runtime_seconds": {**predictor_times, "load_truth_seconds": load_seconds,
                             "pair_total_seconds": time.perf_counter() - pair_start},
    }
    return record, predictor_times, time.perf_counter() - pair_start


def _eval_map_from_partial(partial_flat, n_quads):
    mapping = np.full(n_quads, -1, dtype=np.int32)
    mapping[partial_flat] = np.arange(partial_flat.size, dtype=np.int32)
    return mapping


def aggregate(records):
    successful = [item for item in records if item.get("status") == "ok"]
    scenes = sorted({item["scene"] for item in successful})
    groups = {scene: [item for item in successful if item["scene"] == scene] for scene in scenes}
    groups["overall"] = successful
    summary = {}
    for group_name, group in groups.items():
        summary[group_name] = {}
        for subset in ("all", "one", "two"):
            subset_result = {}
            for predictor in sorted({p for item in group for p in item["predictors"]}):
                values = [item["predictors"].get(predictor, {}).get(subset)
                          for item in group]
                values = [v for v in values if v is not None]
                if not values:
                    subset_result[predictor] = {"n_pairs": 0}
                    continue
                row = {
                    "n_pairs": len(values),
                    "median_pair_log2_abs_mean": float(np.median([v["log2_abs_mean"] for v in values])),
                    "median_pair_angle_deg_mean": float(np.median([v["angle_deg_mean"] for v in values])),
                }
                for baseline in ("OPP", "HUE"):
                    comparisons = []
                    for item in group:
                        result = item["predictors"].get(predictor, {}).get(subset)
                        base = item["predictors"].get(baseline, {}).get(subset)
                        if result is None or base is None:
                            continue
                        difference = result["angle_deg_mean"] - base["angle_deg_mean"]
                        if abs(difference) <= 0.1:
                            comparisons.append("tie")
                        elif difference < 0.0:
                            comparisons.append("win")
                        else:
                            comparisons.append("loss")
                    row[f"vs_{baseline}"] = {
                        "wins": comparisons.count("win"),
                        "ties": comparisons.count("tie"),
                        "losses": comparisons.count("loss"),
                        "n_pairs": len(comparisons),
                    }
                subset_result[predictor] = row
            summary[group_name][subset] = subset_result
    return summary


def fmt(value, digits=3):
    return "-" if value is None else f"{value:.{digits}f}"


def comparison_text(value):
    if not value:
        return "-"
    return f"{value['wins']}/{value['ties']}/{value['losses']}"


def write_report(path, output):
    records = output["pairs"]
    good = [item for item in records if item.get("status") == "ok"]
    skipped = [item for item in records if item.get("status") == "skipped"]
    errors = [item for item in records if item.get("status") == "error"]
    downsampled = [item for item in good if item["grid"]["downsample_factor"] > 1]
    valid_quads = sum(item["counts"]["truth_quads"] for item in good)
    partial_quads = sum(item["counts"]["partly_clipped_quads"] for item in good)
    full_quads = sum(item["counts"]["fully_clipped_quads"] for item in good)
    shares = [item["evaluation_share_of_truth_quads"] for item in good]
    lines = [
        "# Offline camera-space clipping predictor comparison",
        "",
        "The experiment synthetically clipped the darker raw frame in each included pair. It used the darker frame as truth at the brighter frame's exposure, then compared predictions on partly clipped quads.",
        "",
        "## Method and counts",
        "",
        f"The input list had {output['config']['included_pairs']} pairs. The script analyzed {len(good)}, skipped {len(skipped)} non-Bayer pairs, and recorded {len(errors)} pair errors.",
        f"It downsampled {len(downsampled)} pairs by averaging 2x2 quads before clipping when the input grid exceeded {DOWNSAMPLE_TRIGGER_QUADS:,} quads or more than {DOWNSAMPLE_TRIGGER_CLIPPED_QUADS:,} valid quads were clipped before downsampling. All predictors used the same grid for each pair.",
        f"Across successful pairs, {valid_quads:,} quads had valid darker-frame truth, {partial_quads:,} were partly clipped and evaluated, and {full_quads:,} were fully clipped and counted but not evaluated. The per-pair evaluated share of valid truth quads has median {fmt(float(np.median(shares)) if shares else None, 4)} and range {fmt(float(np.min(shares)) if shares else None, 4)} to {fmt(float(np.max(shares)) if shares else None, 4)}.",
        "OPP uses the cube-root opposed-channel reference and one per-channel offset. HUE Laplace-fills normalized RGB chromaticity from the four-neighbor border and estimates each missing channel from measured channels. LREG fits an intercept and one or two slopes in a square window, with weight `(max_j x_j/T[j])^p`; p=4 weights bright training samples more than p=0. GREG fits the p=4 model over training quads within three quads of any clipped quad. LREG-T applies the truncated-normal conditional mean to the LREG(R=32,p=4) fit.",
        f"It used `{output['config']['python_version']}`, one Python process, one OpenCV thread, one BLAS/OpenMP thread, and `nice -n 19`. Laplace systems used pyamg with tolerance {LAPLACE_TOL:g} and up to {LAPLACE_MAXITER} iterations.",
        f"Local fits required an effective weighted sample count of at least {MIN_EFFECTIVE_SAMPLES:g} and total weight of at least {MIN_WEIGHT_SUM:g}. Coefficient fields were harmonically filled over invalid windows inside the synthetic clipped region, using valid fits as Dirichlet values.",
        f"The radii 8, 32, and 128 are measured on the analyzed quad grid. For pairs downsampled by 2, each fit window therefore spans twice as many original quads in each direction. Each predictor replaces a clipped observed channel with its 0.98T lower-bound value.",
        "The tables show the median across pairs of each predictor's per-pair mean absolute log2 error and per-pair mean RGB angle. Angle comparisons use the per-pair mean angle. The final two columns count wins, ties within 0.1 degree, and losses against OPP and HUE.",
        "",
        "Log2 error is in stops. A lower angle means that the estimated RGB vector has a closer chromaticity direction to truth.",
        "",
    ]

    predictor_order = ["OPP", "HUE", "GREG", "LREG(R=8,p=0)", "LREG(R=8,p=4)",
                       "LREG(R=32,p=0)", "LREG(R=32,p=4)", "LREG-T",
                       "LREG(R=128,p=0)", "LREG(R=128,p=4)"]
    summary = output.get("summary", {})
    for subset, title in (("all", "All partly clipped quads"),
                          ("one", "Exactly one clipped channel"),
                          ("two", "Exactly two clipped channels")):
        lines.extend([f"## {title}", ""])
        for group_name in sorted((x for x in summary if x != "overall")) + ["overall"]:
            rows = summary[group_name].get(subset, {})
            if not rows or not any(row.get("n_pairs", 0) for row in rows.values()):
                continue
            lines.extend([
                f"### {group_name}",
                "",
                "| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |",
                "|---|---:|---:|---:|---:|---:|",
            ])
            for predictor in predictor_order:
                row = rows.get(predictor)
                if not row or not row.get("n_pairs"):
                    continue
                lines.append(
                    f"| {predictor} | {row['n_pairs']} | "
                    f"{fmt(row.get('median_pair_log2_abs_mean'))} | "
                    f"{fmt(row.get('median_pair_angle_deg_mean'))} | "
                    f"{comparison_text(row.get('vs_OPP'))} | {comparison_text(row.get('vs_HUE'))} |"
                )
            lines.append("")

    lines.extend(["## Runtime", "",
                  "Times below are summed over successful pairs. Each local fit includes moment filtering, regression, coefficient transport, and prediction. LREG-T reuses the LREG(R=32,p=4) fit in this batch; its standalone estimate adds that fit time to the censored-correction time. The standalone rows can therefore double-count shared work.",
                  "",
                  "| Predictor | Seconds |",
                  "|---|---:|"])
    for predictor in predictor_order:
        value = output["runtime_seconds"].get(predictor)
        if value is not None:
            lines.append(f"| {predictor} | {value:.2f} |")
    lines.append(f"| Total batch wall time | {output['runtime_seconds']['batch_wall_seconds']:.2f} |")
    lines.extend(["", "## Skipped and downsampled pairs", ""])
    if skipped:
        lines.append(f"Skipped non-Bayer pairs ({len(skipped)}): " + ", ".join(
            f"`{item['pair']}` ({item['skip_reason']})" for item in skipped
        ) + ".")
    else:
        lines.append("Skipped non-Bayer pairs: none.")
    if errors:
        lines.append("Pairs that failed: " + ", ".join(
            f"`{item['pair']}` ({item['error']})" for item in errors
        ) + ".")
    if downsampled:
        lines.append(f"Downsampled pairs ({len(downsampled)}, all by a factor of 2):")
        lines.extend(f"- `{item['pair']}`" for item in downsampled)
    else:
        lines.append("Downsampled pairs: none.")

    lines.extend([
        "",
        "## Limits",
        "",
        "This is a synthetic-clipping test. It does not show that any predictor reconstructs the true scene inside a real saturated region. It uses 2x2 Bayer quad means, not individual photosite reconstructions, and it skips non-Bayer raws.",
        "The HUE baseline uses the requested unsmoothed boundary chromaticity and a discrete four-neighbor Laplace fill. The OPP baseline uses the cube-root opposed estimate and one per-channel offset. Both are quad approximations of darktable's per-photosite processing.",
        "The experiment did not compare rendered images, edge appearance, noise, demosaicing behavior, X-Trans behavior, OpenCL, or darktable runtime. It did not verify that exposure-bracket registration or scene motion leaves every reference quad valid beyond the specified darker-frame clipping exclusion.",
        "",
        "Per-pair clipping counts, evaluation shares, error means and medians, predictor details, solver residuals, and runtimes are in `predict.json`.",
        "",
    ])
    path.write_text("\n".join(lines))


def run(max_pairs=None, resume=False, redo_pairs=None):
    included = [line.strip() for line in INCLUDED.read_text().splitlines() if line.strip()]
    if max_pairs is not None:
        included = included[:max_pairs]
    records = []
    runtime_totals = {}
    prior_pair_seconds = 0.0
    redo_pairs = set(redo_pairs or [])
    batch_start = time.perf_counter()
    config = {
        "included_pairs": len(included),
        "downsample_trigger_quads": DOWNSAMPLE_TRIGGER_QUADS,
        "downsample_trigger_clipped_quads": DOWNSAMPLE_TRIGGER_CLIPPED_QUADS,
        "dark_frame_photosite_clip_fraction": DARKER_FRAME_EXCLUSION,
        "synthetic_clip_fraction": CLIP_FRACTION,
        "minimum_effective_samples": MIN_EFFECTIVE_SAMPLES,
        "minimum_weight_sum": MIN_WEIGHT_SUM,
        "slope_ridge_fraction": SLOPE_RIDGE,
        "laplace_tolerance": LAPLACE_TOL,
        "laplace_max_iterations": LAPLACE_MAXITER,
        "python_version": sys.version.split()[0],
        "pyamg_version": pyamg.__version__,
    }

    if resume:
        if max_pairs is not None:
            raise ValueError("--resume cannot be combined with --max-pairs")
        previous_path = HERE / "predict.json"
        if not previous_path.exists():
            raise FileNotFoundError("--resume needs an existing predict.json checkpoint")
        previous = json.loads(previous_path.read_text())
        if previous.get("complete") or previous.get("config", {}).get("included_pairs") != len(included):
            raise ValueError("predict.json is not an incomplete checkpoint for this 103-pair run")
        included_set = set(included)
        records = [item for item in previous.get("pairs", [])
                   if item.get("pair") in included_set
                   and item.get("pair") not in redo_pairs
                   and item.get("status") != "error"]
        saved = {item["pair"] for item in records}
        if not redo_pairs.issubset(included_set):
            raise ValueError("--redo-pair contains a pair outside included.txt")
        if not redo_pairs.issubset({item.get("pair") for item in previous.get("pairs", [])}):
            raise ValueError("--redo-pair must name a pair present in the checkpoint")
        prior_pair_seconds = sum(
            item.get("runtime_seconds", {}).get("pair_total_seconds", 0.0)
            for item in records if item.get("status") == "ok"
        )
        for item in records:
            if item.get("status") != "ok":
                continue
            for name, seconds in item.get("runtime_seconds", {}).items():
                if name not in ("load_truth_seconds", "pair_total_seconds"):
                    runtime_totals[name] = runtime_totals.get(name, 0.0) + seconds
        print(f"Resuming with {len(saved)} saved pairs; recomputing {len(redo_pairs)} requested pairs.",
              flush=True)

    for index, pair_id in enumerate(included, 1):
        if pair_id in {item["pair"] for item in records}:
            continue
        workdir = RUNS / pair_id
        scores_path = workdir / "scores.json"
        try:
            scores = json.loads(scores_path.read_text())
            record, timings, elapsed = process_pair(pair_id, workdir, scores)
            records.append(record)
            if record.get("status") == "ok":
                for name, seconds in timings.items():
                    runtime_totals[name] = runtime_totals.get(name, 0.0) + seconds
                for name, seconds in record["runtime_seconds"].items():
                    if name in ("load_truth_seconds", "pair_total_seconds"):
                        continue
                    if name not in timings:
                        runtime_totals[name] = runtime_totals.get(name, 0.0) + seconds
            status = record["status"]
            count_text = ""
            if status == "ok":
                count_text = (f" partial={record['counts']['partly_clipped_quads']}"
                              f" full={record['counts']['fully_clipped_quads']}"
                              f" factor={record['grid']['downsample_factor']}")
            print(f"[{index}/{len(included)}] {pair_id}: {status}{count_text} {elapsed:.1f}s", flush=True)
        except Exception as error:
            records.append({"pair": pair_id, "scene": "unknown", "status": "error",
                            "error": f"{type(error).__name__}: {error}"})
            print(f"[{index}/{len(included)}] {pair_id}: ERROR {type(error).__name__}: {error}",
                  file=sys.stderr, flush=True)
            traceback.print_exc()

        partial_output = {
            "config": config,
            "pairs": records,
            "runtime_seconds": runtime_totals,
            "summary": aggregate(records),
            "complete": False,
        }
        json_write_atomic(HERE / "predict.json", partial_output)

    records.sort(key=lambda item: included.index(item["pair"]))
    runtime_totals["batch_wall_seconds"] = prior_pair_seconds + time.perf_counter() - batch_start
    output = {
        "config": config,
        "pairs": records,
        "runtime_seconds": runtime_totals,
        "summary": aggregate(records),
        "complete": len(records) == len(included),
    }
    json_write_atomic(HERE / "predict.json", output)
    write_report(HERE / "answer.md", output)
    print(f"Wrote {HERE / 'answer.md'} and {HERE / 'predict.json'}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-pairs", type=int, help="process only the first N included pairs")
    parser.add_argument("--resume", action="store_true", help="continue an incomplete predict.json checkpoint")
    parser.add_argument("--redo-pair", action="append", default=[],
                        help="recompute a named pair from the checkpoint; may be repeated")
    parser.add_argument("--report-only", action="store_true",
                        help="regenerate answer.md from the completed predict.json")
    args = parser.parse_args()
    if args.report_only:
        write_report(HERE / "answer.md", json.loads((HERE / "predict.json").read_text()))
        print(f"Wrote {HERE / 'answer.md'}", flush=True)
    else:
        run(args.max_pairs, args.resume, args.redo_pair)
