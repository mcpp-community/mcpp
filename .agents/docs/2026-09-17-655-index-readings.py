#!/usr/bin/env python3
# #655 K6: every flag-like string literal of an index checkout, read under the
# new flag syntax and under the POSIX and MSVCRT readings the previous build
# applied. Run from the index checkout; prints the literals whose readings differ.
import re, shlex, subprocess, sys
def flag_words(s):
    # SPEC-004 section 8 rule 8: a -D or /D element with a space is one word.
    if (s.startswith('-D') or s.startswith('/D')) and ' ' in s:
        return [s]
    out=[];w=[];st=False;i=0
    while i<len(s):
        c=s[i]
        if c in ' \t':
            if st: out.append(''.join(w)); w=[]; st=False
            i+=1; continue
        st=True
        if c=="'":
            i+=1
            while i<len(s) and s[i]!="'": w.append(s[i]); i+=1
            i+=1; continue
        if c=='"':
            i+=1
            while i<len(s) and s[i]!='"':
                if s[i]=='\\' and i+1<len(s) and s[i+1] in '"\\': i+=1
                w.append(s[i]); i+=1
            i+=1; continue
        if c=='\\' and i+1<len(s) and s[i+1] in ' \t"\'\\':
            w.append(s[i+1]); i+=2; continue
        w.append(c); i+=1
    if st: out.append(''.join(w))
    return out
def crt(s):
    out=[];cur=[];st=False;q=False;i=0
    while i<len(s):
        c=s[i]
        if c=='\\':
            j=i
            while j<len(s) and s[j]=='\\': j+=1
            n=j-i; st=True
            if j<len(s) and s[j]=='"':
                cur.append('\\'*(n//2))
                if n%2: cur.append('"'); i=j+1
                else: i=j
                continue
            cur.append('\\'*n); i=j; continue
        if c=='"': q=not q; st=True; i+=1; continue
        if not q and c in ' \t':
            if st: out.append(''.join(cur)); cur=[]; st=False
            i+=1; continue
        cur.append(c); st=True; i+=1
    if st: out.append(''.join(cur))
    return out
def old_join(e):
    if (e.startswith('-D') or e.startswith('/D')) and ' ' in e:
        return "'"+e.replace("'","'\\''")+"'", '"'+e.replace('"','\\"')+'"'
    return e, e
text=subprocess.run(['git','grep','-h','-oE',r'"(-|/)[A-Za-z]([^"\\]|\\.)*"','origin/main','--','pkgs'],capture_output=True,text=True).stdout
lits=set()
for m in text.splitlines():
    body=m[1:-1]
    # undo Lua escapes
    body=re.sub(r'\\(.)', lambda x: {'n':'\n','t':'\t'}.get(x.group(1), x.group(1)), body)
    lits.add(body)
bad=0
for e in sorted(lits):
    new=flag_words(e); ps, ws = old_join(e)
    try: posix=shlex.split(ps.replace('$','\x00'))  # no expansion modelled
    except ValueError: posix=['<unterminated>']
    posix=[p.replace('\x00','$') for p in posix]
    win=crt(ws)
    if new!=posix or new!=win:
        bad+=1; print('DIFF', repr(e), 'new', new, 'posix', posix, 'win', win)
print(len(lits),'literals;',bad,'differ')
