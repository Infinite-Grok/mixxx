/* Parity port of tools/android-usb-probe/tests/stream_test.c (same expected values). */
#include "usbdirect/transport_core.h"

#include <cstdlib>
#include <cstring>

#include "check.h"
int main() {
    T7Clock c{}; c.rate=T7_NOMINAL; uint64_t total=0;
    for(unsigned i=0;i<1000000;i++) total+=(unsigned)t7_frames(&c,540);
    CHECK(total==44100000 && c.phase==0);
    unsigned char fb[]={0x66,0x06,0x0b}; /* floor(44.1 * 16384) */
    CHECK(t7_feedback(&c,fb,1,1)==-1 && t7_feedback(&c,fb,2,1)==-1);
    CHECK(t7_feedback(&c,fb,4,1)==-1);
    CHECK(t7_feedback(&c,fb,3,1)==1 && c.rate==7225340);
    CHECK(t7_feedback(&c,fb,3,32000001)==1);
    uint64_t age=c.last_valid_ns;
    CHECK(t7_feedback(&c,NULL,0,60000000)==0 && c.last_valid_ns==age);
    CHECK(t7_clock_error(&c,160000001)==2);
    T7Clock startup{}; startup.rate=T7_NOMINAL; startup.first_ns=100;
    CHECK(t7_clock_error(&startup,250000099)==0);
    CHECK(t7_clock_error(&startup,250000100)==1);
    CHECK(t7_feedback(&startup,fb,3,200)==1);
    CHECK(t7_feedback(&startup,fb,3,300)==1);
    CHECK(t7_feedback(&startup,fb,3,400)==1 && startup.valid==3);
    unsigned char zero[3]={0}; CHECK(t7_feedback(&c,zero,3,1)==-1);
    unsigned char low[3]={0,0,10}; CHECK(t7_feedback(&c,low,3,1)==-1);
    unsigned char high[3]={0,0,12}; CHECK(t7_feedback(&c,high,3,1)==-1);
    c=T7Clock{}; c.rate=T7_NOMINAL; t7_frames(&c,540);uint32_t phase=c.phase;
    CHECK(t7_feedback(&c,fb,3,1)==1 && c.phase==phase);
    uint64_t sum=(uint64_t)phase+c.rate;t7_frames(&c,540);CHECK(c.phase==sum%T7_DENOM);
    c=T7Clock{}; c.rate=T7_NOMINAL;uint64_t exact=0;total=0;
    for(unsigned i=0;i<100000;i++){
        if(i%7==0){uint32_t q=722534+i%61;unsigned char changing[]={(unsigned char)q,(unsigned char)(q>>8),(unsigned char)(q>>16)};
            CHECK(t7_feedback(&c,changing,3,i)==1);}
        exact+=c.rate;total+=(unsigned)t7_frames(&c,540);
        CHECK(total==exact/T7_DENOM && c.phase==exact%T7_DENOM);
    }
    c.rate=46*T7_DENOM;phase=c.phase;CHECK(t7_frames(&c,540)==-1 && c.phase==phase);
    unsigned char packed[3];t7_pack24(packed,1);CHECK(packed[0]==255 && packed[1]==255 && packed[2]==127);
    t7_pack24(packed,-1);CHECK(packed[0]==0 && packed[1]==0 && packed[2]==128);
    unsigned char pcm[540];uint64_t sample=0;t7_pcm(pcm,45,&sample,1,1);
    for(unsigned i=0;i<45;i++)for(unsigned j=3;j<12;j++)CHECK(pcm[i*12+j]==0);
    CHECK(memcmp(pcm+12,zero,3)!=0);
    sample=0;t7_pcm(pcm,45,&sample,15,0);for(unsigned i=0;i<540;i++)CHECK(!pcm[i]);
    sample=1;t7_pcm(pcm,1,&sample,15,1);CHECK(memcmp(pcm,pcm+3,3)!=0 && memcmp(pcm+3,pcm+6,3)!=0);
    unsigned char *one_second=(unsigned char*)malloc(44100*12);CHECK(one_second);sample=0;t7_pcm(one_second,44100,&sample,15,1);
    unsigned crossings[4]={0};int previous[4]={0};
    for(unsigned i=0;i<44100;i++)for(unsigned ch=0;ch<4;ch++){
        unsigned char *p=one_second+i*12+ch*3;int32_t value=p[0]|((int32_t)p[1]<<8)|((int32_t)p[2]<<16);
        if(value&0x800000)value-=0x1000000;
        CHECK(abs(value)<=167773); /* 0.02 full scale, quieter than -30 dBFS */
        if(previous[ch]<=0 && value>0)crossings[ch]++;
        previous[ch]=value;
    }
    for(unsigned ch=0;ch<4;ch++)CHECK(crossings[ch]==440+220*ch);
    free(one_second);
    T7Life l{};t7_submitted(&l);t7_submitted(&l); /* partial startup failure */
    t7_stop(&l,10);CHECK(!t7_may_submit(&l));t7_reaped(&l);CHECK(l.live==1);
    CHECK(!t7_pending(&l,2000000009ull));CHECK(t7_pending(&l,2000000010ull));
    t7_stop(&l,100);CHECK(l.stop_ns==10); /* watchdog origin never moved */
    t7_reaped(&l);CHECK(!l.live && !t7_pending(&l,3000000010ull));
    /* stop/device gone before callback: reaped callback cannot resubmit */
    l=T7Life{};t7_submitted(&l);t7_stop(&l,1);t7_reaped(&l);CHECK(!t7_may_submit(&l));
    /* G3 underrun evidence: pure histogram / depth-exceed / candidate accounting */
    {
        T7Gaps g{};
        t7_gap_record(&g,4,1500000,10);   /* exactly 1.5 ms: not > 1.5 */
        CHECK(g.total==1 && g.hist[0]==0);
        t7_gap_record(&g,4,1500001,20);   CHECK(g.hist[0]==1 && g.hist[1]==0);
        t7_gap_record(&g,4,2000000,30);   CHECK(g.hist[0]==2 && g.hist[1]==0); /* exactly 2.000 ms not > 2 ms */
        t7_gap_record(&g,4,2000001,40);   CHECK(g.hist[1]==1 && g.hist[2]==0);
        t7_gap_record(&g,4,3000001,50);   CHECK(g.hist[2]==1 && g.hist[3]==0);
        t7_gap_record(&g,4,4000000,60);   CHECK(g.hist[3]==0);
        t7_gap_record(&g,4,4000001,70);   CHECK(g.hist[3]==1 && g.hist[4]==0);
        t7_gap_record(&g,4,5000001,80);   CHECK(g.hist[4]==1 && g.hist[5]==0);
        t7_gap_record(&g,4,8000000,90);   CHECK(g.hist[5]==0);
        t7_gap_record(&g,4,8000001,100);  CHECK(g.hist[5]==1);
        CHECK(g.total==10);
        /* depth 4: strictly > 4 ms: 4000001, 5000001, 8000000, 8000001 */
        CHECK(g.exceed==4 && g.ncand==4 && g.cand_t[0]==70 && g.cand_gap[0]==4000001 && g.cand_t[3]==100);
    }
    for(unsigned d=2;d<=8;d+=(d==4?4:1)){ /* depth 2,3,4,8 thresholds */
        if(d==5||d==6||d==7)continue;
        T7Gaps g{};
        t7_gap_record(&g,d,(uint64_t)d*1000000,1);       CHECK(g.exceed==0);
        t7_gap_record(&g,d,(uint64_t)d*1000000+1,2);     CHECK(g.exceed==1 && g.cand_gap[0]==(uint64_t)d*1000000+1);
        t7_gap_record(&g,d,1000000,3);                   CHECK(g.exceed==1);
    }
    {   /* more than 64 candidates: list capped, total exact */
        T7Gaps g{};
        for(unsigned i=0;i<100;i++)t7_gap_record(&g,2,3000000+i,1000+i);
        CHECK(g.exceed==100 && g.ncand==T7_CAND_MAX && g.total==100);
        CHECK(g.cand_t[0]==1000 && g.cand_t[63]==1063 && g.cand_gap[63]==3000063);
    }
    {   /* observed 4.8 ms stall then all four OUT transfers in one burst, nominal 1 ms before */
        T7Gaps g1{},g2{};uint64_t t=0;
        for(unsigned i=0;i<50;i++){t+=1000000;t7_gap_record(&g1,4,1000000,t);t7_gap_record(&g2,8,1000000,t);}
        t+=4800000;t7_gap_record(&g1,4,4800000,t);t7_gap_record(&g2,8,4800000,t);
        for(unsigned i=0;i<3;i++){t7_gap_record(&g1,4,1000,t);t7_gap_record(&g2,8,1000,t);} /* burst */
        CHECK(g1.exceed==1 && g1.ncand==1 && g1.cand_gap[0]==4800000 && g1.hist[3]==1 && g1.hist[4]==0);
        CHECK(g2.exceed==0 && g2.ncand==0);
    }
    CHECK(t7_expected_packets(0,5)==0 && t7_expected_packets(100,100)==0);
    CHECK(t7_expected_packets(100,1000099)==0 && t7_expected_packets(100,1000100)==1);
    CHECK(t7_expected_packets(0,3999999999ull)==0 && t7_expected_packets(1,3999999999ull)==3999 && t7_expected_packets(10,5)==0);
    /* G4 affinity helpers: mask validation, bit extraction, hex format, bounded per-CPU samples */
    {
        CHECK(t7_affinity_valid(0) && t7_affinity_valid(255) && !t7_affinity_valid(-1) && !t7_affinity_valid(256));
        CHECK(t7_mask_has_cpu(0xC0,6) && t7_mask_has_cpu(0xC0,7) && !t7_mask_has_cpu(0xC0,5) && !t7_mask_has_cpu(0xC0,8) && !t7_mask_has_cpu(0xC0,-1));
        char b[24];
        t7_hex_mask(b,sizeof b,0);CHECK(!strcmp(b,"0x0"));
        t7_hex_mask(b,sizeof b,0xC0);CHECK(!strcmp(b,"0xc0"));
        t7_hex_mask(b,sizeof b,0xffffffffffffffffull);CHECK(!strcmp(b,"0xffffffffffffffff"));
        T7CpuSamples c{};
        t7_cpu_sample(&c,0);t7_cpu_sample(&c,15);t7_cpu_sample(&c,15);t7_cpu_sample(&c,16);t7_cpu_sample(&c,-1);t7_cpu_sample(&c,100000);
        CHECK(c.n[0]==1 && c.n[15]==2 && c.other==3);
        uint64_t sum=c.other;for(unsigned i=0;i<T7_CPU_MAX;i++)sum+=c.n[i];CHECK(sum==6);
    }
    /* G5 ADPF helpers: clamp to [1us,1ms], at most one report per ms, bounded labels */
    {
        CHECK(t7_adpf_clamp(0)==1000 && t7_adpf_clamp(999)==1000 && t7_adpf_clamp(1000)==1000);
        CHECK(t7_adpf_clamp(500000)==500000 && t7_adpf_clamp(1000000)==1000000 && t7_adpf_clamp(1000001)==1000000);
        CHECK(t7_adpf_clamp(~0ull)==1000000);
        T7AdpfLimiter l{};
        CHECK(t7_adpf_due(&l,5000000));            /* first call always due */
        CHECK(!t7_adpf_due(&l,5000000) && !t7_adpf_due(&l,5999999));
        CHECK(t7_adpf_due(&l,6000000));
        CHECK(!t7_adpf_due(&l,6500000) && t7_adpf_due(&l,7000000));
        CHECK(!t7_adpf_due(&l,6000000));           /* clock never goes backwards: not due */
        T7AdpfLimiter z{};CHECK(t7_adpf_due(&z,0)); /* time 0 still first-due */
        CHECK(!t7_adpf_due(&z,999999) && t7_adpf_due(&z,1000000));
        CHECK(!strcmp(t7_adpf_session_name(T7_ADPF_UNAVAILABLE),"unavailable") && !strcmp(t7_adpf_session_name(T7_ADPF_OK),"ok") &&
               !strcmp(t7_adpf_session_name(T7_ADPF_FAILED),"failed") && !strcmp(t7_adpf_session_name(99),"unavailable"));
        CHECK(t7_adpf_api_ok(33) && t7_adpf_api_ok(35) && !t7_adpf_api_ok(32) && !t7_adpf_api_ok(-1));
    }
    PASS("transport_core parity");
}
