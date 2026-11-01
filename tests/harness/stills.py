# SPDX-License-Identifier: BSD-3-Clause
"""
stills.py - the still pictures of the image tests, from clip42's first frame:
every format and variant the image decoder is tried with, and Pillow's decode
of each small one as raw RGBA (<name>.rgba), the reference.

    python3 stills.py OUTDIR pic.jpg

make_stills() writes OUTDIR/manifest.json too: {name: [width, height]}.  The
recipes are the test material and must not change without a new
MATERIAL_VERSION (material.py).
"""
import json
import os
import struct
import sys

import numpy as np
import png
from PIL import Image


def make_stills(out: str, src: str) -> dict[str, list[int]]:
    """Write every still and its reference into @out from the JPEG @src; return the manifest."""
    os.makedirs(out, exist_ok=True)
    p = lambda n: os.path.join(out, n)  # noqa: E731
    full = Image.open(src).convert('RGB')
    base = full.crop((0, 0, 640, 480))
    odd = full.crop((0, 0, 641, 479))
    yy, xx = np.mgrid[0:480, 0:640]
    alpha = ((xx + yy) * 255 // (640 + 480)).astype(np.uint8)  # a diagonal ramp: alpha with real variation
    rgba = Image.fromarray(np.dstack([np.asarray(base), alpha]), 'RGBA')
    pal = base.quantize(256)

    # decoded by the hardware
    base.save(p('j420.jpg'), quality=90, subsampling=2)
    base.save(p('j422.jpg'), quality=90, subsampling=1)
    base.save(p('j444.jpg'), quality=90, subsampling=0)
    base.convert('L').save(p('jgrey.jpg'), quality=90)
    base.save(p('jrst.jpg'), quality=90, restart_marker_blocks=4)
    jpeg = open(p('j420.jpg'), 'rb').read()  # comments before the frame header: 79 KB within the first 80 KiB packet, 120 KB past it
    comment = lambda n: b'\xff\xfe' + struct.pack('>H', n + 2) + bytes(n)  # noqa: E731
    open(p('jmeta79.jpg'), 'wb').write(jpeg[:2] + comment(60000) + comment(19000) + jpeg[2:])
    open(p('jbigmeta.jpg'), 'wb').write(jpeg[:2] + comment(60000) + comment(60000) + jpeg[2:])
    full.save(p('j1080.jpg'), quality=90)
    jpeg = open(p('j1080.jpg'), 'rb').read()  # metadata in short runs: without its longest the header still ends past the first packet
    app0 = b'\xff\xe0' + struct.pack('>H', 16) + b'JFIF\0\1\1\0\0\1\0\1\0\0'
    open(p('jsplit.jpg'), 'wb').write(jpeg[:2] + (comment(30000) + app0) * 4 + jpeg[2:])
    full.resize((800, 600), Image.BICUBIC).save(p('j800.jpg'), quality=90)  # rows of whole 32 pixels, not 64: the ISP reads I420 in 64s
    full.resize((4000, 3000), Image.BICUBIC).save(p('j4000.jpg'), quality=80)  # 12 MP, likewise
    full.resize((1800, 1101), Image.BICUBIC).save(p('jodd.jpg'), quality=90)  # 2 MP whose rows the VPU cannot write into picture.datatype's buffer
    base.save(p('prgb.png'))
    rgba.save(p('prgba.png'))
    pal.save(p('ppal.png'), transparency=0)
    pal.save(p('palpha.png'), transparency=bytes(range(256)))  # entry i at alpha i: levels per palette entry
    pal.save(p('gstatic.gif'))
    frames = [pal, base.transpose(Image.FLIP_LEFT_RIGHT).quantize(256)]
    frames[0].save(p('ganim.gif'), save_all=True, append_images=frames[1:], duration=100, loop=0)
    base.save(p('b24.bmp'))
    rgba.save(p('b32.bmp'))
    pal.save(p('b8.bmp'))
    raw = np.asarray(base)[:, :, ::-1].tobytes()  # top-down: BGR rows, top first, a negative height
    header = struct.pack('<2sIHHI', b'BM', 54 + len(raw), 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, 640, -480, 1, 24, 0, len(raw), 2835,
                                                                                    2835, 0, 0)
    open(p('btop.bmp'), 'wb').write(header + raw)
    base.save(p('t24.tga'))
    rgba.save(p('t32.tga'))
    base.save(p('p6.ppm'))
    base.convert('L').save(p('p5.pgm'))
    full.resize((6000, 4000), Image.BICUBIC).save(p('j6000.jpg'), quality=80)  # its RGBA through the converter: past the codec heap
    full.resize((6016, 4000), Image.BICUBIC).save(p('j6016.jpg'), quality=80)  # its RGBA straight from the decoder
    full.resize((8000, 6000), Image.BICUBIC).save(p('jhuge.jpg'), quality=80)

    # refused from the header
    base.save(p('jprog.jpg'), quality=90, progressive=True)
    base.convert('CMYK').save(p('jcmyk.jpg'), quality=90)
    base.convert('L').save(p('pgrey.png'))
    rgba.convert('LA').save(p('pgreya.png'))
    odd.save(p('podd.png'))
    rgb48 = (np.asarray(base).astype(np.uint16) * 257).reshape(480, 640 * 3)
    png.Writer(640, 480, greyscale=False, bitdepth=16).write(open(p('p16.png'), 'wb'), rgb48.tolist())
    png.Writer(640, 480, greyscale=False, interlace=True).write(open(p('pinter.png'), 'wb'), np.asarray(base).reshape(480, 640 * 3).tolist())
    pal.save(p('gtrans.gif'), transparency=0)
    keyed = np.asarray(base).copy()
    keyed[100:200, 100:300] = (0, 255, 0)  # a block of the transparent colour
    Image.fromarray(keyed, 'RGB').save(p('pkey.png'), transparency=(0, 255, 0))
    base.save(p('trle.tga'), compression='tga_rle')

    # cut short: refused with -8, their data not all there
    for whole, cut in (('j420.jpg', 'jcut.jpg'), ('prgb.png', 'pcut.png'), ('gstatic.gif', 'gcut.gif'), ('b24.bmp', 'bcut.bmp'),
                       ('t24.tga', 'tcut.tga'), ('p6.ppm', 'p6cut.ppm')):
        data = open(p(whole), 'rb').read()
        open(p(cut), 'wb').write(data[:len(data) * 7 // 10])

    manifest = {}
    for name in sorted(os.listdir(out)):
        if name.endswith(('.json', '.rgba')):
            continue
        im = Image.open(p(name))
        manifest[name] = list(im.size)
        if im.size[0] * im.size[1] <= 1920 * 1080 and 'cut.' not in name:
            im.seek(0)
            open(p(name + '.rgba'), 'wb').write(im.convert('RGBA').tobytes())
    json.dump(manifest, open(p('manifest.json'), 'w'), indent=1)
    return manifest


if __name__ == '__main__':
    make_stills(sys.argv[1], sys.argv[2])
