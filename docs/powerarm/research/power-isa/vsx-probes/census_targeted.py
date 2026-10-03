import sys,re,collections
c=collections.Counter()
for l in sys.stdin:
    p=l.rstrip('\n').split('\t')
    if len(p)<2: continue
    mn=p[-2].strip() if len(p)>=3 else p[-1].strip(); ops=p[-1] if len(p)>=3 else ''
    o=re.sub(r'<[^>]*>','',ops)
    low=bool(re.search(r'\b[vdsqhb](1[6-9]|2\d|3[01])\b', o))
    vec=o.strip().startswith('v')
    key=None
    if mn in ('frintn','fcvtns','fcvtnu') and vec: key='RNE vector '+mn
    elif mn in ('fcvtzs','fcvtzu') and vec: key='trunc vector fcvtz*'
    elif mn in ('fcvtas','fcvtau','fcvtms','fcvtmu','fcvtps','fcvtpu') and vec: key='other-rounding vector fcvt*'
    elif mn in ('frinta','frintm','frintp','frintz','frintx','frinti') and vec: key='non-RNE vector frint*'
    elif mn=='movi' and re.search(r'#0x0\b|#0\b',o): key='movi #0'
    elif mn=='movi': key='movi other'
    elif mn in ('ins','mov') and re.search(r'\.[sd]\[',o): key='ins/mov .s/.d element'
    elif mn in ('ldp','stp') and re.match(r'\s*[qd]\d',o): key=mn+' q/d'
    elif mn in ('fcvtn','fcvtl','fcvtn2','fcvtl2') and re.search(r'\.(2s|2d|4s)',o): key='fcvtn/l 32<->64'
    elif mn in ('fcmeq','fcmgt','fcmge','fcmlt','fcmle') and re.search(r'\.(4s|2s)',o): key='fcm* .4s/.2s'
    elif mn in ('zip1','zip2','trn1','trn2','uzp1','uzp2') and re.search(r'\.(4s|2s|2d)',o): key=mn+' .s/.d'
    elif mn=='ext': key='ext'
    elif mn=='dup' and (re.search(r'\.[sd]\[',o) or re.search(r'\.(4s|2s|2d), [wx]',o)): key='dup .s/.d'
    elif mn in ('umov','smov') or (mn=='fmov' and re.search(r'^\s*[wx]\d+, v',o)): key='umov/fmov from element'
    if key: c[key]+=1; c[key+' [low]']+=low
print(sys.argv[1], dict(sorted(c.items())))
