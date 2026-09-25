#include "ulog_replay.h"
#include "data_source.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Synthetic records only: no flight telemetry or private coordinates.
static const char *path = "synthetic_sparse_replay.ulg";
static const uint64_t start = 1000000;
static void record(FILE *f, char type, const void *p, uint16_t n) {
    unsigned char hdr[] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)type};
    assert(fwrite(hdr, 1, 3, f) == 3);
    assert(fwrite(p, 1, n, f) == n);
}
static void data(FILE *f, uint16_t id, uint64_t ts, const void *p, size_t n) {
    unsigned char buf[128]; assert(n+10 <= sizeof(buf));
    memcpy(buf, &id, 2); memcpy(buf+2, &ts, 8); memcpy(buf+10, p, n);
    record(f, 'D', buf, (uint16_t)(n+10));
}
static void fixture(int optional) {
    FILE *f=fopen(path,"wb"); assert(f);
    const unsigned char magic[]={ 'U','L','o','g',1,0x12,0x35,1 };
    fwrite(magic,1,8,f); fwrite(&start,1,8,f);
    const char *formats[]={
        "vehicle_attitude:uint64_t timestamp;float[4] q;",
        "vehicle_local_position:uint64_t timestamp;float x;float y;float z;float vx;float vy;float vz;",
        "vehicle_global_position:uint64_t timestamp;double lat;double lon;float alt;",
        "home_position:uint64_t timestamp;double lat;double lon;float alt;bool valid_hpos;",
        // Deliberately omit IAS/TAS: missing fields must not read at offset -1.
        "airspeed_validated:uint64_t timestamp;float calibrated_airspeed_m_s;",
        "replay_throttle:uint64_t timestamp;uint64_t timestamp_sample;float throttle_pct;bool valid;"
    };
    int count=optional?6:4;
    for(int i=0;i<count;i++) {
        record(f,'F',formats[i],(uint16_t)strlen(formats[i]));
        unsigned char b[100]={0};uint16_t id=i;memcpy(b+1,&id,2);
        size_t n=strchr(formats[i],':')-formats[i];memcpy(b+3,formats[i],n);
        record(f,'A',b,(uint16_t)(n+3));
    }
    for(int i=0;i<=50;i++) {
        uint64_t t=start+i*200000;
        if(i==5) {
            unsigned char logmsg[13]={4}; memcpy(logmsg+1,&t,8);
            memcpy(logmsg+9,"test",4);record(f,'L',logmsg,13);
        }
        unsigned char b[32];double lat=45,lon=10;float alt=200-i*.2f;
        memcpy(b,&lat,8);memcpy(b+8,&lon,8);float homealt=25;
        memcpy(b+16,&homealt,4);b[20]=1;
        data(f,3,t,b,21);
        float q[]={1,0,0,0};data(f,0,t,q,sizeof(q));
        memcpy(b+16,&alt,4);data(f,2,t,b,20);
        float p[]={1,2,-175+i*.2f,0,0,1};data(f,1,t,p,sizeof(p));
        if(optional) {
            float cas=i==0?-1:i==1?NAN:i==2?0:30;
            data(f,4,t,&cas,4);
            // Throttle stops publishing at 5 s to exercise stale data and seeks.
            if(i<=25) {
                float pct=i<10?0:i<20?50:100;
                if(i==11) pct=NAN;
                if(i==12) pct=101;
                memcpy(b,&t,8);memcpy(b+8,&pct,4);b[12]=1;
                data(f,5,t,b,13);
            }
        }
    }
    fclose(f);
}
static ulog_replay_ctx_t *open_log(void) {
    ulog_replay_ctx_t *c=calloc(1,sizeof(*c));assert(c);
    assert(ulog_replay_init(c,path)==0);return c;
}
static void close_log(ulog_replay_ctx_t *c){ulog_replay_close(c);free(c);}
static void test_clock(int fps,float speed) {
    ulog_replay_ctx_t *c=open_log();int frames=0;bool playing=true;
    while(playing && frames<10000){
        playing=ulog_replay_advance(c,1.0f/fps,speed,false,false);frames++;
        assert(c->state.time_usec <= start+(uint64_t)llround(c->wall_accum*1e6));
        if(c->wall_accum<2) assert(c->state.throttle_pct==0);
    }
    assert(fabs((double)frames/fps-10.0/speed)<=1.01/fps);
    assert(c->state.time_usec==start+10000000); // final sample was consumed
    assert(!c->state.throttle_valid);
    close_log(c);
}
static void test_seek(void) {
    ulog_replay_ctx_t *c=open_log();
    ulog_replay_seek(c,0);assert(c->state.valid);
    assert(c->statustext.count==0);
    assert(c->state.throttle_valid && c->state.throttle_pct==0);
    assert(c->state.calibrated_airspeed_present && !c->state.calibrated_airspeed_valid);
    ulog_replay_seek(c,.2f);assert(!c->state.calibrated_airspeed_valid);
    ulog_replay_seek(c,.4f);assert(c->state.calibrated_airspeed_valid && c->state.calibrated_airspeed==0);
    ulog_replay_seek(c,2);assert(c->statustext.count==1);
    assert(c->state.throttle_pct==50); // all records at t=2
    ulog_replay_seek(c,2.2f);assert(!c->state.throttle_valid);
    ulog_replay_seek(c,2.4f);assert(!c->state.throttle_valid);
    ulog_replay_seek(c,4);assert(c->state.throttle_valid && c->state.throttle_pct==100);
    ulog_replay_seek(c,7);assert(c->state.throttle_valid && c->state.throttle_pct==100);
    ulog_replay_seek(c,8);assert(!c->state.throttle_valid);
    ulog_replay_seek(c,1);assert(c->state.throttle_valid && c->state.throttle_pct==0);
    ulog_replay_seek(c,4);ulog_replay_seek(c,1.9f);
    assert(c->state.throttle_pct==0); // no future state survives a backwards seek
    assert(c->state.vz==100); // VS = -1 m/s (down positive)
    assert(c->home.alt==25000);
    ulog_replay_seek(c,10);
    assert(ulog_replay_advance(c,.1f,1,true,false));
    assert(c->wall_accum==0 && c->state.throttle_pct==0);
    c->time_offset_s=-2;
    ulog_replay_seek(c,0);assert(c->wall_accum==0 && c->state.throttle_pct==0);
    ulog_replay_advance(c,1,1,false,false);
    assert(c->wall_accum==1 && c->state.time_usec==start);
    c->time_offset_s=2;
    ulog_replay_seek(c,0);assert(c->wall_accum==0 && c->state.throttle_pct==50);
    close_log(c);
}
static void test_pause_and_missing(void) {
    data_source_t ds;assert(data_source_ulog_create(&ds,path)==0);
    data_source_seek(&ds,4);ds.playback.paused=true;
    data_source_poll(&ds,30);assert(ds.state.throttle_pct==100 && ds.playback.position_s==4);
    data_source_seek(&ds,1);data_source_poll(&ds,30);
    assert(ds.state.throttle_valid && ds.state.throttle_pct==0);
    data_source_close(&ds);
    fixture(0);ulog_replay_ctx_t *c=open_log();ulog_replay_seek(c,5);
    assert(!c->state.throttle_valid && !c->state.calibrated_airspeed_present);close_log(c);
}
int main(void){
    fixture(1);
    test_clock(30,1);test_clock(60,1);test_clock(144,1);
    test_clock(60,.5f);test_clock(60,2);
    test_seek();test_pause_and_missing();
    assert(remove(path)==0);puts("PASS sparse timing, seek/loop, CAS, THR, pause and missing telemetry");return 0;
}
