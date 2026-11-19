"""Checks that every text passed to T("...") (and every column name/tip, filter name and stored error/remark text)
has an English and a French entry in Source/Lang.cpp.
Usage: python Tools/check_lang.py [--list]"""
import re, glob, os, sys
sys.stdout.reconfigure(encoding='utf-8')

src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Source')
STR = r'"((?:[^"\\]|\\.)*)"'

def unescape(s):
    return s.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')

def literals(chunk):
    """adjacent string literals "a" "b" -> 'ab' (as the compiler concatenates them)"""
    return unescape(''.join(re.findall(STR, chunk)))

def used_keys():
    keys = []
    for f in sorted(glob.glob(os.path.join(src, '*.cpp')) + glob.glob(os.path.join(src, '*.h'))):
        if os.path.basename(f) in ('Lang.cpp', 'Texts.cpp', 'Texts.h'):
            continue
        s = open(f, encoding='utf-8').read()
        for m in re.finditer(r'\bT\s*\(\s*((?:' + STR + r'\s*)+)', s):
            keys.append(literals(m.group(1)))
    s = open(os.path.join(src, 'SidTable.cpp'), encoding='utf-8').read()
    for name, tip in re.findall(r'\{ c\w+,\s*' + STR + r',\s*\d+,\s*\w+,\s*\w+,\s*' + STR + r'\s*\}', s):
        keys += [unescape(name), unescape(tip)]
    s = open(os.path.join(src, 'SidTab.cpp'), encoding='utf-8').read()
    m = re.search(r'const char\* names\[\] = \{([^}]*)\}', s)
    if m:
        keys += [unescape(x) for x in re.findall(STR, m.group(1))]
    # stored error / remark texts shown through TS()
    for f in ('Archive.cpp', 'SidTrace.cpp', 'SidInfo.cpp'):
        s = open(os.path.join(src, f), encoding='utf-8').read()
        for m in re.finditer(r'\b(?:error|note)\s*=\s*' + STR, s):
            keys.append(unescape(m.group(1)))
    out = []
    for k in keys:
        if k and k not in out:
            out.append(k)
    return out

def table_keys():
    s = open(os.path.join(src, 'Lang.cpp'), encoding='utf-8').read()
    body = s.split('// ---- table begin')[1].split('// ---- table end')[0]
    return set(unescape(k) for k in re.findall(r'\{\s*' + STR + r'\s*,', body))

keys = used_keys()
if '--list' in sys.argv:
    print('\n'.join(keys)); sys.exit(0)
have = table_keys()
same = {'C64-SID', 'SID', 'STIL', 'MIDI', 'PAL', 'NTSC', 'HVSC', 'Google', 'YouTube', 'CSDb'}
missing = [k for k in keys if k not in have and k not in same and not k.startswith('@') and not re.fullmatch(r'[\W\d_]*', k)]
unused = sorted(have - set(keys))
for k in missing: print('FEHLT:', k)
for k in unused: print('unbenutzt:', k)
print(f'{len(keys)} Schlüssel, {len(missing)} fehlen, {len(unused)} unbenutzt')
sys.exit(1 if missing else 0)
