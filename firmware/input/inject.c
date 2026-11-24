/*
 * Input injection (inject.h). The core logs every event it takes, injected ones marked as such;
 * here: the switch, refusals and rejections.
 */
#include "inject.h"

#include "smclog.h"

void inject_init(struct inject *in, struct evq *q)
{
    *in = (struct inject){.q = q};
}

static bool decode(const uint8_t *r, uint32_t t_us, struct core_event *ev)
{
    *ev = (struct core_event){.t_us = t_us, .type = r[0], .index = r[1], .flags = CORE_EV_INJECTED};
    int v = (int16_t)(r[2] | r[3] << 8);
    switch (r[0]) {
    case CORE_EV_BUTTON:
        ev->value = r[2];
        return r[1] < CORE_BUTTONS && r[2] <= 1;
    case CORE_EV_ENCODER:
        ev->value = (int16_t)v;
        return r[1] < CORE_STRIPS && v != 0;
    case CORE_EV_FADER:
        ev->value = (int16_t)v;
        return r[1] < CORE_STRIPS && v >= 0 && v <= CORE_FADER_MAX;
    case CORE_EV_MIDI_IN:
        ev->midi[0] = r[2];
        ev->midi[1] = r[3];
        ev->midi[2] = r[4];
        return (r[1] == CORE_FROM_USB || r[1] == CORE_FROM_BLE) && (r[2] & 0x80u);
    default:
        return false; /* ticks are the platform's, not injectable */
    }
}

struct inject_reply inject_command(struct inject *in, uint32_t t_us, const uint8_t *p, size_t len)
{
    struct inject_reply r = {.status = INJECT_OK};
    if (len == 0) {
        r.status = INJECT_BAD_FORMAT;
        return r;
    }
    switch (p[0]) {
    case INJECT_OP_OFF:
    case INJECT_OP_ON:
        if (len != 1) {
            r.status = INJECT_BAD_FORMAT;
            return r;
        }
        if (in->on != (p[0] == INJECT_OP_ON))
            LOG_INFO(test, "input injection %s", p[0] == INJECT_OP_ON ? "on" : "off");
        in->on = p[0] == INJECT_OP_ON;
        return r;
    case INJECT_OP_EVENTS:
        break;
    default:
        r.status = INJECT_BAD_FORMAT;
        return r;
    }

    size_t n = (len - 1) / INJECT_EVENT_BYTES;
    if ((len - 1) % INJECT_EVENT_BYTES || n == 0 || n > INJECT_MAX_EVENTS) {
        r.status = INJECT_BAD_FORMAT;
        return r;
    }
    if (!in->on) {
        LOG_WARN(test, "injection is off: %u events refused", (unsigned)n);
        in->rejected += (uint32_t)n;
        r.status = INJECT_DISABLED;
        return r;
    }
    struct core_event ev;
    for (size_t i = 0; i < n; i++) {
        if (!decode(p + 1 + i * INJECT_EVENT_BYTES, t_us, &ev)) {
            const uint8_t *e = p + 1 + i * INJECT_EVENT_BYTES;
            LOG_WARN(test, "inject: event %u invalid (type %u index %u)", (unsigned)i, e[0], e[1]);
            in->rejected += (uint32_t)n;
            r.status = INJECT_BAD_EVENT;
            return r;
        }
    }
    for (size_t i = 0; i < n; i++) {
        decode(p + 1 + i * INJECT_EVENT_BYTES, t_us, &ev);
        if (!evq_push(in->q, &ev)) {
            r.refused = (uint8_t)(n - i);
            r.status = INJECT_FULL;
            in->refused += r.refused;
            LOG_WARN(test, "inject: queue full, %u of %u events refused", r.refused, (unsigned)n);
            break;
        }
        r.accepted++;
        in->accepted++;
    }
    return r;
}
