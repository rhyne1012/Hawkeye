#include "wasm/wasm_replay.h"
#include "ulog_events.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sparse_seek(void) {
    ulog_att_event_t att[] = {{1000000,{1,0,0,0}},{11000000,{0,1,0,0}}};
    ulog_lpos_event_t pos[] = {{1000000,1,2,3,0,0,0},{11000000,10,20,30,0,0,0}};
    ulog_aspd_event_t air[] = {{2000000,100,120,0},{12000000,800,900,0}};
    ulog_vstatus_event_t status[] = {{2000000,1,0,3,{0}},{12000000,1,0,4,{0}}};
    ulog_statustext_event_t text[] = {{2000000,1,{0},"early"},{12000000,1,{0},"late"}};
    ulog_timeline_t tl = {.att=att,.att_count=2,.lpos=pos,.lpos_count=2,.aspd=air,.aspd_count=2,
        .vstatus=status,.vstatus_count=2,.statustext=text,.statustext_count=2,
        .start_timestamp_us=1000000,.end_timestamp_us=21000000};
    wasm_replay_ctx_t c = {.timeline=&tl,.ref_set=true,.ref_lat=24,.ref_lon=121,
        .home={.lat=240000000,.lon=1210000000,.valid=true}};
    wasm_replay_seek(&c,15);
    assert(c.current_nav_state==4 && c.state.ind_airspeed==800);
    wasm_replay_seek(&c,5);
    assert(c.current_nav_state==3 && c.state.ind_airspeed==100);
    assert(c.state.quaternion[0]==1 && c.state.quaternion[1]==0);
    assert(c.last_x==1 && c.last_y==2);
    assert(c.home.lat==240000000 && c.home.lon==1210000000);
    assert(c.statustext.count==1 && strcmp(c.statustext.entries[0].text,"early")==0);
    wasm_replay_seek(&c,0);
    assert(c.current_nav_state==0xff && c.state.ind_airspeed==0);
    assert(c.statustext.count==0);
    wasm_replay_seek(&c,100);
    assert(c.wall_accum==20);
    wasm_replay_seek(&c,-100);
    assert(c.wall_accum==0);
    wasm_replay_seek(&c,NAN);
    assert(c.wall_accum==0);
    puts("sparse backward seek, earliest sample, bounds and NaN: PASS");
}

static void real_log(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);
    assert(fseek(f,0,SEEK_END)==0);long len=ftell(f);assert(len>16);rewind(f);
    unsigned char *bytes=malloc((size_t)len);assert(bytes);
    assert(fread(bytes,1,(size_t)len,f)==(size_t)len);fclose(f);
    wasm_replay_ctx_t c;assert(wasm_replay_init_from_bytes(&c,bytes,(size_t)len)==0);
    free(bytes);
    const ulog_timeline_t *tl=c.timeline;
    assert(tl->att_count>0 && tl->lpos_count+tl->gpos_count>0);
    double duration=(double)wasm_replay_duration_us(&c)/1e6;
    // Independent linear lookup through extracted samples checks the binary
    // seek path at repeated, out-of-order destinations (including both ends).
    const double fractions[]={.9,.1,.7,.05,.5,1,0,.99,.25,.01};
    for(int k=0;k<10;k++) {
        float target=(float)(duration*fractions[k]);
        wasm_replay_seek(&c,target);
        uint64_t ts=tl->start_timestamp_us+(uint64_t)(fmin(target,duration)*1e6);
        int ai=-1,vi=-1,si=-1;
        for(int i=0;i<tl->att_count;i++)if(tl->att[i].timestamp_us<=ts)ai=i;
        for(int i=0;i<tl->vstatus_count;i++)if(tl->vstatus[i].timestamp_us<=ts)vi=i;
        for(int i=0;i<tl->aspd_count;i++)if(tl->aspd[i].timestamp_us<=ts)si=i;
        if(ai>=0)for(int i=0;i<4;i++)assert(fabs(c.state.quaternion[i]-tl->att[ai].q[i])<1e-6);
        assert(c.current_nav_state==(vi>=0?tl->vstatus[vi].nav_state:0xff));
        assert(c.state.ind_airspeed==(si>=0?tl->aspd[si].ias_cms:0));
        assert(fabs(c.wall_accum-fmin(target,duration))<1e-5);
    }
    printf("real ULog: %.3f s, %d attitude / %d local position / %d global position events; 10 out-of-order seeks PASS\n",duration,tl->att_count,tl->lpos_count,tl->gpos_count);
    wasm_replay_close(&c);
}
int main(int argc,char **argv){sparse_seek();if(argc==2)real_log(argv[1]);return 0;}
