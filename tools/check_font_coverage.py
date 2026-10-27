#!/usr/bin/env python3
"""检查项目内所有 C 字符串字面量的汉字是否都在 font16.bin 字库里。"""
import glob
import re
import struct
import sys

ROOT = __file__.rsplit("\\tools", 1)[0] if "\\tools" in __file__ else "."
FONT = "projects/pokego/main/assets/font16.bin"

data = open(FONT, "rb").read()
cnt = struct.unpack("<I", data[10:14])[0]
have = set(struct.unpack("<%dH" % cnt, data[14:14 + 2 * cnt]))

pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
missing = {}
files = sorted(set(glob.glob("projects/pokego/main/**/*.c", recursive=True) +
                   glob.glob("projects/pokego/main/**/*.h", recursive=True)))
for f in files:
    src = open(f, encoding="utf-8").read()
    for lit in pat.findall(src):
        try:
            txt = lit.encode().decode("unicode_escape").encode("latin-1").decode("utf-8")
        except Exception:
            txt = lit
        for ch in txt:
            if ord(ch) > 0x7F and ord(ch) not in have:
                missing.setdefault(ch, set()).add(f)

for ch, fs in sorted(missing.items()):
    print("缺字 '%s' in %s" % (ch, sorted(fs)))
print("缺字总数:", len(missing))
sys.exit(1 if missing else 0)
