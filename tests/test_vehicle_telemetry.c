#include "vehicle.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

static void init(vehicle_t *v) {
    *v=(vehicle_t){0};v->trail_capacity=4;
    v->trail=calloc(4,sizeof(Vector3));
    v->trail_roll=calloc(4,sizeof(float));v->trail_pitch=calloc(4,sizeof(float));
    v->trail_vert=calloc(4,sizeof(float));v->trail_speed=calloc(4,sizeof(float));
    v->trail_time=calloc(4,sizeof(float));
}
static void cleanup(vehicle_t *v) {
    free(v->trail);free(v->trail_roll);free(v->trail_pitch);free(v->trail_vert);
    free(v->trail_speed);free(v->trail_time);
}
int main(void) {
    vehicle_t v;init(&v);
    hil_state_t s={.quaternion={1,0,0,0},.lat=450000000,.lon=100000000,
        .alt=200000,.vz=2270,.valid=true,.calibrated_airspeed=34.7f,
        .calibrated_airspeed_present=true,.calibrated_airspeed_valid=true,
        .throttle_pct=52,.throttle_valid=true};
    home_position_t h={.lat=450000000,.lon=100000000,.alt=25000,.valid=true};
    vehicle_update(&v,&s,&h);
    assert(v.origin_set && fabs(v.alt0-25)<.001);
    assert(fabs(v.altitude_rel-175)<.001 && fabs(v.position.y-175)<.001);
    assert(fabs(v.vertical_speed+22.7)<.001);
    assert(v.airspeed_is_cas && v.airspeed_valid && fabs(v.airspeed-34.7)<.001);
    assert(v.throttle_valid && v.throttle_pct==52);
    s.throttle_pct=0;vehicle_update(&v,&s,&h);assert(v.throttle_valid && v.throttle_pct==0);
    s.throttle_valid=false;s.calibrated_airspeed_valid=false;
    vehicle_update(&v,&s,&h);assert(!v.throttle_valid && !v.airspeed_valid);
    v.alt0=50;v.origin_set=true;vehicle_update(&v,&s,&h);
    assert(v.alt0==50 && v.altitude_rel==150); // explicit -origin is preserved
    s.calibrated_airspeed_present=false;s.ind_airspeed=1234;
    vehicle_update(&v,&s,&h);assert(!v.airspeed_is_cas && fabs(v.airspeed-12.34)<.001);
    s.relative_alt=123000;s.relative_alt_valid=true;
    vehicle_update(&v,&s,&h);
    assert(v.altitude_rel==123 && v.position.y==123); // keep main's live reconnect fix
    cleanup(&v);puts("PASS actual vehicle_update: Home origin, explicit origin, CAS, THR, VS");
    return 0;
}
