# Gate the robust dome by the image's clipped share

**Measured:** use the robust linear dome only when **more than 19% of image pixels have any clipping**:

```text
c = mean(F.max(axis=2) > 0)
factor = 1 if c > 0.19 else 0
predicted region rise = factor * robust_linear_dome_rise
```

This preserves g057, g213, and g315's gains and disables the dome on the three named city skies. **Inference:** widespread clipping is a useful cue here. Every region in an image gets the same decision.

Sample: 468 eight-connected regions from 58 pairs and 55 scenes, containing 5,056,006 truth-valid pixels. Each region has at least 200 truth-valid pixels. Truth exclusion matches `deficit2.py`; render scaling and luminance weights follow the task.

Error is `sum(n * abs(factor * median_dome_rise - median_truth_rise)) / sum(n)`, where `n` counts truth-valid pixels. Both median log2 rises use those pixels. This measures region medians, not pixelwise or perceptual error.

## Features and validation

I tested 36 features with one-threshold gates and fitted multipliers. Leave-one-scene-out validation groups names before the last two frame numbers. Thresholds are fitted within training; lower error is better.

| Feature tried | Held-out gate error, EV | Predictive value here |
|---|---:|---|
| Image share with any clipping | 0.238 | Chosen in all 55 folds when feature selection also uses only training data |
| Image fully clipped share | 0.240 | Similar cue |
| Image median / mean luminance | 0.237 / 0.272 | Median competitive; mean weaker |
| Rim luminance slope / log slope | 0.245 / 0.245 | Useful, weaker than clipped share |
| Free share of rim at robust threshold 0.8 | 0.244 | Useful, weaker than clipped share |
| Unclipped 4-pixel rim's blue share | 0.247 | Some value |
| Region / unclipped 16-pixel rim saturation | 0.337 / 0.323 | Poor transfer |
| Area / bounding-box fill ratio | 0.279 / 0.316 | Weak |
| Partly clipped share of outside border | 0.310 | Weak |

Slope uses `lift2.py`'s two-ring algorithm. Saturation is `(max-min)/max` on nonnegative RGB. Blue share counts `(2B-R-G)/(R+G+B) > 0.15`. Additional features include aspect, border density, color at two rim widths, brighter rim subsets, luminance levels, and edge contact. Predictors use only opposed and masks; see `regions.csv` and `analysis.json`.

Held-out error is **0.238 EV**, versus **0.308** for opposed and **0.276** for the robust dome: improvements of 23% and 14%. A global multiplier gives 0.305; an EV cap gives 0.245. The full-sample threshold is 18.65%, rounded to 19%; fold thresholds span 16.61–21.67%. The fixed rule's descriptive full-sample error is 0.235. Tested thresholds between 15% and 22% give 0.235–0.237.

## Effect by scene type

| Type | Regions | Opposed | Robust linear dome | Gate, held-out | Expected effect from measurements |
|---|---:|---:|---:|---:|---|
| city | 84 | 0.213 | 0.229 | 0.136 | Removes the named sky overshoots |
| colored | 139 | 0.494 | 0.438 | 0.438 | Retains aggregate gain |
| indoor | 30 | 0.302 | 0.260 | 0.302 | Gives up useful lamp gains |
| night | 7 | 0.501 | 0.499 | 0.501 | Almost no effect |
| phone | 22 | 0.075 | 0.129 | 0.109 | Still worse than opposed in validation |
| sky | 106 | 0.323 | 0.193 | 0.202 | Retains most aggregate cloud gain |
| specular | 80 | 0.235 | 0.203 | 0.203 | Retains aggregate gain |

All errors are pixel-weighted EV. For the fixed 19% rule, the named scenes give:

| Scene | Any-clipped share | Opposed error | Robust error | Rule error |
|---|---:|---:|---:|---:|
| g057 | 26.8% | 0.308 | 0.256 | 0.256 |
| g213 | 33.5% | 0.378 | 0.298 | 0.298 |
| g315 | 37.8% | 0.406 | 0.002 | 0.002 |
| t07, 0624–0620 | 12.7% | 0.187 | 0.269 | 0.187 |
| t20, 0715–0711 | 9.9% | 0.124 | 0.313 | 0.124 |
| t47, 0904–0900 | 13.9% | 0.054 | 0.897 | 0.054 |

With all six scenes withheld together, the clipped-share gate fits a 19.10% threshold and scores 0.123 EV on them, versus 0.230 for opposed and 0.368 for the robust dome. However, unrestricted feature selection then chooses saturation and fails at 0.368 EV. Feature choice remains dataset-dependent.

## Other domes and limits

| Variant | Unscaled error | Clipped-share gate, held-out |
|---|---:|---:|
| Robust linear / log | 0.276 / 0.300 | 0.238 / 0.235 |
| Poisson linear / log | 0.281 / 0.257 | 0.284 / 0.261 |
| Distance linear / log | 0.211 / 0.227 | 0.217 / 0.232 |

Distance linear plus the fixed gate scores 0.183 in-sample. It improves g213 more but raises g315's error to 0.173 from robust linear's 0.002. I retain robust linear to preserve that gain. Selecting both variant and threshold within training gives 0.267 held-out, indicating instability. Results for all 15 existing variants are in `analysis.json`.

The brightest cores are censored: only 5.06 of 6.56 million retained-region pixels have truth, with coverage as low as 0.71% in one region. Core brightness remains unknown. Weighting by entire region area raises fixed-rule error to 0.254 EV. The gate depends on framing and masks and cannot distinguish objects within an image. No new renders or perceptual evaluation were produced. Truth and mask hashes matched after extraction for all 103 pairs; no variants were missing.

Files written, all in this directory:

- Scripts: `collect.py`, `analyze.py`, `final_eval.py`, `verify.py`, `run.sh`.
- Measurements: `regions.json`, `regions.csv`, `inventory.json`.
- Results: `analysis.json`, `folds.json`, `predictions.csv`, `rule_results.json`, `rule_predictions.csv`, `verification.json`.
- Logs: `collect.log`, `analysis.log`, `rule_results.log`, `verification.log`.
- Report: `answer.md`.

Reproduce with `sh run.sh`: requested Python, nice 19, `OMP_NUM_THREADS=4`, single-thread BLAS and OpenCV. No workdirs or other directories were modified; no commits or pushes.
