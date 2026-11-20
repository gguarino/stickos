#!/usr/bin/env python3
"""Checks ui/lang.h against ui/stickui.c:
- every row has all languages, Latin-1 only, same printf specifiers as English;
- every T("...") literal in stickui.c (and the names/prompts/pause items translated
  through T(variable)) has a row; rows nobody uses are reported as warnings;
- lines fit on screen (76 characters, 21 for wizard button names)."""
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
lang = open(os.path.join(here, "lang.h"), encoding="utf-8").read()
src = open(os.path.join(here, "stickui.c"), encoding="utf-8").read()

def strings(s):
    return [bytes(m, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
            for m in re.findall(r'"((?:[^"\\]|\\.)*)"', s)]

rows = [strings(m) for m in re.findall(r"\{ \{(.*?)\} \}", lang, re.S)]
errors = 0
def err(msg):
    global errors
    errors += 1
    print("lang.h:", msg)

spec = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?[a-z]+")
names = set(strings(src[src.index("btn_name[NIN]"):src.index("btn_key[NIN]")]))
keys = {}
for r in rows:
    if len(r) != 5:
        err("%d columns instead of 5: %r" % (len(r), r[0]))
        continue
    if r[0] in keys:
        err("duplicate row %r" % r[0])
    keys[r[0]] = r
    for t in r:
        try:
            t.encode("latin-1")
        except UnicodeEncodeError:
            err("not Latin-1: %r" % t)
        if spec.findall(t) != spec.findall(r[0]):
            err("format specifiers differ: %r / %r" % (r[0], t))
        limit = 21 if r[0] in names else 76
        if len(t) > limit:
            err("too long (%d > %d): %r" % (len(t), limit, t))

used = set(strings(" ".join(re.findall(r'T\(\s*("(?:[^"\\]|\\.)*")\s*\)', src))))
for arr in ("btn_name[NIN]", "btn_prompt[NIN]", "items[] = {"):
    i = src.index(arr)
    used |= set(strings(src[i:src.index(";", i)]))
single = {"B", "Y", "A", "X", "L", "R", "L2", "R2", "L3", "R3", "SELECT", "START"}
for u in sorted(used - set(keys) - single):
    err("no translation for %r" % u)
for k in sorted(set(keys) - used):
    print("lang.h: warning, unused row %r" % k)
print("lang.h: %d rows, %s" % (len(rows), "OK" if not errors else "%d ERRORS" % errors))
sys.exit(1 if errors else 0)
