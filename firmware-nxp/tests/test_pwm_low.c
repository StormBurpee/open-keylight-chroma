#define main retained_pwm_baseline
#include "test_pwm.c"
#undef main
#include "nxp_pwm_low_trial.h"
#include "controller_diagnostic.h"

static uint32_t record_word(const nxp_pwm_low_trial *t,unsigned i) {
    const volatile uint8_t *p=t->record+i*4u;
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static void low_write(void *user,uint32_t address,uint32_t value) {
    static const uint32_t match[5]={COLOR+0x24,COLOR+0x1c,COLOR+0x18,WHITE+0x1c,WHITE+0x18};
    model *m=user; unsigned i,active=0;
    write_register(user,address,value);
    /* At every exposed register write, no more than one channel has a
     * nonzero PWM comparator; all possible values are independently bounded. */
    for(i=0;i<5;++i) {
        unsigned pin=i==0?16:i==1?14:i==2?13:i==3?19:18;
        unsigned function=i<2? (i==0?2:3):i==2?3:2;
        uint32_t period=get(m,(i<3?COLOR:WHITE)+0x20),mr=get(m,match[i]);
        if((get(m,IOCON+pin*4u)&7u)==function && get(m,(i<3?COLOR:WHITE)+4)==1) {
            CHECK(period==(i<3?25499u:254u));
            CHECK(mr==(i<3?25500u:255u) || mr==(i<3?25245u:253u));
            if(mr<=period) { ++active; CHECK((period+1u-mr)*100u<=period+1u); }
        }
    }
    CHECK(active<=1);
}
static void setup(model *m,nxp_board *b,nxp_state *s,nxp_link *l,nxp_pwm_low_trial *t) {
    unsigned i; init(m,b); b->config.qualifications=0x393; b->io.write=low_write;
    nxp_state_init(s); s->part_id=0xbc40; CHECK(nxp_state_platform(s,1,1,0,0x13));
    s->claimed=1; for(i=0;i<6;++i)s->owner[i]=(uint8_t)(i+2);
    nxp_link_init(l,s); b->link=l; CHECK(nxp_pwm_low_init(t,b));
    CHECK(record_word(t,0)==0x4c4f5731 && record_word(t,10)==0x393 && record_word(t,11)==30000);
    CHECK(!nxp_board_start_pwm(b));
}
static void request(uint8_t out[97],nxp_state *s,uint8_t op,const uint8_t *args,unsigned n) {
    unsigned i; memset(out,0,97); memcpy(out,s->owner,6); out[12]=(uint8_t)n; out[14]=op;
    if(n)memcpy(out+15,args,n);
    for(i=9;i<95;++i)out[95]^=out[i];
}
static void arm(nxp_state *s,nxp_link *l,uint32_t now,int body) {
    uint8_t q[97],rx[97],zero[97]={0};
    request(q,s,0x71,(const uint8_t *)"LOW1",4);
    CHECK(nxp_link_transaction(l,q,97,rx,97,now)==NXP_OK && s->low_requested==1);
    CHECK(nxp_link_transaction(l,zero,2,rx,97,now+1)==NXP_OK && rx[1]==97);
    if(body) CHECK(nxp_link_transaction(l,zero,97,rx,97,now+2)==NXP_OK &&
        rx[7]==2 && rx[12]==4 && !memcmp(rx+15,"LOW1",4) && s->low_requested==2);
}
static void full_sequence(void) {
    model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_low_trial t;
    unsigned now,i,page,before; uint8_t q[97],rx[97],index; uint32_t words[256];
    setup(&m,&b,&s,&l,&t);
    for(i=0;i<256;++i)words[i]=record_word(&t,i);
    CHECK(app_low_diagnostic_initial(words));
    arm(&s,&l,1000,1); nxp_pwm_low_service(&t,&b,1003);
    CHECK(t.active && t.pulse_on && s.low_requested==3);
    CHECK(record_word(&t,2)==1 && record_word(&t,3)==1003 && record_word(&t,5)==7503);
    /* SysTick alone performs every later start and cutoff; main never runs. */
    for(now=1004;now<=7503;++now) {
        nxp_pwm_low_tick(&t,&b,now);
        CHECK(t.pulse_on==((now-1003)%1600<100 && now<7503));
        if(!t.pulse_on) { CHECK(!b.pwm_started); for(i=0;i<5;++i)CHECK(!pad(&m,i)); }
    }
    CHECK(!t.active && record_word(&t,2)==2 && record_word(&t,7)==1 && record_word(&t,9)==31);
    for(i=0;i<256;++i)words[i]=record_word(&t,i);
    CHECK(app_low_diagnostic_registers(words,1));
    CHECK(!app_diagnostic_profile(words)); /* OFF1 cannot accept LOW1. */
    for(i=0;i<5;++i) {
        unsigned p=56+i*40,j;
        CHECK(record_word(&t,p)==i && record_word(&t,p+1)==1003+i*1600);
        CHECK(record_word(&t,p+2)==1103+i*1600 && record_word(&t,p+3)==1103+i*1600);
        CHECK(record_word(&t,p+4)==1 && !record_word(&t,p+5) && record_word(&t,p+39)==1);
        for(j=0;j<5;++j) {
            CHECK(record_word(&t,p+16+j)==(i==j?(j<3?25245u:253u):(j<3?25500u:255u)));
            CHECK(record_word(&t,p+34+j)==(j<3?25500u:255u));
        }
        CHECK(record_word(&t,p+32)==2 && record_word(&t,p+33)==2);
    }
    before=m.writes;
    for(now=7504;now<=31000;now+=71) { nxp_pwm_low_tick(&t,&b,now); nxp_pwm_low_service(&t,&b,now); }
    CHECK(m.writes==before && nxp_trial_expired(&s,30000));
    for(page=0;page<16;++page) {
        index=(uint8_t)page; request(q,&s,0xf1,&index,1);
        CHECK(nxp_process_at(&s,q,97,rx,97,8000)==NXP_OK && rx[7]==2 && rx[12]==72);
        CHECK(!memcmp(rx+15,"LOW1",4) && rx[19]==page && rx[20]==16 && rx[21]==64 && !rx[22]);
        for(i=0;i<64;++i)CHECK(rx[23+i]==t.record[page*64+i]);
    }
    request(q,&s,0xf0,NULL,0); CHECK(nxp_process_at(&s,q,97,rx,97,8000)==NXP_OK);
    CHECK(!memcmp(rx+15,"LOW1\3\5\0\144",8));
    request(q,&s,0x71,(const uint8_t *)"LOW1",4); CHECK(nxp_process_at(&s,q,97,rx,97,8000)==NXP_OK && rx[7]!=2);
    request(q,&s,0xfd,(const uint8_t *)"OKLC",4); CHECK(nxp_process_at(&s,q,97,rx,97,8000)==NXP_OK && rx[7]!=2 && !s.trial_confirmed);
}
static void cancellation_and_gates(void) {
    unsigned kind;
    for(kind=0;kind<9;++kind) {
        model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_low_trial t; unsigned before;
        setup(&m,&b,&s,&l,&t); arm(&s,&l,10,kind!=0);
        if(!kind)CHECK(nxp_link_expire(&l,110)==NXP_EXPIRED && s.low_requested==3);
        if(kind==1)s.claimed=0;
        if(kind==2)s.boot_requested=1;
        nxp_pwm_low_service(&t,&b,kind==3?22000:13);
        if(kind<4)CHECK(!t.active && !b.pwm_started);
        else {
            CHECK(t.active);
            if(kind==4)s.owner[0]^=1;
            if(kind==5)++b.errors;
            if(kind==6)s.boot_requested=1;
            if(kind==7)nxp_pwm_low_tick(&t,&b,114); /* Missed100ms cutoff. */
            else if(kind==8) { nxp_pwm_low_tick(&t,&b,113); nxp_pwm_low_tick(&t,&b,1614); }
            else nxp_pwm_low_tick(&t,&b,14);
            CHECK(!t.active && !b.pwm_started && record_word(&t,2)==3);
        }
        before=m.writes; nxp_pwm_low_service(&t,&b,4000); nxp_pwm_low_tick(&t,&b,4000); CHECK(m.writes==before);
    }
    for(kind=0;kind<10;++kind) {
        model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_low_trial t;
        setup(&m,&b,&s,&l,&t); b.config.qualifications^=1u<<kind;
        CHECK(!nxp_pwm_low_init(&t,&b));
    }
}
static void dropped_writes(void) {
    static const uint32_t addresses[]={COLOR+0x24,COLOR+0x1c,COLOR+0x18,WHITE+0x1c,WHITE+0x18};
    unsigned channel;
    for(channel=0;channel<5;++channel) {
        model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_low_trial t; unsigned now;
        setup(&m,&b,&s,&l,&t); arm(&s,&l,10,1);
        m.drop_address=addresses[channel]; m.drop_value=channel<3?25245:253; m.drop_enabled=1;
        nxp_pwm_low_service(&t,&b,13);
        for(now=14;now<=6513 && t.active;++now)nxp_pwm_low_tick(&t,&b,now);
        CHECK(!t.active && record_word(&t,2)==3 && record_word(&t,7)==4 && !b.pwm_started);
    }
    /* Dropped GPIO handoff cannot stop a high PWM latch; fall back to all-off
     * comparators with timers running. The failure is permanent, not success. */
    {
        model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_low_trial t;
        setup(&m,&b,&s,&l,&t); arm(&s,&l,10,1); nxp_pwm_low_service(&t,&b,13);
        m.require_dark=0; m.timer_latch[2]=1; m.drop_enabled=m.drop_any=1;
        m.drop_address=IOCON+16*4; nxp_pwm_low_tick(&t,&b,113);
        CHECK(!t.active && record_word(&t,2)==3 && record_word(&t,7)==4);
        CHECK(get(&m,WHITE+4)==1 && get(&m,COLOR+4)==1 && get(&m,COLOR+0x24)==25500);
    }
}
static void strict_protocol_and_started_state(void) {
    model m; nxp_board b; nxp_state s; nxp_link l; nxp_pwm_low_trial t;
    unsigned i; uint8_t q[97],rx[97],arg[80]={0};
    setup(&m,&b,&s,&l,&t);
    for(i=0;i<256;++i) {
        arg[0]=(uint8_t)i; request(q,&s,0xf1,arg,1);
        CHECK(nxp_process_at(&s,q,97,rx,97,1)==NXP_OK && (rx[7]==2)==(i<16));
    }
    for(i=0;i<=80;++i) {
        memset(arg,0,sizeof(arg)); if(i>=4)memcpy(arg,"LOW1",4);
        request(q,&s,0x71,arg,i);
        if(i==4)q[0]^=2; /* Valid shape, wrong owner. */
        CHECK(nxp_process_at(&s,q,97,rx,97,1)==NXP_OK && rx[7]!=2 && !s.low_requested);
    }
    request(q,&s,0x70,(const uint8_t *)"OFF1",4);
    CHECK(nxp_process_at(&s,q,97,rx,97,1)==NXP_OK && rx[7]==5);
    request(q,&s,0x71,(const uint8_t *)"LOW1",4);
    CHECK(nxp_process_at(&s,q,97,rx,97,22000)==NXP_OK && rx[7]==3 && !s.low_requested);
    /* Stale SPI error cannot begin the new output experiment. */
    arm(&s,&l,10,1); b.errors=1; nxp_pwm_low_service(&t,&b,13);
    CHECK(!t.active && !b.pwm_started && record_word(&t,7)==3);
    for(i=0;i<5;++i) CHECK(!pad(&m,i));
    /* A recovery request or owner loss during a dark gap prevents every later
     * pulse, even with no main-loop service afterward. */
    for(i=0;i<2;++i) {
        unsigned before;
        setup(&m,&b,&s,&l,&t); arm(&s,&l,10,1); nxp_pwm_low_service(&t,&b,13);
        nxp_pwm_low_tick(&t,&b,113); CHECK(t.active && !t.pulse_on);
        if(i)s.boot_requested=1;else s.claimed=0;
        nxp_pwm_low_tick(&t,&b,114); CHECK(!t.active && record_word(&t,2)==3);
        before=m.writes; nxp_pwm_low_tick(&t,&b,1613); CHECK(m.writes==before);
    }
}
int main(void) {
    full_sequence(); cancellation_and_gates(); dropped_writes(); strict_protocol_and_started_state();
    printf("%u fixed LOW1 protocol/MMIO/timing/fault checks passed; no hardware I/O.\n",checks);
    return 0;
}
