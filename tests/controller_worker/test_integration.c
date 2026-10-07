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
static okl_result native_release(okl_nxp *, uint64_t);
#define okl_nxp_execute native_execute
#define okl_nxp_claim native_claim
#define okl_nxp_read_state native_read
#define okl_nxp_release native_release
#include "../../firmware/main/controller_worker.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_read_state
#undef okl_nxp_release
#include "../controller_job/off_fixture.h"

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
    unsigned unsupported_query, malformed_query, claim_fault, poison_query, release_poison;
    unsigned owner_gate, denied_fault, bench_fault, bench_commands, bench_pages, bench_claims, preserves, resumes;
    unsigned bench_fail_page;
    uint32_t bench_words[224];
    uint64_t bench_last_page;
    bool original, loader, observing;
    okl_loader_phase persisted;
    okl_loader_delivery delivery;
    okl_light_state state;
    okl_nxp driver;
    app_controller_job job;
    app_controller_worker_outcome outcome;
} model;
static model m;
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void word(uint8_t *p,uint32_t n) { p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n; }
static void little(uint8_t *p,uint32_t n) { p[3]=(uint8_t)(n>>24);p[2]=(uint8_t)(n>>16);p[1]=(uint8_t)(n>>8);p[0]=(uint8_t)n; }
static uint64_t test_clock(void *unused) { (void)unused;return m.now; }
esp_err_t app_nxp_transport_init(okl_nxp *d,const uint8_t id[6]) { (void)d;(void)id;CHECK(false);return -1; }
okl_result app_nxp_transport_snapshot(okl_nxp *d,app_nxp_transport_diagnostic *out,uint64_t deadline) {
    (void)out;CHECK(d==&m.driver && deadline>m.now);return OKL_IO;
}
static okl_result native_release(okl_nxp *d,uint64_t deadline) { (void)d;(void)deadline;CHECK(false);return OKL_IO; }
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
    if(q->command==OKL_GET_DIAGNOSTIC_PROFILE) {
        CHECK(m.job.image.role==1 && m.observing && m.bench_claims==1);
        r->report.opcode=0xf0;r->report.size=8;memcpy(p,"OFF1",4);p[6]=1;p[7]=144;
        if(m.bench_fault==1)p[0]^=1;
        return OKL_OK;
    }
    if(q->command==OKL_GET_DIAGNOSTIC_PAGE) {
        CHECK(q->size==1 && q->arguments[0]<14 && m.bench_claims==1);
        uint8_t page=q->arguments[0];++m.bench_pages;
        if(m.bench_pages==m.bench_fail_page)return OKL_TIMEOUT;
        r->report.opcode=0xf1;r->report.size=72;memcpy(p,"OFF1",4);p[4]=page;p[5]=14;p[6]=64;
        uint32_t initial[16]={0x4f464631,1,0,0,0,0,0,0,0,0,0,0,0,0x193,30000,17};
        if(m.bench_fault==2)initial[2]=1;
        const uint32_t *values=m.bench_commands?m.bench_words+page*16:initial;
        CHECK(m.bench_commands || !page);
        for(unsigned i=0;i<16;++i)word(p+8+i*4,values[i]);
        if(m.bench_fault==4 && m.bench_commands && page==13)p[71]=1;
        if(m.bench_fault==5 && m.bench_pages==16)p[71]^=1;
        m.bench_last_page=m.now;return OKL_OK;
    }
    if(q->command==OKL_RUN_DIAGNOSTIC_OFF) {
        CHECK(q->size==4 && !memcmp(q->arguments,"OFF1",4) && m.bench_claims==1 && !m.bench_commands);
        ++m.bench_commands;r->report.opcode=0x70;r->report.size=4;memcpy(p,"OFF1",4);
        return m.bench_fault==3?OKL_TIMEOUT:OKL_OK;
    }
    if(q->command==OKL_GET_FIRMWARE) {
        ++m.queries;r->report.opcode=0x87;r->report.size=4;
        if(m.observing)memcpy(p,m.package+24,4);else {p[0]=m.original?0:1;p[1]=m.original?1:3;}
        if(m.wrong_version || (m.observing && m.observation_fault==1))++p[1];
    } else if(q->command==OKL_GET_CONTROLLER_STATUS) {
        ++m.queries;r->report.opcode=0xfc;
        if(!m.original) {
            r->acknowledged=0;r->report.status=5;
            if(m.owner_gate && !m.claims) {
                r->report.status=8;
                if(m.denied_fault==1)r->received=0;
                if(m.denied_fault==2)r->acknowledged=1;
                if(m.denied_fault==3)r->report.command_class=1;
                if(m.denied_fault==4)r->report.opcode=0xfd;
                if(m.denied_fault==5)r->report.size=1;
                return OKL_OWNER_DENIED;
            }
            return OKL_REMOTE;
        }
        r->report.size=24;memcpy(p,"OKLC",4);p[4]=1;p[6]=m.wrong_role?1:2;
        p[7]=m.changed_status && m.writes?2:0;
        word(p+8,m.wrong_caps?1:3);word(p+12,m.wrong_part?0xbc41:OKL_LOADER_PART_ID);word(p+16,5);
        if(m.observing) {
            p[6]=m.job.image.role;word(p+8,p[6]==1?1:3);
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
    if(m.queries==m.poison_query)d->needs_recovery=1;
    if(m.queries==m.malformed_query)r->report.size=1;
    if(m.queries==m.unsupported_query) {
        r->acknowledged=0;r->report.status=5;r->report.size=0;return OKL_REMOTE;
    }
    if(m.queries==m.fail_query || (m.observing && m.observation_fault==9))return OKL_TIMEOUT;
    return OKL_OK;
}
static okl_result native_claim(okl_nxp *d,const uint8_t *name,size_t n,uint64_t deadline) {
    call(d,deadline,600000);CHECK(!m.loader);
    CHECK(n==13 && !memcmp(name,"Open Keylight",13));
    if(m.commits) {CHECK(m.persisted==OKL_LOADER_APPLICATION_SEEN && m.job.image.role==1);++m.bench_claims;}
    else {CHECK(m.persisted==OKL_LOADER_ENTERING);++m.claims;}
    return m.claim_fault?OKL_TIMEOUT:OKL_OK;
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
    if(m.release_poison)d->needs_recovery=1;
}
okl_result app_nxp_loader_preserve_resident(okl_nxp *d,uint32_t id,uint64_t deadline) {
    call(d,deadline,150000);CHECK(id==11 && m.bench_commands==1 && m.bench_pages==16);
    CHECK(m.now-m.bench_last_page>=33000000);++m.preserves;
    return m.bench_fault==7?OKL_IO:OKL_OK;
}
okl_result app_nxp_loader_use_resident(okl_nxp *d,uint32_t id,uint32_t proof,uint64_t deadline) {
    call(d,deadline,120000000);CHECK(id==11 && proof==10);++m.resumes;
    m.loader=true;return m.bench_fault==8?OKL_NEEDS_RECOVERY:OKL_OK;
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
    if(m.commits && m.job.image.role==1) {
        CHECK(m.now-m.bench_last_page>=33000000 && m.bench_commands==1 && m.bench_pages==16);
        CHECK(okl_report_decode(&q,raw,90)==OKL_OK && q.command_class==0x10 && q.opcode==0x80 && q.size==80);
        CHECK(!memcmp(q.arguments,(uint8_t[80]){0},80));m.loader=true;
        CHECK(okl_report_encode(response,0,0x10,0x80,(uint8_t[80]){3,24,1,2,0,0,0,2,93},80)==OKL_OK);
        response[0]=2;*sent=OKL_LOADER_SENT_COMPLETE;
        if(m.bench_fault==6)response[88]^=1;
        return OKL_OK;
    }
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
    okl_loader_result r=app_controller_worker_run(&m.driver,&m.job,a,&m.outcome);
    CHECK(!m.leased && m.ended==(unsigned)(!m.acquire_fault)+(unsigned)(m.bench_claims!=0) && m.entries<=1 && m.commits<=1 && m.aborts<=1);
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
        CHECK(m.outcome.entry==APP_CONTROLLER_MUTATION_ATTEMPTED && m.outcome.synchronized);
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
    /* A stock-style unsupported FE is safe rejection only after complete
     * correlated reads, before claim, with a synchronized released lease. */
    reset(false);m.unsupported_query=3;CHECK(run(&a)==OKL_LOADER_INVALID);
    CHECK(m.outcome.entry==APP_CONTROLLER_READ_ONLY_UNSUPPORTED && m.outcome.synchronized);
    CHECK(a.phase==OKL_LOADER_PRECOMMIT_FAILED && a.result==OKL_LOADER_INVALID && !a.persistence_failed);
    CHECK(!m.claims && !m.writes && !m.entries && !m.exchanges && !m.commits && !m.aborts);
    for(unsigned variant=0;variant<7;++variant) {
        reset(true);
        if(variant==0)m.unsupported_query=1;
        if(variant==1)m.unsupported_query=2;
        if(variant==2)m.unsupported_query=3;
        if(variant==3)m.wrong_role=1;
        if(variant==4)m.wrong_caps=1;
        if(variant==5)m.wrong_part=1;
        if(variant==6){m.original=false;m.job.source=OKL_LOADER_FROM_LEGACY_1_3;m.wrong_version=1;}
        CHECK(run(&a)==OKL_LOADER_INVALID && m.outcome.entry==APP_CONTROLLER_READ_ONLY_UNSUPPORTED);
        CHECK(m.outcome.synchronized && !m.claims && !m.writes && !m.entries);
    }
    for(unsigned variant=0;variant<7;++variant) {
        reset(true);
        if(variant==0)m.fail_query=3;
        if(variant==1)m.malformed_query=3;
        if(variant==2){m.unsupported_query=3;m.poison_query=3;}
        if(variant==3){m.unsupported_query=3;m.release_poison=1;}
        if(variant==4){m.unsupported_query=3;m.fail_persist=OKL_LOADER_PRECOMMIT_FAILED;}
        if(variant==5)m.claim_fault=1;
        if(variant==6)m.fail_write=1;
        CHECK(run(&a)!=OKL_LOADER_INVALID);
        if(variant<3)CHECK(m.outcome.entry==APP_CONTROLLER_ENTRY_UNPROVEN);
        if(variant==3)CHECK(!m.outcome.synchronized);
        if(variant==4)CHECK(a.persistence_failed);
        if(variant>=5)CHECK(m.outcome.entry==APP_CONTROLLER_MUTATION_ATTEMPTED && m.claims==1);
        CHECK(!m.entries && !m.exchanges);
    }
    /* The actual legacy bridge denies unknown FC until we own it. The exact
     * version/denial exception claims once, never weakens the part proof. */
    reset(false);m.owner_gate=1;CHECK(run(&a)==OKL_LOADER_OK && m.claims==1 && m.entries==1);
    for(unsigned bad=1;bad<=5;++bad) {
        reset(false);m.owner_gate=1;m.denied_fault=bad;
        CHECK(run(&a)!=OKL_LOADER_OK && !m.claims && !m.entries && !m.writes);
    }
    reset(false);m.owner_gate=1;m.unsupported_query=4;
    CHECK(run(&a)!=OKL_LOADER_OK && m.claims==1 && !m.entries && !m.writes);
    CHECK(m.outcome.entry==APP_CONTROLLER_MUTATION_ATTEMPTED); /* no read-only clear after claim */
    for(unsigned fault=0;fault<=7;++fault) {
        reset(false);m.owner_gate=1;m.bench_fault=fault;
        m.package[22]=m.job.image.role=1;m.package[26]=m.job.image.version.component[2]=0;
        off_fixture(m.bench_words);
        CHECK(run(&a)==OKL_LOADER_OK && m.outcome.diagnostic_trial_observed && m.bench_commands<=1);
        CHECK(m.commits==1 && m.claims==1 && !m.aborts && !a.observation.controller.trial_confirmed);
        CHECK((m.outcome.resident_proof_job_id==11)==(fault==0));
        CHECK(m.outcome.diagnostic_error[0]!=(fault==0));
        if(!fault)CHECK(m.outcome.registers_verified && m.preserves==1 && m.bench_pages==16 && m.waits==3955);
    }
    for(unsigned page=1;page<=16;++page) {
        reset(true);m.bench_fail_page=page;
        m.package[22]=m.job.image.role=1;m.package[26]=m.job.image.version.component[2]=0;
        off_fixture(m.bench_words);
        CHECK(run(&a)==OKL_LOADER_OK && m.bench_pages==page && !m.preserves && !m.outcome.resident_proof_job_id);
        CHECK(m.bench_commands<=1 && m.outcome.diagnostic_error[0]);
    }
    reset(true);m.job.source=OKL_LOADER_FROM_FRESH_RESIDENT;m.job.resident_proof_job_id=10;
    m.state.effect=0;m.state.white_brightness=0;
    CHECK(run(&a)==OKL_LOADER_OK && m.resumes==1 && !m.entries && !m.claims && !m.writes);
    reset(true);m.job.source=OKL_LOADER_FROM_FRESH_RESIDENT;m.job.resident_proof_job_id=10;m.bench_fault=8;
    CHECK(run(&a)!=OKL_LOADER_OK && m.resumes==1 && !m.exchanges && !m.writes);
    printf("%u real controller-worker/core integration assertions across %u cases passed\n",checks,cases);
    return 0;
}
