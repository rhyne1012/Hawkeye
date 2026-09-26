#include "ulog_replay.h"
#include "ulog_replay_apply.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

// STATUSTEXT logging callback — pushes into ring buffer
static void logging_cb(const ulog_logging_msg_t *msg, void *userdata) {
    ulog_replay_ctx_t *ctx = (ulog_replay_ctx_t *)userdata;
    statustext_ring_t *ring = &ctx->statustext;
    statustext_entry_t *e = &ring->entries[ring->head];
    e->severity = msg->log_level;
    float time_s = (float)((double)(msg->timestamp - ctx->parser.start_timestamp) / 1e6);
    e->time_s = time_s;
    int len = msg->text_len < STATUSTEXT_MSG_MAX - 1 ? msg->text_len : STATUSTEXT_MSG_MAX - 1;
    memcpy(e->text, msg->text, len);
    e->text[len] = '\0';
    ring->head = (ring->head + 1) % STATUSTEXT_RING_SIZE;
    if (ring->count < STATUSTEXT_RING_SIZE) ring->count++;
}

// ---------------------------------------------------------------------------
// Per-message field decode helpers. The event structs (ulog_*_event_t) are
// the interchange format between decode and apply; the same apply helpers
// feed both native (live parser) and WASM (pre-extracted arrays).
// ---------------------------------------------------------------------------

static void decode_attitude(const ulog_data_msg_t *dmsg,
                             const ulog_field_cache_t *cache,
                             ulog_att_event_t *ev) {
    ev->timestamp_us = dmsg->timestamp;
    int off = cache->att_q_offset;
    ev->q[0] = ulog_parser_get_float(dmsg, off + 0);
    ev->q[1] = ulog_parser_get_float(dmsg, off + 4);
    ev->q[2] = ulog_parser_get_float(dmsg, off + 8);
    ev->q[3] = ulog_parser_get_float(dmsg, off + 12);
}

static void decode_gpos(const ulog_data_msg_t *dmsg,
                         const ulog_field_cache_t *cache,
                         ulog_gpos_event_t *ev) {
    ev->timestamp_us = dmsg->timestamp;
    ev->lat_deg = ulog_parser_get_double(dmsg, cache->gpos_lat_offset);
    ev->lon_deg = ulog_parser_get_double(dmsg, cache->gpos_lon_offset);
    ev->alt_m   = ulog_parser_get_float (dmsg, cache->gpos_alt_offset);
    ev->_pad    = 0.0f;
}

static void decode_lpos(const ulog_data_msg_t *dmsg,
                         const ulog_field_cache_t *cache,
                         ulog_lpos_event_t *ev) {
    ev->timestamp_us = dmsg->timestamp;
    ev->x  = ulog_parser_get_float(dmsg, cache->lpos_x_offset);
    ev->y  = ulog_parser_get_float(dmsg, cache->lpos_y_offset);
    ev->z  = ulog_parser_get_float(dmsg, cache->lpos_z_offset);
    ev->vx = ulog_parser_get_float(dmsg, cache->lpos_vx_offset);
    ev->vy = ulog_parser_get_float(dmsg, cache->lpos_vy_offset);
    ev->vz = ulog_parser_get_float(dmsg, cache->lpos_vz_offset);
}

static void decode_aspd(const ulog_data_msg_t *dmsg,
                         const ulog_field_cache_t *cache,
                         ulog_aspd_event_t *ev) {
    ev->timestamp_us = dmsg->timestamp;
    float ias = cache->aspd_ias_offset >= 0
        ? ulog_parser_get_float(dmsg, cache->aspd_ias_offset) : NAN;
    float tas = cache->aspd_tas_offset >= 0
        ? ulog_parser_get_float(dmsg, cache->aspd_tas_offset) : NAN;
    // Validate before unsigned conversion (negative/NaN must not wrap).
    ev->ias_cms = (isfinite(ias) && ias >= 0.0f && ias <= 655.35f)
        ? (uint16_t)(ias * 100.0f) : 0;
    ev->tas_cms = (isfinite(tas) && tas >= 0.0f && tas <= 655.35f)
        ? (uint16_t)(tas * 100.0f) : 0;
    ev->cas_present = cache->aspd_cas_offset >= 0;
    ev->cas_m_s = ev->cas_present
        ? ulog_parser_get_float(dmsg, cache->aspd_cas_offset) : NAN;
    memset(ev->_pad, 0, sizeof(ev->_pad));
}

static void decode_vstatus(const ulog_data_msg_t *dmsg,
                            const ulog_field_cache_t *cache,
                            ulog_vstatus_event_t *ev) {
    ev->timestamp_us = dmsg->timestamp;
    uint8_t px4_type = ulog_parser_get_uint8(dmsg, cache->vstatus_type_offset);
    uint8_t is_vtol  = (cache->vstatus_is_vtol_offset >= 0) &&
                       ulog_parser_get_uint8(dmsg, cache->vstatus_is_vtol_offset);
    ev->is_vtol = is_vtol;

    // PX4 vehicle_type enum → MAVLink MAV_TYPE, matching the inline mapping
    // that used to live in process_message.
    if (is_vtol) {
        ev->vehicle_type = 22; // MAV_TYPE_VTOL_FIXEDROTOR
    } else {
        switch (px4_type) {
            case 1:  ev->vehicle_type = 2;  break; // ROTARY_WING → QUADROTOR
            case 2:  ev->vehicle_type = 1;  break; // FIXED_WING  → FIXED_WING
            case 3:  ev->vehicle_type = 10; break; // ROVER       → GROUND_ROVER
            default: ev->vehicle_type = 2;  break;
        }
    }

    ev->nav_state = (cache->vstatus_nav_state_offset >= 0)
        ? ulog_parser_get_uint8(dmsg, cache->vstatus_nav_state_offset)
        : 0;
    memset(ev->_pad, 0, sizeof(ev->_pad));
}

static void decode_home(const ulog_data_msg_t *dmsg,
                         const ulog_field_cache_t *cache,
                         ulog_home_event_t *ev) {
    ev->timestamp_us = dmsg->timestamp;
    ev->lat_deg = (cache->home_lat_offset >= 0)
        ? ulog_parser_get_double(dmsg, cache->home_lat_offset) : NAN;
    ev->lon_deg = (cache->home_lon_offset >= 0)
        ? ulog_parser_get_double(dmsg, cache->home_lon_offset) : NAN;
    ev->alt_m = (cache->home_alt_offset >= 0)
        ? ulog_parser_get_float(dmsg, cache->home_alt_offset) : NAN;
    ev->valid_alt = cache->home_valid_alt_offset < 0 ||
        ulog_parser_get_uint8(dmsg, cache->home_valid_alt_offset);
    ev->valid_hpos = (cache->home_valid_hpos_offset >= 0)
        ? ulog_parser_get_uint8(dmsg, cache->home_valid_hpos_offset) : 1;
    memset(ev->_pad, 0, sizeof(ev->_pad));
}

// ---------------------------------------------------------------------------
// Process a single data message — dispatch by subscription
// ---------------------------------------------------------------------------

static void process_message(ulog_replay_ctx_t *ctx, const ulog_data_msg_t *dmsg) {
    // Find which subscription this msg_id belongs to
    int sub_idx = -1;
    for (int i = 0; i < ctx->parser.sub_count; i++) {
        if (ctx->parser.subs[i].msg_id == dmsg->msg_id) {
            sub_idx = i;
            break;
        }
    }
    if (sub_idx < 0) return;

    ctx->state.time_usec = dmsg->timestamp;
    // Only mark state valid once we have position data (not just attitude)
    if (ctx->first_pos_set)
        ctx->state.valid = true;

    // home_position topic — authoritative home (Tier 1).
    // Skip if pre-scan rejected this log's home (no GPOS data to confirm it).
    if (sub_idx == ctx->sub_home_pos && ctx->sub_home_pos >= 0) {
        ulog_home_event_t ev;
        decode_home(dmsg, &ctx->cache, &ev);
        ulog_apply_home(ctx, &ev);
    }

    if (sub_idx == ctx->sub_attitude) {
        ulog_att_event_t ev;
        decode_attitude(dmsg, &ctx->cache, &ev);
        ulog_apply_attitude(ctx, &ev);
    }
    else if (ctx->sub_global_pos >= 0 && sub_idx == ctx->sub_global_pos) {
        ulog_gpos_event_t ev;
        decode_gpos(dmsg, &ctx->cache, &ev);
        ulog_apply_gpos(ctx, &ev);
    }
    else if (sub_idx == ctx->sub_local_pos) {
        // LPOS reference-frame capture has to happen with raw parser bytes
        // in hand (xy_global / ref_lat / ref_lon / ref_alt aren't fields on
        // the compact lpos event struct). Done here, before apply_lpos,
        // which reads ctx->ref_* as already-settled state.
        if (!ctx->has_global_pos && !ctx->ref_set &&
            ctx->cache.lpos_xy_global_offset >= 0) {
            uint8_t xy_global = ulog_parser_get_uint8(dmsg, ctx->cache.lpos_xy_global_offset);
            if (xy_global) {
                double rlat = ulog_parser_get_double(dmsg, ctx->cache.lpos_ref_lat_offset);
                double rlon = ulog_parser_get_double(dmsg, ctx->cache.lpos_ref_lon_offset);
                if (rlat < -90.0 || rlat > 90.0 || rlon < -180.0 || rlon > 180.0) {
                    if (!ctx->ref_rejected) {
                        printf("  Warning: LPOS ref out of range (lat=%.1f, lon=%.1f), ignoring\n", rlat, rlon);
                        ctx->ref_rejected = true;
                    }
                } else {
                    ctx->ref_lat = rlat;
                    ctx->ref_lon = rlon;
                    if (ctx->cache.lpos_z_global_offset >= 0 &&
                        ulog_parser_get_uint8(dmsg, ctx->cache.lpos_z_global_offset)) {
                        ctx->ref_alt = ulog_parser_get_float(dmsg, ctx->cache.lpos_ref_alt_offset);
                    }
                    ctx->ref_set = true;
                }
            }
        }

        ulog_lpos_event_t ev;
        decode_lpos(dmsg, &ctx->cache, &ev);
        ulog_apply_lpos(ctx, &ev);
    }
    else if (ctx->sub_airspeed >= 0 && sub_idx == ctx->sub_airspeed) {
        ulog_aspd_event_t ev;
        decode_aspd(dmsg, &ctx->cache, &ev);
        ulog_apply_aspd(ctx, &ev);
    }
    else if (ctx->sub_throttle >= 0 && sub_idx == ctx->sub_throttle) {
        ulog_throttle_event_t ev = {0};
        ev.timestamp_us = dmsg->timestamp;
        ev.pct = ctx->cache.throttle_pct_offset >= 0
            ? ulog_parser_get_float(dmsg, ctx->cache.throttle_pct_offset) : NAN;
        ev.valid = ctx->cache.throttle_valid_offset >= 0 &&
            ulog_parser_get_uint8(dmsg, ctx->cache.throttle_valid_offset);
        ev.sample_timestamp_us = ctx->cache.throttle_sample_offset >= 0
            ? ulog_parser_get_uint64(dmsg, ctx->cache.throttle_sample_offset)
            : dmsg->timestamp;
        ulog_apply_throttle(ctx, &ev);
    }
    else if (ctx->sub_vehicle_status >= 0 && sub_idx == ctx->sub_vehicle_status) {
        ulog_vstatus_event_t ev;
        decode_vstatus(dmsg, &ctx->cache, &ev);
        ulog_apply_vstatus(ctx, &ev);
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

int ulog_replay_init(ulog_replay_ctx_t *ctx, const char *filepath) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->sub_attitude = -1;
    ctx->sub_global_pos = -1;
    ctx->sub_local_pos = -1;
    ctx->sub_airspeed = -1;
    ctx->sub_vehicle_status = -1;
    ctx->sub_home_pos = -1;

    int ret = ulog_parser_open(&ctx->parser, filepath);
    if (ret != 0) return ret;

    // Find subscriptions
    ctx->sub_attitude = ulog_parser_find_subscription(&ctx->parser, "vehicle_attitude");
    ctx->sub_global_pos = ulog_parser_find_subscription(&ctx->parser, "vehicle_global_position");
    ctx->sub_local_pos = ulog_parser_find_subscription(&ctx->parser, "vehicle_local_position");
    ctx->sub_airspeed = ulog_parser_find_subscription(&ctx->parser, "airspeed_validated");
    ctx->sub_vehicle_status = ulog_parser_find_subscription(&ctx->parser, "vehicle_status");
    ctx->sub_home_pos = ulog_parser_find_subscription(&ctx->parser, "home_position");
    ctx->sub_throttle = ulog_parser_find_subscription(&ctx->parser, "replay_throttle");

    // Required topics
    if (ctx->sub_attitude < 0) {
        fprintf(stderr, "ulog_replay: vehicle_attitude topic not found\n");
        ulog_parser_close(&ctx->parser);
        return -1;
    }
    if (ctx->sub_local_pos < 0) {
        fprintf(stderr, "ulog_replay: vehicle_local_position topic not found\n");
        ulog_parser_close(&ctx->parser);
        return -1;
    }

    // has_global_pos starts false and gets set to true when first gpos DATA arrives
    ctx->has_global_pos = false;

    // Cache field offsets for vehicle_attitude
    int att_fmt = ctx->parser.subs[ctx->sub_attitude].format_idx;
    ctx->cache.att_q_offset = ulog_parser_find_field(&ctx->parser, att_fmt, "q");

    // Cache field offsets for vehicle_global_position (if subscription exists)
    if (ctx->sub_global_pos >= 0) {
        int gpos_fmt = ctx->parser.subs[ctx->sub_global_pos].format_idx;
        ctx->cache.gpos_lat_offset = ulog_parser_find_field(&ctx->parser, gpos_fmt, "lat");
        ctx->cache.gpos_lon_offset = ulog_parser_find_field(&ctx->parser, gpos_fmt, "lon");
        ctx->cache.gpos_alt_offset = ulog_parser_find_field(&ctx->parser, gpos_fmt, "alt");
    }

    // Cache field offsets for vehicle_local_position
    int lpos_fmt = ctx->parser.subs[ctx->sub_local_pos].format_idx;
    ctx->cache.lpos_x_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "x");
    ctx->cache.lpos_y_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "y");
    ctx->cache.lpos_z_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "z");
    ctx->cache.lpos_vx_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "vx");
    ctx->cache.lpos_vy_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "vy");
    ctx->cache.lpos_vz_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "vz");
    ctx->cache.lpos_ref_lat_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "ref_lat");
    ctx->cache.lpos_ref_lon_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "ref_lon");
    ctx->cache.lpos_ref_alt_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "ref_alt");
    ctx->cache.lpos_xy_global_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "xy_global");
    ctx->cache.lpos_z_global_offset = ulog_parser_find_field(&ctx->parser, lpos_fmt, "z_global");

    // Optional: airspeed
    if (ctx->sub_airspeed >= 0) {
        int aspd_fmt = ctx->parser.subs[ctx->sub_airspeed].format_idx;
        ctx->cache.aspd_ias_offset = ulog_parser_find_field(&ctx->parser, aspd_fmt, "indicated_airspeed_m_s");
        ctx->cache.aspd_tas_offset = ulog_parser_find_field(&ctx->parser, aspd_fmt, "true_airspeed_m_s");
        ctx->cache.aspd_cas_offset = ulog_parser_find_field(&ctx->parser, aspd_fmt, "calibrated_airspeed_m_s");
    }
    if (ctx->sub_throttle >= 0) {
        int fmt = ctx->parser.subs[ctx->sub_throttle].format_idx;
        ctx->cache.throttle_pct_offset = ulog_parser_find_field(&ctx->parser, fmt, "throttle_pct");
        ctx->cache.throttle_valid_offset = ulog_parser_find_field(&ctx->parser, fmt, "valid");
        ctx->cache.throttle_sample_offset = ulog_parser_find_field(&ctx->parser, fmt, "timestamp_sample");
    }

    // Optional: vehicle_status
    if (ctx->sub_vehicle_status >= 0) {
        int vs_fmt = ctx->parser.subs[ctx->sub_vehicle_status].format_idx;
        ctx->cache.vstatus_type_offset = ulog_parser_find_field(&ctx->parser, vs_fmt, "vehicle_type");
        ctx->cache.vstatus_is_vtol_offset = ulog_parser_find_field(&ctx->parser, vs_fmt, "is_vtol");
        ctx->cache.vstatus_nav_state_offset = ulog_parser_find_field(&ctx->parser, vs_fmt, "nav_state");
    }

    // Optional: home_position
    if (ctx->sub_home_pos >= 0) {
        int hp_fmt = ctx->parser.subs[ctx->sub_home_pos].format_idx;
        ctx->cache.home_lat_offset = ulog_parser_find_field(&ctx->parser, hp_fmt, "lat");
        ctx->cache.home_lon_offset = ulog_parser_find_field(&ctx->parser, hp_fmt, "lon");
        ctx->cache.home_alt_offset = ulog_parser_find_field(&ctx->parser, hp_fmt, "alt");
        ctx->cache.home_valid_hpos_offset = ulog_parser_find_field(&ctx->parser, hp_fmt, "valid_hpos");
        ctx->cache.home_valid_alt_offset = ulog_parser_find_field(&ctx->parser, hp_fmt, "valid_alt");
    }

    // Pre-scan for vehicle_type, flight mode transitions, and home position
    ctx->vehicle_type = 2;  // default MAV_TYPE_QUADROTOR
    ctx->current_nav_state = 0xFF;
    ctx->mode_change_count = 0;
    {
        bool got_type = false;
        bool got_home = false;
        bool seen_gpos_data = false;
        uint8_t prev_nav = 0xFF;

        uint16_t vstatus_msg_id = (ctx->sub_vehicle_status >= 0)
            ? ctx->parser.subs[ctx->sub_vehicle_status].msg_id : 0xFFFF;
        uint16_t home_msg_id = (ctx->sub_home_pos >= 0)
            ? ctx->parser.subs[ctx->sub_home_pos].msg_id : 0xFFFF;
        uint16_t gpos_msg_id = (ctx->sub_global_pos >= 0)
            ? ctx->parser.subs[ctx->sub_global_pos].msg_id : 0xFFFF;
        uint16_t lpos_msg_id = (ctx->sub_local_pos >= 0)
            ? ctx->parser.subs[ctx->sub_local_pos].msg_id : 0xFFFF;

        // CUSUM state for takeoff detection
        float cusum_s = 0.0f;
        const float cusum_k = 0.3f;    // drift allowance (m/s) — below this is noise
        const float cusum_h = 2.0f;    // decision threshold
        bool  cusum_triggered = false;
        float cusum_trigger_time = -1.0f;
        float cusum_peak = 0.0f;

        ulog_data_msg_t scan_msg;
        while (ulog_parser_next(&ctx->parser, &scan_msg)) {
            // Vehicle status: type + nav_state transitions
            if (scan_msg.msg_id == vstatus_msg_id && ctx->sub_vehicle_status >= 0) {
                if (!got_type) {
                    uint8_t px4_type = ulog_parser_get_uint8(&scan_msg, ctx->cache.vstatus_type_offset);
                    bool is_vtol = ctx->cache.vstatus_is_vtol_offset >= 0 &&
                                   ulog_parser_get_uint8(&scan_msg, ctx->cache.vstatus_is_vtol_offset);
                    if (is_vtol) {
                        ctx->vehicle_type = 22;
                    } else {
                        switch (px4_type) {
                            case 1: ctx->vehicle_type = 2;  break;
                            case 2: ctx->vehicle_type = 1;  break;
                            case 3: ctx->vehicle_type = 10; break;
                            default: ctx->vehicle_type = 2; break;
                        }
                    }
                    got_type = true;
                }
                if (ctx->cache.vstatus_nav_state_offset >= 0) {
                    uint8_t nav = ulog_parser_get_uint8(&scan_msg, ctx->cache.vstatus_nav_state_offset);
                    if (nav != prev_nav && ctx->mode_change_count < ULOG_MAX_MODE_CHANGES) {
                        float t = (float)((double)(scan_msg.timestamp - ctx->parser.start_timestamp) / 1e6);
                        ctx->mode_changes[ctx->mode_change_count].time_s = t;
                        ctx->mode_changes[ctx->mode_change_count].nav_state = nav;
                        ctx->mode_change_count++;
                        prev_nav = nav;
                    }
                }
            }

            // Home position: Tier 1 (highest priority)
            // Require valid_hpos. After the scan, we also verify that GPOS data
            // actually exists — subscription alone isn't enough (PX4 subscribes
            // even when EKF never produces data).
            if (!ctx->home_from_topic && scan_msg.msg_id == home_msg_id) {
                ulog_home_event_t ev;
                decode_home(&scan_msg, &ctx->cache, &ev);
                if (ulog_home_event_valid(&ev)) {
                    ctx->home = (home_position_t){
                        .lat=(int32_t)(ev.lat_deg*1e7), .lon=(int32_t)(ev.lon_deg*1e7),
                        .alt=(int32_t)(ev.alt_m*1000.0f), .valid=true};
                    ctx->home_from_topic = true;
                    got_home = true;
                }
            }

            // CUSUM takeoff detection on upward velocity
            if (!cusum_triggered && scan_msg.msg_id == lpos_msg_id && ctx->cache.lpos_vz_offset >= 0) {
                float vz = ulog_parser_get_float(&scan_msg, ctx->cache.lpos_vz_offset);
                float up = -vz;  // NED: -vz = upward
                cusum_s += (up - cusum_k);
                if (cusum_s < 0.0f) cusum_s = 0.0f;
                if (cusum_s > cusum_h) {
                    cusum_trigger_time = (float)((double)(scan_msg.timestamp - ctx->parser.start_timestamp) / 1e6);
                    cusum_peak = cusum_s;
                    cusum_triggered = true;
                }
            }

            // GPOS: track whether any data exists, and use as Tier 2 fallback
            if (scan_msg.msg_id == gpos_msg_id && gpos_msg_id != 0xFFFF)
                seen_gpos_data = true;
            if (!got_home && scan_msg.msg_id == gpos_msg_id) {
                double lat = ulog_parser_get_double(&scan_msg, ctx->cache.gpos_lat_offset);
                double lon = ulog_parser_get_double(&scan_msg, ctx->cache.gpos_lon_offset);
                float alt = ulog_parser_get_float(&scan_msg, ctx->cache.gpos_alt_offset);
                if (lat != 0.0 || lon != 0.0) {
                    ctx->home.lat = (int32_t)(lat * 1e7);
                    ctx->home.lon = (int32_t)(lon * 1e7);
                    ctx->home.alt = (int32_t)(alt * 1000.0f);
                    ctx->home.valid = true;
                    got_home = true;
                }
            }
        }

        // If home came from home_position topic but no GPOS data exists,
        // the position is baro-only and unreliable as an absolute datum.
        if (got_home && ctx->home_from_topic && !seen_gpos_data) {
            ctx->home.valid = false;
            ctx->home_from_topic = false;
            ctx->home_rejected = true;
        }

        // Store CUSUM results
        ctx->takeoff_detected = cusum_triggered;
        ctx->takeoff_time_s = cusum_triggered ? cusum_trigger_time : 0.0f;
        ctx->time_offset_s = 0.0;

        // Confidence: CUSUM sharpness + nav_state corroboration
        float conf = 0.0f;
        if (cusum_triggered) {
            float sharpness = (cusum_peak - cusum_h) / 8.0f;
            if (sharpness < 0.0f) sharpness = 0.0f;
            if (sharpness > 1.0f) sharpness = 1.0f;
            conf = 0.3f + 0.4f * sharpness;  // 0.3–0.7 from CUSUM alone

            // Corroboration: check if a flight nav_state was active at CUSUM trigger
            // Find the mode that was active at cusum_trigger_time
            uint8_t active_ns = 0xFF;
            for (int m = ctx->mode_change_count - 1; m >= 0; m--) {
                if (ctx->mode_changes[m].time_s <= cusum_trigger_time) {
                    active_ns = ctx->mode_changes[m].nav_state;
                    break;
                }
            }
            // Takeoff(17) Mission(3) VTOL-Takeoff(22) Offboard(14) Position(2)
            if (active_ns == 17 || active_ns == 3 || active_ns == 22 ||
                active_ns == 14 || active_ns == 2) {
                conf += 0.3f;
            }
            if (conf > 1.0f) conf = 1.0f;
        } else {
            // No CUSUM trigger — check if already airborne at log start
            if (ctx->mode_change_count > 0) {
                uint8_t ns = ctx->mode_changes[0].nav_state;
                if (ns == 3 || ns == 4 || ns == 14)
                    conf = 0.5f;
            }
        }
        ctx->takeoff_conf = conf;

        if (ctx->mode_change_count > 0)
            ctx->current_nav_state = ctx->mode_changes[0].nav_state;
        ulog_parser_rewind(&ctx->parser);
    }

    ctx->initial_home = ctx->home;
    ctx->initial_home_from_topic = ctx->home_from_topic;

    // Enable STATUSTEXT logging message capture (after pre-scan so the ring
    // only fills with messages encountered during actual playback, not during
    // the full-file pre-scan pass above).
    ulog_parser_set_logging_callback(&ctx->parser, logging_cb, ctx);

    float dur = (float)((double)(ctx->parser.end_timestamp - ctx->parser.start_timestamp) / 1e6);
    printf("ULog replay: %s (%.1fs, %d index entries)\n", filepath, dur, ctx->parser.index_count);
    if (ctx->mode_change_count > 0) {
        printf("  Flight modes:");
        for (int i = 0; i < ctx->mode_change_count; i++) {
            printf(" %s@%.0fs", ulog_nav_state_name(ctx->mode_changes[i].nav_state),
                   ctx->mode_changes[i].time_s);
        }
        printf("\n");
    }
    if (ctx->takeoff_detected)
        printf("  Takeoff: %.1fs (CUSUM conf=%.0f%%)\n", ctx->takeoff_time_s, ctx->takeoff_conf * 100);
    else
        printf("  Takeoff: not detected (offset=0)\n");
    if (ctx->sub_global_pos < 0) printf("  Note: vehicle_global_position not found (using local position)\n");
    if (ctx->sub_airspeed < 0) printf("  Note: airspeed_validated not found (no airspeed data)\n");
    if (ctx->sub_vehicle_status < 0) printf("  Note: vehicle_status not found (defaulting to quadrotor)\n");
    if (ctx->sub_home_pos < 0) printf("  Note: home_position not found (deriving from first GPS fix)\n");

    return 0;
}

// ---------------------------------------------------------------------------
// Advance playback
// ---------------------------------------------------------------------------

// Seeking/looping must rebuild held values, never retain a future throttle,
// attitude, or velocity when a sparse topic has no sample near the target.
static void reset_playback(ulog_replay_ctx_t *ctx) {
    ulog_parser_rewind(&ctx->parser);
    memset(&ctx->state, 0, sizeof(ctx->state));
    memset(&ctx->statustext, 0, sizeof(ctx->statustext));
    ctx->home = ctx->initial_home;
    ctx->home_from_topic = ctx->initial_home_from_topic;
    ctx->current_nav_state = 0xFF;
    ctx->first_pos_set = ctx->has_global_pos = ctx->ref_set = false;
    ctx->ref_lat = ctx->ref_lon = ctx->ref_alt = 0;
    ctx->prev_gpos_usec = ctx->last_pos_usec = ctx->throttle_sample_usec = 0;
    ctx->gpos_vx = ctx->gpos_vy = ctx->gpos_vz = 0;
    ctx->last_vx = ctx->last_vy = ctx->last_vz = 0;
    ctx->last_x = ctx->last_y = ctx->last_z = 0;
    ctx->last_lat_deg = ctx->last_lon_deg = ctx->last_alt_m = 0;
}

static void read_until(ulog_replay_ctx_t *ctx, uint64_t target) {
    ulog_data_msg_t msg;
    while (ulog_parser_next_until(&ctx->parser, &msg, target))
        process_message(ctx, &msg);
    ulog_expire_throttle(ctx, target);
}

bool ulog_replay_advance(ulog_replay_ctx_t *ctx, float dt, float speed, bool looping, bool interpolation) {
    if (isfinite(dt) && isfinite(speed) && dt > 0.0f && speed > 0.0f)
        ctx->wall_accum += (double)dt * speed;
    double effective_time = ctx->wall_accum + ctx->time_offset_s;
    if (effective_time < 0.0) effective_time = 0.0;
    uint64_t target = ctx->parser.start_timestamp + (uint64_t)llround(effective_time * 1e6);
    bool at_end = target >= ctx->parser.end_timestamp;
    if (at_end) target = ctx->parser.end_timestamp;

    // Consume every due record, including all topics at the same timestamp.
    // Leave future records unread instead of advancing one record per frame.
    read_until(ctx, target);

    // Dead-reckon position forward from last sample using velocity.
    // This smooths the 5-10 Hz position updates to match the render rate.
    if (interpolation && ctx->last_pos_usec > 0 && target > ctx->last_pos_usec) {
        float dt_pos = (float)((double)(target - ctx->last_pos_usec) / 1e6);
        // Clamp extrapolation to avoid runaway if data is stale
        if (dt_pos > 0.5f) dt_pos = 0.5f;

        if (ctx->has_global_pos) {
            // Dead-reckon in global coords using velocity.
            // Prefer LPOS velocity if non-zero, otherwise use GPOS-derived velocity.
            float vx = ctx->last_vx, vy = ctx->last_vy, vz = ctx->last_vz;
            if (vx == 0.0f && vy == 0.0f && vz == 0.0f) {
                vx = ctx->gpos_vx;
                vy = ctx->gpos_vy;
                vz = ctx->gpos_vz;
            }

            double meters_per_deg_lat = 111132.92;
            double meters_per_deg_lon = 111132.92 * cos(ctx->last_lat_deg * ULOG_DEG_TO_RAD);
            if (meters_per_deg_lon < 1.0) meters_per_deg_lon = 1.0;

            double lat = ctx->last_lat_deg + (double)(vx * dt_pos) / meters_per_deg_lat;
            double lon = ctx->last_lon_deg + (double)(vy * dt_pos) / meters_per_deg_lon;
            float alt = ctx->last_alt_m - vz * dt_pos;

            ctx->state.lat = (int32_t)(lat * 1e7);
            ctx->state.lon = (int32_t)(lon * 1e7);
            ctx->state.alt = (int32_t)(alt * 1000.0f);
        } else {
            // Dead-reckon in local NED, then convert
            float x = ctx->last_x + ctx->last_vx * dt_pos;
            float y = ctx->last_y + ctx->last_vy * dt_pos;
            float z = ctx->last_z + ctx->last_vz * dt_pos;

            ulog_local_to_global(ctx->ref_lat, ctx->ref_lon, ctx->ref_alt,
                                  x, y, z,
                                  &ctx->state.lat, &ctx->state.lon, &ctx->state.alt);
        }
    }

    if (at_end) {
        if (looping) {
            ulog_replay_seek(ctx, 0.0f);
            return true;
        }
        return false;
    }
    return true;
}

// Replay from the beginning on a seek. Index-only seeks cannot recover held
// optional topics that were last published outside the small index window.
// Sequential reads are cheap for short telemetry clips; correctness takes
// priority over seek latency for large logs until state checkpoints exist.
void ulog_replay_seek(ulog_replay_ctx_t *ctx, float target_s) {
    if (!isfinite(target_s) || target_s < 0.0f) target_s = 0.0f;
    reset_playback(ctx);
    // Keep the playhead in shared wall-clock space, even when an aligned log
    // starts later. advance() clamps only the local file target.
    ctx->wall_accum = (double)target_s;
    ulog_replay_advance(ctx, 0.0f, 1.0f, false, true);
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void ulog_replay_close(ulog_replay_ctx_t *ctx) {
    ulog_parser_close(&ctx->parser);
}
