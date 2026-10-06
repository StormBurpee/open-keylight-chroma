#include "okl_nxp.h"
#include "okl_button.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static const uint8_t identity[6]={2,17,34,51,68,85};

typedef struct {
    uint64_t now, deadline;
    unsigned locked, locks, unlocks, calls, requests, setters, stage;
    unsigned fail_at, late_at, mutation, state_fault, owner_short;
    uint16_t response_size;
    uint8_t request[97], response[97], owner[6], claimed;
    uint8_t effect[12], effect_size, rgb, white;
    uint16_t temperature;
    okl_result recovery_result;
} mock;

static uint8_t xor_bytes(const uint8_t *report) {
    uint8_t result=0;size_t i;
    for(i=2;i<88;++i) result^=report[i];
    return result;
}
static void build_response(mock *m) {
    const uint8_t *q=m->request+7,*a=q+8;
    uint8_t *r=m->response+7,*p=r+8;
    memset(m->response,0,sizeof(m->response));memcpy(m->response,m->request,6);
    r[0]=2;r[1]=q[1];r[6]=q[6];r[7]=q[7];
    if(!(q[7]&0x80)) {++m->setters;m->response[6]=4;memset(m->response,0,6);}
    if(q[6]==0 && q[7]==0x87) {r[5]=4;p[0]=1;p[1]=3;}
    else if(q[6]==0 && q[7]==0x84) r[5]=1;
    else if(q[6]==0 && q[7]==0xc9) {r[5]=(uint8_t)(m->owner_short?7:8);p[0]=m->claimed;memcpy(p+1,m->owner,6);}
    else if(q[6]==0 && q[7]==0x49) {
        m->claimed=a[0];memset(m->owner,0,6);if(m->claimed)memcpy(m->owner,m->request,6);
        r[5]=72;memcpy(p,a,72);
    }
    else if(q[6]==15 && q[7]==0x82) {r[5]=m->effect_size;memcpy(p,m->effect,m->effect_size);}
    else if(q[6]==15 && q[7]==0x84) {r[5]=3;p[2]=m->rgb;}
    else if(q[6]==3 && q[7]==0x83) {r[5]=4;p[1]=32;p[2]=m->white;p[3]=m->effect[2];}
    else if(q[6]==3 && q[7]==0x81) {r[5]=5;p[1]=32;p[2]=(uint8_t)(m->temperature>>8);p[3]=(uint8_t)m->temperature;}
    else {r[5]=q[5];memcpy(p,a,q[5]);}
    if(m->state_fault==q[7])p[0]=1;
    r[88]=xor_bytes(r);
}
static uint64_t clock_now(void *user) {return ((mock *)user)->now;}
static okl_result lock_bus(void *user,uint64_t deadline) {
    mock *m=user;if(m->locked)return OKL_BUSY;
    CHECK(deadline>m->now);m->locked=1;++m->locks;m->deadline=deadline;return OKL_OK;
}
static void unlock_bus(void *user) {mock *m=user;CHECK(m->locked);m->locked=0;++m->unlocks;}
static okl_result step(mock *m,uint64_t deadline) {
    CHECK(m->locked);CHECK(deadline==m->deadline);++m->calls;
    if(m->fail_at==m->calls)return OKL_TIMEOUT;
    if(m->late_at==m->calls)m->now=deadline;
    return OKL_OK;
}
static okl_result arm(void *user,uint64_t deadline) {
    mock *m=user;okl_result result=step(m,deadline);if(result!=OKL_OK)return result;
    CHECK(m->stage==0);m->stage=1;return OKL_OK;
}
static okl_result ready(void *user,uint64_t deadline) {
    mock *m=user;okl_result result=step(m,deadline);if(result!=OKL_OK)return result;
    CHECK(m->stage==2);m->stage=3;return OKL_OK;
}
static okl_result transfer(void *user,const uint8_t *tx,uint8_t *rx,size_t size,uint64_t deadline) {
    mock *m=user;okl_result result=step(m,deadline);size_t i;
    CHECK(tx && rx);if(result!=OKL_OK)return result;
    if(m->stage==1) {
        CHECK(size==97);CHECK(!memcmp(tx,identity,6)&&tx[6]==0);
        CHECK(tx[7+88]==xor_bytes(tx+7));CHECK(tx[7+89]==0);
        memcpy(m->request,tx,97);++m->requests;build_response(m);m->stage=2;
    } else if(m->stage==3) {
        CHECK(size==2&&tx[0]==0&&tx[1]==0);
        rx[0]=(uint8_t)(m->response_size>>8);rx[1]=(uint8_t)m->response_size;m->stage=4;
    } else {
        CHECK(m->stage==4&&size==97);for(i=0;i<size;++i)CHECK(tx[i]==0);
        memcpy(rx,m->response,97);m->stage=0;
        switch(m->mutation) {
        case 1:rx[7+88]^=1;break;
        case 2:rx[7+1]^=1;break;
        case 3:rx[0]^=2;break;
        case 4:rx[6]=9;break;
        case 5:rx[7+6]^=1;rx[7+88]=xor_bytes(rx+7);break;
        case 6:rx[7+7]^=1;rx[7+88]=xor_bytes(rx+7);break;
        case 7:rx[7+0]=8;break;
        case 8:rx[7+0]=3;break;
        case 9:rx[7+5]=81;rx[7+88]=xor_bytes(rx+7);break;
        case 10:rx[7+4]=1;rx[7+88]=xor_bytes(rx+7);break;
        case 11:rx[7+89]=1;break;
        default:break;
        }
    }
    return OKL_OK;
}
static okl_result recover(void *user,uint64_t deadline) {
    mock *m=user;CHECK(m->locked&&deadline==m->deadline);
    if(m->recovery_result==OKL_OK)m->stage=0;
    return m->recovery_result;
}
static void setup(mock *m,okl_nxp *d) {
    okl_transport t;
    memset(m,0,sizeof(*m));m->response_size=97;m->rgb=255;m->temperature=5200;
    m->effect[2]=1;m->effect[5]=1;m->effect[8]=192;m->effect_size=9;
    memset(&t,0,sizeof(t));t.user=m;t.now_us=clock_now;t.lock=lock_bus;t.unlock=unlock_bus;
    t.arm_ready=arm;t.wait_ready=ready;t.transfer=transfer;t.recover=recover;
    CHECK(okl_nxp_init(d,&t,identity)==OKL_OK);
}

static void test_codec(void) {
    uint8_t report[90],args[80],copy[90];okl_report decoded;size_t n,i;
    memset(args,0xa5,sizeof(args));
    for(n=0;n<=80;++n) {
        CHECK(okl_report_encode(report,37,15,3,args,n)==OKL_OK);
        CHECK(report[0]==0&&report[1]==37&&report[5]==n&&report[6]==15&&report[7]==3);
        CHECK(report[88]==xor_bytes(report));CHECK(okl_report_decode(&decoded,report,90)==OKL_OK);
        CHECK(decoded.size==n&&!memcmp(decoded.arguments,args,n));
        for(i=8+n;i<88;++i)CHECK(report[i]==0);
    }
    CHECK(okl_report_encode(report,0,0,0x87,NULL,0)==OKL_OK&&report[88]==0x87);
    {const uint8_t temp[]={0,32};CHECK(okl_report_encode(report,0,3,0x81,temp,2)==OKL_OK&&report[88]==0xa0);}
    memcpy(copy,report,90);CHECK(okl_report_encode(report,0,0,0,args,81)==OKL_INVALID&&!memcmp(copy,report,90));
    CHECK(okl_report_encode(NULL,0,0,0,NULL,0)==OKL_INVALID);
    CHECK(okl_report_encode(report,0,0,0,NULL,1)==OKL_INVALID);
    for(n=0;n<90;++n)CHECK(okl_report_decode(&decoded,report,n)==OKL_PROTOCOL);
    for(i=2;i<90;++i) {memcpy(report,copy,90);report[i]^=1;CHECK(okl_report_decode(&decoded,report,90)==OKL_PROTOCOL);}
    CHECK(okl_report_decode(&decoded,copy,90)==OKL_OK);
}
static void test_requests(void) {
    okl_request request,original;uint8_t rgb[3]={1,2,3};unsigned i;
    for(i=0;i<=OKL_GET_TEMPERATURE;++i)CHECK(okl_request_get(&request,(okl_command)i)==OKL_OK);
    CHECK(okl_request_get(&request,OKL_SET_OWNER)==OKL_INVALID);
    CHECK(okl_request_get(&request,(okl_command)-1)==OKL_INVALID);
    CHECK(okl_request_static(&request,rgb)==OKL_OK&&request.arguments[0]==0&&request.arguments[2]==1);
    CHECK(!memcmp(request.arguments+6,rgb,3));CHECK(okl_request_custom(&request)==OKL_OK&&request.arguments[2]==8);
    CHECK(okl_request_frame(&request,rgb)==OKL_OK&&request.size==9&&!memcmp(request.arguments+5,rgb,3)&&!request.arguments[8]);
    for(i=0;i<256;++i)CHECK(okl_request_color_brightness(&request,(uint8_t)i)==OKL_OK);
    CHECK(okl_request_white_brightness(&request,255,0)==OKL_OK);
    CHECK(okl_request_white_brightness(&request,38,8)==OKL_OK);
    CHECK(okl_request_white_brightness(&request,39,8)==OKL_INVALID);
    for(i=0;i<=65535;++i)CHECK(okl_request_temperature(&request,(uint16_t)i)==(i>=3000&&i<=7000?OKL_OK:OKL_INVALID));
    CHECK(okl_request_static(&request,rgb)==OKL_OK);original=request;request.arguments[0]=1;
    CHECK(okl_request_build(&original,request.command,request.arguments,request.size)==OKL_INVALID);
    CHECK(original.arguments[0]==0); /* Persistent profile is never accepted. */
}
static void test_exchange(void) {
    mock m;okl_nxp d;okl_reply reply;okl_request request;unsigned i;
    okl_firmware_version version;uint8_t mode;
    setup(&m,&d);CHECK(okl_nxp_default_deadline(&d)==150000);
    CHECK(okl_request_get(&request,OKL_GET_FIRMWARE)==OKL_OK);
    CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_OK);
    CHECK(reply.sent&&reply.received&&reply.acknowledged&&reply.report.arguments[0]==1);
    CHECK(okl_reply_decode_firmware(&version,&reply)==OKL_OK&&version.component[0]==1&&version.component[1]==3);
    mode=123;CHECK(okl_reply_decode_mode(&mode,&reply)==OKL_PROTOCOL&&mode==123);
    reply.acknowledged=0;memset(&version,0xa5,sizeof(version));
    CHECK(okl_reply_decode_firmware(&version,&reply)==OKL_PROTOCOL&&version.component[0]==0xa5);
    reply.acknowledged=1;reply.report.opcode=0x84;reply.report.size=1;reply.report.arguments[0]=0;
    CHECK(okl_reply_decode_mode(&mode,&reply)==OKL_OK&&mode==0);
    CHECK(m.requests==1&&m.calls==5&&m.locks==1&&m.unlocks==1&&!d.needs_recovery);
    for(i=0;i<300;++i)CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_OK);
    CHECK(d.transaction==(uint8_t)301); /* Defined byte-counter wrap. */
    m.locked=1;CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_BUSY);m.locked=0;
    CHECK(!reply.sent&&!reply.received);
    m.now=150000;CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_TIMEOUT&&!reply.sent);
    m.now=UINT64_MAX-1;CHECK(okl_nxp_default_deadline(&d)==UINT64_MAX);
    for(i=1;i<=5;++i) {
        setup(&m,&d);m.fail_at=i;
        CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_TIMEOUT);
        CHECK(d.needs_recovery&&m.locks==m.unlocks);
        CHECK(reply.sent==(i!=1));
        CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_NEEDS_RECOVERY&&!reply.sent);
        m.recovery_result=OKL_TIMEOUT;CHECK(okl_nxp_recover(&d,150000)==OKL_TIMEOUT&&d.needs_recovery);
        m.recovery_result=OKL_OK;CHECK(okl_nxp_recover(&d,150000)==OKL_OK&&!d.needs_recovery);
        m.fail_at=0;CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_OK);
    }
    for(i=1;i<=5;++i) {
        setup(&m,&d);m.late_at=i;
        CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_TIMEOUT&&!reply.acknowledged);
        CHECK(m.locks==m.unlocks);CHECK(d.needs_recovery);
        CHECK(okl_nxp_recover(&d,300000)==OKL_OK&&!d.needs_recovery);
    }
    for(i=0;i<65536;++i) {
        if(i==97)continue;
        setup(&m,&d);m.response_size=(uint16_t)i;
        CHECK(okl_nxp_execute(&d,&request,&reply,150000)==OKL_PROTOCOL);
        CHECK(m.calls==4&&d.needs_recovery&&!reply.received); /* Never oversized body transfer. */
    }
    for(i=1;i<=11;++i) {
        setup(&m,&d);m.mutation=i;
        CHECK(okl_nxp_execute(&d,&request,&reply,150000)==(i==7?OKL_OWNER_DENIED:i==8?OKL_REMOTE:OKL_PROTOCOL));
        CHECK(d.needs_recovery==(i!=7&&i!=8));CHECK(!reply.acknowledged&&m.locks==m.unlocks);
    }
}
static void test_owner_and_state(void) {
    mock m;okl_nxp d;okl_owner owner;okl_light_state state,sentinel;unsigned i;
    setup(&m,&d);CHECK(okl_nxp_get_owner(&d,&owner,150000)==OKL_OK&&!owner.claimed);
    CHECK(okl_nxp_claim(&d,(const uint8_t *)"Open Keylight",13,150000)==OKL_OK);
    CHECK(m.claimed&&!memcmp(m.owner,identity,6)&&m.setters==1);
    CHECK(okl_nxp_release(&d,150000)==OKL_OK&&!m.claimed&&m.setters==2);
    CHECK(okl_nxp_release(&d,150000)==OKL_OK&&m.setters==2);
    m.claimed=1;memset(m.owner,0xa4,6);
    CHECK(okl_nxp_release(&d,150000)==OKL_NOT_OWNER&&m.setters==2&&m.owner[0]==0xa4);
    CHECK(m.locks==m.unlocks);
    setup(&m,&d);m.owner_short=1;
    CHECK(okl_nxp_get_owner(&d,&owner,150000)==OKL_OK&&!owner.claimed&&!owner.name_size);
    CHECK(okl_nxp_release(&d,150000)==OKL_OK&&m.setters==0);
    m.owner[0]=2;CHECK(okl_nxp_get_owner(&d,&owner,150000)==OKL_PROTOCOL);
    m.claimed=1;CHECK(okl_nxp_get_owner(&d,&owner,150000)==OKL_PROTOCOL);
    setup(&m,&d);CHECK(okl_nxp_read_state(&d,&state,150000)==OKL_OK);
    CHECK(state.effect==1&&state.color_count==1&&state.colors[2]==192);
    CHECK(state.color_brightness==255&&!state.white_brightness&&state.temperature_kelvin==5200);
    CHECK(m.locks==1&&m.unlocks==1&&m.requests==4);
    memset(&sentinel,0xa5,sizeof(sentinel));
    for(i=0;i<4;++i) {
        const unsigned ops[]={0x82,0x83,0x84,0x81};setup(&m,&d);state=sentinel;m.state_fault=ops[i];
        CHECK(okl_nxp_read_state(&d,&state,150000)!=OKL_OK);CHECK(!memcmp(&state,&sentinel,sizeof(state)));
        CHECK(m.requests==i+1&&m.locks==m.unlocks);
    }
    setup(&m,&d);m.effect[2]=8;m.effect[5]=0;m.effect_size=6;
    CHECK(okl_nxp_read_state(&d,&state,150000)==OKL_OK&&state.effect==8&&state.color_count==0);
    setup(&m,&d);m.effect[5]=3;
    CHECK(okl_nxp_read_state(&d,&state,150000)==OKL_PROTOCOL);
    setup(&m,&d);m.temperature=2999;
    CHECK(okl_nxp_read_state(&d,&state,150000)==OKL_PROTOCOL);
}

static unsigned sample(okl_button *b,int pressed,uint64_t now) {
    unsigned events=999;CHECK(okl_button_sample(b,pressed,now,&events)==0);return events;
}
static void test_button(void) {
    okl_button b,previous;unsigned events=999;
    okl_button_init(&b,0,0);
    CHECK(!sample(&b,1,10));CHECK(!sample(&b,0,20));CHECK(!sample(&b,1,25));
    CHECK(!sample(&b,1,54));CHECK(!sample(&b,1,55)); /* Debounced press. */
    CHECK(!sample(&b,0,100));CHECK(!sample(&b,0,130));CHECK(!sample(&b,0,479));
    CHECK(sample(&b,0,480)==OKL_BUTTON_SINGLE);CHECK(!sample(&b,0,1000));
    okl_button_init(&b,0,0);sample(&b,1,10);sample(&b,1,40);sample(&b,0,80);sample(&b,0,110);
    sample(&b,1,200);sample(&b,1,230);sample(&b,0,260);CHECK(sample(&b,0,290)==OKL_BUTTON_DOUBLE);
    CHECK(!sample(&b,0,1000));
    okl_button_init(&b,0,0);sample(&b,1,10);sample(&b,1,40);
    CHECK(!sample(&b,1,3039));CHECK(sample(&b,1,3040)==OKL_BUTTON_SETUP);
    CHECK(!sample(&b,1,10000));sample(&b,0,10010);CHECK(!sample(&b,0,10040));CHECK(!sample(&b,0,10400));
    okl_button_init(&b,1,0);CHECK(!sample(&b,1,10000));sample(&b,0,10010);CHECK(!sample(&b,0,10040));
    sample(&b,1,10100);sample(&b,1,10130);CHECK(sample(&b,1,13130)==OKL_BUTTON_SETUP);
    previous=b;CHECK(okl_button_sample(&b,1,10,&events)==-1&&events==999&&!memcmp(&b,&previous,sizeof(b)));
    CHECK(okl_button_sample(&b,2,14000,&events)==-1&&events==999);
    /* Release observed before the hold threshold suppresses hold, even
     * while release debounce is still pending. */
    okl_button_init(&b,0,0);sample(&b,1,10);sample(&b,1,40);
    CHECK(!sample(&b,0,3035));CHECK(!sample(&b,0,3040));CHECK(!sample(&b,0,3065));
    CHECK(sample(&b,0,3415)==OKL_BUTTON_SINGLE);
}

int main(void) {
    test_codec();test_requests();test_exchange();test_owner_and_state();test_button();
    printf("%u checks passed; original C99 codec/driver/gesture tests; no hardware I/O.\n",checks);
    return 0;
}
