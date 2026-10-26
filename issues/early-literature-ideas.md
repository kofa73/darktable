# Ideas from the early highlight recovery literature

Status: pending (2026-10-26; ideas collected, none tried). Kind: experiment candidates.

## Goal

Find unpatented ideas in `/tmp/hl-article/early/` (one document, four formats) for inpaint opposed
with the controls of issue 31, aimed at its open problems: the edge line around the recolored
core, the brightness of large fully clipped areas (Laneway's lit wall: flat is dirty gray, the
dome is a centered blob, the truth peaks at a spotlight on the side), and the two-clipped ring.

## Candidates

1. **Transition band between 1-, 2- and 3-channel clipped pixels** (Xu, Doutre, Nasiopoulos,
   IEEE TVCG 17(3) 2011, section 2.5): on the more saturated side of each boundary, a band of width
   w0 is blended, by distance, toward the mean of the nearest less saturated pixels. Aimed at the
   edge line under Laneway's lamp core. Cheap.
2. **Fully clipped brightness from a free-center 2D Gaussian fit** (same paper, 2.4.3): fit
   `A exp(-q(x - x0, y - y0)) + B` (any center, rotation, width) to the luminance of the
   surroundings by least squares, evaluate it inside; lower bound on the gain rising from 1 at the
   rim to a constant over a transition width, upper bound on the gain. The peak can sit off center
   (Laneway's wall, spotlight on its side) and its height follows the glow, unlike the distance
   dome. Risks: a fit with a dark frame around a flat light; needs the "max rise" cap.
3. **Sequential estimate of two clipped channels** (Zhang and Brainard, JOSA A 21(12) 2004):
   estimate the clipped channel nearest its clip level first from the measured one, then the
   second from both; MMSE of a Gaussian truncated at the clip level. Our reference passes do the
   iteration already; the order rule might replace them. Low priority.
4. Correction order (Xu 2.3): 1-channel, then 2, then 3-channel clipped pixels, each nearest the
   unclipped surroundings first. Overlaps with the passes. Low priority.

Not new or not suited: Masood et al. 2009 (local ratio propagation: issues 9 and 27 tried smooth
offset fields), dcraw's ratio grid (same), Guo et al. 2010 (CIELAB lightness ellipsoids; display
referred), Elboher and Werman 2010 (color lines; heavy), Rouf et al. 2012 (Dolby patent, issue 17).

## Patent status (search 2026-10-26; Google Patents' status is not a legal conclusion)

- Zhang and Brainard: US 6,731,794 (HP, filed 2001-04-12), lapsed 2008 for unpaid fees.
- Xu et al.: no patent found.
- Region lightness on a medial-axis skeleton (Thomson / InterDigital, US 10,032,261 B2, priority
  2015): Google lists it as expired for fees; EP 3139342 A1 withdrawn. Related to our distance dome.
- Guo et al. 2010: no patent found.

## Done means

Kofa has chosen which to try; each tried one is checked on Laneway and scored at -2 / -3 EV.
