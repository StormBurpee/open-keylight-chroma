#include "worker_mocks.h"
#include "controller_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static okl_result mock_execute(okl_nxp *,const okl_request *,okl_reply *,uint64_t);
static okl_result mock_claim(okl_nxp *,const uint8_t *,size_t,uint64_t);
static okl_result mock_read(okl_nxp *,okl_light_state *,uint64_t);
static okl_loader_result mock_run(const okl_loader_image *,okl_loader_source,const okl_loader_ops *,uint64_t,okl_loader_audit *);
#define okl_nxp_execute mock_execute
#define okl_nxp_claim mock_claim
#define okl_nxp_read_state mock_read
#define okl_loader_run mock_run
#include "../../firmware/main/controller_worker.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_read_state
#undef okl_loader_run

static unsigned checks, queries, claims, reads, writes, entries, boundaries, waits, leases, ended, persists, progresses;
static unsigned fail_query, fail_write, role, caps, part, wrong_version, non_dark, status_changed;
static bool is_original, persisted;
static uint64_t current_us, entry_us;
static okl_result entry_result;
static okl_loader_delivery entry_delivery;
static okl_light_state native;
static okl_nxp test_driver;
static app_controller_job test_job;
static update_context context;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr,"FAIL controller worker %d: %s\n",__LINE__,#value);exit(1); } } while (0)
static uint64_t test_clock(void *unused) { (void)unused;return current_us; }
esp_err_t app_nxp_transport_init(okl_nxp *driver,const uint8_t identity[6]) {
    (void)driver;(void)identity;CHECK(false);return ESP_ERR_NO_MEM;
}
okl_result app_nxp_transport_snapshot(okl_nxp *driver,app_nxp_transport_diagnostic *out,uint64_t deadline) {
    CHECK(driver==&test_driver && deadline>current_us);memset(out,0,sizeof(*out));return OKL_OK;
}
int mbedtls_sha256(const unsigned char *data,size_t size,unsigned char out[32],int is224) {
    CHECK(data && size && !is224);memset(out,0x42,32);return 0;
}
void vTaskDelay(unsigned ticks) { CHECK(ticks && ticks<=10);++waits;current_us+=(uint64_t)ticks*1000; }
static void bounded_call(okl_nxp *driver,uint64_t deadline,uint64_t bound) {
    CHECK(driver==&test_driver && deadline>current_us && deadline<=current_us+bound);current_us+=100;
}
static void word(uint8_t *p,uint32_t n) { p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n; }
static okl_result mock_execute(okl_nxp *driver,const okl_request *request,okl_reply *reply,uint64_t deadline) {
    bounded_call(driver,deadline,150000);memset(reply,0,sizeof(*reply));
    reply->received=reply->acknowledged=1;reply->report.status=2;
    uint8_t *args=reply->report.arguments;
    if (request->command==OKL_GET_FIRMWARE) {
        ++queries;reply->report.opcode=0x87;reply->report.size=4;
        args[0]=is_original?0:1;args[1]=is_original?1:3;if(wrong_version)++args[1];
    } else if (request->command==OKL_GET_CONTROLLER_STATUS) {
        ++queries;reply->report.opcode=0xfc;
        if(!is_original) { reply->acknowledged=0;reply->report.status=5;return OKL_REMOTE; }
        reply->report.size=24;memcpy(args,"OKLC",4);args[4]=1;args[6]=(uint8_t)role;
        args[7]=status_changed && writes ? 2 : 0;word(args+8,caps);word(args+12,part);word(args+16,12);
    } else if(request->command==OKL_GET_PART_ID) {
        ++queries;reply->report.opcode=0xfe;reply->report.size=4;word(args,part);
    } else {
        CHECK(persisted && claims==1);++writes;
        if(writes==fail_write)return OKL_TIMEOUT;
        if(request->command==OKL_SET_WHITE_BRIGHTNESS) { CHECK(request->arguments[2]==0);native.white_brightness=0; }
        else if(request->command==OKL_SET_EFFECT) { CHECK(request->arguments[2]==0);native.effect=0; }
        else CHECK(false); /* No temperature, RGB, FD, frame or restore writes. */
        return OKL_OK;
    }
    return queries==fail_query ? OKL_TIMEOUT : OKL_OK;
}
static okl_result mock_claim(okl_nxp *driver,const uint8_t *name,size_t size,uint64_t deadline) {
    bounded_call(driver,deadline,600000);CHECK(size==13 && !memcmp(name,"Open Keylight",13));++claims;return OKL_OK;
}
static okl_result mock_read(okl_nxp *driver,okl_light_state *state,uint64_t deadline) {
    bounded_call(driver,deadline,800000);++reads;*state=native;if(non_dark && writes)state->white_brightness=1;return OKL_OK;
}
okl_result app_nxp_loader_acquire(okl_nxp *driver,uint32_t id,uint64_t deadline) {
    bounded_call(driver,deadline,120000000);CHECK(id==11 && !leases);++leases;return OKL_OK;
}
void app_nxp_loader_release(okl_nxp *driver,uint32_t id) { CHECK(driver==&test_driver && id==11 && leases==1);++ended;--leases; }
okl_result app_nxp_loader_preserve_resident(okl_nxp *d,uint32_t id,uint64_t deadline) {
    (void)d;(void)id;(void)deadline;CHECK(false);return OKL_IO;
}
okl_result app_nxp_loader_use_resident(okl_nxp *d,uint32_t id,uint32_t proof,uint64_t deadline) {
    (void)d;(void)id;(void)proof;(void)deadline;CHECK(false);return OKL_IO;
}
okl_result app_nxp_loader_enter(okl_nxp *driver,uint32_t id,okl_loader_source source,okl_loader_delivery *delivery,uint64_t deadline) {
    bounded_call(driver,deadline,2000000);CHECK(id==11 && source==test_job.source && !native.effect && !native.white_brightness);
    ++entries;entry_us=current_us;*delivery=entry_delivery;return entry_result;
}
okl_result app_nxp_loader_reset_boundary(okl_nxp *driver,uint32_t id,uint8_t opcode,okl_loader_delivery delivery,uint64_t deadline) {
    CHECK(driver==&test_driver && id==11 && opcode==0x84 && delivery==OKL_LOADER_SENT_COMPLETE);
    CHECK(current_us-entry_us>=OKL_LOADER_QUIET_US && deadline>current_us);++boundaries;return OKL_OK;
}
okl_result app_nxp_loader_exchange(okl_nxp *d,uint32_t id,const uint8_t q[90],uint8_t r[90],okl_loader_delivery *sent,uint64_t deadline) {
    (void)d;(void)id;(void)q;(void)r;(void)sent;(void)deadline;CHECK(false);return OKL_IO;
}
okl_result app_nxp_loader_send_only(okl_nxp *d,uint32_t id,const uint8_t q[90],okl_loader_delivery *sent,uint64_t deadline) {
    (void)d;(void)id;(void)q;(void)sent;(void)deadline;CHECK(false);return OKL_IO;
}
int app_controller_update_persist(uint32_t id,const okl_loader_audit *a) { CHECK(id==11 && a);persisted=true;++persists;return 0; }
void app_controller_update_progress(uint32_t id,const okl_loader_audit *a) { CHECK(id==11 && a);++progresses; }
static okl_loader_result mock_run(const okl_loader_image *image,okl_loader_source source,const okl_loader_ops *ops,uint64_t deadline,okl_loader_audit *audit) {
    CHECK(image==&test_job.image && source==test_job.source && deadline==current_us+120000000);
    CHECK(ops->begin(ops->user,deadline)==0 && !ops->cancelled(ops->user));
    CHECK(ops->persist(ops->user,audit)==0);ops->progress(ops->user,audit);
    CHECK(ops->enter_loader(ops->user,source,deadline)==0);ops->end(ops->user);
    return OKL_LOADER_OK;
}
static void reset(bool original) {
    queries=claims=reads=writes=entries=boundaries=waits=leases=ended=persists=progresses=0;
    fail_query=fail_write=wrong_version=non_dark=status_changed=0;role=2;caps=3;part=0xbc40;
    persisted=false;is_original=original;current_us=entry_us=0;
    entry_result=OKL_OK;entry_delivery=OKL_LOADER_SENT_COMPLETE;
    memset(&test_driver,0,sizeof(test_driver));test_driver.transport.now_us=test_clock;
    memset(&test_job,0,sizeof(test_job));test_job.id=11;test_job.source=original?OKL_LOADER_FROM_ORIGINAL:OKL_LOADER_FROM_LEGACY_1_3;
    context=(update_context){.driver=&test_driver,.job=&test_job,.deadline=120000000};
    native=(okl_light_state){.effect=8,.color_brightness=255,.white_brightness=51,.temperature_kelvin=4700};
}
int main(void) {
    for(unsigned original=0;original<2;++original) {
        reset(original!=0);okl_loader_audit audit={0};
        app_controller_worker_outcome outcome;
        CHECK(app_controller_worker_run(&test_driver,&test_job,&audit,&outcome)==OKL_LOADER_OK);
        CHECK(outcome.entry==APP_CONTROLLER_MUTATION_ATTEMPTED && outcome.synchronized);
        CHECK(writes==2 && claims==1 && reads==2 && entries==1 && boundaries==1 && waits==300 && ended==1 && !leases);
        CHECK(persists==1 && progresses==1 && native.temperature_kelvin==4700);
    }
    for(unsigned fault=0;fault<8;++fault) {
        reset(fault!=0);persisted=true;
        if(fault==0)part^=1;
        if(fault==1)role=1;
        if(fault==2)caps=1;
        if(fault==3)part^=1;
        if(fault==4)fail_query=1;
        if(fault==5)fail_query=2;
        if(fault==6)fail_query=3;
        if(fault==7)test_job.source=OKL_LOADER_FROM_FRESH_RESIDENT;
        CHECK(enter(&context,test_job.source,120000000)!=0 && !claims && !writes && !entries);
    }
    for(unsigned fault=0;fault<6;++fault) {
        reset(true);persisted=true;
        if(fault<2)fail_write=fault+1;
        if(fault==2)non_dark=1;
        if(fault==3)status_changed=1;
        if(fault==4)entry_result=OKL_TIMEOUT;
        if(fault==5)entry_delivery=OKL_LOADER_MAYBE_SENT;
        CHECK(enter(&context,test_job.source,120000000)!=0 && !boundaries);
        if(fault>=4)CHECK(entries==1 && waits==300 && current_us-entry_us>=OKL_LOADER_QUIET_US);
        else CHECK(!entries && !waits);
        CHECK(writes<=2);
    }
    reset(true);native.effect=0;native.white_brightness=0;okl_loader_observation observation={0};
    CHECK(observe(&context,&observation,10000000)==0 && observation.dark_state_verified && observation.controller.part_id==0xbc40);
    CHECK(!writes && !claims && !entries && reads==1 && queries==3);
    reset(true);CHECK(observe(&context,&observation,10000000)==0 && !observation.dark_state_verified && !writes && !claims);
    reset(false);CHECK(observe(&context,&observation,10000000)!=0 && !writes && !claims);
    printf("%u actual controller-worker adapter assertions passed\n",checks);return 0;
}
