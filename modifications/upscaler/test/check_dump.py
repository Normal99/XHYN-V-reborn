#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
"""Checks the PPM frames the layer dumps (dump_frame) after a harness run.

  check_dump.py DIR FRAME flat  W H OUT_W OUT_H     exact colour preservation
  check_dump.py DIR FRAME edges W H OUT_W OUT_H     structure and sharpness against bilinear
  check_dump.py DIR FRAME moving W H OUT_W OUT_H    the generated frame shows the box halfway

Also writes PNG copies next to the PPM files for viewing.
"""
import math
import pathlib
import sys

from PIL import Image


def pixels(image):
    # Pillow 12.1 deprecates getdata() in favour of get_flattened_data(); runners differ.
    return list(image.get_flattened_data() if hasattr(image, 'get_flattened_data') else image.getdata())


def load(path):
    image = Image.open(path)
    image.load()
    return image.convert('RGB')


def main():
    directory, frame, kind = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    in_w, in_h, out_w, out_h = map(int, sys.argv[4:8])
    source = load(directory / ('xhyn-upscale-in-%s.ppm' % frame))
    result = load(directory / ('xhyn-upscale-out-%s.ppm' % frame))
    source.save(directory / ('in-%s.png' % frame))
    result.save(directory / ('out-%s.png' % frame))
    assert source.size == (in_w, in_h), ('input size', source.size)
    assert result.size == (out_w, out_h), ('output size', result.size)

    if kind == 'moving':
        # The harness box is 48 pixels wide and starts at x = 100 + 16 * frame. FRAME counts from 1
        # and is the current frame, so the generated one lies 8 pixels behind it.
        generated = load(directory / ('xhyn-upscale-gen-%s.ppm' % frame))
        assert generated.size == (in_w, in_h), ('generated size', generated.size)
        bright = [x for y in range(in_h) for x in range(in_w) if generated.getpixel((x, y))[1] > 200]
        assert bright, 'generated frame has no box'
        centre = sum(bright) / len(bright)
        expected = 100 + 16 * (int(frame) - 1) - 8 + 24
        print('moving: box centre x=%.1f, expected %d, %d bright pixels (box has %d)' % (centre, expected, len(bright), 48 * 48))
        assert abs(centre - expected) <= 4, 'box is not halfway between the two frames'
        assert 0.6 * 48 * 48 <= len(bright) <= 1.3 * 48 * 48, 'box is ghosted or smeared'
        return

    if kind == 'flat':
        expected = (100, 150, 200)
        worst = max(abs(a - b) for pixel in pixels(result) for a, b in zip(pixel, expected))
        print('flat colour: worst channel error %d (input %s)' % (worst, source.getpixel((5, 5))))
        assert source.getpixel((5, 5)) == expected, 'input pattern changed'
        assert worst <= 1, 'flat colour not preserved'
        return

    # Edges: the output must stay aligned with, and close to, a bilinear upscale of the input
    # (SGSR only adds a bounded sharpening correction to it).
    bilinear = source.resize((out_w, out_h), Image.BILINEAR)
    pairs = zip(pixels(result), pixels(bilinear))
    mse = sum((a - b) ** 2 for p, q in pairs for a, b in zip(p, q)) / (3 * out_w * out_h)
    psnr = 10 * math.log10(255 * 255 / mse) if mse else 99
    print('edges: PSNR vs bilinear %.1f dB' % psnr)
    assert psnr > 22, 'output differs too much from the bilinear reference (misaligned or wrong colours)'
    extrema = result.getextrema()
    assert extrema[0][1] > 200 and extrema[1][1] > 200, 'output looks blank'


if __name__ == '__main__':
    main()
