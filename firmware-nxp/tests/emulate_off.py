"""Execute the off-only candidate's compiled startup and SysTick cleanup offline.
MMIO, ROM IAP54 and IRQ delivery are modeled; this is not electrical evidence.
"""
from pathlib import Path
import hashlib,json,struct,sys,os
HERE=Path(__file__).resolve().parent
if os.environ.get('NXP_UNICORN_PATH'): sys.path.insert(0,os.environ['NXP_UNICORN_PATH'])
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_SP

ROOT=HERE.parent
BUILD=ROOT/'build/pwm-off-trial'
image=(BUILD/'open-keylight-nxp-pwm-off-trial.bin').read_bytes()
bank=(BUILD/'pwm-off-trial-bank.bin').read_bytes()
elf=(BUILD/'open-keylight-nxp-pwm-off-trial.elf').read_bytes()
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
    assert forbidden not in symbols
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
    log=[];out={'case':case,'reset':False,'rom_calls':0,'minimum_sp':0x10001000,'main_ready':False}
    running_startup=True
    def mem(_u,access,a,size,value,user):
        del access,user
        assert not 0<=a<0x10000,'Flash write'
        if 0x20004000<=a<0x20005000:
            assert (0x20004000<=a and a+size<=0x20004008) or (a==0x200047fc and size==4)
        if a>=0x40000000:log.append((a,value))
        if a==0x50002280:w(0x50002100,r(0x50002100)&~value)
        if a==0x50002200:w(0x50002100,r(0x50002100)|value)
        if a==0xe000ed0c:
            assert value==0x05fa0004 and r(0x200047fc)==0xaaaaaaaa and r(0x40048080)&(1<<27)
            out['reset']=True;out['reset_ms']=r(symbols['milliseconds']);_u.emu_stop()
    def code(_u,a,size,user):
        del size,user
        out['minimum_sp']=min(out['minimum_sp'],_u.reg_read(UC_ARM_REG_SP))
        if a==0x1fff1ff0:
            command,result=_u.reg_read(UC_ARM_REG_R0),_u.reg_read(UC_ARM_REG_R1)
            assert r(command)==54;w(result,1 if case=='rom_failure' else 0)
            w(result+4,0xbc41 if case=='part_mismatch' else 0xbc40)
            out['rom_calls']+=1;_u.reg_write(UC_ARM_REG_PC,_u.reg_read(UC_ARM_REG_LR))
        if a==0x1fff1000:_u.emu_stop()
        if running_startup and a==(symbols['nxp_trial_expired']&~1):
            out['main_ready']=True;_u.emu_stop()
    u.hook_add(UC_HOOK_MEM_WRITE,mem);u.hook_add(UC_HOOK_CODE,code)
    u.reg_write(UC_ARM_REG_SP,vectors[0]);u.emu_start(vectors[1],0,count=300000)
    assert out['rom_calls']==1
    early=case in ['rom_failure','part_mismatch','clock_failure','watchdog_reset','selected_at_start']
    if early:
        assert out['reset'] and out['reset_ms']==0 and not out['main_ready']
        assert not any(0x40014000<=a<0x40019000 for a,v in log)
    else:
        assert out['main_ready'] and not out['reset']
        running_startup=False
        def call(name,args=()):
            u.reg_write(UC_ARM_REG_SP,0x10000f00);u.reg_write(UC_ARM_REG_LR,0x1fff1001)
            for reg,value in zip([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3],args):u.reg_write(reg,value)
            for i,value in enumerate(args[4:]):w(0x10000f00+4*i,value)
            u.emu_start(symbols[name],0,count=150000)
            return u.reg_read(UC_ARM_REG_R0)
        def report(op,args):
            q=bytearray(97);q[0]=2;q[12]=len(args);q[14]=op;q[15:15+len(args)]=args
            for value in q[9:95]:q[95]^=value
            u.mem_write(0x10001000,bytes(q));u.mem_write(0x10001100,b'\xcc'*97)
            assert call('nxp_link_transaction',[symbols['link'],0x10001000,97,0x10001100,97,1000])==0
            u.mem_write(0x10001000,b'\0'*97)
            assert call('nxp_link_transaction',[symbols['link'],0x10001000,2,0x10001100,97,1000])==0
            assert bytes(u.mem_read(0x10001100,2))==b'\0a'
            assert call('nxp_link_transaction',[symbols['link'],0x10001000,97,0x10001100,97,1000])==0
            assert u.mem_read(0x10001107,1)==b'\x02'
        claim=bytearray(72);claim[0]=1;claim[7]=4;claim[8:12]=b'Test'
        report(0x49,claim);report(0x70,b'OFF1')
        w(symbols['milliseconds'],1000);call('nxp_pwm_off_service',[symbols['off_trial'],symbols['board'],1000])
        meta=symbols['off_trial']
        def be(offset): return int.from_bytes(u.mem_read(meta+offset,4),'big')
        assert be(8)==1 and be(16)==1000 and be(24)==1400
        for base,period,pwm in [(0x40014000,254,3),(0x40018000,25499,11)]:
            assert r(base+4)==1 and r(base+0x20)==period and r(base+0x74)==pwm
            for ch in [0,1,3]:assert r(base+0x18+4*ch)==period+1
        before=len(log)
        for tick in range(399):call('SysTick_Handler')
        assert r(symbols['milliseconds'])==1399 and len(log)==before
        call('SysTick_Handler')
        assert be(8)==2 and be(20)==1400 and be(32)==1 and be(40)==31
        for pin,function in [(13,1),(14,1),(16,0),(18,0),(19,0)]:assert r(0x40044000+4*pin)&7==function
        assert r(0x40014004)==r(0x40018004)==2
        after=len(log);call('nxp_pwm_off_service',[symbols['off_trial'],symbols['board'],1401]);assert len(log)==after
        w(symbols['milliseconds'],29999);call('SysTick_Handler')
        assert out['reset'] and out['reset_ms']==30000
        assert be(56)==30000
        out['off_deadline_ms']=1400;out['off_main_service_calls_after_start']=0
    assert bytes(u.mem_read(0x20004780,124))==b'\xa5'*124
    out['stack_bytes_without_exception_frame_or_ROM']=vectors[0]-out.pop('minimum_sp')
    return out

results=[emulate(c) for c in ['off_deadline','rom_failure','part_mismatch','clock_failure','watchdog_reset','selected_at_start']]
report={'status':'pass','device_operations':0,'execution_gate':False,'candidate_sha256':sha(bank),'elf_sha256':sha(elf),
        'image_bytes':len(image),'static_ram_bytes':symbols['_bss_end']-0x100000c0,
        'stack_gap_bytes':symbols['_stack_top']-symbols['_bss_end'],'compiled_cases':results,
        'source_sha256':manifest['source_sha256'],'review_sha256':sha(Path(__file__).read_bytes()),
        'limits':['Modeled MMIO, ROM and explicit handler calls; not physical IRQ latency or electrical validation',
                  'No deployment gate; uploader/observer and complete final artifact pinning remain separate']}
out=BUILD;out.mkdir(exist_ok=True)
(out/'compiled-off-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
