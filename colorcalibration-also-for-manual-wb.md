Maths:

Matrices:
- Characterisation matrices (XYZ->CAM) work with **unscaled** values (project D65 white to camera response).
- The camera **profile** matrix takes **white-balanced** RGB to XYZ ((1, 1, 1) -> D65). This matrix is used in `colorin.c` (_input color profile_ module).

Relationship between the matrices:
For the **characterisation** matrix:
- let CAM_D65: the camera's 'native' D65 response; we can find it by projecting D65 XYZ to CAM: `CAM_D65 = XYZ->CAM * XYZ_D65`
- CAM_to_XYZ = inv(XYZ->CAM) (inverse of the characterisation matrix, without any white balancing), so it projects the native CAM_D65 to the D65 XYZ: `CAM_to_XYZ * CAM_D65 = XYZ_D65`

For the profile matrix:
- neutral (white balanced) RGB = (1, 1, 1) = CAM_D65 * D65_coeffs (component-wise) 
- let `M_profile`: camera profile matrix (neutralised RGB -> XYZ)

The D65 coefficients take CAM_D65 to (1, 1, 1), so
D65coeffs = 1 / CAM_D65 (component-wise)

Then:
CAM_to_XYZ * CAM_D65 = XYZ_D65 and M_profile * (1, 1, 1) = XYZ_D65, so
CAM_to_XYZ * diag(CAM_D65) = M_profile * diag(1, 1, 1) = M_profile * M_identity = M_profile 
M_profile = CAM_to_XYZ * diag(CAM_D65) = inv(XYZ->CAM) * diag(CAM_D65) = inv(XYZ->CAM) * diag(1 / CAM_D65_R,1 / CAM_D65_G, 1 / CAM_D65_B) 


Important data structures:
`dev->chroma`
- `D65coeffs`: set in `temperature.c#reload_defaults`, by calling `_calculate_bogus_daylight_wb`. Coefficients to apply to raw camera RGB to achieve R = G = B for a neutral spot under the D65 illuminant.
- `as_shot`: from EXIF, set in `temperature.c#reload_defaults`, by calling `find_coeffs`
- `wb_coeffs`: the **actual** multipliers (WB coefficients) from `temperature.c`, to be applied on raw data. Unless in 'camera reference mode', these are neutralising coefficients to apply to raw camera RGB to achieve R = G = B for a neutral spot under the scene illuminant.
    - 'camera reference': same as `D65coeffs`
    - 'as shot to reference': same as `as_shot` (from EXIF)
    - user (spot etc) modes: values picked/set by the user
- `late_correction`: flag set in `temperature.c` to be used in `colorin.c`, indicating:
    - 'neutralising' coefficients were used in temperature.c,
    - and we want colorin.c to undo them and apply the D65 coefficients
    - FALSE for 'camera reference' (uses D65 multipliers)
    - TRUE for 'as shot to reference'
    - controlled via new checkbox for user/manual modes (previously: FALSE)
      - should be set to FALSE if using user-set coefficients not to neutralise the image, but to replace D65coeffs derived from a broken matrix

`img`:
- `wb_coeffs`: as-shot coefficients from the raw (set by the loader)
- `adobe_XYZ_to_CAM`: camera **characterisation** matrix XYZ -> RGB from raw library database. Used as a fallback if `d65_color_matrix` is not available.
- `d65_color_matrix`: camera **characterisation** matrix XYZ -> RGB from EXIF/DNG data; may not always be available (fallback to `adobe_XYZ_to_CAM`)

The 'caveat workaround':
- the XYZ->CAM matrix, and therefore the values derived from it (CAM_to_XYZ, CAM_D65, and the 'reference' multipliers) may be wrong (applying the reference multipliers to the **actual** D65 CAM (RGB) response will not result in (1, 1, 1). Suppose the **bad** characterisation matrix projects `XYZ_D65` to `CAM_D65_bad = (0.6, 1, 0.5)`. The **calculated** reference multipliers `D65coeffs = (1 / 0.6, 1 / 1, 1 / 0.5) = (1,67, 1, 2)`) are also wrong, leading to colour casts / incorrect white balance.
- to fix this, the user may define their own corrective multipliers, say `dev->chroma->wb_coeffs = (1.7, 1, 1.95)`, corresponding to the actual native D65 response of `CAM_D65_correct = (1 / 1.7, 1, 1 / 1.95) = (0.588, 1, 0.513)`. The documentation suggests taking a photo of a screen carefully calibrated for D65 to obtain these.
- suppose the scene contains such a pixel, `CAM_D65_correct = (0.588, 1, 0.513)`
- it will be mapped to (1, 1, 1) by the user's multipliers in `dev->chroma->wb_coeffs`
- `colorin.c` then applies the profile matrix `M_profile`, derived from the **bad** characterisation matrix (`M_profile = CAM_to_XYZ * diag(CAM_D65) = inv(XYZ->CAM) * diag(CAM_D65)`). The resulting `XYZ` is still that of D65, because `M_profile` maps `RGB=(1, 1, 1)` to `XYZ_D65`.
  - `XYZ_out = M_profile * (1, 1, 1) = CAM_to_XYZ * diag(1 / CAM_D65_R, 1 / CAM_D65_G, 1 / CAM_D65_B) * (1, 1, 1)`
  - The last can be re-parenthesised as `CAM_to_XYZ * (diag(1 / CAM_D65_R, 1 / CAM_D65_G, 1 / CAM_D65_B) * (1, 1, 1))` = `CAM_to_XYZ * CAM_D65_bad`, so projecting the (1, 1, 1) input back to the **wrong** camera response, which the **wrong** matrix is designed to take to XYZ of D65.



Important code:
`colorspaces.c#dt_colorspaces_conversion_matrices_rgb`:
- returns `FALSE` if failed, `TRUE` if OK
- first tries d65_color_matrix, falls back to adobe_XYZ_to_CAM
- calculates D65 XYZ from an sRGB->XYZ matrix; D65 to camera RGB = XYZ-to-camera * sRGB-to-XYZ
- for each output row, adds up the columns (values of primaries) -> representation of D65 white in camera space (sum of primaries)
- `mul` coefficients = 1 / sum-of-columns-for-row (output if mul != null) - multipliers to take camera D65 response to all channels = 1
- populates out_RGB_to_CAM, out_CAM_to_RGB if out params not null

`temperature.c#_calculate_bogus_daylight_wb`: calculates 'normalised' (green = 1) D65 multipliers, based on `dt_colorspaces_conversion_matrices_rgb`. Returns `FALSE` if OK, `TRUE` on failure.

`temperature.c#_find_coeffs`:
- tries to get coefficients from the raw (`img->wb_coeffs`)
- falls back to `_calculate_bogus_daylight_wb` (D65), if not found; with additional 'safety net' of using a camera preset from DB

matrix in `colorin.c`:
- `commit_params` uses dt_colorspaces_create_xyzimatrix_profile (either using the embedded `d65_color_matrix` or falling back to `adobe_XYZ_to_CAM` from the DB) to create the  


What should happen in channelmixerrgb.c / illuminants.h
- is late_correction set?
  - if yes, we applied WB coefficients in temperature.c that hopefully got colours right. They could be the in-camera multipliers, of user multipliers. We are either in 'as shot to reference' mode, or in one of the user modes, and the user checked the 'late correction' checkbox. Which one, is not important (that's the whole point of these changes). Data has been modified according to the D65 multipliers in colorin.
  - if not:
    - we can be in 'camera reference mode' (dt_dev_equal_chroma(chr->wb_coeffs, chr->D65coeffs)). The data is in D65.
    - we can be in a simple user mode -> 'white balance applied twice' warning
      - but that can still be OK, if in 'caveat workaround' mode: the white balance multipliers set by the user
        are the correct D65 multipliers instead of the ones derived from the characterisation matrix XYZ -> RGB
        
If `late_correction` **is** set, then we now have to rely on the WB multipliers from dev->chroma->wb_coeffs (instead of relying on `img->wb_coeffs`, the camera's as-shot multipliers, which we used before). We trust the camera characterisation matrix. The module's input was processed using the D65 coefficients, colorin took care of undoing the WB multipliers and applying the reference ones.

If `late_correction` **is not** set, then:
- either the multipliers are the D65 coefficients (dt_dev_equal_chroma(chr->wb_coeffs, chr->D65coeffs)) -- camera reference mode
- or they are some other multipliers -> if CAT is enabled, this triggers a warning
  - however, if we are in workaround mode, we should simply use the provided multipliers instead of D65. Since late correction was not set, they properly adjusted the raw input for D65.
In both cases, we proceed with the multipliers in dev->chroma->wb_coeffs as the D65 multipliers.


We need to get the xy of the illuminant, so we can perform chromatic adaptation.

FIXME kofa: continue here
