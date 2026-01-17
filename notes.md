### 1. General Operations and Computations

The pipeline uses the `temperature` module to scale channels early in the pipe. This serves two purposes:
1.  **Demosaicing & Highlight Recovery**: These algorithms perform best on neutral data (where R=G=B for white). By applying white balance coefficients early, `temperature` neutralizes the scene data, optimizing these technical steps.
2.  **Chromatic Adaptation**: In legacy modes, this early scaling is the final white balance. In modern modes, it is a temporary state reversed by `colorin` to allow `color calibration` to handle the visual white balance later using a CAT (Chromatic Adaptation Transform).

**Technical Variables:**
*   **`d->clip`**: The user-defined clipping threshold in `highlights`. It represents the sensor saturation point (usually 1.0).
*   **`wb_coeffs`**: The coefficients currently applied by the `temperature` module to the raw data.
*   **`D65coeffs`**: The coefficients required to convert the camera's raw RGB to the D65 white point of the color matrix (Technical D65).
*   **`late_correction`**: A flag stored in `dev->chroma`.
    *   **FALSE**: `temperature` performs the final WB. `colorin` does standard processing.
    *   **TRUE**: `temperature` applies coefficients for demosaicing/HL benefits (making the image visually neutral), but `colorin` immediately reverses this normalization relative to D65. This essentially "hides" the white balance from the rest of the pipe until `color calibration` reapplies it.
*   **`_dev_is_D65_chroma`**: Returns `TRUE` if `late_correction` is active **OR** if the applied coefficients exactly match `D65coeffs`. Used by `channelmixerrgb` to determine if it needs to calculate manual adaptation ratios.

---

### 2. Workflow Summaries

#### **A. Legacy Workflow (User Mode, no late correction)**
*   **Concept**: The user white-balances the image in `temperature`. The pipe processes this white-balanced data.
*   **`temperature`**: Applies **User** coefficients. Neutralizes the scene (R=G=B) to aid demosaicing.
    *   `late_correction` = `FALSE`.
*   **`highlights`**: Uses `d->clip` directly (Scale = 1.0). Reconstruction happens on the scene-neutralized data.
*   **`colorin`**: Standard processing. Input is assumed to be white-balanced.
*   **`channelmixerrgb`**: `_dev_is_D65_chroma` is `FALSE`. Calculates standard adaptation ratios (1.0). Does not perform WB.

#### **B. Modernised User Mode (User Mode, with late correction)**
*   **Concept**: The user adjusts WB in `temperature`. The data is neutralized for demosaicing, but then normalized to D65 so `channelmixerrgb` can handle the physics of color adaptation.
*   **`temperature`**: Applies **User** coefficients. Neutralizes the scene to aid demosaicing/HL recovery.
    *   `late_correction` = `TRUE`.
*   **`highlights`**: Calculates scale $S = \text{User} / \text{D65}$. Threshold = `d->clip` $\times S$. This ensures the clipping point matches the User gains applied.
*   **`colorin`**: Calculates compensation $K = \text{D65} / \text{User}$. Multiplies data by $K$.
    *   **Result**: The User WB is undone. Data exits in the Camera D65 space.
*   **`channelmixerrgb`**: `_dev_is_D65_chroma` is `TRUE`. Sets illuminant mode to `DT_ILLUMINANT_FROM_WB` (intended to read User coeffs to build the CAT, though current provided code defaults to EXIF here due to `ratios=1.0`).

#### **C. Existing Modern Mode ('Camera Reference')**
*   **Concept**: `temperature` applies fixed D65 scaling. If the scene is not D65, the data will **not** be neutral during demosaicing.
*   **`temperature`**: Applies **D65** coefficients.
    *   `late_correction` = **`FALSE`** (Logic: The data is already in the Reference D65 state required by the modern pipe, so no "late correction" reversal is needed).
*   **`highlights`**: Scale = 1.0. Threshold = `d->clip`.
*   **`colorin`**: Standard processing. Data enters and leaves in D65 state.
*   **`channelmixerrgb`**: `_dev_is_D65_chroma` is `TRUE`. Detects illuminant (usually from EXIF) and applies CAT.

#### **D. Existing Modern Mode ('As Shot to Reference')**
*   **Concept**: `temperature` applies the Camera's "As Shot" WB (neutralizing the scene for demosaicing), but the pipe treats it as D65 compliant via late correction.
*   **`temperature`**: Applies **EXIF (As Shot)** coefficients. Neutralizes the scene to aid demosaicing/HL recovery.
    *   `late_correction` = `TRUE`.
*   **`highlights`**: Calculates scale $S = \text{As Shot} / \text{D65}$. Threshold = `d->clip` $\times S$.
*   **`colorin`**: Calculates compensation $K = \text{D65} / \text{As Shot}$. Multiplies data by $K$.
    *   **Result**: As Shot WB is undone. Data normalized to Camera D65 space.
*   **`channelmixerrgb`**: `_dev_is_D65_chroma` is `TRUE`. Uses EXIF data to build the CAT.