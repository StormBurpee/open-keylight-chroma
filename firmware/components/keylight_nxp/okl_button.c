#include "okl_button.h"
#include <string.h>

void okl_button_init(okl_button *b, int pressed, uint64_t now) {
    if(!b) return;
    memset(b,0,sizeof(*b));
    b->last_sample=b->candidate_since=b->pressed_since=b->released_since=now;
    b->candidate=b->stable=b->ignore_until_release=(uint8_t)(pressed!=0);
}

int okl_button_sample(okl_button *b, int pressed, uint64_t now, unsigned *events) {
    unsigned emitted=0;
    if(!b || !events || (pressed!=0 && pressed!=1) || now<b->last_sample) return -1;
    b->last_sample=now;
    if(b->pending_single && !b->stable && now-b->released_since>=350) {
        b->pending_single=0;emitted|=OKL_BUTTON_SINGLE;
    }
    if((unsigned)pressed!=b->candidate) {
        b->candidate=(uint8_t)pressed;b->candidate_since=now;
    }
    if(b->candidate!=b->stable && now-b->candidate_since>=30) {
        b->stable=b->candidate;
        if(b->stable) {
            b->pressed_since=now;b->hold_sent=0;
            b->second_press=b->pending_single;
            b->pending_single=0;
        } else if(b->ignore_until_release) {
            b->ignore_until_release=0;
        } else if(!b->hold_sent) {
            if(b->second_press) emitted|=OKL_BUTTON_DOUBLE;
            else {b->pending_single=1;b->released_since=now;}
            b->second_press=0;
        }
    }
    if(b->stable && b->candidate && !b->ignore_until_release && !b->hold_sent &&
       now-b->pressed_since>=3000) {
        b->hold_sent=1;b->pending_single=0;b->second_press=0;emitted|=OKL_BUTTON_SETUP;
    }
    *events=emitted;return 0;
}
