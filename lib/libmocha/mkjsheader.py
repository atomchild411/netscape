#!/usr/bin/env python3
# Turn a JavaScript file into a C header holding it as one string:
#   mkjsheader.py qjs_dom.js qjs_dom_js.h qjs_dom_js
import sys

src, out, name = sys.argv[1:4]
with open(src, encoding='utf-8') as f:
    lines = f.read().split('\n')
with open(out, 'w', encoding='utf-8') as f:
    f.write('/* Generated from %s by mkjsheader.py: do not edit. */\n\n' % src)
    f.write('static const char %s[] =\n' % name)
    for line in lines:
        esc = line.replace('\\', '\\\\').replace('"', '\\"').replace('??', '?\\?')
        f.write('"%s\\n"\n' % esc)
    f.write(';\n')
