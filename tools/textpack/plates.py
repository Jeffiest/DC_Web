import numpy as np, cv2
from PIL import Image

def text_core(rgb):
    lum = rgb.astype(np.float32).mean(2)
    return lum > 0.72 * np.percentile(lum, 99.8)

def build_plate(cards, grow=22):
    """cards: list of HxWx3 uint8 (same card, different languages). Text differs; backdrop is common."""
    masks = []
    for c in cards:
        core = text_core(c).astype(np.uint8)
        core = cv2.morphologyEx(core, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8))
        masks.append(cv2.dilate(core, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2*grow+1, 2*grow+1))) > 0)
    st = np.stack(cards).astype(np.float32)           # N,H,W,3
    valid = ~np.stack(masks)                           # N,H,W
    cnt = valid.sum(0)
    # median over valid cards = mean of valid, robust enough since backdrops are identical
    num = (st * valid[..., None]).sum(0)
    plate = num / np.maximum(cnt, 1)[..., None]
    hole = (cnt == 0).astype(np.uint8)
    plate = plate.astype(np.uint8)
    if hole.any():
        hole = cv2.dilate(hole, np.ones((5, 5), np.uint8))
        plate = cv2.inpaint(plate, hole, 9, cv2.INPAINT_TELEA)
    union = (~np.stack(masks)).sum(0) < len(cards)
    sm = cv2.GaussianBlur(plate, (0, 0), 4)
    u = cv2.GaussianBlur(union.astype(np.float32), (0, 0), 6)[..., None]
    plate = (plate * (1 - u) + sm * u).astype(np.uint8)
    return plate, cnt
