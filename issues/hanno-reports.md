# Highlights bugs and question for Hanno (segmentation based, inpaint opposed)

Status: done (summary written; filing is Kofa's step). Kind: bug reports.

- Reports and patch: `/tmp/dt-issues/open/highlights-for-hanno/` (threshold bug with "as shot to
  reference", plane write bug, question on the D65 / as-shot mix).
- Test and summary: `summary-for-hanno.md` there. No fix or variant clearly improves inpaint
  opposed or segmentation based on the 52 hard images or the 12 moderate scenes; tuning
  segmentation based's sliders matters more; the plane fix changes nothing.
- Caveat in the summary: scores were mean dE and segmentation based ran at its defaults.
