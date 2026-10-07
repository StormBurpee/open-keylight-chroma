#ifndef KEYLIGHT_TEST_LOW_FIXTURE_H
#define KEYLIGHT_TEST_LOW_FIXTURE_H
#include <stdint.h>
#include <string.h>
/* Fixed LOW1 protocol/register fixture, not optical evidence. The author NXP
 * module is separately composed with the same real validation predicates. */
static inline void low_fixture(uint32_t w[256]) {
    const uint32_t pins = (1u<<13)|(1u<<14)|(1u<<16)|(1u<<18)|(1u<<19);
    const uint32_t gpio[5] = {0x81,0x81,0x80,0,0}, pwm[5] = {0x83,0x83,0x82,2,2};
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

#endif
