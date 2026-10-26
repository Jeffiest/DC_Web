"""Find the text slots (word boxes) of an existing texture and put re-rendered words into them."""
import numpy as np, cv2
import textpack


def ink_runs(mask, x_close=14):
    """Column runs of ink in a boolean mask (rows already cropped); small gaps closed."""
    cols = mask.any(0)
    xs = np.where(cols)[0]
    if len(xs) == 0:
        return []
    runs = []
    s = p = xs[0]
    for x in xs[1:]:
        if x - p > x_close:
            runs.append((s, p)); s = x
        p = x
    runs.append((s, p))
    return runs


def group_runs(runs, k):
    """Merge runs into exactly k groups by splitting at the k-1 widest gaps."""
    if len(runs) <= k:
        return runs
    gaps = sorted(range(len(runs) - 1), key=lambda i: runs[i + 1][0] - runs[i][1], reverse=True)[:k - 1]
    cuts = sorted(gaps)
    out = []; start = 0
    for c in cuts + [len(runs) - 1]:
        out.append((runs[start][0], runs[c][1])); start = c + 1
    return out


def boxes(alpha, y0, y1, k, thr=70):
    """k word boxes (x0,y0,x1,y1) in rows y0..y1 of an alpha plane, left to right."""
    band = alpha[y0:y1] > thr
    runs = group_runs(ink_runs(band), k)
    out = []
    for a, b in runs:
        ys = np.where(band[:, a:b + 1].any(1))[0]
        out.append((a, y0 + ys.min(), b, y0 + ys.max()))
    return out


def emboss_rgba(M, hi, lo):
    """Beveled lettering: light rim top-left, black rim bottom-right, see-through middle. M uint8 mask. Returns HxWx4 uint8."""
    H, W = M.shape
    m = (M > 127).astype(np.uint8)
    k = np.ones((3, 3), np.uint8)
    inner = cv2.erode(m, k, iterations=6)
    rim = (m > 0) & (inner == 0)
    g = cv2.GaussianBlur(m.astype(np.float32), (0, 0), 4)
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0); gy = cv2.Sobel(g, cv2.CV_32F, 0, 1)
    shade = -(gx + gy) / 0.5                       # >0 on the lit (top-left) side
    t = np.clip(0.5 + shade, 0, 1)[..., None]
    col = np.array(lo, np.float32) * (1 - t) + np.array(hi, np.float32) * t
    out = np.zeros((H, W, 4), np.float32)
    # no drop shadow here: the port casts it, at the strength of the Options slider
    inner_a = cv2.GaussianBlur(inner.astype(np.float32), (0, 0), 1.2) * 28
    out[..., 3] = inner_a
    ra = cv2.GaussianBlur(rim.astype(np.float32), (0, 0), 1.0) * 255
    for c in range(3):
        out[..., c] = np.where(ra > 8, col[..., c], 0)
    out[..., 3] = np.maximum(out[..., 3], ra)
    return np.clip(out, 0, 255).astype(np.uint8)


def fit_word(text, box, cap_h, align_bottom=None, xsq_max=1.0, xsq_min=0.45):
    """Mask (H,W uint8 canvas-sized later) for text fitted into box width, cap height cap_h; returns (mask, cap_top_in_mask, x0)."""
    x0, y0, x1, y1 = box
    bw = x1 - x0 + 1
    nat, ct = textpack.text_mask(text, cap_h, 1.0)
    xsq = np.clip(bw / max(1, nat.shape[1]), xsq_min, xsq_max)
    m, ct = textpack.text_mask(text, cap_h, xsq, max_w=int(bw * 1.04))
    return m, ct
