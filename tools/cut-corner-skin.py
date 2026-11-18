"""Cuts a skin for Exodium's corner ornaments out of one picture of its four corners.

The picture has the four corners laid out the way they sit on a window, each in its own
quarter, on black: the top right one with the minimize, maximize and close buttons. Image
AIs make them that way when given a picture of the ones there are as an example.

It writes the four pieces to exodium\\res as corner_<name>_<corner>.png. Each is turned to
be drawn for the top right corner, as ornament.cpp has them (it turns them back), and all
four are the same size with the frame's corner on the same point. It then prints the
skin's line for corner_skins.h, with what it measured.

Some image AIs hand back the corners on a see-through background instead. What is see-through
in such a picture is taken as it is, black shadows in the ironwork staying solid.

What it can't tell by itself is given in the picture's own pixels:
  --buttons     the middles of the three buttons, minimize first
  --pod-bottom  the row where the top right one's pod, which holds the buttons, ends and
                its upright goes on below. Tucked into the frame's corner, as on a
                maximized window, the ornament is cut off there.
  --split       where the quarters meet, if not at the picture's middle: a column and a
                row with no ironwork on them, such as the middle of the gaps between
                the corners. It warns when the ironwork runs across them.

    python tools\\cut-corner-skin.py venom.png venom --buttons 1340,81 1405,81 1471,81 --pod-bottom 153

Needs numpy, Pillow and scipy.
"""
import argparse
import math
import pathlib

import numpy as np
from PIL import Image
from scipy import ndimage

# The pictures are shown at 340 of the screen's pixels to 748 of their own (ornament.cpp),
# and the tab strip lies 24 to 50 pixels under the frame's top (browser.h).
SHOWN, ART = 340, 748
STRIP_TOP, STRIP_BOTTOM = 24, 24 + 26
CORNERS = ("top_right", "top_left", "bottom_right", "bottom_left")
ON_LEFT = {"top_right": False, "top_left": True, "bottom_right": False, "bottom_left": True}
ON_BOTTOM = {"top_right": False, "top_left": False, "bottom_right": True, "bottom_left": True}
# How far into the upright the frame's edge goes, of its width: 13 of the 30 pixels on Iron.
INTO_UPRIGHT = 0.43


def point(text):
    x, y = text.split(",")
    return int(x), int(y)


def cut_out(rgb, buttons, button_radius):
    """The picture with the black around and inside the ironwork made see-through.

    Over black, what each pixel shows is its colour already multiplied by how much of it
    there is; at the ironwork's edges that is told from how bright the pixel is.
    """
    brightest = rgb.max(axis=2)
    h, w = brightest.shape
    # The background is the near black that reaches the picture's edge...
    labels, _ = ndimage.label(brightest <= 6)
    outside = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))) - {0}
    background = np.isin(labels, list(outside))
    # ...and the holes in the ironwork, such as inside a chain's links, but not the black
    # signs on the buttons.
    ys, xs = np.mgrid[0:h, 0:w]
    on_buttons = np.zeros_like(background)
    for bx, by in buttons:
        on_buttons |= (xs - bx) ** 2 + (ys - by) ** 2 <= (button_radius + 5) ** 2
    dark, count = ndimage.label(brightest <= 20)
    sizes = ndimage.sum(np.ones_like(brightest), dark, range(1, count + 1))
    for i in np.nonzero(sizes >= 30)[0]:
        hole = dark == i + 1
        if not (hole & on_buttons).any():
            background |= hole & (brightest <= 6)
    # Near the background a pixel is as solid as it is bright; further in, all of it.
    near = ndimage.distance_transform_edt(~background) <= 4
    alpha = np.where(near, np.clip(brightest / 40.0, 0, 1), 1.0)
    alpha[background] = 0
    return finish(rgb / np.maximum(alpha, 1e-6)[:, :, None], alpha)


def keep_own_alpha(rgba):
    """A picture that is see-through around the ironwork already says what is ironwork,
    shadows black as the background included. Only the faint haze some image AIs leave
    around it goes."""
    alpha = rgba[:, :, 3] / 255.0
    alpha[alpha < 16 / 255.0] = 0
    return finish(rgba[:, :, :3], alpha)


def finish(colours, alpha):
    """The ironwork's colours, as they are where it is solid, with its alpha, less the
    specks of the AI's noise floating loose, away from the ironwork."""
    h, w = alpha.shape
    loose, count = ndimage.label(alpha > 0)
    sizes = ndimage.sum(np.ones_like(alpha), loose, range(1, count + 1))
    alpha[~np.isin(loose, np.nonzero(sizes >= 2000)[0] + 1)] = 0
    out = np.zeros((h, w, 4))
    out[:, :, :3] = np.clip(colours, 0, 255)
    out[:, :, 3] = alpha * 255
    out[alpha == 0] = 0
    return out


def frame_corner(piece):
    """Where the frame's corner lies on a piece drawn for the top right corner: under the
    bar along the top, and a little way into the upright that hangs down the side."""
    solid = piece[:, :, 3] > 128
    h, w = solid.shape
    undersides = [np.nonzero(solid[: h // 2, x])[0].max() for x in range(int(w * 0.3), int(w * 0.55), 2)
                  if solid[: h // 2, x].any()]
    lefts, rights = [], []
    for y in range(int(h * 0.5), int(h * 0.75), 2):
        xs = np.nonzero(solid[y, w // 2:])[0] + w // 2
        if len(xs):
            lefts.append(xs.min())
            rights.append(xs.max())
    left, right = np.median(lefts), np.median(rights)
    return int(round(left + INTO_UPRIGHT * (right - left))), int(np.median(undersides)) + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("picture")
    parser.add_argument("name", help="the skin's id, in lower case, such as venom")
    parser.add_argument("--buttons", type=point, nargs=3, required=True, metavar="X,Y")
    parser.add_argument("--button-radius", type=int, default=29)
    parser.add_argument("--pod-bottom", type=int, required=True, metavar="Y")
    parser.add_argument("--split", type=point, metavar="X,Y", help="where the quarters meet; the middle if not given")
    parser.add_argument("--out", default=str(pathlib.Path(__file__).resolve().parent.parent / "exodium" / "res"))
    parser.add_argument("--preview", help="also write the pieces as they will look on a window, on white, here")
    args = parser.parse_args()

    picture = Image.open(args.picture)
    rgba = np.asarray(picture.convert("RGBA")).astype(np.float64)
    h, w = rgba.shape[:2]
    split_x, split_y = args.split or (w // 2, h // 2)
    if (rgba[:, :, 3] == 0).mean() > 0.1:
        art = keep_own_alpha(rgba)
    else:
        # On black, or as good as: whatever is see-through is laid on black.
        rgb = rgba[:, :, :3] * (rgba[:, :, 3:] / 255.0)
        art = cut_out(rgb, args.buttons, args.button_radius)
    if art[split_y - 1: split_y + 1].any() or art[:, split_x - 1: split_x + 1].any():
        print("warning: the ironwork runs across where the quarters meet; each corner must keep to its quarter")

    # Each corner from its quarter, turned to be drawn for the top right corner and cut
    # down to the ironwork.
    pieces, placed = {}, {}
    for name in CORNERS:
        top, bottom = (split_y, h) if ON_BOTTOM[name] else (0, split_y)
        left, right = (0, split_x) if ON_LEFT[name] else (split_x, w)
        piece = art[top:bottom, left:right]
        if ON_LEFT[name]:
            piece = piece[:, ::-1]
        if ON_BOTTOM[name]:
            piece = piece[::-1]
        there = piece[:, :, 3] > 0
        rows, cols = np.nonzero(there.any(axis=1))[0], np.nonzero(there.any(axis=0))[0]
        pieces[name] = piece[rows.min(): rows.max() + 1, cols.min(): cols.max() + 1]
        placed[name] = (left + cols.min(), top + rows.min())  # where the top right one came from
    corners = {name: frame_corner(piece) for name, piece in pieces.items()}

    # All four the same size, with the frame's corner on the same point.
    cx = max(c[0] for c in corners.values())
    cy = max(c[1] for c in corners.values())
    width = cx + max(p.shape[1] - corners[n][0] for n, p in pieces.items())
    height = cy + max(p.shape[0] - corners[n][1] for n, p in pieces.items())
    canvases = {}
    for name, piece in pieces.items():
        canvas = np.zeros((height, width, 4))
        dx, dy = cx - corners[name][0], cy - corners[name][1]
        canvas[dy: dy + piece.shape[0], dx: dx + piece.shape[1]] = piece
        canvases[name] = canvas
        path = pathlib.Path(args.out) / f"corner_{args.name}_{name}.png"
        Image.fromarray(np.round(canvas).astype(np.uint8), "RGBA").save(path, optimize=True)
        print("wrote", path)

    # The picture's pixels on the top right one.
    shift_x = cx - corners["top_right"][0] - placed["top_right"][0]
    shift_y = cy - corners["top_right"][1] - placed["top_right"][1]
    buttons = [(x + shift_x, y + shift_y) for x, y in args.buttons]
    pod_bottom = args.pod_bottom + shift_y

    top_right = canvases["top_right"][:, :, 3]
    # The bar's far tip, and whatever hangs from it well below the bar, ends here: the
    # last of the stretches below the bar that reach well down, such as a star's arms.
    below = top_right[cy:, : width // 3] > 0
    stretches, _ = ndimage.label(below.any(axis=0))
    deep = set(np.unique(stretches[below[8:].any(axis=0)])) - {0}
    tip_right = max((int(np.nonzero(stretches == s)[0].max()) + 1 for s in deep), default=0)
    # Tucked in, the tab strip ends where the pod begins, over the rows the strip lies in.
    strip_rows = round((STRIP_BOTTOM - STRIP_TOP) * ART / SHOWN)
    pod = np.nonzero((top_right[pod_bottom - strip_rows: pod_bottom + 1, width // 3:] > 128).any(axis=0))[0]
    pod_left = int(pod.min()) + width // 3
    if pod.min() == 0:
        # The pod runs on to where it was looked for: the buttons sit in the bar itself,
        # which is thick enough to lie over the strip all along, tucked in. The strip ends
        # where the bar begins to, less its tip, which is cut off then.
        band = top_right[pod_bottom - strip_rows: pod_bottom + 1] > 128
        band[max(0, cy - (pod_bottom - strip_rows)):, :tip_right] = False
        pod_left = int(np.nonzero(band.sum(axis=0) >= strip_rows / 3)[0].min())
    # Out on its corner, it ends where the ironwork coming down the side reaches furthest
    # in over the strip.
    strip = slice(cy + math.ceil(STRIP_TOP * ART / SHOWN), cy + STRIP_BOTTOM * ART // SHOWN + 1)
    shoulders = {}
    for name in ("top_right", "top_left"):
        over = np.nonzero((canvases[name][strip, width // 2:, 3] > 128).any(axis=0))[0]
        shoulders[name] = int(over.min()) + width // 2 if len(over) else cx

    if args.preview:
        shown = Image.new("RGBA", (2 * width + 40, 2 * height + 40), (255, 255, 255, 255))
        for name, canvas in canvases.items():
            piece = Image.fromarray(np.round(canvas).astype(np.uint8), "RGBA")
            if ON_LEFT[name]:
                piece = piece.transpose(Image.FLIP_LEFT_RIGHT)
            if ON_BOTTOM[name]:
                piece = piece.transpose(Image.FLIP_TOP_BOTTOM)
            shown.alpha_composite(piece, (0 if ON_LEFT[name] else width + 40, height + 40 if ON_BOTTOM[name] else 0))
        shown.convert("RGB").save(args.preview)
        print("wrote", args.preview)

    ids = ", ".join(f"IDR_CORNER_{args.name.upper()}_{name.upper()}" for name in CORNERS)
    print(f"""
    CornerSkin{{
        .id = L"{args.name}",
        .name = L"{args.name.capitalize()}",
        .pictures = {{{ids}}},
        .size = {{{width}, {height}}},
        .corner = {{{cx}, {cy}}},
        .podLeft = {pod_left},
        .podBottom = {pod_bottom},
        .tipRight = {tip_right},
        .shoulderTopRight = {shoulders["top_right"]},
        .shoulderTopLeft = {shoulders["top_left"]},
        .buttons = {{{{{", ".join(f"{{{x}, {y}}}" for x, y in buttons)}}}}},
        .buttonRadius = {args.button_radius},
    }},""")


if __name__ == "__main__":
    main()
