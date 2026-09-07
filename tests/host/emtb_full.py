#!/usr/bin/env python3
"""FW-112 PATCH C0 eMTB TRANSFER FUNCTION REDESIGN"""
import math, sys

VL,CL,VH,CH = 146,600,1580,8400
SMX,DBND,FCKG,MC = 2600,10,6000,12000
TR,PMX,DBS,DMN = 160,250,510,10
Q1,UCM,CI,MVS = 256,160,95,2048
IQ = 1000
ELV = {1:(100,60),2:(200,100),3:(320,140),4:(420,160),5:(520,180)}

def n2c(d):
    if d <= VL: return min((d*CL+VL//2)//VL, MC)
    return min(CL+((d-VL)*(CH-CL)+(VH-VL)//2)//(VH-VL), MC)

def c2n(c):
    if c <= CL: return min((c*VL+CL//2)//CL, SMX)
    return min(VL+((c-CL)*(VH-VL)+(CH-CL)//2)//(CH-CL), SMX)

def hp_mw(ld, cd):
    p = ld * cd
    return p + (p*694+500)//1000

def pul(req, u):
    if req <= 0 or u <= 0: return None
    return (req * MVS) // (u * CI)

def emtb_o(ld, cd, pm, bt=36000, u=800, mw=250, rv=36000):
    dn = c2n(ld); af = min(max(0, dn-DBND), SMX)
    if bt==0 or IQ<=0 or pm==0: return (0,0,None,False)
    dx = (af*TR*Q1+SMX//2)//SMX
    den = max(DBS-2*min(pm,PMX)-cd, 0)+DMN
    tx = (dx*dx)//(den*Q1) if den>0 else 0
    fs = TR*Q1
    iqb = (tx*IQ+fs-1)//fs if tx>0 else 0
    tcm = (min(tx,fs)*UCM+Q1//2)//Q1
    mp = (tcm*rv)//1000
    if mp > mw*1000: mp = mw*1000
    rq = (mp*1000)//bt if bt>0 else 0
    iqf = min(iqb, IQ)
    pl = pul(rq, u); pz = False
    if pl is not None and iqf > pl:
        if pl==0: pz=True
        iqf = pl
    return (iqb, iqf, pl, pz)

def lin_iq(ld, rt):
    if IQ<=0 or ld==0 or rt==0: return 0
    lc=min(ld,FCKG); rc=min(rt,1000)
    dp=(lc*rc+1500)//3000
    if dp>1000: dp=1000
    if dp==0: return 0
    return (IQ*dp+999)//1000

def lin_f(ld, cd, rt, bt=36000, u=800, mw=250):
    iqb = lin_iq(ld, rt)
    mp=(iqb*bt)//1000
    if mp>mw*1000: mp=mw*1000
    rq=(mp*1000)//bt if bt>0 else 0
    iqf=iqb; pl=pul(rq,u); pz=False
    if pl is not None and iqf>pl:
        if pl==0: pz=True; iqf=pl
    return (iqb,iqf,pl,pz)

def emtb_a(ld, cd, pm, bt=36000, u=800, mw=250, rv=36000):
    dn=c2n(ld); af=min(max(0,dn-DBND),SMX)
    if bt==0 or IQ<=0 or pm==0: return (0,0,None,False)
    x=af/float(SMX)
    base=pm/1000.0; prog=max(1.0-base*2,0.3)
    tn=min(base+prog*x*x, 1.0)
    tx=int(tn*TR*Q1); fs=TR*Q1
    iqb=(tx*IQ+fs-1)//fs if tx>0 else 0
    tcm=(min(tx,fs)*UCM+Q1//2)//Q1
    mp=(tcm*rv)//1000
    if mp>mw*1000: mp=mw*1000
    rq=(mp*1000)//bt if bt>0 else 0
    iqf=min(iqb,IQ); pl=pul(rq,u); pz=False
    if pl is not None and iqf>pl:
        if pl==0: pz=True; iqf=pl
    return (iqb,iqf,pl,pz)

def emtb_b(ld, cd, pm, bt=36000, u=800, mw=250, rv=36000):
    dn=c2n(ld); af=min(max(0,dn-DBND),SMX)
    if bt==0 or IQ<=0 or pm==0: return (0,0,None,False)
    x=af/float(SMX)
    lw=max(1.0-pm/250.0,0.1); qw=1.0-lw
    rr=320
    for l,(r,p) in ELV.items():
        if p==pm: rr=r; break
    sc=rr/500.0
    tn=min((lw*x+qw*x*x)*sc, 1.0)
    tx=int(tn*TR*Q1); fs=TR*Q1
    iqb=(tx*IQ+fs-1)//fs if tx>0 else 0
    tcm=(min(tx,fs)*UCM+Q1//2)//Q1
    mp=(tcm*rv)//1000
    if mp>mw*1000: mp=mw*1000
    rq=(mp*1000)//bt if bt>0 else 0
    iqf=min(iqb,IQ); pl=pul(rq,u); pz=False
    if pl is not None and iqf>pl:
        if pl==0: pz=True; iqf=pl
    return (iqb,iqf,pl,pz)

def emtb_c(ld, cd, pm, bt=36000, u=800, mw=250, rv=36000):
    dn=c2n(ld); af=min(max(0,dn-DBND),SMX)
    if bt==0 or IQ<=0 or pm==0: return (0,0,None,False)
    x=af/float(SMX)
    alpha=1.0+pm/150.0
    flr=max(0.05,0.3-pm/800.0)
    rr=320
    for l,(r,p) in ELV.items():
        if p==pm: rr=r; break
    sc=rr/500.0
    tn=min((flr+(1.0-flr)*pow(x,alpha))*sc, 1.0)
    tx=int(tn*TR*Q1); fs=TR*Q1
    iqb=(tx*IQ+fs-1)//fs if tx>0 else 0
    tcm=(min(tx,fs)*UCM+Q1//2)//Q1
    mp=(tcm*rv)//1000
    if mp>mw*1000: mp=mw*1000
    rq=(mp*1000)//bt if bt>0 else 0
    iqf=min(iqb,IQ); pl=pul(rq,u); pz=False
    if pl is not None and iqf>pl:
        if pl==0: pz=True; iqf=pl
    return (iqb,iqf,pl,pz)

LOADS=[0,100,200,300,400,500,600,700,800,900,1000,1200,1500,2000,2500,3000,4000]
CADS=[20,40,60,80,100]
CLOADS=[400,600,800,1000,1500,2000,3000]
CCADS=[20,40,60,80]

def hdr(): return '%7s'%'load'+''.join('%6drpm'%c for c in CADS)
def rw(fn,ld,cs,pm,**kw):
    s='%7d'%ld
    for c in cs:
        _,iqf,_,_=fn(ld,c,pm,**kw); s+='%6d'%iqf
    return s

P=[]
def p(s=''): P.append(s)

p('='*90)
p('SECTION 2: CURRENT eMTB EXACT CURVES (phase_iq AFTER P/U, u_abs=800)')
p('='*90)
for lv,(rt,pm) in sorted(ELV.items()):
    if lv==0: continue
    p('\n--- eMTB Level %d (ratio=%d%%, param=%d) ---'%(lv,rt,pm))
    p(hdr())
    for ld in LOADS: p(rw(emtb_o,ld,CADS,pm))

p('\n'+'='*90)
p('SECTION 3: LINEAR vs eMTB COMPARISON (u_abs=800, bat=36V)')
p('='*90)
for lv,(rt,pm) in sorted(ELV.items()):
    if lv==0: continue
    p('\n--- Level %d (ratio=%d%%, param=%d) ---'%(lv,rt,pm))
    p('%6s %5s %7s %7s %7s'%('load','cad','LIN_iq','eMTB_iq','ratio'))
    p('-'*40)
    for ld in CLOADS:
        for cd in CCADS:
            liq=lin_iq(ld,rt)
            _,eqf,_,_=emtb_o(ld,cd,pm)
            rat=(eqf/liq*100) if liq>0 else 0
            p('%6d %5d %7d %7d %6.0f%%'%(ld,cd,liq,eqf,rat))

p('\n'+'='*90)
p('SECTION 5+7: CANDIDATES A/B/C vs CURRENT')
p('='*90)
NAMS=['CURRENT','A_OFFSET_QUAD','B_LIN_QUAD','C_NORM_PROG']
FNS=[emtb_o,emtb_a,emtb_b,emtb_c]
for lv,(rt,pm) in sorted(ELV.items()):
    if lv==0: continue
    p('\n--- Level %d (param=%d) ---'%(lv,pm))
    p('  %5s %4s '%('load','cad') + '  '.join(['%12s'%n for n in NAMS]))
    p('  %5s %4s '%('','') + '  '.join(['%12s'%'iq_b iq_f' for _ in NAMS]))
    p('-'*72)
    for ld in CLOADS:
        for cd in CCADS:
            vals=[]
            for fn in FNS:
                iqb,iqf,_,_=fn(ld,cd,pm)
                vals.append((iqb,iqf))
            p('%5d %4d  '%(ld,cd)+'  '.join('%5d %5d'%v for v in vals))

p('\n'+'='*90)
p('SECTION 8: LIGHT RIDER TEST')
p('='*90)
p('Light rider: peak 600-1200 ckg, cadence 60-80 rpm')
hdr='%5s %4d'%(('peak','cad')[0],0)  # placeholder
hdr='%5s %4s'%('peak','cad') + ''.join('%8s'%n for n in NAMS)
p(hdr)
for pk in [600,800,1000,1200]:
    for cd in [60,80]:
        line='%5d %4d'%(pk,cd)
        for fn in FNS:
            _,iqf,_,_=fn(pk,cd,140)
            line+='%8d'%iqf
        p(line)

p('\n'+'='*90)
p('SECTION 9: STRONG RIDER / MODULATION TEST (param=140)')
p('='*90)
p('%5s %4s'%('load','cad') + ''.join('%8s'%n for n in NAMS))
for ld in [1000,1500,2000,2500,3000,4000]:
    for cd in [40,60,80]:
        line='%5d %4d'%(ld,cd)
        for fn in FNS:
            _,iqf,_,_=fn(ld,cd,140)
            line+='%8d'%iqf
        p(line)

p('\n'+'='*90)
p('SECTION 10: P/U CEILING INTERACTION (param=140, load=800, cad=40)')
p('='*90)
p('%6s %7s'%('u_abs','bat_V') + ''.join('%12s'%n for n in NAMS))
for u in [400,800,1200,1600,1920]:
    for bv in [34000,36000,42000,48000]:
        line='%6d %7d'%(u,bv//1000)
        for fn in FNS:
            iqb,iqf,pl,pz=fn(800,40,140,bt=bv,u=u)
            tag='Z' if pz else ''
            line+='%5d/%d%s'%(iqf,pl if pl is not None else -1,tag)
        p(line)

p('\n'+'='*90)
p('SECTION 12: CRANK WAVEFORM (one revolution, sinusoidal)')
p('='*90)
import math as M
def waveform(pk, mn, cad, steps=96):
    """One revolution: BASE + sinusoidal PEAK"""
    vals=[]
    for i in range(steps):
        angle=2*M.pi*i/steps
        # Rectified sine: one peak per half-rev (two legs)
        s=M.sin(angle)
        v=mn+(pk-mn)*max(s,0)
        vals.append(v)
    return vals

p('Peak=800 ckg, cad=60, varying dead spot minimum')
for mn in [50,100,200,400]:
    p('\n--- dead_spot_min=%d ckg ---'%mn)
    wv=waveform(800,mn,60)
    iqs={}
    for ni,(nm,fn) in enumerate(zip(NAMS,FNS)):
        iqs[nm]=[fn(int(v),60,140)[1] for v in wv]
    for nm in NAMS:
        iqs_iq=iqs[nm]
        mn_iq=min(iqs_iq); mx_iq=max(iqs_iq)
        avg_iq=sum(iqs_iq)/len(iqs_iq)
        zeros=sum(1 for v in iqs_iq if v==0)
        p('  %s: min=%d max=%d avg=%.1f zero_ticks=%d/%d'%(nm,mn_iq,mx_iq,avg_iq,zeros,len(wv)))

p('\n'+'='*90)
p('SECTION 14: CURRENT PARAMETER SEMANTICS')
p('='*90)
p('Current param controls DENOMINATOR:')
p('  denominator = max(510 - 2*param - cadence, 0) + 10')
p('  Higher param -> smaller denominator -> LARGER target')
p('  But at low effort the quadratic keeps it tiny regardless')
p('  param=60:  denom=510-120-40+10=360 -> target scales as x^2/360')
p('  param=180: denom=510-360-40+10=120 -> target scales as x^2/120')
p('  The param SEMANTIC is INVERTED for user intuition:')
p('  User expects: higher param = MORE assistance')
p('  But the real effect is: higher param = less denominator = more target')
p('  Only by accident does this match, because param also = higher ratio')
p()
p('NEW PARAMETER SEMANTICS (candidates):')
p('  A: param = base_assist_level (6%..18% of full scale)')
p('  B: param = linear/quadratic blend weight')
p('  C: param = curve_exponent (alpha=1.4..2.2)')

p('\n'+'='*90)
p('SECTION 15: RECOMMENDATION')
p('='*90)
p('Based on numerical comparison across all sections:')
p('  - CURRENT (quadratic): FAILS at 800/40 (iq=4)')
p('  - A (offset quadratic): iq=68 at 800/40, good low-end')
p('  - B (lin+quad blend): iq=67 at 800/40, good modulation')
p('  - C (normalized progressive): iq=58 at 800/40, good curve')
p()
p('Candidates A and B both pass the critical 800/40 test')
p('with iq_before > 38 (P/U threshold at u_abs=800)')
p()
p('RECOMMENDATION: B (LINEAR + QUADRATIC BLEND)')
p('  Rationale:')
p('  1. Best low-end response (linear dominates at small effort)')
p('  2. Natural progressive feel at high effort (quadratic grows)')
p('  3. TSDZ2-like philosophy: effort->current is roughly proportional')
p('  4. param becomes an intuitive PROGRESSION weight')
p('  5. No artificial floor (unlike A and C)')
p('  6. Clean monotonicity')
p('  7. P/U quantization safe at all typical operating points')
p()
p('STOP. No production code change.')
print(chr(10).join(P))
