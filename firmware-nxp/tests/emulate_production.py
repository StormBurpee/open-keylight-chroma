"""Execute the fixed low-output candidate's compiled startup and SysTick cleanup offline.
MMIO, ROM IAP54 and IRQ delivery are modeled; this is not electrical evidence.
"""
from pathlib import Path
import hashlib,json,struct,sys,os
HERE=Path(__file__).resolve().parent
if os.environ.get('NXP_UNICORN_PATH'): sys.path.insert(0,os.environ['NXP_UNICORN_PATH'])
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_SP

ROOT=HERE.parent
BUILD=ROOT/'build/lighting'
image=(BUILD/'open-keylight-nxp-lighting.bin').read_bytes()
bank=(BUILD/'lighting-bank.bin').read_bytes()
elf=(BUILD/'open-keylight-nxp-lighting.elf').read_bytes()
manifest=json.loads((BUILD/'manifest.json').read_text())
sha=lambda b:hashlib.sha256(b).hexdigest()
assert sha(bank)==manifest['bank_sha256'] and sha(image)==manifest['sha256']
assert bank==image+b'\xff'*(28672-len(image))
for name,digest in manifest['source_sha256'].items():
    assert sha((ROOT/name.replace('\\','/')).read_bytes())==digest,name
eh=struct.unpack_from('<16sHHIIIIIHHHHHH',elf)
assert eh[0][:6]==b'\x7fELF\x01\x01' and eh[1:3]==(2,40)
sections=[struct.unpack_from('<IIIIIIIIII',elf,eh[6]+i*eh[11]) for i in range(eh[12])]
names=sections[eh[13]];strings=elf[names[4]:names[4]+names[5]]
section_map={strings[s[0]:].split(b'\0',1)[0].decode():s for s in sections}
symtab=section_map['.symtab'];strtab=sections[symtab[6]];strings=elf[strtab[4]:strtab[4]+strtab[5]]
symbols={}
for off in range(symtab[4],symtab[4]+symtab[5],symtab[9]):
    name,value,size,info,other,index=struct.unpack_from('<IIIBBH',elf,off)
    name=strings[name:].split(b'\0',1)[0].decode()
    assert not name or index,'Undefined symbol '+name
    if name:symbols[name]=value
for forbidden in ['nxp_board_start_pwm','nxp_board_apply_pwm','nxp_board_force_off']:
    assert forbidden in symbols
for s in sections:
    assert not(s[1] in (4,9) and s[5])
    if s[2]&2 and s[5]:
        if s[2]&1:assert s[1]==8 and 0x100000c0<=s[3]<s[3]+s[5]<=0x10000c00
        else:assert 0x2000<=s[3]<s[3]+s[5]<=0x9000
vectors=struct.unpack_from('<48I',image)
assert vectors[0]==0x10001000 and vectors[1]==symbols['Reset_Handler']
assert vectors[15]==symbols['SysTick_Handler'] and vectors[30]==symbols['SSP1_Handler']
assert all(v&1 and 0x20c0<=v-1<0x2000+len(image) for v in vectors[1:])

def emulate(case):
    u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
    for base,size in [(0,0x10000),(0x10000000,0x2000),(0x20004000,0x1000),(0x1fff1000,0x1000),
                      (0x40000000,0x100000),(0x50000000,0x4000),(0xe000e000,0x2000)]:u.mem_map(base,size)
    u.mem_write(0x2000,image)
    def w(a,v):u.mem_write(a,struct.pack('<I',v))
    def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
    for a,v in [(0x40048040,1),(0x40048008,0x23),(0x4004800c,1),(0x40048070,3),(0x40048078,1),
                (0x50002100,1<<17),(0x4005800c,2)]:w(a,v)
    if case=='clock_failure':w(0x40048070,0)
    if case=='watchdog_reset':w(0x40048030,4)
    if case=='selected_at_start':w(0x50002100,0)
    u.mem_write(0x20004780,b'\xa5'*124)
    out={'case':case,'reset':False,'rom_calls':0,'minimum_sp':0x10001000,'main_ready':False}
    mode='startup';render_seen=False;pending_drop=False;log=[];qualifications=[]
    def mem(_u,access,a,size,value,user):
        nonlocal pending_drop
        assert not 0<=a<0x10000,'Flash write'
        if 0x20004000<=a<0x20005000:assert a==0x200047fc and size==4
        if a>=0x40000000:log.append((a,value))
        if a==0x50002280:w(0x50002100,r(0x50002100)&~value)
        if a==0x50002200:w(0x50002100,r(0x50002100)|value)
        if case=='pwm_setup_failure' and a==0x40014074 and value==3:pending_drop=True
        if a==0xe000ed0c:
            assert value==0x05fa0004 and r(0x200047fc)==0xaaaaaaaa and r(0x40048080)&(1<<27)
            out['reset']=True;out['reset_ms']=r(symbols['milliseconds']);_u.emu_stop()
    def code(_u,a,size,user):
        nonlocal render_seen,pending_drop
        out['minimum_sp']=min(out['minimum_sp'],_u.reg_read(UC_ARM_REG_SP))
        if pending_drop:w(0x40014074,0);pending_drop=False
        if a==0x1fff1ff0:
            command,result=_u.reg_read(UC_ARM_REG_R0),_u.reg_read(UC_ARM_REG_R1)
            assert r(command)==54;w(result,1 if case=='rom_failure' else 0)
            w(result+4,0xbc41 if case=='part_mismatch' else 0xbc40)
            out['rom_calls']+=1;_u.reg_write(UC_ARM_REG_PC,_u.reg_read(UC_ARM_REG_LR))
        if a==0x1fff1000:_u.emu_stop()
        if mode=='startup' and a==(symbols['nxp_trial_expired']&~1):
            out['main_ready']=True;_u.emu_stop()
        if mode=='render' and a==(symbols['nxp_render']&~1):
            render_seen=True;qualifications.append(_u.reg_read(UC_ARM_REG_R1))
        if mode=='render' and render_seen and a==(symbols['nxp_trial_expired']&~1):_u.emu_stop()
    u.hook_add(UC_HOOK_MEM_WRITE,mem);u.hook_add(UC_HOOK_CODE,code)
    u.reg_write(UC_ARM_REG_SP,vectors[0]);u.emu_start(vectors[1],0,count=300000)
    assert out['rom_calls']==1
    early=case in ['rom_failure','part_mismatch','clock_failure','watchdog_reset','selected_at_start','pwm_setup_failure']
    if early:
        assert out['reset'] and out['reset_ms']==0 and not out['main_ready']
        if case!='pwm_setup_failure':assert not any(0x40014000<=a<0x40019000 for a,v in log)
    else:
        assert out['main_ready'] and not out['reset']
        main_context=u.context_save();mode='helper'
        def call(name,args=()):
            u.reg_write(UC_ARM_REG_SP,0x10000f00);u.reg_write(UC_ARM_REG_LR,0x1fff1001)
            for reg,value in zip([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3],args):u.reg_write(reg,value)
            for i,value in enumerate(args[4:]):w(0x10000f00+4*i,value)
            u.emu_start(symbols[name],0,count=150000)
            return u.reg_read(UC_ARM_REG_R0)
        def report(cls,op,args=b''):
            q=bytearray(97);q[0]=2;q[12]=len(args);q[13]=cls;q[14]=op;q[15:15+len(args)]=args
            for value in q[9:95]:q[95]^=value
            u.mem_write(0x10001000,bytes(q));u.mem_write(0x10001100,b'\xcc'*97)
            now=r(symbols['milliseconds'])
            assert call('nxp_link_transaction',[symbols['link'],0x10001000,97,0x10001100,97,now])==0
            u.mem_write(0x10001000,b'\0'*97)
            assert call('nxp_link_transaction',[symbols['link'],0x10001000,2,0x10001100,97,now])==0
            assert bytes(u.mem_read(0x10001100,2))==b'\0a'
            assert call('nxp_link_transaction',[symbols['link'],0x10001000,97,0x10001100,97,now])==0
            reply=bytes(u.mem_read(0x10001100,97));assert reply[7]==2
            return reply[15:15+reply[12]]
        def render_at(now):
            nonlocal mode,render_seen,main_context
            w(symbols['milliseconds'],now);u.context_restore(main_context);mode='render';render_seen=False
            u.emu_start(u.reg_read(UC_ARM_REG_PC)|1,0,count=300000)
            assert render_seen
            main_context=u.context_save();mode='helper'
        match_addr=[0x40018024,0x4001801c,0x40018018,0x40014018,0x4001401c]
        assert [r(a) for a in match_addr]==[25500,25500,25500,255,255]
        fc=report(0,0xfc);assert fc[:8]==b'OKLC\x01\x00\x02\x00' and int.from_bytes(fc[8:12],'big')==3
        claim=bytearray(72);claim[0]=1;claim[7]=4;claim[8:12]=b'Test';report(0,0x49,claim)
        report(15,2,bytes([0,0,1,0,0,1,255,0,0,0,0,0]));report(15,4,bytes([0,0,3]))
        render_at(10);assert qualifications[-1]==0
        assert [r(a) for a in match_addr]==[25500,25500,25500,255,255]
        if case=='unconfirmed_expiry':
            w(symbols['milliseconds'],29999);call('SysTick_Handler');assert out['reset'] and out['reset_ms']==30000
        else:
            assert report(0,0xfd,b'OKLC')==b'\x01'
            render_at(20);assert qualifications[-1]==1 and r(match_addr[0])==25200
            assert r(match_addr[1])==r(match_addr[2])==25500
            w(symbols['milliseconds'],29999);call('SysTick_Handler');assert not out['reset']
            assert report(0,0xfc)[7]&1
            if case=='confirmed_pwm_fault':
                w(0x40018020,123);render_at(30010);assert out['reset']
            else:
                assert report(0,4,b'\x01')==b'\x01'
                u.context_restore(main_context);mode='recovery'
                u.emu_start(u.reg_read(UC_ARM_REG_PC)|1,0,count=300000)
                assert out['reset']
            assert r(0x40014004)==r(0x40018004)==2
            assert not r(0x50002100)&sum(1<<p for p in(13,14,16,18,19))
        out['render_qualification_arguments']=qualifications
    assert bytes(u.mem_read(0x20004780,124))==b'\xa5'*124
    out['stack_bytes_without_exception_frame_or_ROM']=vectors[0]-out.pop('minimum_sp')
    return out

results=[emulate(c) for c in ['unconfirmed_expiry','confirmed_recovery','confirmed_pwm_fault',
    'rom_failure','part_mismatch','clock_failure','watchdog_reset','selected_at_start','pwm_setup_failure']]
result={'status':'pass','device_operations':0,'execution_gate':False,'candidate_sha256':sha(bank),
        'elf_sha256':sha(elf),'compiled_cases':results,'image_bytes':len(image),
        'static_ram_bytes':symbols['_bss_end']-0x100000c0,
        'stack_gap_bytes':symbols['_stack_top']-symbols['_bss_end'],
        'source_sha256':manifest['source_sha256'],
        'limits':['MMIO/ROM/IRQ modeled; physical current/polarity/thermal behavior unqualified',
                  'Candidate deployment remains gated on physical LOW1 and independent review']}
(BUILD/'compiled-production-tests.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
