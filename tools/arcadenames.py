#!/usr/bin/env python3
"""Extracts the full game titles of the arcade cores from their source code.

Usage: arcadenames.py CORE SRCDIR OUT
  CORE = fbalpha2012 | fbalpha | mame2003
Writes OUT with one line per romset, sorted by zip name:
  <zip name> TAB <flags> TAB <title>
flags: "b" = BIOS / not a game (hidden from the menu), "-" = game.
"""
import os, re, sys

def c_string(tok):
    """Value of a C string literal token (possibly several concatenated)."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', tok)
    if not parts:
        return None
    s = "".join(parts)
    s = s.replace("\\0", "").replace('\\"', '"').replace("\\\\", "\\")
    return s

def split_args(text, start):
    """Splits the macro/initializer arguments starting after '(' or '{' at start."""
    args, depth, cur, i, n = [], 0, [], start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if text.startswith("/*", i):
            i = text.find("*/", i) + 2
            continue
        if text.startswith("//", i):
            i = text.find("\n", i)
            continue
        if c in "({[":
            depth += 1
        elif c in ")}]":
            if depth == 0:
                args.append("".join(cur).strip())
                return args
            depth -= 1
        elif c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return None

def sources(root, exts):
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith(exts):
                p = os.path.join(d, f)
                yield p, open(p, encoding="latin-1").read()

# FB Alpha drivers for home systems (not arcade): skipped
FBA_SKIP = re.compile(r"^(md|pce|tg|sgx|sms|gg|sg1k|cv|msx|spec|nes|fds|snes|ngp|ngpc|chf|c64|channelf)_")

def fba(root):
    out = {}
    for path, text in sources(root, (".cpp", ".c")):
        for m in re.finditer(r"struct\s+BurnDriver\w*\s+\w+\s*=\s*\{", text):
            a = split_args(text, m.end())
            if not a or len(a) < 10:
                continue
            name, title = c_string(a[0]), c_string(a[5])
            if not name or not title or FBA_SKIP.match(name):
                continue
            flags = a[13]                               # after the 4 wide-string fields
            bios = "BDF_BOARDROM" in flags
            if name in out and out[name][0] == "b":
                continue                                # e.g. "neogeo": keep the BIOS entry
            out[name] = ("b" if bios else "-", title)
    return out

def mame(root, version):
    out = {}
    if version == 2003:
        macro = re.compile(r"\b(GAME|GAMEX|GAMEB|GAMEBX)\s*\(")
    else:
        macro = re.compile(r"\b(GAME|GAMEL|CONS)\s*\(")
    for path, text in sources(root, (".c", ".cpp", ".hxx", ".h")):
        for m in macro.finditer(text):
            line = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
            if "#define" in line:
                continue                                # a macro definition, not a game
            a = split_args(text, m.end())
            if not a:
                continue
            kind = m.group(1)
            if version == 2003:
                # GAME(YEAR,NAME,PARENT,MACHINE,INPUT,INIT,MONITOR,COMPANY,FULLNAME[,FLAGS])
                # GAMEB(YEAR,NAME,PARENT,BIOS,MACHINE,INPUT,INIT,MONITOR,COMPANY,FULLNAME[,FLAGS])
                ti = 9 if kind.startswith("GAMEB") else 8
                fl = a[ti + 1] if len(a) > ti + 1 else ""
            else:
                # GAME(YEAR,NAME,PARENT,MACHINE,INPUT,CLASS,INIT,MONITOR,COMPANY,FULLNAME,FLAGS)
                ti = 9
                fl = a[10] if len(a) > 10 else ""
            if len(a) <= ti:
                continue
            name, title = a[1].strip(), c_string(a[ti])
            if not title or not re.match(r"^\w+$", name):
                continue
            bios = "NOT_A_DRIVER" in fl or "IS_BIOS_ROOT" in fl
            if kind == "CONS" and not bios:
                continue                                # home consoles: only BIOS roots (neogeo)
            out[name] = ("b" if bios else "-", title)
    return out

def main():
    core, root, dst = sys.argv[1:4]
    if core.startswith("fbalpha"):
        names = fba(root)
    else:
        names = mame(root, 2003 if core == "mame2003" else 2016)
    with open(dst, "w", encoding="utf-8") as f:
        for k in sorted(names):
            fl, t = names[k]
            t = " ".join(t.split()).replace("\t", " ")
            f.write("%s\t%s\t%s\n" % (k, fl, t))
    print("%s: %d titles (%d BIOS)" % (core, len(names), sum(1 for v in names.values() if v[0] == "b")))

main()
