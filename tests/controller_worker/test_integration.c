/* Real updater adapter + real loader core. Hardware and durable storage are
 * deterministic boundaries; this does not establish physical SPI timing. */
#include "worker_mocks.h"
#include "controller_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static okl_result native_execute(okl_nxp *, const okl_request *, okl_reply *, uint64_t);
static okl_result native_claim(okl_nxp *, const uint8_t *, size_t, uint64_t);
static okl_result native_read(okl_nxp *, okl_light_state *, uint64_t);
#define okl_nxp_execute native_execute
#define okl_nxp_claim native_claim
#define okl_nxp_read_state native_read
#include "../../firmware/main/controller_worker.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_read_state

static unsigned checks, cases;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"case %u, line %d: %s\n",cases,__LINE__,#x); exit(1); } } while (0)
typedef struct {
    uint8_t package[OKL_LOADER_PACKAGE_BYTES], reference[OKL_LOADER_BANK_BYTES];
    uint8_t flash[OKL_LOADER_BANK_BYTES], pending[4096];
    int sector, fail_persist;
    uint64_t now, reset_at;
    unsigned leased, ended, claims, writes, reads, queries, entries, boundaries;
    unsigned exchanges, programs, readbacks, flushes, commits, aborts, waits;
    unsigned fail_exchange, corrupt_exchange, fail_query, fail_write, fail_read;
    unsigned wrong_part, wrong_version, wrong_role, wrong_caps, non_dark, changed_status;
    unsigned entry_fault, send_fault, boundary_fault, observation_fault, acquire_fault;
    unsigned postcommit_queries, observation_reads, persist_count;
    bool original, loader, observing;
    okl_loader_phase persisted;
    okl_loader_delivery delivery;
    okl_light_state state;
    okl_nxp driver;
    app_controller_job job;
} model;
static model m;
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void word(uint8_t *p,uint32_t n) { p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n; }
static void little(uint8_t *p,uint32_t n) { p[3]=(uint8_t)(n>>24);p[2]=(uint8_t)(n>>16);p[1]=(uint8_t)(n>>8);p[0]=(uint8_t)n; }
static uint64_t test_clock(void *unused) { (void)unused;return m.now; }
/* Equality oracle for SHA boundary injection. Real SHA256/package admission is
 * independently tested by tests/loader's Python hashlib-to-C integration. */
int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char out[32],int is224) {
    CHECK(p && n==OKL_LOADER_BANK_BYTES && !is224);
    memset(out,0x42,32);if(memcmp(p,m.reference,n))out[0]^=1;return 0;
}
void vTaskDelay(unsigned ticks) {
    CHECK(m.leased && ticks && ticks<=10 && (m.entries || m.commits || m.aborts));
    ++m.waits;m.now+=(uint64_t)ticks*1000;
}
static void call(okl_nxp *d,uint64_t deadline,uint64_t maximum) {
    CHECK(d==&m.driver && m.leased && deadline>m.now && deadline<=m.now+maximum);m.now+=100;
}
static okl_result native_execute(okl_nxp *d,const okl_request *q,okl_reply *r,uint64_t deadline) {
    call(d,deadline,150000);CHECK(!m.loader);memset(r,0,sizeof(*r));
    r->received=r->acknowledged=1;r->report.status=2;uint8_t *p=r->report.arguments;
    if(q->command==OKL_GET_FIRMWARE) {
        ++m.queries;r->report.opcode=0x87;r->report.size=4;
        if(m.observing)memcpy(p,m.package+24,4);else {p[0]=m.original?0:1;p[1]=m.original?1:3;}
        if(m.wrong_version || (m.observing && m.observation_fault==1))++p[1];
    } else if(q->command==OKL_GET_CONTROLLER_STATUS) {
        ++m.queries;r->report.opcode=0xfc;
        if(!m.original) {r->acknowledged=0;r->report.status=5;return OKL_REMOTE;}
        r->report.size=24;memcpy(p,"OKLC",4);p[4]=1;p[6]=m.wrong_role?1:2;
        p[7]=m.changed_status && m.writes?2:0;
        word(p+8,m.wrong_caps?1:3);word(p+12,m.wrong_part?0xbc41:OKL_LOADER_PART_ID);word(p+16,5);
        if(m.observing) {
            if(m.observation_fault==2)p[6]=1;
            if(m.observation_fault==3)word(p+8,1);
            if(m.observation_fault==4)p[4]=2;
            if(m.observation_fault==5)p[5]=1;
            if(m.observation_fault==6)p[7]=1;
            if(m.observation_fault==7)p[7]=2;
        }
    } else if(q->command==OKL_GET_PART_ID) {
        ++m.queries;r->report.opcode=0xfe;r->report.size=4;
        word(p,m.wrong_part || (m.observing && m.observation_fault==8)?0xbc41:OKL_LOADER_PART_ID);
    } else {
        CHECK(m.persisted==OKL_LOADER_ENTERING && m.claims==1 && !m.commits && !m.aborts);
        ++m.writes;if(m.writes==m.fail_write)return OKL_TIMEOUT;
        if(q->command==OKL_SET_WHITE_BRIGHTNESS) {CHECK(q->arguments[2]==0);m.state.white_brightness=0;}
        else if(q->command==OKL_SET_EFFECT) {CHECK(q->arguments[2]==0);m.state.effect=0;}
        else CHECK(false); /* FD, frames, brightness, temperature and replay forbidden. */
        return OKL_OK;
    }
    if(m.observing)++m.postcommit_queries;
    if(m.queries==m.fail_query || (m.observing && m.observation_fault==9))return OKL_TIMEOUT;
    return OKL_OK;
}
static okl_result native_claim(okl_nxp *d,const uint8_t *name,size_t n,uint64_t deadline) {
    call(d,deadline,600000);CHECK(m.persisted==OKL_LOADER_ENTERING && !m.loader && !m.commits);
    CHECK(n==13 && !memcmp(name,"Open Keylight",13));++m.claims;return OKL_OK;
}
static okl_result native_read(okl_nxp *d,okl_light_state *s,uint64_t deadline) {
    call(d,deadline,800000);CHECK(!m.loader);++m.reads;*s=m.state;
    if(m.observing)++m.observation_reads;
    if((m.non_dark && m.writes) || (m.observing && m.observation_fault==10))s->white_brightness=1;
    return m.reads==m.fail_read?OKL_TIMEOUT:OKL_OK;
}
okl_result app_nxp_loader_acquire(okl_nxp *d,uint32_t id,uint64_t deadline) {
    CHECK(d==&m.driver && id==11 && !m.leased && deadline>m.now);
    if(m.acquire_fault)return OKL_BUSY;
    m.leased=1;return OKL_OK;
}
void app_nxp_loader_release(okl_nxp *d,uint32_t id) {
    CHECK(d==&m.driver && id==11 && m.leased);
    if(m.entries || m.commits || m.aborts)CHECK(m.now-m.reset_at>=OKL_LOADER_QUIET_US);
    m.leased=0;++m.ended;
}
okl_result app_nxp_loader_enter(okl_nxp *d,uint32_t id,okl_loader_source source,okl_loader_delivery *sent,uint64_t deadline) {
    call(d,deadline,2000000);CHECK(id==11 && source==m.job.source && !m.state.effect && !m.state.white_brightness);
    CHECK(m.persisted==OKL_LOADER_ENTERING && !m.entries);++m.entries;m.reset_at=m.now;
    *sent=m.entry_fault==2?OKL_LOADER_NOT_SENT:m.entry_fault?OKL_LOADER_MAYBE_SENT:OKL_LOADER_SENT_COMPLETE;
    m.delivery=*sent;return m.entry_fault==1?OKL_TIMEOUT:OKL_OK;
}
okl_result app_nxp_loader_reset_boundary(okl_nxp *d,uint32_t id,uint8_t op,okl_loader_delivery sent,uint64_t deadline) {
    call(d,deadline,120000000);CHECK(id==11 && sent==OKL_LOADER_SENT_COMPLETE && sent==m.delivery);
    CHECK(m.now-m.reset_at>=OKL_LOADER_QUIET_US);++m.boundaries;
    if(m.boundary_fault==m.boundaries)return OKL_IO;
    if(op==0x84) {CHECK(m.entries==1 && !m.commits && !m.aborts);m.loader=true;}
    else if(op==5) {CHECK(m.commits==1 && !m.aborts);m.loader=false;m.original=true;m.observing=true;}
    else {CHECK(op==4 && m.aborts==1 && !m.commits);m.loader=false;}
    return OKL_OK;
}
static void flush(void) {
    if(m.sector>=0) {memcpy(m.flash+m.sector,m.pending,4096);m.sector=-1;++m.flushes;}
}
okl_result app_nxp_loader_exchange(okl_nxp *d,uint32_t id,const uint8_t raw[90],uint8_t response[90],okl_loader_delivery *sent,uint64_t deadline) {
    okl_report q;uint8_t args[80];call(d,deadline,2000000);
    CHECK(id==11 && m.loader && !m.commits && !m.aborts);
    CHECK(okl_report_decode(&q,raw,90)==OKL_OK && !q.status && !q.transaction && q.command_class==0x10);
    ++m.exchanges;*sent=OKL_LOADER_SENT_COMPLETE;
    if(m.exchanges==m.fail_exchange) {*sent=OKL_LOADER_MAYBE_SENT;return OKL_TIMEOUT;}
    memcpy(args,q.arguments,q.size);
    if(q.opcode==0x80) {
        CHECK(m.exchanges==1 && q.size==80 && !memcmp(args,(uint8_t[80]){0},80));
        memcpy(args,(uint8_t[80]){3,24,1,2,0,0,0,2,93},80);
    } else if(q.opcode==1) {
        CHECK(m.persisted==OKL_LOADER_ERASING && q.size==8 && read32(args)==0x2000 && read32(args+4)==0x8fff);
        memset(m.flash,0xff,sizeof(m.flash));
    } else if(q.opcode==2) {
        uint32_t offset=read32(args+1)-0x2000;
        CHECK(m.persisted==OKL_LOADER_PROGRAMMING && q.size==69 && args[0]==64);
        CHECK(offset==m.programs*64 && offset<=OKL_LOADER_BANK_BYTES-64);
        CHECK(!memcmp(args+5,m.reference+offset,64));
        if(m.sector!=(int)(offset&~4095u)) {flush();m.sector=(int)(offset&~4095u);memcpy(m.pending,m.flash+m.sector,4096);}
        memcpy(m.pending+(offset&4095u),args+5,64);++m.programs;
    } else {
        uint32_t offset=read32(args+1)-0x2000;
        CHECK(q.opcode==0x83 && m.persisted==OKL_LOADER_VERIFYING && m.programs==448);
        CHECK(q.size==69 && args[0]==64 && offset==m.readbacks*64 && offset<=OKL_LOADER_BANK_BYTES-64);
        CHECK(!memcmp(args+5,(uint8_t[64]){0},64));flush();memcpy(args+5,m.flash+offset,64);++m.readbacks;
    }
    CHECK(okl_report_encode(response,0,0x10,q.opcode,args,q.size)==OKL_OK);response[0]=2;
    if(m.exchanges==m.corrupt_exchange)response[88]^=1;
    return OKL_OK;
}
okl_result app_nxp_loader_send_only(okl_nxp *d,uint32_t id,const uint8_t raw[90],okl_loader_delivery *sent,uint64_t deadline) {
    okl_report q;call(d,deadline,2000000);CHECK(id==11 && m.loader && !m.commits && !m.aborts);
    CHECK(okl_report_decode(&q,raw,90)==OKL_OK && !q.status && !q.transaction && q.command_class==0x10 && !q.size);
    if(q.opcode==5) {
        CHECK(m.persisted==OKL_LOADER_COMMITTING && m.programs==448 && m.readbacks==448);
        CHECK(m.flushes==7 && m.sector==-1 && !memcmp(m.flash,m.reference,sizeof(m.flash)));++m.commits;
    } else {CHECK(q.opcode==4);++m.aborts;}
    m.reset_at=m.now;*sent=m.send_fault?OKL_LOADER_MAYBE_SENT:OKL_LOADER_SENT_COMPLETE;m.delivery=*sent;
    return m.send_fault==1?OKL_TIMEOUT:OKL_OK;
}
int app_controller_update_persist(uint32_t id,const okl_loader_audit *a) {
    CHECK(id==11 && m.leased);++m.persist_count;
    if((int)a->phase==m.fail_persist)return -1;
    m.persisted=a->phase;return 0;
}
void app_controller_update_progress(uint32_t id,const okl_loader_audit *a) {CHECK(id==11 && m.leased && a);}
static void reset(bool original) {
    ++cases;memset(&m,0,sizeof(m));m.sector=-1;m.fail_persist=-1;m.original=original;
    m.driver.transport.now_us=test_clock;m.job.id=11;
    m.job.source=original?OKL_LOADER_FROM_ORIGINAL:OKL_LOADER_FROM_LEGACY_1_3;
    m.state=(okl_light_state){.effect=8,.white_brightness=51,.color_brightness=255,.temperature_kelvin=4700};
    memcpy(m.package,"OKLCNXP",8);m.package[9]=1;m.package[11]=64;
    word(m.package+12,OKL_LOADER_BANK_BYTES);word(m.package+16,OKL_LOADER_PART_ID);
    m.package[20]=1;m.package[22]=2;m.package[25]=1;m.package[26]=2;memset(m.package+28,0x42,32);
    memset(m.reference,0x5a,sizeof(m.reference));little(m.reference,0x10001000);
    for(unsigned i=4;i<192;i+=4)little(m.reference+i,0x20c1);
    memcpy(m.package+64,m.reference,sizeof(m.reference));
    update_context c={.driver=&m.driver,.job=&m.job};okl_loader_ops ops={.user=&c,.sha256=digest};
    CHECK(okl_loader_prepare(&m.job.image,m.package,sizeof(m.package),&ops)==OKL_LOADER_OK);
}
static okl_loader_result run(okl_loader_audit *a) {
    okl_loader_result r=app_controller_worker_run(&m.driver,&m.job,a);
    CHECK(!m.leased && m.ended==!m.acquire_fault && m.entries<=1 && m.commits<=1 && m.aborts<=1);
    CHECK(!(m.commits && m.aborts));return r;
}
int main(void) {
    okl_loader_audit a;
    for(unsigned original=0;original<2;++original) {
        reset(original!=0);CHECK(run(&a)==OKL_LOADER_OK);
        CHECK(a.phase==OKL_LOADER_APPLICATION_SEEN && a.complete_bank_verified && !a.observation.controller.trial_confirmed);
        CHECK(m.claims==1 && m.writes==2 && m.reads==3 && m.entries==1 && m.boundaries==2 && m.commits==1 && !m.aborts);
        CHECK(m.exchanges==898 && m.programs==448 && m.readbacks==448 && m.waits==600);
        CHECK(a.program_blocks_acked==448 && a.readback_blocks_verified==448 && a.quiet_completed && a.reset_boundary_established);
        CHECK(m.postcommit_queries==3 && m.observation_reads==1 && m.state.temperature_kelvin==4700);
    }
    for(unsigned fault=0;fault<10;++fault) {
        reset(fault!=0);
        if(fault<2)m.wrong_part=1;
        if(fault==2)m.wrong_role=1;
        if(fault==3)m.wrong_caps=1;
        if(fault>=4 && fault<=6)m.fail_query=fault-3;
        if(fault==7)m.job.source=OKL_LOADER_FROM_FRESH_RESIDENT;
        if(fault==8){m.original=false;m.job.source=OKL_LOADER_FROM_LEGACY_1_3;m.wrong_version=1;}
        if(fault==9)m.acquire_fault=1;
        CHECK(run(&a)!=OKL_LOADER_OK && !m.claims && !m.writes && !m.entries && !m.exchanges);
    }
    for(unsigned fault=0;fault<9;++fault) {
        reset(true);
        if(fault<2)m.fail_write=fault+1;
        if(fault==2)m.non_dark=1;
        if(fault==3)m.changed_status=1;
        if(fault==4)m.fail_read=1;
        if(fault==5)m.fail_read=2;
        if(fault>=6)m.entry_fault=fault-5;
        CHECK(run(&a)!=OKL_LOADER_OK && !m.exchanges && !m.commits && !m.aborts);
        if(fault>=6)CHECK(m.waits==300 && m.entries==1 && !m.boundaries);
    }
    /* Every raw transaction boundary faults against the real core+adapter. */
    for(unsigned fault=1;fault<=898;++fault) {
        reset(true);m.fail_exchange=fault;CHECK(run(&a)==OKL_LOADER_IO && !m.commits);
        CHECK(m.exchanges==fault && m.aborts==(fault>1));
        CHECK(m.boundaries==(fault>1?2u:1u) && m.waits==(fault>1?600u:300u));
    }
    for(unsigned fault=1;fault<=898;fault+=53) {
        reset(false);m.corrupt_exchange=fault;CHECK(run(&a)==OKL_LOADER_PROTOCOL && !m.commits);
        CHECK(m.exchanges==fault && m.aborts==(fault>1));
    }
    for(unsigned fault=1;fault<=10;++fault) {
        reset(true);m.observation_fault=fault;CHECK(run(&a)==OKL_LOADER_UNRESOLVED);
        CHECK(m.commits==1 && !m.aborts && m.writes==2 && m.claims==1 && m.boundaries==2 && m.waits==600);
    }
    for(unsigned fault=1;fault<=2;++fault) {
        reset(true);m.send_fault=fault;CHECK(run(&a)==OKL_LOADER_UNRESOLVED);
        CHECK(m.commits==1 && !m.aborts && m.waits==600 && m.boundaries==1 && !m.postcommit_queries);
        reset(true);m.fail_exchange=400;m.send_fault=fault;CHECK(run(&a)==OKL_LOADER_IO);
        CHECK(m.aborts==1 && !m.commits && m.waits==600 && m.boundaries==1 && !m.postcommit_queries);
        reset(true);m.boundary_fault=fault;CHECK(run(&a)!=OKL_LOADER_OK);
        CHECK(m.boundaries==fault && !m.aborts && !m.postcommit_queries);
    }
    for(unsigned phase=OKL_LOADER_VALIDATED;phase<=OKL_LOADER_OBSERVING;++phase) {
        reset(true);m.fail_persist=(int)phase;CHECK(run(&a)!=OKL_LOADER_OK && a.persistence_failed);
        if(phase<=OKL_LOADER_ENTERING)CHECK(!m.claims && !m.entries && !m.exchanges);
        if(phase==OKL_LOADER_COMMITTING)CHECK(!m.commits && m.aborts==1);
    }
    printf("%u real controller-worker/core integration assertions across %u cases passed\n",checks,cases);
    return 0;
}
