# Offline camera-space clipping predictor comparison

The experiment synthetically clipped the darker raw frame in each included pair. It used the darker frame as truth at the brighter frame's exposure, then compared predictions on partly clipped quads.

## Method and counts

The input list had 103 pairs. The script analyzed 103, skipped 0 non-Bayer pairs, and recorded 0 pair errors.
It downsampled 88 pairs by averaging 2x2 quads before clipping when the input grid exceeded 3,000,000 quads or more than 200,000 valid quads were clipped before downsampling. All predictors used the same grid for each pair.
Across successful pairs, 127,675,437 quads had valid darker-frame truth, 11,244,348 were partly clipped and evaluated, and 1,955,039 were fully clipped and counted but not evaluated. The per-pair evaluated share of valid truth quads has median 0.0880 and range 0.0016 to 0.4782.
OPP uses the cube-root opposed-channel reference and one per-channel offset. HUE Laplace-fills normalized RGB chromaticity from the four-neighbor border and estimates each missing channel from measured channels. LREG fits an intercept and one or two slopes in a square window, with weight `(max_j x_j/T[j])^p`; p=4 weights bright training samples more than p=0. GREG fits the p=4 model over training quads within three quads of any clipped quad. LREG-T applies the truncated-normal conditional mean to the LREG(R=32,p=4) fit.
It used `3.13.12`, one Python process, one OpenCV thread, one BLAS/OpenMP thread, and `nice -n 19`. Laplace systems used pyamg with tolerance 1e-07 and up to 100 iterations.
Local fits required an effective weighted sample count of at least 32 and total weight of at least 1. Coefficient fields were harmonically filled over invalid windows inside the synthetic clipped region, using valid fits as Dirichlet values.
The radii 8, 32, and 128 are measured on the analyzed quad grid. For pairs downsampled by 2, each fit window therefore spans twice as many original quads in each direction. Each predictor replaces a clipped observed channel with its 0.98T lower-bound value.
The tables show the median across pairs of each predictor's per-pair mean absolute log2 error and per-pair mean RGB angle. Angle comparisons use the per-pair mean angle. The final two columns count wins, ties within 0.1 degree, and losses against OPP and HUE.

Log2 error is in stops. A lower angle means that the estimated RGB vector has a closer chromaticity direction to truth.

## All partly clipped quads

### city

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 41 | 0.035 | 0.633 | 0/41/0 | 28/3/10 |
| HUE | 41 | 0.098 | 1.694 | 10/3/28 | 0/41/0 |
| GREG | 41 | 0.127 | 2.225 | 0/8/33 | 11/6/24 |
| LREG(R=8,p=0) | 41 | 0.113 | 1.860 | 8/6/27 | 10/8/23 |
| LREG(R=8,p=4) | 41 | 0.158 | 2.544 | 3/2/36 | 2/7/32 |
| LREG(R=32,p=0) | 41 | 0.112 | 1.882 | 8/6/27 | 7/9/25 |
| LREG(R=32,p=4) | 41 | 0.122 | 2.292 | 4/5/32 | 5/8/28 |
| LREG-T | 41 | 0.116 | 2.279 | 5/4/32 | 5/8/28 |
| LREG(R=128,p=0) | 41 | 0.097 | 1.792 | 6/3/32 | 16/5/20 |
| LREG(R=128,p=4) | 41 | 0.103 | 2.085 | 3/6/32 | 17/5/19 |

### colored

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 14 | 0.117 | 2.642 | 0/14/0 | 7/1/6 |
| HUE | 14 | 0.120 | 2.094 | 6/1/7 | 0/14/0 |
| GREG | 14 | 0.267 | 4.349 | 1/0/13 | 0/0/14 |
| LREG(R=8,p=0) | 14 | 0.148 | 2.289 | 5/0/9 | 1/2/11 |
| LREG(R=8,p=4) | 14 | 0.189 | 2.946 | 3/0/11 | 0/2/12 |
| LREG(R=32,p=0) | 14 | 0.159 | 2.333 | 4/0/10 | 2/2/10 |
| LREG(R=32,p=4) | 14 | 0.189 | 3.097 | 3/0/11 | 0/2/12 |
| LREG-T | 14 | 0.168 | 3.054 | 3/0/11 | 2/2/10 |
| LREG(R=128,p=0) | 14 | 0.145 | 2.346 | 5/0/9 | 1/2/11 |
| LREG(R=128,p=4) | 14 | 0.199 | 2.980 | 3/0/11 | 0/1/13 |

### indoor

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 8 | 0.070 | 1.267 | 0/8/0 | 2/1/5 |
| HUE | 8 | 0.064 | 1.084 | 5/1/2 | 0/8/0 |
| GREG | 8 | 0.151 | 2.195 | 0/0/8 | 1/0/7 |
| LREG(R=8,p=0) | 8 | 0.068 | 1.058 | 4/1/3 | 2/1/5 |
| LREG(R=8,p=4) | 8 | 0.094 | 1.535 | 1/2/5 | 1/1/6 |
| LREG(R=32,p=0) | 8 | 0.061 | 0.912 | 5/0/3 | 2/1/5 |
| LREG(R=32,p=4) | 8 | 0.071 | 1.004 | 5/0/3 | 2/1/5 |
| LREG-T | 8 | 0.065 | 0.962 | 5/0/3 | 2/1/5 |
| LREG(R=128,p=0) | 8 | 0.060 | 0.952 | 5/1/2 | 2/3/3 |
| LREG(R=128,p=4) | 8 | 0.077 | 1.172 | 4/1/3 | 2/1/5 |

### night

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 13 | 0.250 | 4.438 | 0/13/0 | 0/1/12 |
| HUE | 13 | 0.132 | 2.454 | 12/1/0 | 0/13/0 |
| GREG | 13 | 0.404 | 5.904 | 2/1/10 | 0/0/13 |
| LREG(R=8,p=0) | 13 | 0.205 | 3.415 | 10/0/3 | 1/0/12 |
| LREG(R=8,p=4) | 13 | 0.261 | 4.398 | 6/3/4 | 0/0/13 |
| LREG(R=32,p=0) | 13 | 0.176 | 2.911 | 10/1/2 | 2/1/10 |
| LREG(R=32,p=4) | 13 | 0.241 | 4.025 | 8/2/3 | 1/0/12 |
| LREG-T | 13 | 0.226 | 3.813 | 9/1/3 | 1/1/11 |
| LREG(R=128,p=0) | 13 | 0.149 | 2.389 | 11/1/1 | 3/2/8 |
| LREG(R=128,p=4) | 13 | 0.182 | 2.852 | 7/1/5 | 3/0/10 |

### phone

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 9 | 0.107 | 1.683 | 0/9/0 | 3/2/4 |
| HUE | 9 | 0.105 | 2.003 | 4/2/3 | 0/9/0 |
| GREG | 9 | 0.213 | 3.618 | 0/1/8 | 1/0/8 |
| LREG(R=8,p=0) | 9 | 0.174 | 3.067 | 2/0/7 | 0/1/8 |
| LREG(R=8,p=4) | 9 | 0.224 | 3.932 | 0/0/9 | 0/1/8 |
| LREG(R=32,p=0) | 9 | 0.187 | 3.308 | 2/1/6 | 1/0/8 |
| LREG(R=32,p=4) | 9 | 0.232 | 3.971 | 0/1/8 | 1/0/8 |
| LREG-T | 9 | 0.209 | 3.803 | 1/1/7 | 1/0/8 |
| LREG(R=128,p=0) | 9 | 0.128 | 1.968 | 3/1/5 | 2/0/7 |
| LREG(R=128,p=4) | 9 | 0.182 | 3.066 | 0/2/7 | 1/0/8 |

### sky

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 12 | 0.074 | 1.099 | 0/12/0 | 8/2/2 |
| HUE | 12 | 0.112 | 1.890 | 2/2/8 | 0/12/0 |
| GREG | 12 | 0.177 | 2.966 | 0/0/12 | 0/0/12 |
| LREG(R=8,p=0) | 12 | 0.163 | 2.792 | 2/0/10 | 2/0/10 |
| LREG(R=8,p=4) | 12 | 0.205 | 3.659 | 0/1/11 | 1/1/10 |
| LREG(R=32,p=0) | 12 | 0.159 | 2.681 | 3/1/8 | 2/0/10 |
| LREG(R=32,p=4) | 12 | 0.180 | 3.140 | 1/0/11 | 1/1/10 |
| LREG-T | 12 | 0.178 | 3.134 | 2/1/9 | 2/1/9 |
| LREG(R=128,p=0) | 12 | 0.106 | 1.656 | 1/3/8 | 3/2/7 |
| LREG(R=128,p=4) | 12 | 0.121 | 1.966 | 1/1/10 | 3/2/7 |

### specular

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 6 | 0.097 | 1.684 | 0/6/0 | 2/1/3 |
| HUE | 6 | 0.088 | 1.462 | 3/1/2 | 0/6/0 |
| GREG | 6 | 0.158 | 2.540 | 0/0/6 | 0/0/6 |
| LREG(R=8,p=0) | 6 | 0.110 | 1.945 | 1/0/5 | 0/0/6 |
| LREG(R=8,p=4) | 6 | 0.132 | 2.169 | 1/0/5 | 0/0/6 |
| LREG(R=32,p=0) | 6 | 0.104 | 1.758 | 2/0/4 | 0/1/5 |
| LREG(R=32,p=4) | 6 | 0.120 | 1.895 | 1/1/4 | 0/0/6 |
| LREG-T | 6 | 0.111 | 1.840 | 1/2/3 | 0/0/6 |
| LREG(R=128,p=0) | 6 | 0.098 | 1.553 | 2/1/3 | 0/3/3 |
| LREG(R=128,p=4) | 6 | 0.125 | 1.904 | 1/1/4 | 0/1/5 |

### overall

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 103 | 0.087 | 1.499 | 0/103/0 | 50/11/42 |
| HUE | 103 | 0.100 | 1.694 | 42/11/50 | 0/103/0 |
| GREG | 103 | 0.188 | 3.279 | 3/10/90 | 13/6/84 |
| LREG(R=8,p=0) | 103 | 0.138 | 2.271 | 32/7/64 | 16/12/75 |
| LREG(R=8,p=4) | 103 | 0.187 | 3.159 | 14/8/81 | 4/12/87 |
| LREG(R=32,p=0) | 103 | 0.133 | 2.349 | 34/9/60 | 16/14/73 |
| LREG(R=32,p=4) | 103 | 0.164 | 2.733 | 22/9/72 | 10/12/81 |
| LREG-T | 103 | 0.151 | 2.634 | 26/9/68 | 13/13/77 |
| LREG(R=128,p=0) | 103 | 0.112 | 1.813 | 33/10/60 | 27/17/59 |
| LREG(R=128,p=4) | 103 | 0.140 | 2.302 | 19/12/72 | 26/10/67 |

## Exactly one clipped channel

### city

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 41 | 0.023 | 0.420 | 0/41/0 | 35/5/1 |
| HUE | 41 | 0.053 | 0.977 | 1/5/35 | 0/41/0 |
| GREG | 41 | 0.042 | 0.780 | 4/11/26 | 20/8/13 |
| LREG(R=8,p=0) | 41 | 0.061 | 1.088 | 2/7/32 | 12/12/17 |
| LREG(R=8,p=4) | 41 | 0.066 | 1.182 | 0/5/36 | 9/7/25 |
| LREG(R=32,p=0) | 41 | 0.051 | 0.931 | 3/12/26 | 19/8/14 |
| LREG(R=32,p=4) | 41 | 0.056 | 1.021 | 1/11/29 | 12/13/16 |
| LREG-T | 41 | 0.056 | 0.996 | 1/11/29 | 13/13/15 |
| LREG(R=128,p=0) | 41 | 0.030 | 0.554 | 3/13/25 | 28/7/6 |
| LREG(R=128,p=4) | 41 | 0.035 | 0.640 | 2/14/25 | 24/11/6 |

### colored

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 14 | 0.067 | 1.265 | 0/14/0 | 8/2/4 |
| HUE | 14 | 0.065 | 1.219 | 4/2/8 | 0/14/0 |
| GREG | 14 | 0.107 | 1.897 | 1/1/12 | 0/4/10 |
| LREG(R=8,p=0) | 14 | 0.089 | 1.466 | 4/2/8 | 3/5/6 |
| LREG(R=8,p=4) | 14 | 0.101 | 1.645 | 3/1/10 | 1/3/10 |
| LREG(R=32,p=0) | 14 | 0.068 | 1.171 | 4/2/8 | 5/6/3 |
| LREG(R=32,p=4) | 14 | 0.080 | 1.483 | 3/0/11 | 2/4/8 |
| LREG-T | 14 | 0.075 | 1.345 | 3/3/8 | 3/4/7 |
| LREG(R=128,p=0) | 14 | 0.049 | 0.880 | 3/7/4 | 7/3/4 |
| LREG(R=128,p=4) | 14 | 0.069 | 1.287 | 3/2/9 | 4/5/5 |

### indoor

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 8 | 0.048 | 0.882 | 0/8/0 | 4/0/4 |
| HUE | 8 | 0.041 | 0.773 | 4/0/4 | 0/8/0 |
| GREG | 8 | 0.059 | 1.083 | 1/3/4 | 1/1/6 |
| LREG(R=8,p=0) | 8 | 0.043 | 0.795 | 4/1/3 | 3/1/4 |
| LREG(R=8,p=4) | 8 | 0.067 | 1.200 | 2/2/4 | 1/1/6 |
| LREG(R=32,p=0) | 8 | 0.030 | 0.549 | 5/1/2 | 3/2/3 |
| LREG(R=32,p=4) | 8 | 0.028 | 0.524 | 5/1/2 | 2/3/3 |
| LREG-T | 8 | 0.028 | 0.514 | 5/1/2 | 2/3/3 |
| LREG(R=128,p=0) | 8 | 0.026 | 0.483 | 6/0/2 | 3/4/1 |
| LREG(R=128,p=4) | 8 | 0.027 | 0.502 | 5/1/2 | 2/3/3 |

### night

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 13 | 0.078 | 1.416 | 0/13/0 | 1/2/10 |
| HUE | 13 | 0.060 | 1.077 | 10/2/1 | 0/13/0 |
| GREG | 13 | 0.155 | 2.843 | 1/0/12 | 0/1/12 |
| LREG(R=8,p=0) | 13 | 0.059 | 1.021 | 6/4/3 | 4/3/6 |
| LREG(R=8,p=4) | 13 | 0.073 | 1.335 | 5/2/6 | 0/0/13 |
| LREG(R=32,p=0) | 13 | 0.046 | 0.848 | 9/3/1 | 6/3/4 |
| LREG(R=32,p=4) | 13 | 0.055 | 1.004 | 8/2/3 | 2/6/5 |
| LREG-T | 13 | 0.052 | 0.925 | 8/2/3 | 6/2/5 |
| LREG(R=128,p=0) | 13 | 0.048 | 0.887 | 10/3/0 | 6/2/5 |
| LREG(R=128,p=4) | 13 | 0.053 | 0.931 | 8/3/2 | 3/4/6 |

### phone

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 9 | 0.051 | 0.916 | 0/9/0 | 2/3/4 |
| HUE | 9 | 0.034 | 0.621 | 4/3/2 | 0/9/0 |
| GREG | 9 | 0.052 | 0.961 | 2/3/4 | 2/1/6 |
| LREG(R=8,p=0) | 9 | 0.060 | 1.097 | 2/1/6 | 2/1/6 |
| LREG(R=8,p=4) | 9 | 0.094 | 1.703 | 0/2/7 | 0/1/8 |
| LREG(R=32,p=0) | 9 | 0.054 | 0.984 | 2/1/6 | 2/1/6 |
| LREG(R=32,p=4) | 9 | 0.076 | 1.321 | 2/1/6 | 2/1/6 |
| LREG-T | 9 | 0.075 | 1.314 | 2/1/6 | 2/2/5 |
| LREG(R=128,p=0) | 9 | 0.032 | 0.583 | 5/3/1 | 3/5/1 |
| LREG(R=128,p=4) | 9 | 0.046 | 0.842 | 3/3/3 | 3/3/3 |

### sky

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 12 | 0.034 | 0.625 | 0/12/0 | 7/4/1 |
| HUE | 12 | 0.037 | 0.696 | 1/4/7 | 0/12/0 |
| GREG | 12 | 0.055 | 1.008 | 2/0/10 | 1/1/10 |
| LREG(R=8,p=0) | 12 | 0.073 | 1.346 | 2/0/10 | 4/1/7 |
| LREG(R=8,p=4) | 12 | 0.099 | 1.792 | 1/1/10 | 2/1/9 |
| LREG(R=32,p=0) | 12 | 0.065 | 1.205 | 2/2/8 | 3/2/7 |
| LREG(R=32,p=4) | 12 | 0.085 | 1.569 | 1/3/8 | 2/1/9 |
| LREG-T | 12 | 0.084 | 1.529 | 1/4/7 | 3/0/9 |
| LREG(R=128,p=0) | 12 | 0.034 | 0.605 | 3/3/6 | 7/1/4 |
| LREG(R=128,p=4) | 12 | 0.038 | 0.704 | 3/1/8 | 3/4/5 |

### specular

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 6 | 0.033 | 0.611 | 0/6/0 | 3/3/0 |
| HUE | 6 | 0.047 | 0.872 | 0/3/3 | 0/6/0 |
| GREG | 6 | 0.064 | 1.182 | 0/0/6 | 1/1/4 |
| LREG(R=8,p=0) | 6 | 0.057 | 1.066 | 0/1/5 | 0/2/4 |
| LREG(R=8,p=4) | 6 | 0.059 | 1.092 | 0/0/6 | 0/0/6 |
| LREG(R=32,p=0) | 6 | 0.047 | 0.872 | 1/2/3 | 2/1/3 |
| LREG(R=32,p=4) | 6 | 0.056 | 1.042 | 0/1/5 | 0/2/4 |
| LREG-T | 6 | 0.052 | 0.975 | 0/1/5 | 1/1/4 |
| LREG(R=128,p=0) | 6 | 0.039 | 0.720 | 1/4/1 | 4/2/0 |
| LREG(R=128,p=4) | 6 | 0.041 | 0.755 | 0/3/3 | 2/3/1 |

### overall

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 103 | 0.038 | 0.691 | 0/103/0 | 60/19/24 |
| HUE | 103 | 0.051 | 0.946 | 24/19/60 | 0/103/0 |
| GREG | 103 | 0.067 | 1.235 | 11/18/74 | 25/17/61 |
| LREG(R=8,p=0) | 103 | 0.061 | 1.126 | 20/16/67 | 28/25/50 |
| LREG(R=8,p=4) | 103 | 0.073 | 1.324 | 11/13/79 | 13/13/77 |
| LREG(R=32,p=0) | 103 | 0.051 | 0.945 | 26/23/54 | 40/23/40 |
| LREG(R=32,p=4) | 103 | 0.057 | 1.046 | 20/19/64 | 22/30/51 |
| LREG-T | 103 | 0.054 | 0.992 | 20/23/60 | 30/25/48 |
| LREG(R=128,p=0) | 103 | 0.040 | 0.729 | 31/33/39 | 58/24/21 |
| LREG(R=128,p=4) | 103 | 0.045 | 0.794 | 24/27/52 | 41/33/29 |

## Exactly two clipped channels

### city

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 34 | 0.112 | 2.254 | 0/34/0 | 21/2/11 |
| HUE | 34 | 0.208 | 4.316 | 11/2/21 | 0/34/0 |
| GREG | 34 | 0.395 | 10.647 | 0/1/33 | 3/1/30 |
| LREG(R=8,p=0) | 34 | 0.224 | 4.938 | 10/1/23 | 11/5/18 |
| LREG(R=8,p=4) | 34 | 0.342 | 8.321 | 3/1/30 | 2/1/31 |
| LREG(R=32,p=0) | 34 | 0.254 | 5.901 | 9/2/23 | 3/4/27 |
| LREG(R=32,p=4) | 34 | 0.308 | 7.478 | 4/2/28 | 3/1/30 |
| LREG-T | 34 | 0.286 | 7.274 | 5/0/29 | 3/0/31 |
| LREG(R=128,p=0) | 34 | 0.318 | 7.011 | 6/1/27 | 4/1/29 |
| LREG(R=128,p=4) | 34 | 0.329 | 8.613 | 3/1/30 | 2/1/31 |

### colored

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 14 | 0.190 | 4.198 | 0/14/0 | 8/1/5 |
| HUE | 14 | 0.205 | 4.465 | 5/1/8 | 0/14/0 |
| GREG | 14 | 0.467 | 10.193 | 0/1/13 | 0/0/14 |
| LREG(R=8,p=0) | 14 | 0.239 | 5.425 | 4/0/10 | 1/0/13 |
| LREG(R=8,p=4) | 14 | 0.339 | 6.422 | 1/0/13 | 0/1/13 |
| LREG(R=32,p=0) | 14 | 0.268 | 5.708 | 3/0/11 | 1/0/13 |
| LREG(R=32,p=4) | 14 | 0.328 | 6.961 | 1/1/12 | 1/0/13 |
| LREG-T | 14 | 0.306 | 6.814 | 2/0/12 | 1/0/13 |
| LREG(R=128,p=0) | 14 | 0.299 | 7.186 | 3/0/11 | 1/1/12 |
| LREG(R=128,p=4) | 14 | 0.366 | 7.706 | 1/0/13 | 0/0/14 |

### indoor

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 8 | 0.133 | 3.388 | 0/8/0 | 2/0/6 |
| HUE | 8 | 0.137 | 2.733 | 6/0/2 | 0/8/0 |
| GREG | 8 | 0.349 | 7.632 | 0/0/8 | 1/0/7 |
| LREG(R=8,p=0) | 8 | 0.178 | 3.644 | 3/1/4 | 1/0/7 |
| LREG(R=8,p=4) | 8 | 0.271 | 5.635 | 1/0/7 | 1/0/7 |
| LREG(R=32,p=0) | 8 | 0.157 | 3.141 | 4/1/3 | 2/1/5 |
| LREG(R=32,p=4) | 8 | 0.198 | 4.108 | 1/1/6 | 1/0/7 |
| LREG-T | 8 | 0.183 | 4.064 | 2/0/6 | 1/0/7 |
| LREG(R=128,p=0) | 8 | 0.176 | 3.643 | 3/1/4 | 2/0/6 |
| LREG(R=128,p=4) | 8 | 0.252 | 5.260 | 2/0/6 | 1/0/7 |

### night

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 13 | 0.345 | 7.172 | 0/13/0 | 0/0/13 |
| HUE | 13 | 0.171 | 4.327 | 13/0/0 | 0/13/0 |
| GREG | 13 | 0.548 | 9.278 | 3/0/10 | 0/0/13 |
| LREG(R=8,p=0) | 13 | 0.325 | 6.216 | 9/0/4 | 0/0/13 |
| LREG(R=8,p=4) | 13 | 0.398 | 6.657 | 6/1/6 | 0/0/13 |
| LREG(R=32,p=0) | 13 | 0.278 | 5.453 | 9/0/4 | 1/0/12 |
| LREG(R=32,p=4) | 13 | 0.360 | 6.738 | 6/1/6 | 0/0/13 |
| LREG-T | 13 | 0.336 | 6.218 | 7/1/5 | 1/1/11 |
| LREG(R=128,p=0) | 13 | 0.254 | 4.807 | 10/0/3 | 3/1/9 |
| LREG(R=128,p=4) | 13 | 0.389 | 5.853 | 6/0/7 | 2/0/11 |

### phone

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 9 | 0.136 | 2.874 | 0/9/0 | 5/1/3 |
| HUE | 9 | 0.139 | 3.358 | 3/1/5 | 0/9/0 |
| GREG | 9 | 0.330 | 7.737 | 0/0/9 | 1/0/8 |
| LREG(R=8,p=0) | 9 | 0.240 | 5.047 | 2/0/7 | 1/0/8 |
| LREG(R=8,p=4) | 9 | 0.329 | 6.826 | 0/0/9 | 1/0/8 |
| LREG(R=32,p=0) | 9 | 0.256 | 5.771 | 2/0/7 | 1/0/8 |
| LREG(R=32,p=4) | 9 | 0.334 | 6.980 | 0/0/9 | 1/0/8 |
| LREG-T | 9 | 0.278 | 6.666 | 1/1/7 | 1/0/8 |
| LREG(R=128,p=0) | 9 | 0.247 | 4.704 | 2/2/5 | 2/0/7 |
| LREG(R=128,p=4) | 9 | 0.339 | 7.005 | 0/1/8 | 1/0/8 |

### sky

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 12 | 0.137 | 2.738 | 0/12/0 | 8/2/2 |
| HUE | 12 | 0.153 | 3.503 | 2/2/8 | 0/12/0 |
| GREG | 12 | 0.321 | 7.055 | 0/0/12 | 0/0/12 |
| LREG(R=8,p=0) | 12 | 0.255 | 5.652 | 2/0/10 | 2/0/10 |
| LREG(R=8,p=4) | 12 | 0.295 | 6.284 | 1/0/11 | 2/0/10 |
| LREG(R=32,p=0) | 12 | 0.236 | 5.246 | 3/0/9 | 2/0/10 |
| LREG(R=32,p=4) | 12 | 0.288 | 6.539 | 1/0/11 | 2/0/10 |
| LREG-T | 12 | 0.282 | 6.248 | 2/1/9 | 2/1/9 |
| LREG(R=128,p=0) | 12 | 0.227 | 4.693 | 3/0/9 | 3/0/9 |
| LREG(R=128,p=4) | 12 | 0.176 | 3.963 | 1/1/10 | 5/0/7 |

### specular

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 6 | 0.182 | 4.240 | 0/6/0 | 1/0/5 |
| HUE | 6 | 0.149 | 3.322 | 5/0/1 | 0/6/0 |
| GREG | 6 | 0.347 | 7.126 | 0/0/6 | 0/0/6 |
| LREG(R=8,p=0) | 6 | 0.222 | 4.894 | 1/1/4 | 0/0/6 |
| LREG(R=8,p=4) | 6 | 0.259 | 5.696 | 2/0/4 | 0/0/6 |
| LREG(R=32,p=0) | 6 | 0.215 | 4.660 | 2/0/4 | 0/0/6 |
| LREG(R=32,p=4) | 6 | 0.255 | 5.510 | 2/1/3 | 0/0/6 |
| LREG-T | 6 | 0.239 | 5.345 | 2/1/3 | 0/0/6 |
| LREG(R=128,p=0) | 6 | 0.213 | 4.433 | 2/1/3 | 0/0/6 |
| LREG(R=128,p=4) | 6 | 0.282 | 5.468 | 2/0/4 | 1/0/5 |

### overall

| Predictor | Pairs | Mean log2 error | Mean angle (degrees) | vs OPP W/T/L | vs HUE W/T/L |
|---|---:|---:|---:|---:|---:|
| OPP | 96 | 0.172 | 3.680 | 0/96/0 | 45/6/45 |
| HUE | 96 | 0.175 | 3.861 | 45/6/45 | 0/96/0 |
| GREG | 96 | 0.399 | 9.356 | 3/2/91 | 5/1/90 |
| LREG(R=8,p=0) | 96 | 0.239 | 5.075 | 31/3/62 | 16/5/75 |
| LREG(R=8,p=4) | 96 | 0.335 | 7.314 | 14/2/80 | 6/2/88 |
| LREG(R=32,p=0) | 96 | 0.250 | 5.514 | 32/3/61 | 10/5/81 |
| LREG(R=32,p=4) | 96 | 0.313 | 7.092 | 15/6/75 | 8/1/87 |
| LREG-T | 96 | 0.283 | 6.840 | 21/4/71 | 9/2/85 |
| LREG(R=128,p=0) | 96 | 0.263 | 5.587 | 29/5/62 | 15/3/78 |
| LREG(R=128,p=4) | 96 | 0.320 | 7.374 | 15/3/78 | 12/1/83 |

## Runtime

Times below are summed over successful pairs. Each local fit includes moment filtering, regression, coefficient transport, and prediction. LREG-T reuses the LREG(R=32,p=4) fit in this batch; its standalone estimate adds that fit time to the censored-correction time. The standalone rows can therefore double-count shared work.

| Predictor | Seconds |
|---|---:|
| OPP | 17.59 |
| HUE | 80.88 |
| GREG | 18.62 |
| LREG(R=8,p=0) | 286.56 |
| LREG(R=8,p=4) | 356.77 |
| LREG(R=32,p=0) | 161.81 |
| LREG(R=32,p=4) | 175.85 |
| LREG-T | 176.28 |
| LREG(R=128,p=0) | 43.95 |
| LREG(R=128,p=4) | 47.07 |
| Total batch wall time | 1358.76 |

## Skipped and downsampled pairs

Skipped non-Bayer pairs: none.
Downsampled pairs (88, all by a factor of 2):
- `allpairs/t00-galleria-glass-roof-0528-0524`
- `allpairs/t00-galleria-glass-roof-0528-0526`
- `allpairs/t02-galleria-arch-crowd-0537-0539`
- `allpairs/t03-galleria-nave-roof-0549-0545`
- `allpairs/t04-duomo-facade-sun-0603-0599`
- `allpairs/t04-duomo-facade-sun-0603-0601`
- `allpairs/t06-galleria-entrance-sky-0616-0614`
- `allpairs/t07-facade-spire-sky-0622-0620`
- `allpairs/t07-facade-spire-sky-0624-0620`
- `allpairs/t10-duomo-spires-street-0647-0643`
- `allpairs/t12-tower-lamp-post-0661-0657`
- `allpairs/t12-tower-lamp-post-0661-0659`
- `allpairs/t14-tower-top-0675-0671`
- `allpairs/t14-tower-top-0675-0673`
- `allpairs/t16-tower-shaded-side-0689-0685`
- `allpairs/t16-tower-shaded-side-0689-0687`
- `allpairs/t20-street-looking-up-0713-0711`
- `allpairs/t20-street-looking-up-0715-0711`
- `allpairs/t22-brick-tower-overcast-0731-0727`
- `allpairs/t22-brick-tower-overcast-0731-0729`
- `allpairs/t23-statue-piazza-sky-0736-0734`
- `allpairs/t23-statue-piazza-sky-0738-0734`
- `allpairs/t25-statue-silhouette-sky-0750-0748`
- `allpairs/t25-statue-silhouette-sky-0752-0748`
- `allpairs/t27-piazza-red-house-clouds-0764-0762`
- `allpairs/t27-piazza-red-house-clouds-0766-0762`
- `allpairs/t29-yellow-facade-sky-0776-0774`
- `allpairs/t29-yellow-facade-sky-0778-0774`
- `allpairs/t32-piazza-houses-0797-0795`
- `allpairs/t32-piazza-houses-0801-0797`
- `allpairs/t34-fountain-frescoes-0815-0811`
- `allpairs/t34-fountain-frescoes-0815-0813`
- `allpairs/t36-statue-frescoes-0829-0825`
- `allpairs/t36-statue-frescoes-0829-0827`
- `allpairs/t39-piazza-palace-lamps-0848-0846`
- `allpairs/t39-piazza-palace-lamps-0850-0846`
- `allpairs/t42-sun-behind-building-0867-0865`
- `allpairs/t45-narrow-street-sky-0892-0888`
- `allpairs/t45-narrow-street-sky-0892-0890`
- `allpairs/t47-narrow-street-church-0902-0900`
- `allpairs/t47-narrow-street-church-0904-0900`
- `allpairs/g065-sunset-field-0347-0345`
- `allpairs/g353-yellow-autumn-tree-0397-0395`
- `allpairs/g369-yellow-tree-sky-0445-0443`
- `allpairs/g373-larch-sky-0455-0456`
- `allpairs/g441-red-reels-0470-0468`
- `allpairs/g476-poppies-sun-0010-0008`
- `allpairs/t38-balcony-geraniums-0841-0839`
- `allpairs/t38-balcony-geraniums-0843-0839`
- `allpairs/t51-purple-neon-shop-0934-0930`
- `allpairs/t51-purple-neon-shop-0934-0932`
- `allpairs/t40-shop-window-spotlights-0851-0852`
- `allpairs/t46-eyewear-shop-0899-0895`
- `allpairs/t46-eyewear-shop-0899-0897`
- `allpairs/t58-escalator-led-strips-0016-0014`
- `allpairs/t60-wall-lamp-1-0031-0033`
- `allpairs/t60-wall-lamp-1-0034-0030`
- `allpairs/t64-church-windows-0056-0057`
- `allpairs/t64-church-windows-0058-0057`
- `allpairs/g300-station-hall-0140-0138`
- `allpairs/g486-platform-lamps-tower-0277-0276`
- `allpairs/t53-dusk-lit-facade-0984-0980`
- `allpairs/t55-arcade-night-wide-0994-0992`
- `allpairs/t55-arcade-night-wide-0998-0994`
- `allpairs/t57-arcade-night-moon-0013-0009`
- `allpairs/t57-arcade-night-moon-0013-0011`
- `allpairs/p04-clouds-portrait-45_4-45_3`
- `allpairs/q00-steamroller-sun-leaves-31_3-31_1`
- `allpairs/q07-street-sign-buildings-23_3-23_2`
- `allpairs/q07-street-sign-buildings-23_4-23_2`
- `allpairs/q08-park-blue-sky-45_4-45_2`
- `allpairs/q08-park-blue-sky-45_4-45_3`
- `allpairs/g057-overcast-clouds-0317-0315`
- `allpairs/g098-restaurant-windows-0446-0445`
- `allpairs/g213-park-clouds-1-0852-0850`
- `allpairs/g216-park-clouds-2-0861-0859`
- `allpairs/g281-storm-clouds-0075-0073`
- `allpairs/g313-mountain-clouds-0182-0180`
- `allpairs/g315-lake-glare-0280-0278`
- `allpairs/g319-sun-through-trees-0292-0290`
- `allpairs/g403-underpass-sky-0353-0351`
- `allpairs/g483-backlit-trees-0250-0251`
- `allpairs/g483-backlit-trees-0252-0251`
- `allpairs/g105-car-mirror-0467-0465`
- `allpairs/g112-white-car-chrome-0488-0486`
- `allpairs/g120-chrome-trim-0512-0510`
- `allpairs/g419-green-truck-0401-0399`
- `allpairs/g426-cream-car-0422-0420`

## Limits

This is a synthetic-clipping test. It does not show that any predictor reconstructs the true scene inside a real saturated region. It uses 2x2 Bayer quad means, not individual photosite reconstructions, and it skips non-Bayer raws.
The HUE baseline uses the requested unsmoothed boundary chromaticity and a discrete four-neighbor Laplace fill. The OPP baseline uses the cube-root opposed estimate and one per-channel offset. Both are quad approximations of darktable's per-photosite processing.
The experiment did not compare rendered images, edge appearance, noise, demosaicing behavior, X-Trans behavior, OpenCL, or darktable runtime. It did not verify that exposure-bracket registration or scene motion leaves every reference quad valid beyond the specified darker-frame clipping exclusion.

Per-pair clipping counts, evaluation shares, error means and medians, predictor details, solver residuals, and runtimes are in `predict.json`.
