#!/usr/bin/env python3
"""Convert binary file to C array header.
Usage: bin2header.py <input> <output.h> <symbol_name>"""
import sys

data = open(sys.argv[1], 'rb').read()
name = sys.argv[3] if len(sys.argv) > 3 else 'wasm_test_wasm'
with open(sys.argv[2], 'w') as f:
    f.write('/* auto-generated */\n')
    f.write('unsigned char %s[] = {' % name)
    f.write(','.join(str(b) for b in data))
    f.write('};\n')
    f.write('unsigned int %s_len = ' % name + str(len(data)) + ';\n')