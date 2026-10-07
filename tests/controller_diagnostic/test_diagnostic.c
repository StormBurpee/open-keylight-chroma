#include "controller_diagnostic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static const uint32_t pins = (1u<<13)|(1u<<14)|(1u<<16)|(1u<<18)|(1u<<19);
static const uint32_t gpio[5] = {0x81,0x81,0x80,0,0}, pwm[5] = {0x83,0x83,0x82,2,2};
static void fixture(uint32_t w[256]) {
    memset(w,0,1024);
    w[0]=0x4c4f5731; w[1]=1; w[2]=2; w[3]=1000; w[4]=w[5]=7500;
    w[6]=w[7]=1; w[9]=31; w[10]=0x393; w[11]=30000; w[12]=77; w[13]=100; w[14]=1500; w[15]=5;
    uint32_t *b=w+16; b[0]=1000; b[2]=0x600; b[3]=pins;
    for(unsigned p=0;p<5;++p) b[5+p]=gpio[p];
    for(unsigned c=0;c<2;++c) {
        uint32_t *t=b+(c?24:10), period=c?25499:254;
        t[0]=1; t[2]=c?0:47; t[4]=0x80; t[5]=t[6]=t[8]=period+1; t[7]=period; t[12]=c?11:3;
    }
    for(unsigned c=0;c<5;++c) {
        uint32_t *s=w+56+c*40;
        s[0]=c; s[1]=1000+c*1600; s[2]=s[3]=s[1]+100; s[4]=1;
        s[6]=0x600; s[7]=s[25]=pins; s[14]=s[15]=1; s[21]=254; s[22]=25499;
        s[23]=3; s[24]=11; s[32]=s[33]=2; s[39]=1;
        for(unsigned p=0;p<5;++p) {
            s[9+p]=pwm[p]; s[27+p]=gpio[p]; s[34+p]=p<3?25500:255;
            s[16+p]=p==c?(p<3?25245:253):s[34+p];
        }
    }
}
static void test_registers(void) {
    uint32_t valid[256], changed[256], masks[256]={0}; fixture(valid);
    CHECK(app_low_diagnostic_registers(valid,77)); CHECK(!app_low_diagnostic_registers(valid,78));
    CHECK(!app_low_diagnostic_registers(NULL,77));
    for(unsigned i=0;i<16;++i) masks[i]=UINT32_MAX;
    for(unsigned i=0;i<40;++i) masks[16+i]=UINT32_MAX;
    masks[18]=0x600; masks[19]=masks[20]=pins;
    for(unsigned p=0;p<5;++p) masks[21+p]=p<3?0x87:7;
    for(unsigned c=0;c<2;++c) {
        unsigned offset=16+(c?24:10); masks[offset+1]=masks[offset+3]=masks[offset+13]=0;
    }
    const uint32_t channel_pin[5]={1u<<16,1u<<14,1u<<13,1u<<19,1u<<18};
    for(unsigned c=0;c<5;++c) {
        unsigned offset=56+c*40;
        for(unsigned i=0;i<40;++i) masks[offset+i]=UINT32_MAX;
        masks[offset+6]=0x600; masks[offset+7]=masks[offset+25]=masks[offset+26]=pins;
        masks[offset+8]=pins&~channel_pin[c];
        for(unsigned p=0;p<5;++p) masks[offset+9+p]=masks[offset+27+p]=p<3?0x87:7;
    }
    for(unsigned i=0;i<256;++i) for(unsigned bit=0;bit<32;++bit) {
        memcpy(changed,valid,sizeof(valid)); changed[i]^=UINT32_C(1)<<bit;
        if(masks[i]&(UINT32_C(1)<<bit)) CHECK(!app_low_diagnostic_registers(changed,77));
        else CHECK(app_low_diagnostic_registers(changed,77));
    }
    /* Selected output is phase-dependent; both sampled levels are valid. */
    for(unsigned c=0;c<5;++c) {
        memcpy(changed,valid,sizeof(valid)); changed[56+c*40+8]|=channel_pin[c];
        CHECK(app_low_diagnostic_registers(changed,77));
    }
    memcpy(changed,valid,sizeof(valid)); changed[2]=0;
    for(unsigned i=3;i<10;++i) changed[i]=0;
    CHECK(app_low_diagnostic_initial(changed));
    for(unsigned i=2;i<10;++i) { changed[i]=1; CHECK(!app_low_diagnostic_initial(changed)); changed[i]=0; }
    CHECK(!app_low_diagnostic_initial(NULL));
    for(unsigned i=0;i<5;++i) {
        memcpy(changed,valid,sizeof(valid)); changed[56+i*40+2]++; CHECK(!app_low_diagnostic_registers(changed,77));
    }
}
static void test_identity(void) {
    okl_firmware_version v={{0,1,0,0}};
    okl_controller_status good={.abi_major=1,.role=OKL_ROLE_SPI_DIAGNOSTIC,
        .capabilities=OKL_CAP_RECOVERY_READY,.part_id=0xbc40,.uptime_ms=19999};
    CHECK(app_low_diagnostic_identity(&good,&v));
    CHECK(!app_low_diagnostic_identity(NULL,&v)); CHECK(!app_low_diagnostic_identity(&good,NULL));
    for(unsigned at=0;at<4;++at) { v.component[at]^=1; CHECK(!app_low_diagnostic_identity(&good,&v)); v.component[at]^=1; }
    for(unsigned field=0;field<9;++field) {
        okl_controller_status s=good;
        if(field==0) s.abi_major=2;
        if(field==1) s.abi_minor=1;
        if(field==2) s.role=OKL_ROLE_LIGHTING;
        if(field==3) s.capabilities|=OKL_CAP_LIGHTING_READY;
        if(field==4) s.capabilities=0;
        if(field==5) s.part_id++;
        if(field==6) s.trial_confirmed=1;
        if(field==7) s.boot_requested=1;
        if(field==8) s.uptime_ms=20000;
        CHECK(!app_low_diagnostic_identity(&s,&v));
    }
}
static okl_reply reply(uint8_t op,uint8_t size) {
    okl_reply r={.received=1,.acknowledged=1}; r.report.status=2; r.report.opcode=op; r.report.size=size; return r;
}
static void test_codec(void) {
    okl_request q, before; memset(&before,0xa5,sizeof(before));
    CHECK(okl_request_get(&q,OKL_GET_LOW_DIAGNOSTIC_PROFILE)==OKL_OK && !q.size);
    CHECK(okl_request_diagnostic_low(&q)==OKL_OK && q.command==OKL_RUN_DIAGNOSTIC_LOW && q.size==4 && !memcmp(q.arguments,"LOW1",4));
    for(unsigned p=0;p<256;++p) {
        q=before; CHECK(okl_request_low_diagnostic_page(&q,(uint8_t)p)==(p<16?OKL_OK:OKL_INVALID));
        if(p<16) CHECK(q.size==1 && q.arguments[0]==p);
        else CHECK(!memcmp(&q,&before,sizeof(q)));
    }
    for(unsigned at=0;at<4;++at) for(unsigned bit=0;bit<8;++bit) {
        uint8_t a[4]={'L','O','W','1'}; a[at]^=(uint8_t)(1u<<bit); q=before;
        CHECK(okl_request_build(&q,OKL_RUN_DIAGNOSTIC_LOW,a,4)==OKL_INVALID && !memcmp(&q,&before,sizeof(q)));
    }
    okl_reply r=reply(0xf0,8); memcpy(r.report.arguments,"LOW1",4); r.report.arguments[5]=5; r.report.arguments[7]=100;
    okl_low_diagnostic_profile profile, sentinel; memset(&sentinel,0xa5,sizeof(sentinel));
    for(unsigned state=0;state<256;++state) {
        r.report.arguments[4]=(uint8_t)state; profile=sentinel;
        CHECK(okl_reply_decode_low_diagnostic_profile(&profile,&r)==(state<=3?OKL_OK:OKL_PROTOCOL));
        if(state<=3) CHECK(profile.requested==state && profile.duration_ms==100 && profile.channels==5);
        else CHECK(!memcmp(&profile,&sentinel,sizeof(profile)));
    }
    r.report.arguments[4]=0; okl_reply valid=r;
    for(unsigned at=0;at<8;++at) if(at!=4) for(unsigned bit=0;bit<8;++bit) {
        r=valid; r.report.arguments[at]^=(uint8_t)(1u<<bit); profile=sentinel;
        CHECK(okl_reply_decode_low_diagnostic_profile(&profile,&r)==OKL_PROTOCOL && !memcmp(&profile,&sentinel,sizeof(profile)));
    }
    okl_diagnostic_profile off; CHECK(okl_reply_decode_diagnostic_profile(&off,&valid)==OKL_PROTOCOL);
    uint8_t data[64], keep[64]; memset(keep,0xa5,64);
    for(uint8_t page=0;page<16;++page) {
        r=reply(0xf1,72); memcpy(r.report.arguments,"LOW1",4); r.report.arguments[4]=page;
        r.report.arguments[5]=16; r.report.arguments[6]=64;
        for(unsigned i=0;i<64;++i) r.report.arguments[8+i]=(uint8_t)(page*64+i);
        CHECK(okl_reply_decode_low_diagnostic_page(data,page,&r)==OKL_OK && !memcmp(data,r.report.arguments+8,64));
        valid=r;
        for(unsigned at=0;at<8;++at) for(unsigned bit=0;bit<8;++bit) {
            r=valid; r.report.arguments[at]^=(uint8_t)(1u<<bit); memcpy(data,keep,64);
            CHECK(okl_reply_decode_low_diagnostic_page(data,page,&r)==OKL_PROTOCOL && !memcmp(data,keep,64));
        }
        CHECK(okl_reply_decode_diagnostic_page(data,page,&valid)!=OKL_OK);
    }
    r=reply(0x71,4); memcpy(r.report.arguments,"LOW1",4);
    CHECK(okl_reply_check_diagnostic_low(&r)==OKL_OK && okl_reply_check_diagnostic_off(&r)==OKL_PROTOCOL);
    for(unsigned at=0;at<4;++at) for(unsigned bit=0;bit<8;++bit) {
        r.report.arguments[at]^=(uint8_t)(1u<<bit); CHECK(okl_reply_check_diagnostic_low(&r)==OKL_PROTOCOL); r.report.arguments[at]^=(uint8_t)(1u<<bit);
    }
    for(unsigned failure=0;failure<7;++failure) {
        r=reply(0x71,4); memcpy(r.report.arguments,"LOW1",4);
        if(failure==0) r.received=0;
        if(failure==1) r.acknowledged=0;
        if(failure==2) r.report.status=8;
        if(failure==3) r.report.command_class=1;
        if(failure==4) r.report.opcode=0x70;
        if(failure==5) r.report.size=3;
        if(failure==6) r.report.size=5;
        CHECK(okl_reply_check_diagnostic_low(&r)==OKL_PROTOCOL);
    }
}
int main(void) {
    test_registers(); test_identity(); test_codec();
    printf("diagnostic predicates and typed LOW1 codec: %u checks passed; no device I/O\n",checks); return 0;
}
