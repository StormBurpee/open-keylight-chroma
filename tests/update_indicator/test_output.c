#include "update_indicator_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#v); exit(1); } } while(0)
static okl_result mock_execute(okl_nxp*,const okl_request*,okl_reply*,uint64_t);
static okl_result mock_claim(okl_nxp*,const uint8_t*,size_t,uint64_t);
static okl_result mock_release(okl_nxp*,uint64_t);
static okl_result mock_read(okl_nxp*,okl_light_state*,uint64_t);
static okl_result mock_owner(okl_nxp*,okl_owner*,uint64_t);
#define okl_nxp_execute mock_execute
#define okl_nxp_claim mock_claim
#define okl_nxp_release mock_release
#define okl_nxp_read_state mock_read
#define okl_nxp_get_owner mock_owner
#include "../../firmware/main/update_indicator_output.c"
#undef okl_nxp_execute
#undef okl_nxp_claim
#undef okl_nxp_release
#undef okl_nxp_read_state
#undef okl_nxp_get_owner

static okl_nxp driver;
static kl_update_output output;
static kl_update_indicator evidence;
static okl_light_state state, initial;
static uint64_t now, latency, admission_wait;
static unsigned admission_call, wire_calls;
static uint64_t published_deadline;
static unsigned publish_call;
static uint32_t current_revision;
static unsigned calls, claim_calls, release_calls, read_calls, frames, writes;
static unsigned fail_call, cancel_call, steal_call, lose_call;
static unsigned pair_stop_call;
static bool claimed, different_name, corrupt_read, slow_compound;
static uint8_t framebuffer[3];
static okl_request history[4096];

static uint64_t time_us(void *user) { CHECK(user == &driver); return now; }
static bool guard(void *user, uint32_t revision) {
    CHECK(user == &output);
    kl_update_output_limit(&output,published_deadline);
    return revision == current_revision;
}
static okl_result boundary(uint64_t deadline, uint64_t maximum) {
    CHECK(deadline > now && deadline <= now + maximum); ++calls;
    driver.last_admission_wait_us=0;
    if (calls == cancel_call) ++current_revision;
    if (calls == steal_call) claimed = false;
    if (calls == lose_call) driver.needs_recovery = 1;
    if (calls == pair_stop_call) output.pairing_stop = true;
    if (calls==publish_call) published_deadline=now+1400000;
    uint64_t hard=driver.admission_deadline_us;
    if (published_deadline && (!hard || published_deadline<hard)) hard=published_deadline;
    if (hard && now>=hard) return OKL_TIMEOUT;
    if (hard && deadline>hard) deadline=hard;
    uint64_t original=deadline;
    if (admission_wait && (!admission_call || calls == admission_call)) {
        if (hard && admission_wait >= hard-now) {
            now=hard;return OKL_TIMEOUT;
        }
        now += admission_wait;
        deadline += admission_wait;
    }
    if (hard && deadline>hard) deadline=hard;
    driver.last_admission_wait_us=deadline>original?deadline-original:0;
    ++wire_calls;
    uint64_t elapsed=latency+(slow_compound && maximum>150000?51000:0);
    if (elapsed > deadline - now) { now = deadline; driver.needs_recovery = 1; return OKL_TIMEOUT; }
    now += elapsed;
    return calls == fail_call ? OKL_REMOTE : OKL_OK;
}
static okl_result mock_claim(okl_nxp *d,const uint8_t *name,size_t size,uint64_t end) {
    CHECK(d==&driver && size==13 && !memcmp(name,"Open Keylight",13)); ++claim_calls;
    okl_result r=boundary(end,600000); if(r==OKL_OK)claimed=true;return r;
}
static okl_result mock_release(okl_nxp *d,uint64_t end) {
    CHECK(d==&driver && !d->needs_recovery); ++release_calls;
    okl_result r=boundary(end,600000); if(r==OKL_OK && !different_name)claimed=false;return r;
}
static okl_result mock_owner(okl_nxp *d,okl_owner *owner,uint64_t end) {
    CHECK(d==&driver && !d->needs_recovery);okl_result r=boundary(end,150000);
    if(r==OKL_OK){memset(owner,0,sizeof(*owner));owner->claimed=claimed;
        memcpy(owner->identity,d->identity,6);owner->name_size=13;memcpy(owner->name,"Open Keylight",13);
        if(different_name)owner->name[0]='X';}
    return r;
}
static okl_result mock_read(okl_nxp *d,okl_light_state *s,uint64_t end) {
    CHECK(d==&driver && !d->needs_recovery);++read_calls;okl_result r=boundary(end,800000);
    if(r==OKL_OK){*s=state;if(corrupt_read && writes>5)s->color_brightness^=1;}
    return r;
}
static okl_result mock_execute(okl_nxp *d,const okl_request *q,okl_reply *reply,uint64_t end) {
    CHECK(d==&driver && !d->needs_recovery && claimed && writes<4096);history[writes++]=*q;
    memset(reply,0,sizeof(*reply));okl_result r=boundary(end,150000);if(r!=OKL_OK)return r;
    const uint8_t *a=q->arguments;
    switch(q->command){
    case OKL_SET_EFFECT:
        CHECK(!state.color_brightness); /* All mode changes are muted. */
        state.effect=a[2];state.flags=a[3];state.speed=a[4];state.color_count=a[5];memcpy(state.colors,a+6,6);break;
    case OKL_SET_COLOR_BRIGHTNESS:state.color_brightness=a[2];break;
    case OKL_SET_WHITE_BRIGHTNESS:state.white_brightness=a[2];break;
    case OKL_SET_FRAME:
        CHECK(state.effect==8 && (state.color_brightness==0 || state.color_brightness==12));
        memcpy(framebuffer,a+5,3);++frames;break;
    default:CHECK(false);
    }
    return OKL_OK;
}
static void reset(void) {
    memset(&driver,0,sizeof(driver));memset(&output,0,sizeof(output));memset(&evidence,0,sizeof(evidence));
    driver.identity[0]=2;driver.transport.now_us=time_us;driver.transport.user=&driver;
    now=latency=admission_wait=published_deadline=0;admission_call=wire_calls=publish_call=0;
    current_revision=7;calls=claim_calls=release_calls=read_calls=frames=writes=0;
    fail_call=cancel_call=steal_call=lose_call=0;claimed=different_name=corrupt_read=slow_compound=false;
    pair_stop_call=0;
    memset(framebuffer,0,sizeof(framebuffer));memset(history,0,sizeof(history));
    state=(okl_light_state){.effect=1,.color_count=1,.colors={30,80,140},.color_brightness=51,.temperature_kelvin=5300};
    initial=state;CHECK(kl_update_indicator_begin(&evidence,1000,0));
}
static kl_update_output_result step(const uint8_t *known) {
    return kl_update_output_step(&output,&driver,&evidence,7,known,guard,&output);
}
static void begin(void) {
    CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    CHECK(claim_calls==1 && state.effect==8 && !state.white_brightness && state.color_brightness==12);
    CHECK(history[0].command==OKL_SET_COLOR_BRIGHTNESS && history[0].arguments[2]==0);
    CHECK(history[3].command==OKL_SET_FRAME && framebuffer[0]==0 && framebuffer[1]==0);
    CHECK(frames==1 && !release_calls && output.active);
}
static void complete(bool success) {
    if(success){CHECK(kl_update_indicator_advance(&evidence,1000));CHECK(kl_update_indicator_verify(&evidence,now/1000));}
    else kl_update_indicator_fail(&evidence);
    CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    if(success)now+=300000;else now+=1600000;
    CHECK(step(NULL)==KL_INDICATOR_RESTORED);
    CHECK(!output.active && output.finished && !output.resume_custom && !claimed);
    CHECK(same(&initial,&state) && same(&state,&output.restored));
    unsigned old=writes;now+=UINT64_C(10000000);CHECK(step(NULL)==KL_INDICATOR_NONE && writes==old);
}
static void native_restoration(void) {
    for(unsigned success=0;success<2;++success)for(unsigned mode=0;mode<4;++mode){
        reset();if(mode==1){state.effect=0;state.color_count=0;memset(state.colors,0,6);state.white_brightness=71;}
        if(mode==2){state.effect=0;state.color_count=0;memset(state.colors,0,6);state.color_brightness=0;}
        if(mode==3){state.effect=2;state.flags=1;state.speed=2;state.color_count=2;state.colors[5]=5;}
        initial=state;begin();complete(success!=0);
    }
    reset();begin();kl_update_indicator_fail(&evidence);
    CHECK(step(NULL)==KL_INDICATOR_ACTIVE && framebuffer[0]==43 && !framebuffer[1] && !framebuffer[2]);
    for(unsigned ms=20;ms<1600;ms+=20){now=ms*1000;CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
        CHECK(framebuffer[0]>=43 && !framebuffer[1] && !framebuffer[2] && state.color_brightness==12);}
    now=1600000;CHECK(step(NULL)==KL_INDICATOR_RESTORED && same(&state,&initial));
}
static void custom_restoration(void) {
    const uint8_t known[]={4,17,33};
    for(unsigned success=0;success<2;++success){
        reset();state.effect=8;state.color_count=0;memset(state.colors,0,6);state.color_brightness=255;
        CHECK(step(known)==KL_INDICATOR_ACTIVE);
        if(success){CHECK(kl_update_indicator_advance(&evidence,1000));CHECK(kl_update_indicator_verify(&evidence,0));}
        else kl_update_indicator_fail(&evidence);
        CHECK(step(known)==KL_INDICATOR_ACTIVE);now=success?300000:1600000;
        CHECK(step(known)==KL_INDICATOR_RESTORED);
        CHECK(output.resume_custom==!success && state.color_brightness==255 && claimed==!success);
        CHECK(success ? state.effect==1 && !memcmp(state.colors,known,3) : state.effect==8 && !memcmp(framebuffer,known,3));
    }
    reset();state.effect=8;initial=state;
    CHECK(step(NULL)==KL_INDICATOR_NONE && !writes && !claimed && same(&state,&initial));
    reset();state.white_brightness=60;initial=state;
    CHECK(step(NULL)==KL_INDICATOR_NONE && !writes && !claimed && same(&state,&initial));
    reset();state.effect=8;fail_call=3;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && !writes && release_calls==1);
}
static void cancellation_and_failures(void) {
    for(unsigned boundary_index=1;boundary_index<=7;++boundary_index){
        reset();cancel_call=boundary_index;kl_update_output_result r=step(NULL);
        if(boundary_index==7){CHECK(r==KL_INDICATOR_ACTIVE);r=step(NULL);}
        CHECK(r==KL_INDICATOR_CANCELLED && !output.active && !claimed);
        unsigned prior=writes;CHECK(step(NULL)==KL_INDICATOR_NONE && writes==prior);
    }
    for(unsigned failed=1;failed<=7;++failed){
        reset();fail_call=failed;CHECK(step(NULL)==KL_INDICATOR_ERROR);
        CHECK(output.finished && !output.active && claim_calls==1);
        unsigned prior=writes;for(unsigned n=0;n<5;++n)CHECK(step(NULL)==KL_INDICATOR_NONE && writes==prior);
    }
    for(unsigned changed=0;changed<4;++changed){
        reset();begin();now=250000;
        if(changed==0)claimed=false;
        if(changed==1)different_name=true;
        if(changed==2)state.color_brightness=200;
        if(changed==3)state.white_brightness=10;
        unsigned prior=writes;CHECK(step(NULL)==KL_INDICATOR_ERROR && writes==prior);
        CHECK(claim_calls==1); /* Never take ownership back. */
        if(changed<2)CHECK(!release_calls);
    }
    reset();begin();driver.needs_recovery=1;now=20000;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && !release_calls);
    reset();begin();now=20000;fail_call=calls+1;CHECK(step(NULL)==KL_INDICATOR_ERROR);
    CHECK(frames==1 && claim_calls==1);
    /* Every restoration exchange can fail, and a newer Off can interrupt it.
     * No later setter is sent, no whole-scene restore is retried. */
    for(unsigned phase=1;phase<=8;++phase)for(unsigned cancel=0;cancel<2;++cancel){
        reset();begin();kl_update_indicator_fail(&evidence);CHECK(step(NULL)==KL_INDICATOR_ACTIVE);now=1600000;
        if(cancel)cancel_call=calls+phase;else fail_call=calls+phase;
        kl_update_output_result r=step(NULL);
        CHECK(r==KL_INDICATOR_ERROR || r==KL_INDICATOR_CANCELLED || (cancel && phase==8 && r==KL_INDICATOR_RESTORED));
        unsigned prior=writes;CHECK(step(NULL)==KL_INDICATOR_NONE && writes==prior && claim_calls==1);
        CHECK(release_calls<=1);
    }
}
static void deadlines_and_generations(void) {
    reset();begin();CHECK(kl_update_indicator_advance(&evidence,1000));CHECK(kl_update_indicator_verify(&evidence,0));
    now=300000;latency=149000;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && now<=1400000 && output.finished);
    unsigned prior=writes;now=5000000;CHECK(step(NULL)==KL_INDICATOR_NONE && writes==prior);
    reset();begin();now=1500000;CHECK(kl_update_indicator_advance(&evidence,1000));
    CHECK(kl_update_indicator_verify(&evidence,0));prior=writes;CHECK(step(NULL)==KL_INDICATOR_ERROR && writes==prior);
    reset();begin();now=100000;kl_update_indicator_fail(&evidence);CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    CHECK(kl_update_indicator_begin(&evidence,1000,now/1000));CHECK(step(NULL)==KL_INDICATOR_ACTIVE && claim_calls==1);
    CHECK(!output.failure_seen && same(&output.saved,&initial));complete(true);
    reset();CHECK(kl_update_indicator_advance(&evidence,1000));CHECK(kl_update_indicator_verify(&evidence,0));
    CHECK(step(NULL)==KL_INDICATOR_NONE && !calls); /* Missed upload cannot race reboot. */
    reset();now=60000000;kl_update_indicator_fail(&evidence);
    CHECK(step(NULL)==KL_INDICATOR_ACTIVE && output.failure_seen && output.failure_ms==60000);
    CHECK(framebuffer[0]==43 && !framebuffer[1] && !framebuffer[2]);
    now+=1600000;CHECK(step(NULL)==KL_INDICATOR_RESTORED && same(&state,&initial));
    reset();begin();now=10000000;prior=frames;CHECK(step(NULL)==KL_INDICATOR_ACTIVE && frames==prior+1);
    CHECK(step(NULL)==KL_INDICATOR_ACTIVE && frames==prior+1); /* No catch-up loop. */
}
static void flash_admission_budgets(void) {
    /* Every driver boundary may queue behind a different flash operation.
     * Credit it once, including compound claim/read/owner/release operations. */
    reset();admission_wait=2000000;latency=1000;begin();
    CHECK(calls==7 && now==14007000 && output.deadline_us==15200000);
    CHECK(driver.last_admission_wait_us==0 && !output.fixed_deadline);
    unsigned before=calls;uint64_t started=now;
    now+=250000;CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    CHECK(calls==before+3 && now==started+6253000);
    CHECK(output.deadline_us==started+7450000);
    CHECK(driver.last_admission_wait_us==0);
    kl_update_indicator_fail(&evidence);CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    now+=1600000;before=calls;started=now;
    CHECK(step(NULL)==KL_INDICATOR_RESTORED && same(&state,&initial));
    CHECK(calls==before+8 && now==started+16008000);
    CHECK(output.deadline_us==started+17200000 && release_calls==1);

    /* A credit from an earlier API cannot be reused by a later one. */
    reset();admission_wait=2000000;admission_call=1;latency=1000;begin();
    CHECK(now==2007000 && output.deadline_us==3200000);
    CHECK(driver.last_admission_wait_us==0);

    /* A credited admission does not turn a wire failure into success or retry
     * it. One guarded release is still allowed within the credited budget. */
    reset();admission_wait=2000000;admission_call=3;fail_call=3;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && output.error==OKL_REMOTE);
    CHECK(calls==4 && writes==1 && release_calls==1 && !claimed);
    CHECK(driver.last_admission_wait_us==0 && output.deadline_us==3200000);
    before=calls;CHECK(step(NULL)==KL_INDICATOR_NONE && calls==before);

    /* Actual wire time remains charged. Seven slow operations exceed the
     * handoff budget despite a valid unrelated flash wait before each one. */
    reset();admission_wait=2000000;latency=149000;slow_compound=true;
    CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    kl_update_indicator_fail(&evidence);latency=0;CHECK(step(NULL)==KL_INDICATOR_ACTIVE);
    now+=1600000;latency=149000;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && output.error==OKL_TIMEOUT);
    CHECK(output.finished && !output.active && driver.last_admission_wait_us==0);

    /* A verified upload's 1400 ms deadline remains absolute. Waiting behind
     * flash cannot start a late output exchange or postpone reboot cleanup. */
    reset();begin();CHECK(kl_update_indicator_advance(&evidence,1000));
    CHECK(kl_update_indicator_verify(&evidence,0));now=300000;
    admission_wait=2000000;before=wire_calls;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && output.error==OKL_TIMEOUT);
    CHECK(now==1400000 && wire_calls==before && output.deadline_us==1400000);
    CHECK(driver.admission_deadline_us==0 && driver.last_admission_wait_us==0);
    CHECK(!driver.needs_recovery && !release_calls);

    /* Preserve a stricter ceiling imposed by the caller, then restore it on
     * failure instead of leaking the indicator's temporary limit. */
    reset();begin();CHECK(kl_update_indicator_advance(&evidence,1000));
    CHECK(kl_update_indicator_verify(&evidence,0));now=300000;
    driver.admission_deadline_us=900000;admission_wait=1000000;before=wire_calls;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && output.error==OKL_TIMEOUT);
    CHECK(now==900000 && wire_calls==before && driver.admission_deadline_us==900000);
    CHECK(output.deadline_us==1400000 && driver.last_admission_wait_us==0);

    /* A worker guard refreshes an older RECEIVING snapshot before admission;
     * a success published during admission is independently enforced by the
     * native gate, with no app mutex taken inside that gate. */
    reset();published_deadline=1400000;admission_wait=2000000;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && output.error==OKL_TIMEOUT);
    CHECK(now==1400000 && !wire_calls && output.fixed_deadline);
    CHECK(output.deadline_us==1400000 && driver.admission_deadline_us==0);
    reset();publish_call=1;admission_wait=2000000;
    CHECK(step(NULL)==KL_INDICATOR_ERROR && output.error==OKL_TIMEOUT);
    CHECK(now==1400000 && !wire_calls && frames==0);
    reset();publish_call=1;admission_call=1;admission_wait=1000000;latency=1000;
    begin();CHECK(output.fixed_deadline && output.deadline_us==1400000);
    CHECK(now==1007000 && driver.last_admission_wait_us==0 && !driver.admission_deadline_us);

    uint64_t ceiling=output.deadline_us;
    kl_update_output_limit(&output,0);CHECK(output.deadline_us==ceiling);
    kl_update_output_limit(&output,ceiling+1000);CHECK(output.deadline_us==ceiling);
    kl_update_output_limit(&output,ceiling-1000);CHECK(output.deadline_us==ceiling-1000);
}
static kl_update_output_result pair_step(const uint8_t *known, bool stop) {
    return kl_pairing_output_step(&output,&driver,1,0,7,known,stop,guard,&output);
}
static void pairing_feedback(void) {
    for(unsigned mode=0;mode<5;++mode) {
        reset();
        if(mode==1){state.effect=0;state.color_count=0;memset(state.colors,0,6);state.white_brightness=71;}
        if(mode==2){state.effect=0;state.color_count=0;memset(state.colors,0,6);state.color_brightness=0;}
        if(mode==3){state.effect=2;state.flags=1;state.speed=2;state.color_count=2;}
        uint8_t known[3]={18,27,36};
        if(mode==4){state.effect=8;state.color_count=0;memset(state.colors,0,6);state.color_brightness=255;claimed=true;memcpy(framebuffer,known,3);}
        initial=state;
        CHECK(pair_step(mode==4?known:NULL,false)==KL_INDICATOR_ACTIVE);
        CHECK(!framebuffer[0] && !framebuffer[1] && !framebuffer[2] && state.color_brightness==12);
        uint8_t previous=0;
        for(unsigned ms=20;ms<600;ms+=20) {
            now=ms*1000;
            CHECK(pair_step(NULL,false)==KL_INDICATOR_ACTIVE && !framebuffer[0] && !framebuffer[2]);
            if(ms<=300)CHECK(framebuffer[1]>=previous);else CHECK(framebuffer[1]<=previous);
            if(ms==300)CHECK(framebuffer[1]==255);
            previous=framebuffer[1];
        }
        now=600000;CHECK(pair_step(NULL,false)==KL_INDICATOR_RESTORED);
        CHECK(same(&state,&initial) && output.resume_custom==(mode==4));
        if(mode==4)CHECK(claimed && !memcmp(framebuffer,known,3));else CHECK(!claimed);
        unsigned before=calls;now=30000000;CHECK(pair_step(NULL,false)==KL_INDICATOR_NONE && calls==before);
    }
    reset();now=250000;CHECK(pair_step(NULL,false)==KL_INDICATOR_NONE && !calls);
    reset();CHECK(pair_step(NULL,true)==KL_INDICATOR_NONE && !calls);
    reset();CHECK(pair_step(NULL,false)==KL_INDICATOR_ACTIVE);now=40000;
    CHECK(pair_step(NULL,true)==KL_INDICATOR_RESTORED && same(&state,&initial));
    reset();CHECK(pair_step(NULL,false)==KL_INDICATOR_ACTIVE);now=40000;
    CHECK(kl_pairing_output_step(&output,&driver,2,40,7,NULL,false,guard,&output)==KL_INDICATOR_RESTORED);
    unsigned before_new=calls;
    CHECK(kl_pairing_output_step(&output,&driver,2,40,7,NULL,false,guard,&output)==KL_INDICATOR_NONE && calls==before_new);
    reset();claimed=true;different_name=true;
    CHECK(pair_step(NULL,false)==KL_INDICATOR_NONE && !writes && !claim_calls && !release_calls);
    reset();uint8_t known[3]={1,2,3};
    CHECK(pair_step(known,false)==KL_INDICATOR_NONE && !writes && !claim_calls); /* Lost custom provenance. */
    reset();state.effect=8;state.color_count=0;memset(state.colors,0,6);state.color_brightness=255;claimed=true;
    pair_stop_call=3; /* Update/lock interrupts the saved-state read before any output change. */
    CHECK(pair_step(known,false)==KL_INDICATOR_NONE && !writes && !release_calls && claimed);
    reset();state.effect=8;state.color_brightness=255;
    CHECK(pair_step(NULL,false)==KL_INDICATOR_NONE && !writes && release_calls==1);
    reset();admission_wait=300000;CHECK(pair_step(NULL,false)==KL_INDICATOR_NONE && !wire_calls && !writes);
    CHECK(now==250000 && !driver.admission_deadline_us); /* No flash after a blocked admission. */
    reset();CHECK(pair_step(NULL,false)==KL_INDICATOR_ACTIVE);now=20000;
    admission_wait=1000000;admission_call=calls+1;unsigned earlier_frames=frames;
    CHECK(pair_step(NULL,false)==KL_INDICATOR_RESTORED && same(&state,&initial));
    CHECK(now==600000 && frames==earlier_frames); /* Waiting behind flash cannot enqueue a late green frame. */
    reset();CHECK(pair_step(NULL,false)==KL_INDICATOR_ACTIVE);current_revision++;
    unsigned before=writes;CHECK(pair_step(NULL,false)==KL_INDICATOR_CANCELLED && writes==before);
    for(unsigned failed=4;failed<=8;++failed) {
        reset();fail_call=failed;
        CHECK(pair_step(NULL,false)==KL_INDICATOR_ERROR);
        before=calls;now=1000000;CHECK(pair_step(NULL,false)==KL_INDICATOR_NONE && calls==before);
    }
    reset();CHECK(pair_step(NULL,false)==KL_INDICATOR_ACTIVE);different_name=true;now=600000;
    before=writes;CHECK(pair_step(NULL,false)==KL_INDICATOR_ERROR && output.error==OKL_NOT_OWNER);
    CHECK(writes==before && !release_calls);
}
int main(void) {
    native_restoration();custom_restoration();cancellation_and_failures();deadlines_and_generations();
    flash_admission_budgets();
    pairing_feedback();
    printf("%u indicator output assertions passed; actual coordinator, bounded mocked driver, no device I/O\n",checks);
    return 0;
}
