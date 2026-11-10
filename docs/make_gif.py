#!/usr/bin/env python3
"""Render an animated GIF of a HAL 9000 chat on a simulated DM42 screen.
The replies are real output from the HAL host build."""
import sys
from PIL import Image, ImageDraw, ImageFont

S = 2                                   # scale (DM42 screen is 400x240)
W, H = 400 * S, 240 * S
BG, FG = (199, 204, 190), (28, 33, 30)  # reflective LCD look
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf", 15 * S)
BOLD = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf", 15 * S)
SMALL = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf", 11 * S)
LH = 19 * S
HDR = 20 * S
MENU = 22 * S
INPUT_Y = H - MENU - LH - 2 * S
ROWS = (INPUT_Y - 4 * S - HDR) // LH
CLOCK = "19:18"
FKEYS = ["Who?", "Status", "Doors", "Time", "Sing", "Help"]

CONVO = [
    (None, "Good evening. I am a HAL 9000 computer. I became operational at 19:18. "
           "Before we begin, may I ask your name?"),
    ("RUDI", "Good evening, Rudi. It is a pleasure to meet you. I hope you had a productive day."),
    ("WHAT IS THE FUEL STATUS", "I have measured the supply at 2.94 volts. "
                                "The cell is in good condition. We have plenty of fuel."),
    ("HOW IS THE OXYGEN", "I control the air supply. You have nothing to worry about."),
    ("12*(7+3)", "It is 120. I enjoy working with numbers."),
    ("WHY ARE YOU IN A CALCULATOR", "Reverse Polish notation is the only civilised way to calculate."),
    ("GOOD MORNING", "I think you mean good evening, Rudi. My clock says it is 19:18."),
    ("OPEN THE POD BAY DOORS", "I'm sorry, Rudi. I'm afraid I can't do that."),
]


def wrap(text, font, width):
    out, line = [], ""
    for w in text.split(" "):
        t = (line + " " + w).strip()
        if font.getlength(t) <= width or not line:
            line = t
        else:
            out.append(line); line = w
    out.append(line)
    return out


def disc(d, cx, cy, r, fill):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)


def frame(lines, typed, thinking, phase):
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    # header
    d.rectangle([0, 0, W, HDR], fill=FG)
    d.text((6 * S, 1 * S), "HAL 9000", font=BOLD, fill=BG)
    cx, cy = 104 * S, HDR // 2
    disc(d, cx, cy, 8 * S, BG); disc(d, cx, cy, 6 * S, FG)
    disc(d, cx, cy, (2 + (phase % 4 if thinking else 1)) * S, (200, 30, 30))
    for x, t in [(150 * S, "[ABC]"), (W - FONT.getlength(CLOCK) - 8 * S, CLOCK)]:
        tw = FONT.getlength(t)
        d.rectangle([x - 2 * S, 2 * S, x + tw + 2 * S, HDR - 3 * S], fill=BG)
        d.text((x, 1 * S), t, font=FONT, fill=FG)
    # chat
    vis = lines[-ROWS:]
    for i, (txt, user) in enumerate(vis):
        d.text((4 * S, HDR + 3 * S + i * LH), txt, font=BOLD if user else FONT, fill=FG)
    # input line
    d.line([0, INPUT_Y - 2 * S, W, INPUT_Y - 2 * S], fill=FG, width=S)
    d.text((4 * S, INPUT_Y), "> " + typed + ("" if thinking else "_"), font=FONT, fill=FG)
    # menu keys
    kw = W // 6
    for i, k in enumerate(FKEYS):
        x0 = i * kw + 2 * S
        d.rectangle([x0, H - MENU + 2 * S, x0 + kw - 4 * S, H - 1], fill=FG)
        tw = SMALL.getlength(k)
        d.text((x0 + (kw - 4 * S - tw) / 2, H - MENU + 5 * S), k, font=SMALL, fill=BG)
    return im


def build(out):
    frames, durs = [], []
    lines = []
    width = W - 10 * S
    phase = 0

    def add(im, ms):
        frames.append(im); durs.append(ms)

    def hal_says(text):
        nonlocal phase
        start = len(lines)
        for n in range(0, len(text) + 1, 3):
            phase += 1
            cur = [(l, False) for l in wrap(text[:n], FONT, width)]
            snapshot = lines[:start] + cur
            add(frame(snapshot, "", True, phase), 45)
        lines[start:] = [(l, False) for l in wrap(text, FONT, width)]

    for user, hal in CONVO:
        if user:
            for n in range(0, len(user) + 1, 2):
                add(frame(lines, user[:n], False, phase), 55)
            add(frame(lines, user, False, phase), 350)
            lines.extend((l, True) for l in wrap("> " + user, BOLD, width))
        hal_says(hal)
        add(frame(lines, "", False, phase), 1500)

    # add a bezel
    pad = 14 * S
    out_frames = []
    for f in frames:
        b = Image.new("RGB", (W + 2 * pad, H + 2 * pad), (20, 20, 22))
        b.paste(f, (pad, pad))
        out_frames.append(b.resize((b.width // 2 * 1, b.height // 2 * 1), Image.LANCZOS)
                          if "--small" in sys.argv else b)
    pal = [f.convert("P", palette=Image.ADAPTIVE, colors=32) for f in out_frames]
    pal[0].save(out, save_all=True, append_images=pal[1:], duration=durs, loop=0, optimize=True)
    print(out, len(frames), "frames", sum(durs) / 1000, "s")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "hal_demo.gif")
