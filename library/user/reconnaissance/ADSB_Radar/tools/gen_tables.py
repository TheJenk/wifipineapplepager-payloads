#!/usr/bin/env python3
"""gen_tables.py - regenerate the lookup tables used by src/ (run from this folder).
    python3 gen_tables.py      -> ../src/nl_table.h  ../src/trig_tables.h
nl_table.h:     CPR "NL" latitude-zone transition latitudes (ADS-B position decoding)
trig_tables.h:  sine per degree (Q14) and tangent for 0..45 degrees (Q16)
"""
import math, os
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
with open(os.path.join(src, "nl_table.h"), "w") as f:
    f.write("// CPR NL transition latitudes, 2^32 units (360 deg); index k -> NL = 59-k when |lat| < thr[k]\n")
    f.write("static const uint32_t nl_thr[58] = {\n")
    vals = []
    for nl in range(59, 1, -1):
        a = math.acos(math.sqrt((1 - math.cos(math.pi / 30)) / (1 - math.cos(2 * math.pi / nl)))) * 180 / math.pi
        vals.append(int(round(a * 2**32 / 360)))
    f.write("  " + ",".join(str(v) + "u" for v in vals) + "\n};\n")
with open(os.path.join(src, "trig_tables.h"), "w") as f:
    f.write("// generated: sin per degree (Q14), tan per degree 0..45 (Q16)\n")
    f.write("static const int16_t SIN360[360] = {" + ",".join(str(int(round(math.sin(math.radians(d)) * 16384))) for d in range(360)) + "};\n")
    f.write("static const int32_t TAN46[46] = {" + ",".join(str(int(round(math.tan(math.radians(d)) * 65536))) for d in range(46)) + "};\n")
print("wrote", os.path.normpath(os.path.join(src, "nl_table.h")), "and trig_tables.h")
