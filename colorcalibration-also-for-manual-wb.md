`dev->chroma`
- `wb_coeffs`: WB coefficients from `temperature.c`. Neutralising coefficients to apply to raw camera RGB to achieve R = G = B for a netural spot under the scene illuminant. Could come from the camera or from the user.
- `D65coeffs`: extracted in `temperature.c`. Coefficients to apply to raw camera RGB to achieve R = G = B for a netural spot under the D65 illuminant.
- `late_correction`: flag set in temperature.c to be used in colorin.c, indicating
                                    - 'neutralising' coefficients were used in temperature.c,
                                    - and we want colorin.c to undo them and apply the D65 coefficients
                                    - FALSE for 'camera reference' (uses D65 multipliers)
                                    - TRUE for 'as shot to reference'
                                    - controlled via new checkbox for user/manual modes (previously: FALSE)
`img`:
- `wb_coeffs`: as-shot coefficients from the raw
- `adobe_XYZ_to_CAM`: camera characterisation matrix XYZ -> RGB from raw library database. Used as a fallback if `d65_color_matrix` is not available.
- `d65_color_matrix`: camera characterisation matrix XYZ -> RGB from EXIF/DNG data; may not always be available (fallback to `adobe_XYZ_to_CAM`)

D65 multipliers:
Normally, we can push the D65 WP XYZ coordinates through the XYZ -> RGB camera characterisation matrix to get the camera's native response to D65 light (e.g. `RGB_camera_D65 = (0.6, 1, 0.5)`).
We can then find the WB coefficients that neutralise that response (making R=G=B=1):
`img->wb_coeffs = (1 / R_camera_D65, 1 / G_camera_D65, 1 / B_CAMERA_D65)`
These are the multipliers used in 'camera reference' mode (e.g. 1.67, 1, 2).

However, as described in the 'caveats' workaround in the docs, if the multipliers are wonky, the user may take a photo of a D65-calibrated screen, and pick the WB multipliers from there, which can be used instead (but this will provoke a warning). In this mode, 'late correction' must not be checked by the user, so the pipeline remains in 'D65 reference' mode, but uses the right multipliers (which could be e.g. 1.7, 1, 1.95 or some other triplet). A warning will be displayed, but that's OK.

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
