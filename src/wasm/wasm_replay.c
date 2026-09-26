// WASM replay path. Consumes pre-extracted events from a ulog_timeline_t via
// cursor iteration and calls the shared apply helpers (src/ulog_replay_apply)
// to update replay state. The context type is ulog_replay_ctx_t — same struct
// as native, with timeline + cursor fields gated behind #ifdef __EMSCRIPTEN__.

#include "wasm_replay.h"
#include "ulog_extractor.h"
#include "ulog_timeline.h"
#include "ulog_events.h"
#include "ulog_replay_apply.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ulog_timeline_t *tl_of(wasm_replay_ctx_t *ctx) {
    return (ulog_timeline_t *)ctx->timeline;
}

static const ulog_timeline_t *ctl_of(const wasm_replay_ctx_t *ctx) {
    return (const ulog_timeline_t *)ctx->timeline;
}

// ---------------------------------------------------------------------------
// STATUSTEXT ring push — WASM-only. Native has an equivalent tiny helper
// inside ulog_replay.c's logging_cb (the ring-write logic is 8 lines; not
// worth a shared-file hop for such a small widget). The inputs here come
// from a pre-extracted ulog_statustext_event_t plus the timeline's log
// start, which is the moral equivalent of native's parser.start_timestamp.
// ---------------------------------------------------------------------------

static void apply_statustext(wasm_replay_ctx_t *ctx,
                              const ulog_statustext_event_t *ev,
                              uint64_t log_start_us) {
    statustext_ring_t *ring = &ctx->statustext;
    statustext_entry_t *e = &ring->entries[ring->head];
    e->severity = ev->severity;
    float time_s = (float)((double)(ev->timestamp_us - log_start_us) / 1e6);
    e->time_s = time_s;
    int len = (int)strnlen(ev->text, STATUSTEXT_MSG_MAX - 1);
    memcpy(e->text, ev->text, len);
    e->text[len] = '\0';
    ring->head = (ring->head + 1) % STATUSTEXT_RING_SIZE;
    if (ring->count < STATUSTEXT_RING_SIZE) ring->count++;
}

// ---------------------------------------------------------------------------
// Cursor advance: walk every per-type cursor forward to the target timestamp,
// calling the shared apply helper for each event consumed.
// ---------------------------------------------------------------------------

static void advance_cursors_to(wasm_replay_ctx_t *ctx, uint64_t target_us) {
    const ulog_timeline_t *tl = ctl_of(ctx);

    while (ctx->att_cursor < tl->att_count &&
           tl->att[ctx->att_cursor].timestamp_us <= target_us) {
        ulog_apply_attitude(ctx, &tl->att[ctx->att_cursor++]);
    }
    while (ctx->lpos_cursor < tl->lpos_count &&
           tl->lpos[ctx->lpos_cursor].timestamp_us <= target_us) {
        ulog_apply_lpos(ctx, &tl->lpos[ctx->lpos_cursor++]);
    }
    while (ctx->gpos_cursor < tl->gpos_count &&
           tl->gpos[ctx->gpos_cursor].timestamp_us <= target_us) {
        ulog_apply_gpos(ctx, &tl->gpos[ctx->gpos_cursor++]);
    }
    while (ctx->aspd_cursor < tl->aspd_count &&
           tl->aspd[ctx->aspd_cursor].timestamp_us <= target_us) {
        ulog_apply_aspd(ctx, &tl->aspd[ctx->aspd_cursor++]);
    }
    while (ctx->vstatus_cursor < tl->vstatus_count &&
           tl->vstatus[ctx->vstatus_cursor].timestamp_us <= target_us) {
        ulog_apply_vstatus(ctx, &tl->vstatus[ctx->vstatus_cursor++]);
    }
    while (ctx->home_cursor < tl->home_count &&
           tl->home[ctx->home_cursor].timestamp_us <= target_us) {
        ulog_apply_home(ctx, &tl->home[ctx->home_cursor++]);
    }
    while (ctx->statustext_cursor < tl->statustext_count &&
           tl->statustext[ctx->statustext_cursor].timestamp_us <= target_us) {
        apply_statustext(ctx, &tl->statustext[ctx->statustext_cursor++],
                          tl->start_timestamp_us);
    }
}

// Dead-reckon state.lat/lon/alt forward from the last position sample to
// target_us using velocity. Mirrors the interpolation block in native
// ulog_replay.c so WASM playback animates smoothly between LPOS samples at
// render rate.
static void interpolate_position(wasm_replay_ctx_t *ctx, uint64_t target_us) {
    if (ctx->last_pos_usec == 0 || target_us <= ctx->last_pos_usec) return;

    float dt_pos = (float)((double)(target_us - ctx->last_pos_usec) / 1e6);
    if (dt_pos > 0.5f) dt_pos = 0.5f;  // clamp runaway if data is stale

    if (ctx->has_global_pos) {
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
        float  alt = ctx->last_alt_m - vz * dt_pos;

        ctx->state.lat = (int32_t)(lat * 1e7);
        ctx->state.lon = (int32_t)(lon * 1e7);
        ctx->state.alt = (int32_t)(alt * 1000.0f);
    } else {
        float x = ctx->last_x + ctx->last_vx * dt_pos;
        float y = ctx->last_y + ctx->last_vy * dt_pos;
        float z = ctx->last_z + ctx->last_vz * dt_pos;
        ulog_local_to_global(ctx->ref_lat, ctx->ref_lon, ctx->ref_alt,
                              x, y, z,
                              &ctx->state.lat, &ctx->state.lon, &ctx->state.alt);
    }
    ctx->state.time_usec = target_us;
}

static bool any_cursor_remaining(const wasm_replay_ctx_t *ctx) {
    const ulog_timeline_t *tl = ctl_of(ctx);
    return ctx->att_cursor     < tl->att_count     ||
           ctx->lpos_cursor    < tl->lpos_count    ||
           ctx->gpos_cursor    < tl->gpos_count    ||
           ctx->aspd_cursor    < tl->aspd_count    ||
           ctx->vstatus_cursor < tl->vstatus_count ||
           ctx->home_cursor    < tl->home_count;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int wasm_replay_init_from_bytes(wasm_replay_ctx_t *ctx,
                                 const uint8_t *buf, size_t len) {
    memset(ctx, 0, sizeof(*ctx));

    ulog_timeline_t *tl = calloc(1, sizeof(*tl));
    if (!tl) return -1;
    ulog_timeline_init(tl);
    ctx->timeline = tl;

    ulog_extract_result_t result;
    memset(&result, 0, sizeof(result));
    result.timeline = tl;

    if (ulog_extract(buf, len, &result) != 0) {
        ulog_timeline_free(tl);
        free(tl);
        ctx->timeline = NULL;
        return -1;
    }

    // Copy pre-scan state from the extractor result to the replay ctx.
    ctx->vehicle_type       = result.vehicle_type;
    ctx->current_nav_state  = result.current_nav_state;
    ctx->mode_change_count  = result.mode_change_count;
    memcpy(ctx->mode_changes, result.mode_changes,
           sizeof(ctx->mode_changes[0]) * (size_t)result.mode_change_count);
    ctx->home               = result.home;
    ctx->home_from_topic    = result.home_from_topic != 0;
    ctx->home_rejected      = result.home_rejected != 0;
    ctx->ref_lat            = result.ref_lat_deg;
    ctx->ref_lon            = result.ref_lon_deg;
    ctx->ref_alt            = result.ref_alt_m;
    ctx->ref_set            = result.ref_set != 0;
    ctx->ref_rejected       = result.ref_rejected != 0;
    ctx->takeoff_detected   = result.takeoff_detected != 0;
    ctx->takeoff_time_s     = result.takeoff_time_s;
    ctx->takeoff_conf       = result.takeoff_conf;
    ctx->time_offset_s      = 0.0;
    ctx->has_global_pos     = false;
    ctx->first_pos_set      = false;
    ctx->wall_accum         = 0.0;

    // Log summary (matches native's init-time print)
    uint64_t dur_us = tl->end_timestamp_us - tl->start_timestamp_us;
    float dur = (float)((double)dur_us / 1e6);
    printf("WASM replay: %d att, %d lpos, %d gpos events (%.1fs)\n",
           tl->att_count, tl->lpos_count, tl->gpos_count, dur);
    return 0;
}

bool wasm_replay_advance(wasm_replay_ctx_t *ctx, float dt, float speed,
                          bool looping, bool interpolation) {
    const ulog_timeline_t *tl = ctl_of(ctx);
    if (!tl || tl->start_timestamp_us == 0) return false;

    // Convert accumulated wall-clock time to a target log-relative timestamp,
    // then to an absolute log timestamp.
    ctx->wall_accum += (double)(dt * speed);
    double log_rel_s = ctx->wall_accum - ctx->time_offset_s;
    uint64_t dur_us = tl->end_timestamp_us - tl->start_timestamp_us;

    if (log_rel_s < 0.0) {
        return true;  // still waiting for our aligned start
    }

    uint64_t rel_us = (uint64_t)(log_rel_s * 1e6);

    if (rel_us >= dur_us) {
        if (looping) {
            ctx->wall_accum = ctx->time_offset_s;
            ctx->att_cursor = ctx->lpos_cursor = ctx->gpos_cursor = 0;
            ctx->aspd_cursor = ctx->vstatus_cursor = ctx->home_cursor = 0;
            ctx->statustext_cursor = 0;
            ctx->has_global_pos = false;
            ctx->first_pos_set  = false;
            ctx->prev_gpos_usec = 0;
            ctx->last_pos_usec  = 0;
            ctx->last_vx = ctx->last_vy = ctx->last_vz = 0.0f;
            ctx->gpos_vx = ctx->gpos_vy = ctx->gpos_vz = 0.0f;
            return true;
        }
        rel_us = dur_us;
    }

    uint64_t target_us = tl->start_timestamp_us + rel_us;
    advance_cursors_to(ctx, target_us);

    if (interpolation) interpolate_position(ctx, target_us);

    return any_cursor_remaining(ctx) || rel_us < dur_us;
}

void wasm_replay_seek(wasm_replay_ctx_t *ctx, float target_s) {
    const ulog_timeline_t *tl = ctl_of(ctx);
    if (!tl || tl->start_timestamp_us == 0) return;
    if (!isfinite(target_s)) return;
    double duration = (double)(tl->end_timestamp_us - tl->start_timestamp_us) / 1e6;
    double target = fmax(0.0, fmin((double)target_s, duration));
    uint64_t target_us = tl->start_timestamp_us + (uint64_t)(target * 1e6);

    // Rebuild from the latest sample of each topic, independent of the old
    // playhead. A one-second forward scan alone leaves sparse status/airspeed
    // topics (and data before their first sample) stuck in the future.
    home_position_t saved_home = ctx->home;
    memset(&ctx->state, 0, sizeof(ctx->state));
    ctx->state.quaternion[0] = 1.0f;
    ctx->current_nav_state = 0xff;
    ctx->has_global_pos = false;
    ctx->first_pos_set = false;
    ctx->last_pos_usec = ctx->prev_gpos_usec = 0;
    ctx->last_vx = ctx->last_vy = ctx->last_vz = 0.0f;
    ctx->gpos_vx = ctx->gpos_vy = ctx->gpos_vz = 0.0f;
    memset(&ctx->statustext, 0, sizeof(ctx->statustext));

    int att_idx        = ulog_timeline_find_att_at       (tl, target_us);
    int lpos_idx       = ulog_timeline_find_lpos_at      (tl, target_us);
    int gpos_idx       = ulog_timeline_find_gpos_at      (tl, target_us);
    int aspd_idx       = ulog_timeline_find_aspd_at      (tl, target_us);
    int vstatus_idx    = ulog_timeline_find_vstatus_at   (tl, target_us);
    int home_idx       = ulog_timeline_find_home_at      (tl, target_us);
    int statustext_idx = ulog_timeline_find_statustext_at(tl, target_us);

    if (lpos_idx >= 0) ulog_apply_lpos(ctx, &tl->lpos[lpos_idx]);
    // Two global samples are needed for derived velocity in GPS-only logs.
    if (gpos_idx > 0) ulog_apply_gpos(ctx, &tl->gpos[gpos_idx - 1]);
    if (gpos_idx >= 0) ulog_apply_gpos(ctx, &tl->gpos[gpos_idx]);
    // Preserve a fallback origin resolved during extraction; seeking must
    // not move the flight's home to the current playhead's position.
    if (saved_home.valid) ctx->home = saved_home;
    if (att_idx >= 0) ulog_apply_attitude(ctx, &tl->att[att_idx]);
    if (aspd_idx >= 0) ulog_apply_aspd(ctx, &tl->aspd[aspd_idx]);
    if (vstatus_idx >= 0) ulog_apply_vstatus(ctx, &tl->vstatus[vstatus_idx]);
    if (home_idx >= 0) ulog_apply_home(ctx, &tl->home[home_idx]);
    int text_start = statustext_idx - STATUSTEXT_RING_SIZE + 1;
    if (text_start < 0) text_start = 0;
    for (int i = text_start; i <= statustext_idx; i++)
        apply_statustext(ctx, &tl->statustext[i], tl->start_timestamp_us);

    ctx->att_cursor        = att_idx        < 0 ? 0 : att_idx + 1;
    ctx->lpos_cursor       = lpos_idx       < 0 ? 0 : lpos_idx + 1;
    ctx->gpos_cursor       = gpos_idx       < 0 ? 0 : gpos_idx + 1;
    ctx->aspd_cursor       = aspd_idx       < 0 ? 0 : aspd_idx + 1;
    ctx->vstatus_cursor    = vstatus_idx    < 0 ? 0 : vstatus_idx + 1;
    ctx->home_cursor       = home_idx       < 0 ? 0 : home_idx + 1;
    ctx->statustext_cursor = statustext_idx < 0 ? 0 : statustext_idx + 1;

    interpolate_position(ctx, target_us);
    ctx->wall_accum = target + ctx->time_offset_s;
}

void wasm_replay_close(wasm_replay_ctx_t *ctx) {
    ulog_timeline_t *tl = tl_of(ctx);
    if (tl) {
        ulog_timeline_free(tl);
        free(tl);
    }
    memset(ctx, 0, sizeof(*ctx));
}

uint64_t wasm_replay_duration_us(const wasm_replay_ctx_t *ctx) {
    const ulog_timeline_t *tl = ctl_of(ctx);
    if (!tl) return 0;
    return tl->end_timestamp_us - tl->start_timestamp_us;
}
