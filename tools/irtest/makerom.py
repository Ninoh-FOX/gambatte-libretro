#!/usr/bin/env python3
# Infrared test ROM for the dual link (Game Boy Color only).
# Holding A: blinks the LED (RP bit 0), toggling every ~40 loop turns.
# Not holding A: counts in C000-C001 every change its sensor (RP bit 1) sees.
# C004 counts the LED toggles of a sender. Usage: makerom.py out.gbc
import sys

code = bytearray()
labels, fixups = {}, []
def emit(*b): code.extend(b)
def label(n): labels[n] = 0x150 + len(code)
def jr(op, n): emit(op, 0); fixups.append(('jr', len(code) - 1, n))
def jp(n): emit(0xC3, 0, 0); fixups.append(('jp', len(code) - 2, n))

emit(0xF3)                       # di
emit(0x31, 0xFF, 0xDF)           # ld sp,$DFFF
emit(0x3E, 0xC0, 0xE0, 0x56)     # ld a,$C0 ; ldh ($56),a  (reading on, LED off)
emit(0xAF)                       # xor a
for a in (0xC000, 0xC001, 0xC002, 0xC003, 0xC004, 0xC005):
    emit(0xEA, a & 0xFF, a >> 8) # ld (a),a
emit(0x3E, 0x02, 0xEA, 0x02, 0xC0)  # ld a,2 ; ld (C002),a  (sensor idle: bit 1 set)
label('loop')
emit(0x3E, 0x10, 0xE0, 0x00)     # ld a,$10 ; ldh ($00),a  (buttons)
emit(0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00)  # ldh a,($00) x3
emit(0xCB, 0x47)                 # bit 0,a  (A: 0 = pressed)
jr(0x20, 'receiver')             # jr nz,receiver
label('sender')
emit(0xFA, 0x03, 0xC0)           # ld a,(C003)
emit(0xEE, 0x01)                 # xor 1
emit(0xEA, 0x03, 0xC0)           # ld (C003),a
emit(0xF6, 0xC0, 0xE0, 0x56)     # or $C0 ; ldh ($56),a
emit(0x21, 0x04, 0xC0, 0x34)     # ld hl,C004 ; inc (hl)
emit(0x06, 40)                   # ld b,40
label('delay')
emit(0x05)                       # dec b
jr(0x20, 'delay')                # jr nz,delay
jp('loop')
label('receiver')
emit(0x3E, 0xC0, 0xE0, 0x56)     # LED off, reading on
emit(0xF0, 0x56, 0xE6, 0x02, 0x4F)  # ldh a,($56) ; and 2 ; ld c,a
emit(0xFA, 0x02, 0xC0, 0xB9)     # ld a,(C002) ; cp c
jr(0x28, 'loop')                 # jr z,loop
emit(0x79, 0xEA, 0x02, 0xC0)     # ld a,c ; ld (C002),a
emit(0x21, 0x00, 0xC0, 0x34)     # ld hl,C000 ; inc (hl)
jr(0x20, 'loop2')                # jr nz,loop2
emit(0x23, 0x34)                 # inc hl ; inc (hl)
label('loop2')
jp('loop')

for kind, at, n in fixups:
    tgt = labels[n]
    if kind == 'jr':
        off = tgt - (0x150 + at + 1)
        assert -128 <= off < 128
        code[at] = off & 0xFF
    else:
        code[at] = tgt & 0xFF
        code[at + 1] = tgt >> 8

rom = bytearray(b'\xFF' * 0x8000)
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
rom[0x104:0x134] = bytes.fromhex(
    'CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999'
    'BBBB67636E0EECCCDDDC999FBBB9333E')
rom[0x134:0x143] = b'IRTEST'.ljust(15, b'\0')
rom[0x143] = 0xC0                # Game Boy Color only
rom[0x147] = 0x00                # ROM only
rom[0x148] = 0x00
rom[0x149] = 0x00
rom[0x14A] = 0x01
c = 0
for i in range(0x134, 0x14D):
    c = (c - rom[i] - 1) & 0xFF
rom[0x14D] = c
rom[0x150:0x150 + len(code)] = code
s = sum(rom) - rom[0x14E] - rom[0x14F]
rom[0x14E] = (s >> 8) & 0xFF
rom[0x14F] = s & 0xFF
open(sys.argv[1], 'wb').write(rom)
