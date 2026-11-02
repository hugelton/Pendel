#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""A Pocket AMP's whole-flash image, as far as Pendel needs it: find app.bin, read it, put another one of the same size
in its place. Standard library only.

The layout is the JieLi chip's (AC79 / WL82), as the boot loader reads it:
  0x00     the flash header, 32 bytes, ENC-scrambled (key 0xFFFF), CRC16 of bytes 2..31 in bytes 0..1
  0x20...  32-byte directory entries, each ENC-scrambled: hcrc, dcrc, offset, size, flags, reserved, index, name[16];
           the last has a non-zero index. isd_config.ini carries the chip key (a 32-byte blob and its CRC16),
           app_dir_head (flags 0x81) the app area's base
  base...  the app area, SFC-scrambled: ENC per 32-byte block with the chip key XOR (block offset from base) >> 2. Its
           first entry, app_area_head, spans the entries after it (app.bin flags 0x82, cfg_tool.bin, ...) and the
           data; its dcrc covers all of that
ENC: each byte XORed with the low byte of a 16-bit key that steps as a CRC-16/CCITT register (poly 0x1021) does.
The chip key's encoding follows kagaimiq's jl-misctools (chipkeybin, MIT).
"""
import struct


def crc16(data, crc=0):
    """CRC-16/CCITT, MSB first, init 0 (XMODEM)"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def enc(buf, off, size, key=0xFFFF):
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def sfc(buf, off, size, base, key):
    for i in range(0, size, 32):
        enc(buf, off + i, min(size - i, 32), key ^ ((off + i - base) >> 2))


def chip_key(blob):
    """the chip key from isd_config.ini's 32-byte blob"""
    s = sum(blob[:16]) & 0xFF
    s = 0xAA if s >= 0xE0 else 0x55 if s <= 0x10 else s
    return sum(1 << i for i in range(16) if (blob[16 + i] ^ blob[15 - i]) < s)


class Entry:
    FMT = "<HHIIBBH16s"

    def __init__(self, buf, off):
        (self.hcrc, self.dcrc, self.offset, self.size, self.flags, self.resvd, self.index,
         self.raw_name) = struct.unpack_from(self.FMT, buf, off)
        self.at = off
        self.name = self.raw_name.split(b"\0")[0].decode("ascii", "replace")
        if crc16(buf[off + 2:off + 32]) != self.hcrc:
            raise ValueError(f"directory entry CRC mismatch at {off:#x}")

    def store(self, buf):
        body = struct.pack("<HIIBBH16s", self.dcrc, self.offset, self.size, self.flags, self.resvd, self.index,
                           self.raw_name)
        self.hcrc = crc16(body)
        buf[self.at:self.at + 32] = struct.pack("<H", self.hcrc) + body


class FlashImage:
    def __init__(self, flash: bytes):
        self.flash = bytearray(flash)
        plain = bytearray(flash)
        enc(plain, 0, 32)
        if crc16(plain[2:32]) != struct.unpack_from("<H", plain)[0]:
            raise ValueError("flash header CRC mismatch")
        self.key = self.base = None
        off = 32
        while True:
            enc(plain, off, 32)
            e = Entry(plain, off)
            if e.name == "isd_config.ini":
                blob = plain[e.offset:e.offset + 34]
                if crc16(blob[:32]) == struct.unpack_from("<H", blob, 32)[0]:
                    self.key = chip_key(blob[:32])
            if e.flags == 0x81 and e.name == "app_dir_head" and self.base is None:
                self.base = e.offset
            if e.index:
                break
            off += 32
        if self.key is None or self.base is None:
            raise ValueError("chip key or app area not found")
        b = self.base
        head = bytearray(self.flash[b:b + 32])
        sfc(head, 0, 32, 0, self.key)
        self.head = Entry(head, 0)
        self.area = bytearray(self.flash[b:b + self.head.size])   # the head's block, unscrambled
        sfc(self.area, 0, len(self.area), 0, self.key)
        self.head = Entry(self.area, 0)
        self.app = None
        off = 32
        while True:
            e = Entry(self.area, off)
            if e.flags == 0x82 and e.name == "app.bin":
                self.app = e
            if e.index:
                break
            off += 32
        if self.app is None:
            raise ValueError("app.bin not found")

    def app_bytes(self):
        return bytes(self.area[self.app.offset:self.app.offset + self.app.size])

    def set_app(self, new: bytes):
        """app.bin replaced by one of the same size: its dcrc, the head's dcrc, the block scrambled again"""
        if len(new) != self.app.size:
            raise ValueError(f"app is {len(new)} B, the slot {self.app.size} B")
        a = self.app.offset
        self.area[a:a + len(new)] = new
        self.app.dcrc = crc16(new)
        self.app.store(self.area)
        self.head.dcrc = crc16(self.area[32:self.head.size])
        self.head.store(self.area)
        out = bytearray(self.area)
        sfc(out, 0, len(out), 0, self.key)
        self.flash[self.base:self.base + len(out)] = out
        return bytes(self.flash)
