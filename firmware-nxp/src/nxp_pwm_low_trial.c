#include "nxp_pwm_low_trial.h"
#define SYSCON UINT32_C(0x40048000)
#define GPIO UINT32_C(0x50000000)
#define IOCON UINT32_C(0x40044000)
#define WHITE UINT32_C(0x40014000)
#define COLOR UINT32_C(0x40018000)
#define COUNTER_ADDRESS UINT32_C(0x20004000)
#define COUNTER_MAGIC UINT32_C(0x31574f4c)
#define OUTPUT_PINS ((1u<<13)|(1u<<14)|(1u<<16)|(1u<<18)|(1u<<19))
#define PROFILE_FLAGS (NXP_QUAL_PART | NXP_QUAL_CLOCK | NXP_QUAL_RECOVERY | NXP_ALLOW_SPI_TRIAL | NXP_ALLOW_PWM_OFF_TRIAL | NXP_ALLOW_PWM_LOW_TRIAL)
enum { M_MAGIC, M_ABI, M_PHASE, M_START, M_END, M_DEADLINE, M_ACCEPTED, M_REASON,
       M_ERRORS, M_COMPLETED, M_FLAGS, M_RECOVERY, M_GENERATION, M_PULSE, M_GAP, M_CHANNELS };
static const uint8_t pins[5] = {13,14,16,18,19};
/* Report and experiment order: red, green, blue, warm, cool. */
static const uint32_t matches[5] = {COLOR+0x24,COLOR+0x1c,COLOR+0x18,WHITE+0x1c,WHITE+0x18};
static uint32_t rd(nxp_board *b, uint32_t a) { return b->io.read(b->io.user,a); }
static void wr(nxp_board *b, uint32_t a, uint32_t v) { b->io.write(b->io.user,a,v); }
static void word(nxp_pwm_low_trial *t, unsigned i, uint32_t v) {
    volatile uint8_t *p=t->record+i*4u;
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t value(const nxp_pwm_low_trial *t, unsigned i) {
    const volatile uint8_t *p=t->record+i*4u;
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static uint32_t off(unsigned channel) { return channel<3 ? 25500u : 255u; }
static uint32_t low(unsigned channel) { return channel<3 ? 25245u : 253u; }
static int permitted(const nxp_board *b) {
    return b && b->io.read && b->io.write && b->link && b->link->state &&
        b->config.observed_part==0xbc40u && b->config.approved_part==0xbc40u &&
        b->config.clock_hz==48000000u && b->config.qualifications==PROFILE_FLAGS;
}
static int owner_matches(const nxp_state *s) {
    unsigned i; if (!s->claimed) return 0;
    for(i=0;i<6;++i) if(s->owner[i]!=s->low_owner[i]) return 0;
    return 1;
}
static int dark_matches(nxp_board *b) {
    unsigned i; int valid=1;
    for(i=0;i<5;++i) wr(b,matches[i],off(i));
    for(i=0;i<5;++i) if(rd(b,matches[i])!=off(i)) valid=0;
    return valid;
}
static int pulse_valid(nxp_pwm_low_trial *t,nxp_board *b) {
    unsigned i;
    if(rd(b,WHITE+4)!=1 || rd(b,COLOR+4)!=1 || rd(b,WHITE+0x20)!=254 ||
       rd(b,COLOR+0x20)!=25499 || rd(b,WHITE+0xc)!=47 || rd(b,COLOR+0xc)!=0 ||
       rd(b,WHITE+0x74)!=3 || rd(b,COLOR+0x74)!=11 ||
       rd(b,WHITE+0x14)!=0x80 || rd(b,COLOR+0x14)!=0x80 ||
       rd(b,WHITE+0x70) || rd(b,COLOR+0x70) || rd(b,WHITE+0x28) || rd(b,COLOR+0x28)) return 0;
    for(i=0;i<5;++i) if(rd(b,matches[i])!=(i==t->channel?low(i):off(i))) return 0;
    for(i=0;i<5;++i) if((rd(b,IOCON+pins[i]*4u)&(i<3?0x87u:7u))!=(i<2?0x83u:i==2?0x82u:2u)) return 0;
    return (rd(b,SYSCON+0x80)&((1u<<9)|(1u<<10)))==((1u<<9)|(1u<<10));
}
static void baseline(nxp_pwm_low_trial *t,nxp_board *b,uint32_t now) {
    static const uint8_t offsets[14]={4,8,12,16,20,24,28,32,36,40,60,112,116,0};
    unsigned i;
    word(t,16,now); word(t,17,0); word(t,18,rd(b,SYSCON+0x80));
    word(t,19,rd(b,GPIO+0x2000)); word(t,20,rd(b,GPIO+0x2100));
    for(i=0;i<5;++i) word(t,21+i,rd(b,IOCON+pins[i]*4u));
    for(i=0;i<14;++i) { word(t,26+i,rd(b,WHITE+offsets[i])); word(t,40+i,rd(b,COLOR+offsets[i])); }
    word(t,54,0); word(t,55,b->errors);
}
static void snapshot_active(nxp_pwm_low_trial *t,nxp_board *b,uint32_t now) {
    unsigned i,p=56u+t->channel*40u;
    word(t,p,t->channel); word(t,p+1,now); word(t,p+3,now+NXP_LOW_PULSE_MS);
    word(t,p+5,b->errors); word(t,p+6,rd(b,SYSCON+0x80));
    word(t,p+7,rd(b,GPIO+0x2000)); word(t,p+8,rd(b,GPIO+0x2100));
    for(i=0;i<5;++i) { word(t,p+9+i,rd(b,IOCON+pins[i]*4u)); word(t,p+16+i,rd(b,matches[i])); }
    word(t,p+14,rd(b,WHITE+4)); word(t,p+15,rd(b,COLOR+4));
    word(t,p+21,rd(b,WHITE+0x20)); word(t,p+22,rd(b,COLOR+0x20));
    word(t,p+23,rd(b,WHITE+0x74)); word(t,p+24,rd(b,COLOR+0x74));
}
static uint32_t finish_pulse(nxp_pwm_low_trial *t,nxp_board *b,uint32_t reason,uint32_t now) {
    unsigned i,p=56u+t->channel*40u;
    /* GPIO ownership precedes stopping either timer. If mux handoff fails,
     * put every comparator above period while timers continue running; do not
     * freeze a potentially HIGH output latch by stopping the clock. */
    int handed=nxp_board_trial_dark(b), matches_ok=dark_matches(b);
    if(handed) { wr(b,WHITE+4,2); wr(b,COLOR+4,2); }
    if(!handed || !matches_ok || rd(b,WHITE+4)!=2 || rd(b,COLOR+4)!=2) reason=NXP_LOW_REGISTER;
    word(t,p+2,now); word(t,p+4,reason); word(t,p+5,b->errors);
    word(t,p+25,rd(b,GPIO+0x2000)); word(t,p+26,rd(b,GPIO+0x2100));
    for(i=0;i<5;++i) { word(t,p+27+i,rd(b,IOCON+pins[i]*4u)); word(t,p+34+i,rd(b,matches[i])); }
    word(t,p+32,rd(b,WHITE+4)); word(t,p+33,rd(b,COLOR+4));
    word(t,p+39,reason==NXP_LOW_DEADLINE);
    if(reason==NXP_LOW_DEADLINE) word(t,M_COMPLETED,value(t,M_COMPLETED)|(1u<<t->channel));
    t->pulse_on=0; return reason;
}
static void finish(nxp_pwm_low_trial *t,nxp_board *b,uint32_t reason,uint32_t now) {
    if(t->pulse_on) reason=finish_pulse(t,b,reason,now);
    else if(!nxp_board_trial_dark(b)) reason=NXP_LOW_REGISTER;
    t->active=0;
    word(t,M_END,now); word(t,M_REASON,reason); word(t,M_ERRORS,b->errors);
    word(t,M_PHASE,reason==NXP_LOW_DEADLINE && value(t,M_COMPLETED)==31 ? NXP_LOW_COMPLETED : NXP_LOW_FAILED);
}
static int start_pulse(nxp_pwm_low_trial *t,nxp_board *b,uint32_t now) {
    if(!nxp_board_prepare_pwm_off_trial(b)) return 0;
    if(!t->channel) baseline(t,b,now);
    /* Mark the exposure before muxing; recovery covers partial setup. The
     * only nonzero comparator write is a fixed, internally selected value. */
    t->pulse_on=1;
    if(!nxp_board_connect_pwm_off_trial(b)) return 0;
    wr(b,matches[t->channel],low(t->channel));
    if(!pulse_valid(t,b)) return 0;
    snapshot_active(t,b,now); return 1;
}
int nxp_pwm_low_init(nxp_pwm_low_trial *t,nxp_board *b) {
    unsigned i; uint32_t generation;
    if(!t || !permitted(b)) return 0;
    t->initialized=t->active=t->pulse_on=t->started_ms=t->initial_errors=t->channel=0;
    for(i=0;i<sizeof(t->record);++i) t->record[i]=0;
    if(!nxp_state_low_trial(b->link->state,t->record)) return 0;
    wr(b,SYSCON+0x80,rd(b,SYSCON+0x80)|(1u<<27));
    generation=rd(b,COUNTER_ADDRESS)==COUNTER_MAGIC?rd(b,COUNTER_ADDRESS+4)+1u:1u;
    if(!generation) generation=1;
    wr(b,COUNTER_ADDRESS,COUNTER_MAGIC); wr(b,COUNTER_ADDRESS+4,generation);
    if(rd(b,COUNTER_ADDRESS)!=COUNTER_MAGIC || rd(b,COUNTER_ADDRESS+4)!=generation) return 0;
    word(t,M_MAGIC,UINT32_C(0x4c4f5731)); word(t,M_ABI,1); word(t,M_FLAGS,PROFILE_FLAGS);
    word(t,M_RECOVERY,30000); word(t,M_GENERATION,generation); word(t,M_PULSE,NXP_LOW_PULSE_MS);
    word(t,M_GAP,NXP_LOW_GAP_MS); word(t,M_CHANNELS,5);
    t->initialized=1; return 1;
}
void nxp_pwm_low_tick(nxp_pwm_low_trial *t,nxp_board *b,uint32_t now) {
    uint32_t boundary,reason;
    if(!t || !t->initialized || !t->active) return;
    if(!owner_matches(b->link->state)) { finish(t,b,NXP_LOW_OWNER,now); return; }
    if(b->fault || b->pwm_fault || b->errors!=t->initial_errors) { finish(t,b,NXP_LOW_SPI,now); return; }
    if(b->link->state->boot_requested) { finish(t,b,NXP_LOW_CANCELLED,now); return; }
    if(t->pulse_on) {
        boundary=t->started_ms+t->channel*NXP_LOW_SLOT_MS+NXP_LOW_PULSE_MS;
        if(now<boundary) {
            if(!pulse_valid(t,b)) finish(t,b,NXP_LOW_REGISTER,now);
            return;
        }
        reason=finish_pulse(t,b,now==boundary?NXP_LOW_DEADLINE:NXP_LOW_LATE,now);
        if(reason!=NXP_LOW_DEADLINE || t->channel==4) { finish(t,b,reason,now); return; }
        ++t->channel;
    }
    boundary=t->started_ms+t->channel*NXP_LOW_SLOT_MS;
    if(now<boundary) return;
    if(now!=boundary) { finish(t,b,NXP_LOW_LATE,now); return; }
    if(!start_pulse(t,b,now)) finish(t,b,NXP_LOW_REGISTER,now);
}
void nxp_pwm_low_service(nxp_pwm_low_trial *t,nxp_board *b,uint32_t now) {
    nxp_state *s;
    if(!t || !t->initialized || !permitted(b)) return;
    /* Once armed, only SysTick advances this sequence. */
    if(t->active || value(t,M_ACCEPTED)) return;
    s=b->link->state;
    if(s->low_requested!=2 || b->link->phase!=NXP_LINK_REQUEST) return;
    s->low_requested=3; word(t,M_ACCEPTED,1);
    if(!owner_matches(s) || s->boot_requested || now>=22000u) { finish(t,b,NXP_LOW_CANCELLED,now); return; }
    if(b->fault || b->pwm_fault || b->errors) { finish(t,b,NXP_LOW_SPI,now); return; }
    t->started_ms=now; t->initial_errors=b->errors; t->active=1;
    word(t,M_START,now); word(t,M_DEADLINE,now+NXP_LOW_SEQUENCE_MS);
    if(!start_pulse(t,b,now)) { finish(t,b,NXP_LOW_REGISTER,now); return; }
    word(t,M_PHASE,NXP_LOW_RUNNING);
}
void nxp_pwm_low_recovery(nxp_pwm_low_trial *t,nxp_board *b,uint32_t now) {
    if(!t || !t->initialized) return;
    if(t->active) finish(t,b,NXP_LOW_RECOVERY,now);
    else if(permitted(b)) (void)nxp_board_trial_dark(b);
}
