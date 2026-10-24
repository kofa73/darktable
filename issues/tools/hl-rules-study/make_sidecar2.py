# make_sidecar2.py: make_sidecar.py of /tmp/claude-gd-artefacts/scripts plus --recovery N --strength S
# (segmentation based "rebuild" mode and strength) and --combine C --candidating K (its two sliders). make_sidecar.py <out.xmp> <mode> [--full] [--off] [--wb N] [--exp N] [--ev EV] [--black B] [--no-cat] [--no-comp]
# Writes a sidecar whose history holds a v6 highlights item. darktable prepends
# the workflow's auto-applied presets, because auto_presets_applied is 0.
#   --full     full_resolution = 1 (gradient domain)
#   --off      the highlights item is disabled: no reconstruction
#   --wb, --exp  write-back and experiment values
#   --ev EV    also disable sigmoid and agx, which desaturate highlights, and set
#              exposure to EV, with the compensation flags of the default preset
#   --no-comp  turn off both compensation flags of the exposure item: the EXIF exposure
#              bias of bracket frames (often -2 to -4 EV) would otherwise be undone,
#              so frames would not render at their raw brightness
#   --black B  exposure black level (default that of the preset); synth_eval.py sets 0,
#              so that raising the raw data and lowering the exposure cancel exactly
#   --tone smooth  with --ev: enable sigmoid with its built-in "smooth" preset
#              (sigmoid.c init_presets) instead of disabling it, for display-referred scoring
#   --no-cat   disable color calibration. The scene-referred default white balance
#              (D65 late) runs highlights on as-shot data and converts it to D65 in
#              colorin, so the render is daylight balanced, not as shot
import argparse, struct, math
ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('mode', type=int)
ap.add_argument('--full', action='store_true'); ap.add_argument('--off', action='store_true')
ap.add_argument('--wb', type=int, default=0); ap.add_argument('--exp', type=int, default=0)
ap.add_argument('--ev', type=float); ap.add_argument('--black', type=float, default=-0.000244140625); ap.add_argument('--no-cat', action='store_true')
ap.add_argument('--no-comp', action='store_true'); ap.add_argument('--tone', choices=['smooth'])
ap.add_argument('--recovery', type=int, default=0); ap.add_argument('--strength', type=float, default=0.0)
ap.add_argument('--combine', type=float, default=2.0); ap.add_argument('--candidating', type=float, default=0.4)
a = ap.parse_args()

hl = struct.pack('<ifffiiffifiii', a.mode, a.strength, 1.0, 0.0, 30, 6, a.candidating, a.combine, a.recovery, 0.0, int(a.full), a.wb, a.exp).hex()
items = [('highlights', 0 if a.off else 1, 6, hl)]
if a.ev is not None:
    # sigmoid v3 and agx v7 params of their scene-referred defaults, taken from existing sidecars
    if a.tone == 'smooth':
        # sigmoid v3 "smooth": contrast 1.5, skew -0.2, white 100, black 0.0152, per channel,
        # preserve hue 0, red/green/blue attenuation 0.1/0.1/0.15 and rotation 2/-1/-3 degrees,
        # purity 0, base primaries Rec2020
        d = math.pi / 180
        items.append(('sigmoid', 1, 3, struct.pack('<ffffif7fi', 1.5, -0.2, 100.0, 0.0152, 0, 0.0,
                                                   0.1, 2 * d, 0.1, -1 * d, 0.15, -3 * d, 0.0, 1).hex()))
    else:
        items.append(('sigmoid', 0, 3, '0000c03f000000000000c8426c09793c000000000000c842'
                      '0000000000000000000000000000000000000000000000000000000000000000'))
    items.append(('agx', 0, 7, '000000000000803f0000803f0000803f9a99193f000020c10000d040cdcccc3dca261b3f'
                  'ec51383e0000404000000000000000000000c03f33335340cdcc0c4000000000000000000000803f'
                  '020000000000000006d9963e0a03113dbe69843e41bcacbc7aed153e272481bd0000803f00000000'
                  'a5e0943e0a03113d4ebc863e41bcacbc06a43b3d272481bd00000000'))
    # mode, black, exposure, deflicker percentile and target, compensate bias, compensate highlight preservation
    items.append(('exposure', 1, 7, struct.pack('<iffffii', 0, a.black, a.ev, 50.0, -4.0, *((0, 0) if a.no_comp else (1, 1))).hex()))
if a.no_cat:
    # channelmixerrgb v3 params from a darktable sidecar of P1050600.RW2; unused while disabled
    items.append(('channelmixerrgb', 0, 3, 'gz04eJxjYGiwZ8AAxIqRD9iBmAmIWYCYEYi541/ZKadfs9s6WcMVZBcjVB4AmAMH8w=='))
lis = ''.join('     <rdf:li darktable:num="%d" darktable:operation="%s" darktable:enabled="%d" darktable:modversion="%d"'
              ' darktable:params="%s" darktable:multi_name="" darktable:multi_name_hand_edited="0"'
              ' darktable:multi_priority="0" darktable:blendop_version="14"'
              ' darktable:blendop_params="gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU="/>\n'
              % ((i,) + it) for i, it in enumerate(items))
open(a.out, 'w').write('''<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="XMP Core 4.4.0-Exiv2">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:darktable="http://darktable.sf.net/"
   darktable:xmp_version="5"
   darktable:raw_params="0"
   darktable:auto_presets_applied="0"
   darktable:history_end="%d"
   darktable:iop_order_version="4">
   <darktable:masks_history>
    <rdf:Seq/>
   </darktable:masks_history>
   <darktable:history>
    <rdf:Seq>
%s    </rdf:Seq>
   </darktable:history>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
''' % (len(items), lis))
