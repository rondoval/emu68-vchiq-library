#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Write videocore.audio's AHI mode file (DEVS:AudioModes/VIDEOCORE).

An IFF FORM AHIM: AUDN names the driver, one AUDM per mode holds its tags
(TagItems, big-endian, ending in TAG_DONE) with the mode's name after them;
a string tag's value is the string's offset from the start of the chunk's
data.  The outputs (HDMI0, HDMI1, the jack) are not modes: the driver
names them (AHIDB_Outputs) and AHI's preferences pick one per unit.

ahi.device reads every file in DEVS:AudioModes at its first open (and on
AddAudioModes REFRESH), opens the driver AUDN names, and lists the modes.

Usage: modefile.py <output file>
"""
import struct
import sys

# Tag numbers, as in the AHI developer kit's devices/ahi.h.  A tag whose
# value is a pointer (the mode's name) is in the AHI_TagBaseR range: in a
# mode file its value is an offset, which ahi.device turns into a pointer.
TAG_USER = 0x80000000
AHI_TAGBASE = TAG_USER
AHI_TAGBASE_R = AHI_TAGBASE | 0x8000

AHIDB_AUDIOID = AHI_TAGBASE + 100
AHIDB_VOLUME = AHI_TAGBASE + 103
AHIDB_PANNING = AHI_TAGBASE + 104
AHIDB_STEREO = AHI_TAGBASE + 105
AHIDB_HIFI = AHI_TAGBASE + 106
AHIDB_MULTTABLE = AHI_TAGBASE + 108
AHIDB_NAME = AHI_TAGBASE_R + 109
AHIDB_MULTICHANNEL = AHI_TAGBASE + 144  # ahi.device 6
TAG_DONE = 0

# The driver's name without ".audio": ahi.device opens DEVS:AHI/videocore.audio.
DRIVER = "videocore"

# (audio ID, HiFi, 7.1, name).  The upper word of an ID is the hardware's
# (0x0E68, "E68", free of every allocated one); the lower word is the
# driver's: the mixer in bits 0-4 (1 16-bit, 2 HiFi, 3 HiFi 7.1).  AHI names
# a stereo mode with panning "stereo++".  A 7.1 mode must be HiFi too: AHI 6
# mixes 7.1 only into 32-bit buffers.
MODES = [
    (0x0E680001, False, False, "VideoCore:16 bit stereo++"),
    (0x0E680002, True, False, "VideoCore:HiFi 16 bit stereo++"),
    (0x0E680003, True, True, "VideoCore:HiFi 16 bit 7.1 multichannel"),
]


def chunk(cid, data):
    """An IFF chunk: id, size, data, padded to an even length."""
    pad = b"\0" if len(data) & 1 else b""  # the size field excludes the pad byte
    return cid + struct.pack(">I", len(data)) + data + pad


def audm(audio_id, hifi, multichannel, name):
    """One mode's chunk: its tags, TAG_DONE, then its name."""
    tags = [
        (AHIDB_AUDIOID, audio_id),
        (AHIDB_VOLUME, 1),  # AHI's mixer sets each channel's volume
        (AHIDB_PANNING, 1),  # and its position between left and right
        (AHIDB_STEREO, 1),  # the driver takes stereo buffers (and 7.1 ones)
        (AHIDB_HIFI, 1 if hifi else 0),  # 32-bit mixing, no pre-division by the channel count
        (AHIDB_MULTTABLE, 0),  # AHI's private: no multiplication table
    ]
    if multichannel:
        tags.append((AHIDB_MULTICHANNEL, 1))  # 8-channel buffers (AHIST_L7_1); AHI 4 ignores the tag
    # AHIDB_Name's value is where the name starts, counted from the start of
    # the chunk's data.  The name follows the tag list, which is:
    #   len(tags) TagItems above, plus AHIDB_Name's own one (appended below):
    #     (len(tags) + 1) items of 8 bytes each (ti_Tag and ti_Data, 4 + 4)
    #   then TAG_DONE: a lone 4-byte tag, no data
    # Six tags above: (6 + 1) * 8 + 4 = 60 = 0x3C (seven for the 7.1 mode: 68).
    name_offset = (len(tags) + 1) * 8 + 4
    tags.append((AHIDB_NAME, name_offset))
    data = b"".join(struct.pack(">II", t, v) for t, v in tags) + struct.pack(">I", TAG_DONE)
    return chunk(b"AUDM", data + name.encode("ascii") + b"\0")


def build():
    """The whole file: FORM AHIM around AUDN and one AUDM per mode."""
    body = b"AHIM" + chunk(b"AUDN", DRIVER.encode("ascii") + b"\0")
    body += b"".join(audm(*mode) for mode in MODES)
    return b"FORM" + struct.pack(">I", len(body)) + body


def main():
    """Build the file and write it."""
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "wb") as f:
        f.write(build())


if __name__ == "__main__":
    main()
