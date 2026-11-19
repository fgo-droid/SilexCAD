"""Draws the dimensioned drawing of the tutorial bracket (docs/manual/img/plan_reference.png).

Geometry (mm), origin at the middle of the base plate's front edge, X right, Y back, Z up:
  base plate 40 x 85 x 10, front rail 70 x 20 x 15 with a 5 mm chamfer on its front-top edge,
  upright 40 x 20 x 65 at the back, step 40 x 10 x 25 in front of it, through hole d20 at (0, 37.5),
  two d10 holes at X = +/-10, Z = 55 through the upright, R10 rounds on the upright's top edges.

    python docs/manual/draw_plan.py
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Arc, Circle

OUT = Path(__file__).parent / "img" / "plan_reference.png"
LINE = dict(color="#1a1a1a", lw=1.6)
THIN = dict(color="#1a1a1a", lw=0.8)
HIDDEN = dict(color="#1a1a1a", lw=0.8, ls=(0, (4, 2.5)))
AXIS = dict(color="#c0392b", lw=0.6, ls=(0, (10, 3, 2, 3)))
DIM = "#1f5fa8"


def poly(ax, pts, **kw):
    xs, ys = zip(*(pts + [pts[0]]))
    ax.plot(xs, ys, **kw)


def seg(ax, a, b, **kw):
    ax.plot([a[0], b[0]], [a[1], b[1]], **kw)


def dim(ax, a, b, off, text, horizontal):
    """Linear dimension between points a and b, offset by `off` perpendicular to the measure."""
    if horizontal:
        y = a[1] + off
        pa, pb = (a[0], y), (b[0], y)
        for p, q in ((a, pa), (b, pb)):
            ax.plot([p[0], q[0]], [p[1], q[1] + (1.5 if q[1] > p[1] else -1.5)], color=DIM, lw=0.5)
        ax.annotate("", pa, pb, arrowprops=dict(arrowstyle="<|-|>", color=DIM, lw=0.7, mutation_scale=7))
        ax.text((pa[0] + pb[0]) / 2, y + 1.2, text, color=DIM, ha="center", va="bottom", fontsize=8.5)
    else:
        x = a[0] + off
        pa, pb = (x, a[1]), (x, b[1])
        for p, q in ((a, pa), (b, pb)):
            ax.plot([p[0], q[0] + (1.5 if q[0] > p[0] else -1.5)], [p[1], q[1]], color=DIM, lw=0.5)
        ax.annotate("", pa, pb, arrowprops=dict(arrowstyle="<|-|>", color=DIM, lw=0.7, mutation_scale=7))
        ax.text(x - 1.2 if off < 0 else x + 1.2, (pa[1] + pb[1]) / 2, text, color=DIM,
                ha="right" if off < 0 else "left", va="center", fontsize=8.5)


def hdim(ax, x1, x2, y_feat, y_line, text):
    dim(ax, (x1, y_feat), (x2, y_feat), y_line - y_feat, text, True)


def vdim(ax, z1, z2, x_feat, x_line, text):
    dim(ax, (x_feat, z1), (x_feat, z2), x_line - x_feat, text, False)


def leader(ax, target, text_pos, text):
    ax.annotate(text, target, text_pos, color=DIM, fontsize=8.5, ha="center", va="center",
                arrowprops=dict(arrowstyle="-|>", color=DIM, lw=0.6, mutation_scale=7))


def front_view(ax):
    """Looking along +Y: X to the right, Z up."""
    poly(ax, [(-35, 0), (35, 0), (35, 15), (-35, 15)], **LINE)          # rail
    seg(ax, (-35, 10), (35, 10), **THIN)                                  # chamfer edge
    # upright with the two R10 rounds (centres at X = +/-10, Z = 55)
    seg(ax, (-20, 15), (-20, 55), **LINE)
    seg(ax, (20, 15), (20, 55), **LINE)
    seg(ax, (-10, 65), (10, 65), **LINE)
    ax.add_patch(Arc((-10, 55), 20, 20, theta1=90, theta2=180, **LINE))
    ax.add_patch(Arc((10, 55), 20, 20, theta1=0, theta2=90, **LINE))
    seg(ax, (-20, 25), (20, 25), **THIN)                                  # top of the step
    for x in (-10, 10):
        ax.add_patch(Circle((x, 55), 5, fill=False, **LINE))
        seg(ax, (x - 8, 55), (x + 8, 55), **AXIS)
        seg(ax, (x, 47), (x, 63), **AXIS)
    seg(ax, (0, -4), (0, 69), **AXIS)
    hdim(ax, -35, 35, 0, -8, "70")
    hdim(ax, -20, 20, 65, 73, "40")
    hdim(ax, -10, 10, 55, 41, "20")
    vdim(ax, 0, 65, 35, 45, "65")
    vdim(ax, 0, 25, -20, -42, "25")
    vdim(ax, 10, 15, 35, 39, "5")
    leader(ax, (-13.5, 58.5), (-31, 70), "2 × Ø10")
    leader(ax, (17.1, 62.1), (32, 72), "R10")
    ax.set_title("Vue de face", fontsize=10, pad=4)


def side_view(ax):
    """Looking along -X (from the right): Y to the right, Z up."""
    poly(ax, [(0, 0), (85, 0), (85, 65), (65, 65), (65, 25), (55, 25), (55, 10),
              (20, 10), (20, 15), (5, 15), (0, 10)], **LINE)
    seg(ax, (65, 55), (85, 55), **THIN)                                   # tangent edge of the rounds
    for z in (50, 60):
        seg(ax, (65, z), (85, z), **HIDDEN)                               # d10 holes
    for y in (27.5, 47.5):
        seg(ax, (y, 0), (y, 10), **HIDDEN)                                # d20 hole
    seg(ax, (37.5, -3), (37.5, 13), **AXIS)
    seg(ax, (62, 55), (88, 55), **AXIS)
    hdim(ax, 0, 85, 0, -16, "85")
    hdim(ax, 0, 20, 15, 22, "20")
    hdim(ax, 55, 65, 25, 31, "10")
    hdim(ax, 65, 85, 65, 71, "20")
    hdim(ax, 27.5, 47.5, 0, -7, "Ø20")
    vdim(ax, 0, 10, 85, 90, "10")
    vdim(ax, 10, 15, 0, -5, "5")
    vdim(ax, 0, 55, 85, 101, "55")
    ax.set_title("Vue de droite", fontsize=10, pad=4)


def top_view(ax):
    """Looking down -Z: X to the right, Y up (front edge at the bottom)."""
    poly(ax, [(-35, 0), (35, 0), (35, 20), (20, 20), (20, 85), (-20, 85), (-20, 20), (-35, 20)], **LINE)
    seg(ax, (-35, 5), (35, 5), **THIN)                                    # chamfer edge
    seg(ax, (-20, 55), (20, 55), **THIN)                                  # step front edge
    seg(ax, (-20, 65), (20, 65), **THIN)                                  # upright front face
    for x in (-10, 10):
        seg(ax, (x, 65), (x, 85), **THIN)                                 # tangent edges of the rounds
    for x in (-15, -5, 5, 15):
        seg(ax, (x, 65), (x, 85), **HIDDEN)                               # d10 holes
    ax.add_patch(Circle((0, 37.5), 10, fill=False, **LINE))
    seg(ax, (-13, 37.5), (13, 37.5), **AXIS)
    seg(ax, (0, -3), (0, 88), **AXIS)
    hdim(ax, -20, 20, 85, 92, "40")
    vdim(ax, 0, 37.5, 20, 43, "37,5")
    leader(ax, (7.1, 44.6), (22, 50), "Ø20")
    ax.set_title("Vue de dessus", fontsize=10, pad=4)


def main():
    fig = plt.figure(figsize=(10, 9.2), dpi=150)
    fig.patch.set_facecolor("white")
    gs = fig.add_gridspec(2, 2, width_ratios=[100, 125], height_ratios=[105, 95],
                          left=0.03, right=0.98, top=0.93, bottom=0.03, wspace=0.08, hspace=0.12)
    views = [(gs[1, 0], front_view, (-52, 52), (-14, 80)),
             (gs[1, 1], side_view, (-12, 114), (-22, 80)),
             (gs[0, 0], top_view, (-52, 60), (-6, 99))]
    for spec, draw, xl, yl in views:
        ax = fig.add_subplot(spec)
        draw(ax)
        ax.set_xlim(*xl)
        ax.set_ylim(*yl)
        ax.set_aspect("equal")
        ax.axis("off")
    fig.text(0.56, 0.86, "Équerre de fixation", fontsize=15, weight="bold")
    fig.text(0.56, 0.82, "Cotes en mm — matière pleine", fontsize=10, color="#444")
    fig.text(0.56, 0.78, "Trait plein : arête visible\nTirets : arête cachée (perçages)\n"
                         "Trait mixte rouge : axe", fontsize=9, color="#444", va="top", linespacing=1.5)
    fig.savefig(OUT, facecolor="white")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
