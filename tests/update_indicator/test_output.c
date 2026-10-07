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
static uint64_t now, latency;
static uint32_t current_revision;
static unsigned calls, claim_calls, release_calls, read_calls, frames, writes;
static unsigned fail_call, cancel_call, steal_call, lose_call;
static bool claimed, different_name, corrupt_read;
static uint8_t framebuffer[3];
static okl_request history[4096];

static uint64_t time_us(void *user) { CHECK(user == &driver); return now; }
static bool guard(void *user, uint32_t revision) { CHECK(user == &output); return revision == current_revision; }
static okl_result boundary(uint64_t deadline, uint64_t maximum) {
    CHECK(deadline > now && deadline <= now + maximum); ++calls;
    if (calls == cancel_call) ++current_revision;
    if (calls == steal_call) claimed = false;
    if (calls == lose_call) driver.needs_recovery = 1;
    if (latency > deadline - now) { now = deadline; driver.needs_recovery = 1; return OKL_TIMEOUT; }
    now += latency;
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
    now=latency=0;current_revision=7;calls=claim_calls=release_calls=read_calls=frames=writes=0;
    fail_call=cancel_call=steal_call=lose_call=0;claimed=different_name=corrupt_read=false;
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
int main(void) {
    native_restoration();custom_restoration();cancellation_and_failures();deadlines_and_generations();
    printf("%u indicator output assertions passed; actual coordinator, bounded mocked driver, no device I/O\n",checks);
    return 0;
}
