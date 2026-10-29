#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
"""Checks the PPM frames the layer dumps (dump_frame) after a harness run.

  check_dump.py DIR FRAME flat  W H OUT_W OUT_H     exact colour preservation
  check_dump.py DIR FRAME edges W H OUT_W OUT_H     structure and sharpness against bilinear

Also writes PNG copies next to the PPM files for viewing.
"""
import math
import pathlib
import sys

from PIL import Image


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

    if kind == 'flat':
        expected = (100, 150, 200)
        worst = max(abs(a - b) for pixel in result.get_flattened_data() for a, b in zip(pixel, expected))
        print('flat colour: worst channel error %d (input %s)' % (worst, source.getpixel((5, 5))))
        assert source.getpixel((5, 5)) == expected, 'input pattern changed'
        assert worst <= 1, 'flat colour not preserved'
        return

    # Edges: the output must stay aligned with, and close to, a bilinear upscale of the input
    # (SGSR only adds a bounded sharpening correction to it).
    bilinear = source.resize((out_w, out_h), Image.BILINEAR)
    pairs = zip(result.get_flattened_data(), bilinear.get_flattened_data())
    mse = sum((a - b) ** 2 for p, q in pairs for a, b in zip(p, q)) / (3 * out_w * out_h)
    psnr = 10 * math.log10(255 * 255 / mse) if mse else 99
    print('edges: PSNR vs bilinear %.1f dB' % psnr)
    assert psnr > 22, 'output differs too much from the bilinear reference (misaligned or wrong colours)'
    extrema = result.getextrema()
    assert extrema[0][1] > 200 and extrema[1][1] > 200, 'output looks blank'


if __name__ == '__main__':
    main()
