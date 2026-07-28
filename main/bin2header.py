#!/usr/bin/env python3
"""Convert binary file to C array header."""
import sys

data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('/* auto-generated */\n')
    f.write('unsigned char wasm_test_wasm[] = {')
    f.write(','.join(str(b) for b in data))
    f.write('};\n')
    f.write('unsigned int wasm_test_wasm_len = ' + str(len(data)) + ';\n')