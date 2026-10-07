#ifndef OFF_FIXTURE_H
#define OFF_FIXTURE_H
/* Complete ABI1 all-off captures, independent of the validator's control flow.
 * Timers run before mux and GPIO-low is captured before stopping them. */
static void off_fixture(uint32_t w[224]) {
    memset(w, 0, 224 * sizeof(*w));
    w[0]=0x4f464631;w[1]=1;w[2]=2;w[3]=w[4]=5000;w[5]=w[6]=5400;
    w[7]=w[8]=1;w[10]=31;w[11]=w[12]=2;w[13]=0x193;w[14]=30000;w[15]=17;
    const uint32_t gpio[5]={0x81,0x81,0x80,0,0}, pwm[5]={0x83,0x83,0x82,2,2};
    const uint32_t state[5]={0,0,1,2,2};
    for(unsigned i=0;i<5;++i) {
        uint32_t *s=w+16+40*i;s[0]=i<3?5000:5400;s[1]=i<3?0:1;s[2]=0x80601;
        s[3]=(1u<<13)|(1u<<14)|(1u<<16)|(1u<<18)|(1u<<19);
        for(unsigned p=0;p<5;++p)s[5+p]=i==2?pwm[p]:gpio[p];
        s[38]=state[i];
        if(!i)continue;
        const uint32_t white[14]={1,19,47,3,0x80,255,255,254,255,0,0,0,3,0};
        const uint32_t color[14]={1,710,0,0,0x80,25500,25500,25499,25500,0,0,0,11,0};
        memcpy(s+10,white,sizeof(white));memcpy(s+24,color,sizeof(color));
        if(i==4){s[10]=s[24]=2;s[11]=s[13]=s[25]=s[27]=0;}
    }
}
#endif
