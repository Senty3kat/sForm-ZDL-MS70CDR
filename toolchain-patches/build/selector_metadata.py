"""Audited release selectors; raw ranges preserve the existing patch ABI.

Mode thresholds use float32 UI*0.01, matching shared edit handlers, then the
comparisons in the DSP. Do not infer discrete controls from small max alone.
"""
import struct

def f32(v): return struct.unpack('<f',struct.pack('<f',v))[0]
def normalized(raw): return f32(raw*f32(.01))
def ladder(raw,limits): return sum(normalized(raw)>=f32(x) for x in limits)
def groups(labels, classify, maximum=100):
    result=[]
    for raw in range(maximum+1):
        label=labels[classify(raw)]
        if not result or result[-1]['label']!=label:
            result.append({'label':label,'from':raw,'to':raw,'value':raw})
        else: result[-1]['to']=raw
    for g in result: g['value']=(g['from']+g['to'])//2
    return result

def _sform_rate(r):
    # sForm Rate knob 0..99 -> 0.1 .. 10.0 Hz in 0.1 Hz steps (sform.c: 0.1*(r+1)).
    return f'{(r+1)/10:.1f}'

_NOTE_NAMES=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
def _sform_note(r):
    # sForm Note knob (max 101, the largest proven on the pedal): 0 Off,
    # 1..94 = C0..A7 (MIDI 12..105), 95..101 = in-48 in-36 in-24 in-12 in in+12 in+24.
    if r==0: return 'Off'
    if r>94: return ['in-48','in-36','in-24','in-12','in','in+12','in+24'][r-95]
    return f'{_NOTE_NAMES[(r-1)%12]}{(r-1)//12}'

SFORM_LABELS={'Rate':[_sform_rate(r) for r in range(100)],'Note':[_sform_note(r) for r in range(102)]}

SELECTORS={
 'Tonic':{
   'Mode':groups(['Decim','Crush','FM','Ring'],lambda r:ladder(r,[.25,.5,.75])),
   'Order':groups(['Fwd','Rev'],lambda r:ladder(r,[.5])),
 },
 'Rooms6':{'Mode':groups(['Room','Digit','Peak','Gate','Wave','Gong'],lambda r:r,5)},
 'Rooms':{'Mode':groups(['Room','Digit','Peak','Gate','Wave','Gong'],lambda r:min(5,int(f32(normalized(r)*f32(5.999)))))},
 'Spool':{'Div':groups(['1/16','1/8T','1/8','1/8.','1/4','1/4.'],lambda r:ladder(r,[.16666667,.33333334,.5,.6666667,.8333333]))},
 'Hydra':{'Div':groups(['Grain','1/32','1/16T','1/16','1/8T','1/8','1/8.','1/4'],lambda r:ladder(r,[.0625,.1875,.3125,.4375,.5625,.6875,.8125]))},
 'Spiral':{'Div':groups(['1/32','1/16T','1/16','1/8T','1/8','1/8.','1/4','1/4.'],lambda r:ladder(r,[.125,.25,.375,.5,.625,.75,.875]))},
 'Rewire':{'Route':groups(['BDRCS','SCRDB','DSBCR','RCSBD','SBRDC'],lambda r:ladder(r,[.2,.4,.6,.8]))},
 'Stasis':{'Capture':groups(['Stomp','Release','Hold'],lambda r:r,2)},
 'XDuck':{
   'Type':groups(['Duck','Gate','Tex'],lambda r:r,2),
   'Route':groups(['X','R>L','R<L','KeyR','KeyL','sForm','sFPan'],lambda r:r,6),
   'Freq':groups(['Full','40','60','100','150','230','360','570','880','1.4k','2.1k','3.3k','5.1k','8k'],lambda r:r,13),
 },
 'sForm':{
   # Rate and Note labels are computed in sform.c (ZDL_GetLabel_1/_8): as
   # if-chains they cost ~9 KB; SFORM_LABELS below is for previews/tests.
   'Mult':groups(['x.01','x.02','x.05','x0.1','x0.2','x0.5','x1','x2','x5','x10','x20','x50','x100'],lambda r:r,12),
   'Wave':groups(['Sine','Tri','SawUp','SawDn','Squar','S&H','Drift'],lambda r:r,6),
   'Out':groups(['XDuck','Drone','DrnSt'],lambda r:r,2),
 },
 'XProbe':{
   'Src':groups(['FxL','FxR','RawL','RawR','Mute','Pass'],lambda r:r,5),
   'Dest':groups(['Both','L','R'],lambda r:r,2),
 },
 'RndmFLTR':{
   'Type':groups(['HPF','BPF','LPF'],lambda r:r,2),
   'Chara':groups(['2Pole','4Pole'],lambda r:r,1),
 },
}
def attach_selectors(effect):
    for p in effect['params']:
        choices=SELECTORS.get(effect['name'],{}).get(p['name'])
        if choices and choices[-1]['to']==p['max']:
            p['choices']=choices
    return effect

def pedal_label(label):
    return {'Release':'Rels', 'Stomp':'Stmp'}.get(label,label)

def pedal_label_c(manifest):
    """Stock GetString ABI: A4=integer value, B4=destination; return length.

    Values use at most five ASCII chars to fit a pedal column; the copy is
    bounded to the stock eight-byte string buffer. Enabled for release builds
    after the Stasis hardware confirmation.
    """
    import json
    lines=[]
    for i,p in enumerate(manifest['params']):
        choices=SELECTORS.get(manifest['effect_name'],{}).get(p['name'])
        if not choices: continue
        if choices[-1]['to']!=p['max']: raise ValueError('selector range mismatch')
        lines += [f'int ZDL_GetLabel_{i}(unsigned int value, char *out) {{',
                  '    const char *s; int i;']
        for j,c in enumerate(choices):
            label=pedal_label(c['label'])
            assert len(label.encode('ascii'))<=5
            if j==len(choices)-1: prefix='else'
            else: prefix=('if' if j==0 else 'else if')+f' (value <= {c["to"]}u)'
            lines.append(f'    {prefix} s = {json.dumps(label)};')
        lines += ['    for (i=0; i<7; ++i) { out[i]=s[i]; if (!s[i]) return i; }',
                  '    out[7]=0; return 7;', '}']
    return '\n'.join(lines)
