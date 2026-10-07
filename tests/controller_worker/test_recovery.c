/* Actual recovery adapter; physical transport, storage and clock are boundaries. */
#include "worker_mocks.h"
#include "controller_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static okl_result native_execute(okl_nxp *,const okl_request *,okl_reply *,uint64_t);
static okl_result native_claim(okl_nxp *,const uint8_t *,size_t,uint64_t);
static okl_result native_read(okl_nxp *,okl_light_state *,uint64_t);
static okl_result native_release(okl_nxp *,uint64_t);
#define okl_nxp_execute native_execute
#define okl_nxp_claim native_claim
#define okl_nxp_read_state native_read
#define okl_nxp_release native_release
#include "../../firmware/main/controller_worker.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_read_state
#undef okl_nxp_release

static unsigned checks,cases;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"recovery case%u line%d: %s\n",cases,__LINE__,#x);exit(1); } } while(0)
static const uint8_t mac[6]={2,1,2,3,4,5};
static struct {
    okl_nxp driver;app_controller_job job;app_controller_worker_outcome out;
    uint64_t now,initialized_at;unsigned init_calls,leased,releases,waits,wires,fail_at;
    unsigned init_fault,lease_fault,preserve_fault,release_poison,info_kind,info_fault,info_delivery,info_payload_byte;
    unsigned malformed,read_count,claims,writes,owner_releases,preserves,snapshots,snapshot_fault;
    char sequence[32];uint8_t last_report[90];unsigned has_report;
    okl_light_state state;
} m;
static uint64_t test_clock(void *p) { (void)p;return m.now; }
static bool wire(char kind,uint64_t deadline) {
    CHECK(m.leased && m.now-m.initialized_at>=33000000 && deadline>m.now);
    CHECK(m.wires+1<sizeof(m.sequence));m.sequence[m.wires++]=kind;m.now+=100;
    return m.wires==m.fail_at;
}
static void save_reply(const okl_reply *r) {
    CHECK(okl_report_encode(m.last_report,r->report.transaction,r->report.command_class,r->report.opcode,
        r->report.arguments,r->report.size)==OKL_OK);
    m.last_report[0]=r->report.status;m.has_report=1;
}
static void word(uint8_t *p,uint32_t n) { p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n; }
esp_err_t app_nxp_transport_init(okl_nxp *d,const uint8_t identity[6]) {
    CHECK(d==&m.driver && !d->transport.now_us && !memcmp(identity,mac,6));++m.init_calls;
    if(m.init_fault)return -1;
    d->transport.now_us=test_clock;d->transport.user=&m;m.initialized_at=m.now;return ESP_OK;
}
okl_result app_nxp_transport_snapshot(okl_nxp *d,app_nxp_transport_diagnostic *out,uint64_t deadline) {
    CHECK(d==&m.driver && deadline>m.now);++m.snapshots;
    if(m.snapshot_fault)return OKL_BUSY;
    memset(out,0,sizeof(*out));out->phase=d->needs_recovery?APP_NXP_BUS_UNKNOWN:APP_NXP_BUS_IDLE;
    out->ready=1;out->last_delivery=OKL_LOADER_SENT_COMPLETE;out->has_report=m.has_report;
    memcpy(out->report,m.last_report,90);return OKL_OK;
}
int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char out[32],int is224) {
    CHECK(p && n==90 && !is224);memset(out,0x42,32);return 0;
}
void vTaskDelay(unsigned ticks) {
    CHECK(m.leased && !m.wires && ticks>0 && ticks<=10);++m.waits;m.now+=(uint64_t)ticks*1000;
}
okl_result app_nxp_loader_acquire(okl_nxp *d,uint32_t id,uint64_t deadline) {
    CHECK(d==&m.driver && id==19 && !m.leased && deadline>m.now);
    if(m.lease_fault)return OKL_BUSY;
    m.leased=1;return OKL_OK;
}
void app_nxp_loader_release(okl_nxp *d,uint32_t id) {
    CHECK(d==&m.driver && id==19 && m.leased);++m.releases;m.leased=0;
    if(m.release_poison)d->needs_recovery=1;
}
okl_result app_nxp_loader_preserve_resident(okl_nxp *d,uint32_t id,uint64_t deadline) {
    CHECK(d==&m.driver && id==19 && m.leased && deadline>m.now && m.wires==1 && !m.info_kind);++m.preserves;
    return m.preserve_fault?OKL_NEEDS_RECOVERY:OKL_OK;
}
okl_result app_nxp_loader_exchange(okl_nxp *d,uint32_t id,const uint8_t raw[90],uint8_t response[90],
                                  okl_loader_delivery *delivery,uint64_t deadline) {
    okl_report q;CHECK(d==&m.driver && id==19);
    CHECK(okl_report_decode(&q,raw,90)==OKL_OK && !q.status && !q.transaction && q.command_class==0x10 && q.opcode==0x80 && q.size==80);
    CHECK(!memcmp(q.arguments,(uint8_t[80]){0},80));
    bool fail=wire('I',deadline);CHECK(m.wires==1);*delivery=OKL_LOADER_SENT_COMPLETE;
    if(fail) {d->needs_recovery=1;*delivery=OKL_LOADER_MAYBE_SENT;return OKL_TIMEOUT;}
    if(m.info_delivery)*delivery=m.info_delivery==1?OKL_LOADER_NOT_SENT:OKL_LOADER_MAYBE_SENT;
    uint8_t args[80]={3,24,1,2,0,0,0,2,93};
    if(m.info_kind)memset(args,0,sizeof(args));
    if(m.info_payload_byte)args[m.info_payload_byte-1]=1;
    CHECK(okl_report_encode(response,0,0x10,0x80,args,80)==OKL_OK);
    response[0]=m.info_kind?(uint8_t)m.info_kind:2;
    if(m.info_fault) {
        okl_report r;CHECK(okl_report_decode(&r,response,90)==OKL_OK);
        if(m.info_fault==1)r.transaction=1;
        if(m.info_fault==2)r.command_class=0;
        if(m.info_fault==3)r.opcode=0x81;
        if(m.info_fault==4)r.size=79;
        if(m.info_fault==5)r.arguments[8]^=1;
        if(m.info_fault==6)r.arguments[79]=1;
        if(m.info_fault==8)r.size=0;
        CHECK(okl_report_encode(response,r.transaction,r.command_class,r.opcode,r.arguments,r.size)==OKL_OK);response[0]=r.status;
        if(m.info_fault==7)response[88]^=1;
    }
    memcpy(m.last_report,response,90);m.has_report=1;return OKL_OK;
}
static okl_result native_execute(okl_nxp *d,const okl_request *q,okl_reply *r,uint64_t deadline) {
    CHECK(d==&m.driver && m.info_kind && m.job.allow_legacy_reconcile);memset(r,0,sizeof(*r));
    r->received=r->acknowledged=1;r->report.status=2;
    bool fail=false;
    if(q->command==OKL_GET_FIRMWARE) {
        fail=wire('V',deadline);r->report.opcode=0x87;r->report.size=4;memcpy(r->report.arguments,(uint8_t[]){1,3,0,0},4);
        if(m.malformed==1)r->report.arguments[1]=2;
        if(m.malformed==2)r->report.size=3;
    } else if(q->command==OKL_GET_CONTROLLER_STATUS) {
        fail=wire('S',deadline);CHECK(m.claims==1);r->report.opcode=0xfc;r->report.status=5;r->acknowledged=0;
        if(m.malformed==3)r->report.status=8;
        if(m.malformed==4)r->report.size=1;
        if(m.malformed==5)r->report.opcode=0xfd;
        if(m.malformed==6)r->report.command_class=1;
        if(m.malformed==7)r->report.status=2;
    } else if(q->command==OKL_GET_PART_ID) {
        fail=wire('P',deadline);r->report.opcode=0xfe;r->report.size=4;word(r->report.arguments,m.malformed==8?0xbc41:0xbc40);
        if(m.malformed==9)r->report.size=3;
    } else {
        CHECK(m.claims==1 && m.read_count==1);
        if(q->command==OKL_SET_WHITE_BRIGHTNESS) {CHECK(!q->arguments[2]);fail=wire('W',deadline);m.state.white_brightness=0;}
        else {CHECK(q->command==OKL_SET_EFFECT && !q->arguments[2]);fail=wire('E',deadline);m.state.effect=0;}
        ++m.writes;
    }
    if(fail){d->needs_recovery=1;return OKL_TIMEOUT;}
    save_reply(r);return q->command==OKL_GET_CONTROLLER_STATUS?(m.malformed==7?OKL_OK:OKL_REMOTE):OKL_OK;
}
static okl_result native_claim(okl_nxp *d,const uint8_t *name,size_t n,uint64_t deadline) {
    CHECK(d==&m.driver && n==13 && !memcmp(name,"Open Keylight",13) && !m.claims);++m.claims;
    if(wire('C',deadline)){d->needs_recovery=1;return OKL_TIMEOUT;}return OKL_OK;
}
static okl_result native_read(okl_nxp *d,okl_light_state *s,uint64_t deadline) {
    CHECK(d==&m.driver && m.claims==1);++m.read_count;*s=m.state;
    if(m.malformed==10 && m.read_count==2)s->white_brightness=1;
    if(m.malformed==11 && m.read_count==2)s->effect=1;
    if(wire('R',deadline)){d->needs_recovery=1;return OKL_TIMEOUT;}return OKL_OK;
}
static okl_result native_release(okl_nxp *d,uint64_t deadline) {
    CHECK(d==&m.driver && m.claims==1 && m.read_count==2 && !m.state.effect && !m.state.white_brightness);++m.owner_releases;
    if(wire('L',deadline)){d->needs_recovery=1;return OKL_TIMEOUT;}return OKL_OK;
}
/* Recovery must never invoke these mutating loader/lifecycle operations. */
okl_result app_nxp_loader_enter(okl_nxp *d,uint32_t id,okl_loader_source s,okl_loader_delivery *x,uint64_t t) {
    (void)d;(void)id;(void)s;(void)x;(void)t;CHECK(false);return OKL_IO;
}
okl_result app_nxp_loader_send_only(okl_nxp *d,uint32_t id,const uint8_t q[90],okl_loader_delivery *x,uint64_t t) {
    (void)d;(void)id;(void)q;(void)x;(void)t;CHECK(false);return OKL_IO;
}
okl_result app_nxp_loader_reset_boundary(okl_nxp *d,uint32_t id,uint8_t op,okl_loader_delivery x,uint64_t t) {
    (void)d;(void)id;(void)op;(void)x;(void)t;CHECK(false);return OKL_IO;
}
okl_result app_nxp_loader_use_resident(okl_nxp *d,uint32_t id,uint32_t proof,uint64_t t) {
    (void)d;(void)id;(void)proof;(void)t;CHECK(false);return OKL_IO;
}
int app_controller_update_persist(uint32_t id,const okl_loader_audit *a) {(void)id;(void)a;CHECK(false);return -1;}
void app_controller_update_progress(uint32_t id,const okl_loader_audit *a) {(void)id;(void)a;CHECK(false);}
static void reset(void) {
    ++cases;memset(&m,0,sizeof(m));m.job.id=19;m.job.recovery_only=true;m.now=1000000;
    m.state=(okl_light_state){.effect=8,.white_brightness=38,.temperature_kelvin=5200};
}
static void run(void) {
    app_controller_worker_recover(&m.driver,&m.job,mac,&m.out);
    CHECK(!m.leased && m.claims<=1 && m.owner_releases<=1 && m.preserves<=1);
    CHECK(!(m.out.resident_proof_job_id && m.out.legacy_reconciled));
    if(m.wires)CHECK(m.waits==3300 && m.now-m.initialized_at>=33000000 && m.sequence[0]=='I');
}
int main(void) {
    reset();run();CHECK(m.out.resident_proof_job_id==19 && !m.out.diagnostic_error[0] && m.out.synchronized);
    CHECK(m.wires==1 && !m.claims && !m.writes && !m.owner_releases && m.preserves==1 && m.releases==1);
    CHECK(!strcmp(m.out.stage,"recovery.resident") && m.out.transport_snapshot && m.out.raw_reply_received);
    CHECK(m.out.reply_kind==0 && m.out.reply_class==0x10 && m.out.reply_opcode==0x80 && m.out.reply_size==80);
    CHECK(strlen(m.out.reply_sha256)==64 && !strcmp(m.out.reply_sha256,"4242424242424242424242424242424242424242424242424242424242424242"));
    for(unsigned fault=0;fault<10;++fault) {
        reset();
        if(fault==0)m.init_fault=1;
        if(fault==1)m.lease_fault=1;
        if(fault==2)m.fail_at=1;
        if(fault==3)m.preserve_fault=1;
        if(fault==4)m.info_delivery=1;
        if(fault==5)m.info_delivery=2;
        if(fault==6)m.driver.transport.now_us=test_clock;
        if(fault==7)m.driver.transport.user=&m;
        if(fault==8)m.job.recovery_only=false;
        if(fault==9)m.info_fault=7;
        run();CHECK(!m.out.resident_proof_job_id && !m.out.legacy_reconciled && m.out.diagnostic_error[0]);
        CHECK(m.wires<=1 && !m.claims && !m.writes);
        if(fault>=6 && fault<=8)CHECK(!m.init_calls && !m.snapshots);
        if(fault==4 || fault==5)CHECK(!strcmp(m.out.stage,"recovery.information_delivery") && m.out.transport_result==OKL_PROTOCOL);
    }
    for(unsigned fault=1;fault<=8;++fault) {
        reset();m.info_fault=fault;run();CHECK(!m.out.resident_proof_job_id && !m.claims && m.out.diagnostic_error[0]);
        CHECK(!strcmp(m.out.stage,"recovery.classify") && m.out.transport_result==OKL_PROTOCOL && m.out.raw_reply_received);
    }
    for(unsigned status=0;status<256;++status) if(status!=2) {
        reset();m.info_kind=status?status:1;run();CHECK(!m.out.resident_proof_job_id && !m.out.legacy_reconciled && !m.claims);
    }
    for(unsigned status=5;status<=8;status+=3) {
        reset();m.info_kind=status;m.job.allow_legacy_reconcile=true;run();
        CHECK(m.out.legacy_reconciled && !m.out.diagnostic_error[0] && m.out.synchronized);
        CHECK(!strcmp(m.sequence,"IVCSPRWERL") && m.claims==1 && m.writes==2 && m.owner_releases==1 && !m.preserves);
        CHECK(m.out.entry==APP_CONTROLLER_MUTATION_ATTEMPTED && !strcmp(m.out.stage,"recovery.release"));
        for(unsigned fault=1;fault<=10;++fault) {
            reset();m.info_kind=status;m.job.allow_legacy_reconcile=true;m.fail_at=fault;run();
            CHECK(!m.out.legacy_reconciled && !m.out.resident_proof_job_id && m.out.diagnostic_error[0] && m.wires==fault);
            CHECK(!m.out.synchronized && m.out.transport_result==OKL_TIMEOUT);
        }
    }
    for(unsigned fault=1;fault<=8;++fault) {
        reset();m.info_kind=5;m.info_fault=fault;m.job.allow_legacy_reconcile=true;
        run();CHECK(m.wires==1 && !m.claims && !m.out.legacy_reconciled);
    }
    /* Exact live class10/80 unsupported reply retained all80 request zeros.
     * Its90-byte SHA256 is pinned independently in the Python runner. */
    for(unsigned byte=1;byte<=80;++byte) {
        reset();m.info_kind=5;m.info_payload_byte=byte;m.job.allow_legacy_reconcile=true;
        run();CHECK(m.wires==1 && !m.claims && !m.writes && !m.out.legacy_reconciled);
    }
    for(unsigned fault=1;fault<=11;++fault) {
        reset();m.info_kind=5;m.job.allow_legacy_reconcile=true;m.malformed=fault;run();
        CHECK(!m.out.legacy_reconciled && m.out.diagnostic_error[0] && !m.owner_releases && m.claims<=1);
        if(fault<=2)CHECK(!m.claims && !m.writes);
        if(fault<=9)CHECK(!m.writes);
    }
    reset();m.snapshot_fault=1;run();CHECK(m.out.resident_proof_job_id==19 && !m.out.transport_snapshot);
    reset();m.release_poison=1;run();CHECK(m.out.resident_proof_job_id==19 && !m.out.synchronized);
    printf("actual controller recovery adapter: %u checks across %u cases passed\n",checks,cases);return 0;
}
