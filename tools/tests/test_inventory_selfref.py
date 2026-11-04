#!/usr/bin/env python3
"""[UIT 0364] tools/shaders/inventory.py sharcb_shaders: an agl package (BAHS) whose control holds the size word
0x880 followed by a control offset of 0 (a spot inside a control that reads like a record pointing at itself) gives
only the real shader, not a second "shader" made of the bytes around that spot. Built in memory; no game data.
    python3 tools/tests/test_inventory_selfref.py"""
import pathlib, struct, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from shaders.inventory import sharcb_shaders

START, POOL, POOL_SIZE = 32, 4096, 64
data = bytearray(POOL + POOL_SIZE)
data[:4] = b'BAHS'
struct.pack_into('<3I', data, 20, START, POOL_SIZE, POOL)
CONTROL = 48                                        # the real control, 0x880 bytes
struct.pack_into('<QIi', data, START, 0, 0x880, CONTROL - START)   # the real record: code at pool+0, its control
struct.pack_into('<II', data, CONTROL + 0x700, 0, 16)              # code 16 bytes
struct.pack_into('<I', data, CONTROL + 0x714, 0)                    # vertex shader
for i in range(POOL_SIZE):
    data[POOL + i] = (i * 37 + 11) & 0xff
# inside the control: <u64 0, 0x880, i32 0>, which reads as a record whose control starts at itself
FAKE = CONTROL + 0x100 - 8
struct.pack_into('<QIi', data, FAKE, 0, 0x880, 0)
struct.pack_into('<II', data, FAKE + 0x700, 0, 8)                   # makes that "control" look valid too
struct.pack_into('<I', data, FAKE + 0x714, 1)

found = list(sharcb_shaders(bytes(data)))
fails = []
if len(found) != 1:
    fails.append(f'{len(found)} shaders found, want 1 (the size word inside the control is not a record)')
elif found[0][1] != 'vs' or found[0][3] != bytes(data[POOL:POOL + 16]) or found[0][2] != bytes(data[CONTROL:CONTROL + 0x880]):
    fails.append('the real shader is not the one found')
# a real record whose control comes before it (negative offset) still counts
data2 = bytearray(data)
REC2 = CONTROL + 0x880 + 8
struct.pack_into('<QIi', data2, REC2, 16, 0x88c, CONTROL - REC2)    # second record reuses the first control, other code
found2 = list(sharcb_shaders(bytes(data2)))
if len(found2) != 2:
    fails.append(f'with a second record that reuses the control: {len(found2)} shaders, want 2')
for f in fails: print('FAIL:', f)
print('all ok' if not fails else f'{len(fails)} failure(s)')
sys.exit(1 if fails else 0)
