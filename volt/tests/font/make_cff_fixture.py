"""Writes cff_fixture.hpp: a small OpenType CFF font, as bytes, with the
answers the font tests expect from it.

No CFF font ships with the repository, so the CFF test carries its own. Its
glyphs are DefaultFont.ttf's outlines turned into cubics, with what real CFF
fonts contain added by hand: the advance width in the charstrings, stem
hints and hint masks (one with an implied vstem), local and global
subroutines (one ending in endchar), the operators the specializer picks
(hvcurveto and friends), and one made-up glyph that draws all four flex
operators. The expected areas and boxes come from fontTools' own charstring
decompiler and pens, independently of Volt.

    python make_cff_fixture.py   (needs fontTools; run from this directory)
"""

import io
import os

from fontTools.cffLib import GlobalSubrsIndex, SubrsIndex
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.pens.areaPen import AreaPen
from fontTools.pens.boundsPen import BoundsPen, ControlBoundsPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "assets", "fonts", "DefaultFont.ttf")
LOCAL_BIAS = 107
GLOBAL_BIAS = 107

source = TTFont(SOURCE)
glyphs = source.getGlyphSet()
cmap = source.getBestCmap()
names = [".notdef", "A", "V", "g", "o", "ampersand", "eacute", "flexes"]
codes = {"A": 0x41, "V": 0x56, "g": 0x67, "o": 0x6F, "ampersand": 0x26, "eacute": 0xE9, "flexes": 0xE000}


def program_of(name):
    pen = T2CharStringPen(width=None, glyphSet=glyphs)
    glyphs[name].draw(pen)
    return list(pen.getCharString().program)


def operator_indices(program):
    return [i for i, token in enumerate(program) if isinstance(token, str)]


programs = {}
for name in names[:-1]:
    programs[name] = program_of(name)

# The made-up glyph: flex, hflex, a line, hflex1, flex1, then endchar
# closes it. Every flex keeps its end level with its start, so the shape is
# a band with wavy top and bottom.
programs["flexes"] = [
    100, 100, "rmoveto",
    50, 0, 50, 20, 50, 0, 50, 0, 50, -20, 50, 0, 50, "flex",
    30, 30, 25, 30, 30, 30, 30, "hflex",
    0, 200, "rlineto",
    -30, 10, -30, 15, -30, -30, -30, -15, -30, "hflex1",
    -30, 5, -30, 5, -30, 5, -30, -5, -30, -5, -30, "flex1",
    "endchar",
]

widths = {name: source["hmtx"][name][0] for name in names[:-1]}
widths["flexes"] = 700

# Hints, and the width in front of them (an odd operand count says so).
for name in names:
    program = programs[name]
    if name in ("A", "o", "flexes"):
        programs[name] = [widths[name], 0, 50, "hstemhm", 100, 40, 300, 40, "vstemhm", "hintmask", bytes([0xE0])] + program
    elif name == "g":
        # hstemhm, then a hintmask whose operands are an implied vstem.
        programs[name] = [widths[name], -400, 60, "hstemhm", 80, 50, "hintmask", bytes([0xC0])] + program
    else:
        programs[name] = [widths[name]] + program

# Subroutines: V's tail (endchar included) is local subroutine 0, o's
# middle is global subroutine 0, ampersand's middle local subroutine 1,
# reached through global subroutine 1, which calls it.
local = []
global_ = []


def split(name, start_fraction, end_fraction):
    program = programs[name]
    ops = operator_indices(program)
    begin = ops[int(len(ops) * start_fraction)] + 1
    end = ops[min(int(len(ops) * end_fraction), len(ops) - 1)] + 1
    return program[:begin], program[begin:end], program[end:]


head, tail, _ = split("V", 0.4, 1.0)
local.append(tail + ([] if tail[-1] == "endchar" else ["endchar"]))
programs["V"] = head + [0 - LOCAL_BIAS, "callsubr"]

head, middle, rest = split("o", 0.3, 0.7)
global_.append(middle + ["return"])
programs["o"] = head + [0 - GLOBAL_BIAS, "callgsubr"] + rest

head, middle, rest = split("ampersand", 0.2, 0.6)
local.append(middle + ["return"])
global_.append([1 - LOCAL_BIAS, "callsubr", "return"])
programs["ampersand"] = head + [1 - GLOBAL_BIAS, "callgsubr"] + rest

builder = FontBuilder(2048, isTTF=False)
builder.setupGlyphOrder(names)
builder.setupCharacterMap({codes[n]: n for n in names if n in codes})
charstrings = {name: T2CharString(program=programs[name]) for name in names}
builder.setupCFF("VoltCFFFixture", {"FullName": "Volt CFF Fixture"}, charstrings, {})
builder.setupHorizontalMetrics({name: (widths[name], 0) for name in names})
builder.setupHorizontalHeader(ascent=1854, descent=-434)
builder.setupNameTable({"familyName": "Volt CFF Fixture", "styleName": "Regular"})
builder.setupOS2(sTypoAscender=1491, sTypoDescender=-431, usWinAscent=1854, usWinDescent=434)
builder.setupPost()

top = builder.font["CFF "].cff.topDictIndex[0]
private = top.Private
gsubrs = builder.font["CFF "].cff.GlobalSubrs
for program in global_:
    gsubrs.append(T2CharString(program=program, private=private, globalSubrs=gsubrs))
private.Subrs = SubrsIndex(private=private, globalSubrs=gsubrs)
for program in local:
    private.Subrs.append(T2CharString(program=program, private=private, globalSubrs=gsubrs))
for name in names:
    top.CharStrings[name].private = private
    top.CharStrings[name].globalSubrs = gsubrs

stream = io.BytesIO()
builder.save(stream)
data = stream.getvalue()

# The answers, from a fresh parse of the bytes.
check = TTFont(io.BytesIO(data))
check_glyphs = check.getGlyphSet()
rows = []
for gid, name in enumerate(check.getGlyphOrder()):
    area, control, tight = AreaPen(check_glyphs), ControlBoundsPen(check_glyphs), BoundsPen(check_glyphs)
    for pen in (area, control, tight):
        check_glyphs[name].draw(pen)
    rows.append((gid, name, codes.get(name, 0), check["hmtx"][name][0], area.value, control.bounds, tight.bounds))

lines = [
    "#pragma once",
    "",
    "// Generated by make_cff_fixture.py; do not edit. A small OpenType CFF font",
    "// and what fontTools says about each of its glyphs.",
    "",
    "#include <cstddef>",
    "#include <cstdint>",
    "",
    "namespace volt::tests::font",
    "{",
    "    inline constexpr unsigned char cff_fixture[] = {",
]
for i in range(0, len(data), 16):
    lines.append("        " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
lines += [
    "    };",
    "",
    "    struct cff_fixture_glyph",
    "    {",
    "        std::uint16_t glyph;",
    "        const char *name;",
    "        char32_t code_point;",
    "        int advance;",
    "        double area; // signed, y up: negative for clockwise outer contours",
    "        float control[4]; // x min, y min, x max, y max",
    "        float tight[4];",
    "    };",
    "",
    "    inline constexpr cff_fixture_glyph cff_fixture_glyphs[] = {",
]
for gid, name, code, advance, area, control, tight in rows:
    control = control or (0, 0, 0, 0)
    tight = tight or (0, 0, 0, 0)
    lines.append(
        "        {%d, \"%s\", 0x%X, %d, %.6f, {%.1ff, %.1ff, %.1ff, %.1ff}, {%.4ff, %.4ff, %.4ff, %.4ff}},"
        % ((gid, name, code, advance, area) + tuple(control) + tuple(tight))
    )
lines += ["    };", "} // namespace volt::tests::font", ""]

with open(os.path.join(HERE, "cff_fixture.hpp"), "w", newline="\n") as out:
    out.write("\n".join(lines))
print("cff_fixture.hpp: %d bytes of font, %d glyphs" % (len(data), len(rows)))
