/* Actual board callback integration with a cycle-level PWM latch model.
 * The manual defines output reset/match behavior; GPIO and CPU bus timing are
 * modeled. Access costs are varied test inputs, not target measurements. The negative
 * control demonstrates why register readback alone does not settle a latch.
 * This model does not reproduce or establish the cause of a hardware brownout. */
#include "nxp_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 0x40014000u
#define C 0x40018000u
#define G 0x50000000u
#define I 0x40044000u
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);} checks++; } while(0)
static unsigned checks;
typedef struct {
    uint32_t a[100],v[100],n,tc[2],high[2],prescale,cycle,gpio_latch;
    unsigned monitor,overlap,peak,stop_clock,access_ticks,monitor_kind;
    unsigned counter_reads, invalid_counter, frozen_mask;
} model;
static uint32_t get(model*m,uint32_t a){for(unsigned i=0;i<m->n;i++)if(m->a[i]==a)return m->v[i];return 0;}
static void set(model*m,uint32_t a,uint32_t v){for(unsigned i=0;i<m->n;i++)if(m->a[i]==a){m->v[i]=v;return;}CHECK(m->n<100);m->a[m->n]=a;m->v[m->n++]=v;}
static void tick(model*m){
    ++m->cycle;
    for(unsigned t=0;t<2;t++) {
        uint32_t base=t?C:W;if(get(m,base+4)!=1||m->stop_clock||(m->frozen_mask&(1u<<t)))continue;
        if(!t&&m->cycle%48)continue;
        uint32_t period=get(m,base+0x20);
        m->tc[t] = m->tc[t]>=period ? 0 : m->tc[t]+1;
        for(unsigned ch=0;ch<4;ch++)if(get(m,base+0x74)&(1u<<ch)){
            uint32_t mr=get(m,base+0x18+4*ch);
            if(!m->tc[t]){m->high[t]&=~(1u<<ch);if(!mr)m->high[t]|=1u<<ch;}
            else if(mr&&m->tc[t]==mr)m->high[t]|=1u<<ch;
        }
    }
    if(m->monitor){
        unsigned on=!!(m->high[1]&8)+!!(m->high[1]&2)+!!(m->high[1]&1);
        if(on>m->peak)m->peak=on;
        if(m->monitor_kind==1) {
            if((m->high[0]&3)==3)m->overlap++;
        } else if(m->monitor_kind==2) {
            if((m->high[0]&2)&&(m->high[1]&2))m->overlap++;
        } else if(m->monitor_kind==3) {
            if((m->high[0]&3)==3||(m->high[1]&10)==10)m->overlap++;
        } else if((m->high[1]&10)==10)m->overlap++;
    }
}
static void advance(model*m,unsigned count){while(count--)tick(m);}
static uint32_t read_reg(void*u,uint32_t a){model*m=u;advance(m,m->access_ticks);
    if(a==C+8){m->counter_reads++;return m->invalid_counter==2?25500:m->tc[1];}
    if(a==W+8){m->counter_reads++;return m->invalid_counter==1?255:m->tc[0];}
    if(a==G+0x2100)return m->gpio_latch;
    return get(m,a);}
static void write_reg(void*u,uint32_t a,uint32_t v){model*m=u;advance(m,m->access_ticks);set(m,a,v);
    if(a==G+0x2280)m->gpio_latch&=~v;
    if(a==G+0x2200)m->gpio_latch|=v;
    if(a==W+4&&v==2){m->tc[0]=m->high[0]=0;}
    if(a==C+4&&v==2){m->tc[1]=m->high[1]=0;}
}
static void init(model*m,nxp_board*b){memset(m,0,sizeof(*m));m->access_ticks=8;nxp_register_io io={m,read_reg,write_reg};
    nxp_board_config config={0xbc40,0xbc40,48000000,127,3};nxp_board_init(b,&io,&config,NULL);CHECK(nxp_board_start_pwm(b));}
static unsigned run(unsigned new_green,int naive){
    model m;nxp_board b;init(&m,&b);
    nxp_pwm_frame old={0,25500,25500,255,255},next={25500,(uint16_t)new_green,25500,255,255};
    CHECK(nxp_board_apply_pwm(&b,&old));
    /* Establish settled full red at a point before the future green match. */
    advance(&m,51000);while(m.tc[1]!=10000)tick(&m);
    CHECK((m.high[1]&10)==8);m.monitor=1;
    if(naive){ /* Negative control: immediate independent compare writes. */
        write_reg(&m,C+0x24,next.red_match);write_reg(&m,C+0x1c,next.green_match);
        write_reg(&m,C+0x18,next.blue_match);write_reg(&m,W+0x18,255);write_reg(&m,W+0x1c,255);
    }else CHECK(nxp_board_apply_pwm(&b,&next));
    advance(&m,51000);
    CHECK(get(&m,C+0x24)==25500&&get(&m,C+0x1c)==new_green);
    printf("{\"path\":\"%s\",\"new_green_compare\":%u,\"overlap_rgb_clock_cycles\":%u,\"overlap_us\":%.3f,\"peak_red_green_channels\":%u}",naive?"unsynchronized_write_control":"actual_original_board",new_green,m.overlap,m.overlap/48.0,m.peak);
    return m.overlap;
}
static void boundary_cases(void) {
    const unsigned phases[]={0,1,100,10000,12749,12750,20000,25450,25499};
    const unsigned targets[]={0,1,100,5000,12750,20000,25499};
    for(unsigned c=0;c<3;c++)for(unsigned p=0;p<9;p++)for(unsigned t=0;t<7;t++) {
        model m;nxp_board b;init(&m,&b);m.access_ticks=c==0?8:c==1?48:96;
        nxp_pwm_frame old={0,25500,25500,255,255},next={25500,(uint16_t)targets[t],25500,255,255};
        CHECK(nxp_board_apply_pwm(&b,&old));advance(&m,51000);
        while(m.tc[1]!=phases[p])tick(&m);
        m.monitor=1;CHECK(nxp_board_apply_pwm(&b,&next));advance(&m,51000);CHECK(!m.overlap);
    }
    /* Separate all-decrease then increase cannot bypass the pending latch. */
    {model m;nxp_board b;init(&m,&b);nxp_pwm_frame old={0,25500,25500,255,255},dark={25500,25500,25500,255,255},next={25500,12750,25500,255,255};
     CHECK(nxp_board_apply_pwm(&b,&old));advance(&m,51000);while(m.tc[1]!=10000)tick(&m);
     m.monitor=1;CHECK(nxp_board_apply_pwm(&b,&dark));CHECK(nxp_board_apply_pwm(&b,&next));advance(&m,51000);CHECK(!m.overlap);}
    /* Warm→cool and warm→RGB cross timer boundaries; non-selected latch must settle. */
    for(unsigned kind=1;kind<=2;kind++){
        model m;nxp_board b;init(&m,&b);nxp_pwm_frame old={25500,25500,25500,255,0};
        nxp_pwm_frame next={25500,kind==2?12750:25500,25500,kind==1?127:255,255};
        CHECK(nxp_board_apply_pwm(&b,&old));advance(&m,51000);while(m.tc[0]!=100)tick(&m);
        m.monitor=1;m.monitor_kind=kind;CHECK(nxp_board_apply_pwm(&b,&next));advance(&m,51000);CHECK(!m.overlap);
    }
    /* A stopped hardware counter cannot produce a false settling success. */
    {model m;nxp_board b;init(&m,&b);nxp_pwm_frame old={0,25500,25500,255,255},next={25500,12750,25500,255,255};
     CHECK(nxp_board_apply_pwm(&b,&old));advance(&m,51000);m.stop_clock=1;
     CHECK(!nxp_board_apply_pwm(&b,&next));CHECK(b.pwm_fault&&!b.pwm_started);CHECK(get(&m,C+0x1c)==25500);}
    /* Both timers may carry pending reductions. Neither may be skipped. */
    for(unsigned fault=0;fault<5;fault++) {
        model m;nxp_board b;init(&m,&b);
        nxp_pwm_frame old={0,25500,25500,255,217},next={25500,12750,25500,217,255};
        CHECK(nxp_board_apply_pwm(&b,&old));advance(&m,51000);
        while(m.tc[1]!=10000)tick(&m);
        m.monitor=1;m.monitor_kind=3;
        if(fault==1||fault==2)m.invalid_counter=fault;
        if(fault==3||fault==4)m.frozen_mask=1u<<(fault-3);
        int ok=nxp_board_apply_pwm(&b,&next);
        if(!fault){CHECK(ok);advance(&m,51000);CHECK(!m.overlap);}
        else{CHECK(!ok);CHECK(b.pwm_fault);CHECK(get(&m,C+0x1c)==25500);CHECK(get(&m,W+0x18)==255);}
    }
    /* Unchanged / monotonically increasing duty has no settling poll. */
    {model m;nxp_board b;init(&m,&b);nxp_pwm_frame a={12750,25500,25500,255,255},z={0,25500,25500,255,255};
     CHECK(nxp_board_apply_pwm(&b,&a));CHECK(m.counter_reads==0);
     CHECK(nxp_board_apply_pwm(&b,&a));CHECK(m.counter_reads==0);
     CHECK(nxp_board_apply_pwm(&b,&z));CHECK(m.counter_reads==0);
     CHECK(nxp_board_apply_pwm(&b,&a));CHECK(m.counter_reads==1);
     CHECK(nxp_board_apply_pwm(&b,&a));CHECK(m.counter_reads==1);}
}
int main(void) {
    unsigned original, naive, full;
    printf("{\"cases\":[");
    original = run(12750, 0); printf(",");
    naive = run(12750, 1); printf(",");
    full = run(0, 0);
    CHECK(naive > 0); CHECK(!full); CHECK(!original);
    boundary_cases();
    printf("],\"assertions\":%u,\"device_operations\":0}\n", checks);
    return 0;
}
