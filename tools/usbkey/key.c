#include "key.h"

static void release_both(const struct key_io *io)
{
    io->release(KEY_DP);
    io->release(KEY_DM);
}

bool key_send_once(const struct key_io *io)
{
    for (int bit = 15; bit >= 0; bit--) {
        /* D+ is released only while the clock is low, so high here is the chip's pull-up. */
        io->release(KEY_DP);
        io->delay_us(KEY_SENSE_US);
        if (io->read(KEY_DP)) {
            release_both(io);
            return true;
        }
        io->drive(KEY_DP, 0);
        io->drive(KEY_DM, (int)((KEY_VALUE >> bit) & 1u));
        io->delay_us(KEY_HALF_US - KEY_SENSE_US);
        io->drive(KEY_DP, 1);
        io->delay_us(KEY_HALF_US);
        io->drive(KEY_DP, 0);
    }
    return false;
}

void key_start(struct key_attempt *a, const struct key_io *io)
{
    release_both(io);
    *a = (struct key_attempt){ .state = KEY_ARMING, .start_us = io->now_us() };
}

void key_abort(struct key_attempt *a, const struct key_io *io)
{
    release_both(io);
    a->state = KEY_IDLE;
}

bool key_dropped(const struct key_attempt *a)
{
    return a->low_at_end || a->longest_low_us >= KEY_DROP_US;
}

enum key_event key_poll(struct key_attempt *a, const struct key_io *io)
{
    uint32_t now = io->now_us();

    if ((a->state == KEY_ARMING || a->state == KEY_KEYING) && now - a->start_us >= KEY_TIMEOUT_US) {
        release_both(io);
        a->state = KEY_IDLE;
        return KEY_EV_TIMEOUT;
    }

    switch (a->state) {
    case KEY_ARMING:
        /* A device still on the bus keeps D+ high, and the host's packets toggle it; only a
         * bus without a device stays low. The lines are left alone until then. */
        io->delay_us(KEY_SENSE_US);
        if (io->read(KEY_DP)) {
            a->low = false;
        } else if (!a->low) {
            a->low = true;
            a->low_since_us = now;
        } else if (now - a->low_since_us >= KEY_ARM_LOW_US) {
            io->drive(KEY_DP, 0);
            io->drive(KEY_DM, 0);
            a->state = KEY_KEYING;
            return KEY_EV_ARMED;
        }
        return KEY_EV_NONE;

    case KEY_KEYING:
        if (key_send_once(io)) {
            a->answer_us = io->now_us() - a->start_us;
            a->state = KEY_WATCH;
            a->watch_start_us = io->now_us();
            a->low = false;
            return KEY_EV_ANSWERED;
        }
        a->keys++;
        return KEY_EV_NONE;

    case KEY_WATCH:
        /* After answering, the chip keeps its pull-up on while it calibrates its clock and
         * enumerates. Short lows are the host's bus resets; a long one means it left the bus
         * (it reset and started the firmware in flash). */
        if (!io->read(KEY_DP)) {
            if (!a->low) {
                a->low = true;
                a->low_since_us = now;
                a->low_pulses++;
            }
            if (now - a->low_since_us > a->longest_low_us)
                a->longest_low_us = now - a->low_since_us;
        } else {
            a->low = false;
        }
        if (now - a->watch_start_us >= KEY_WATCH_US) {
            a->low_at_end = a->low;
            a->state = KEY_IDLE;
            return KEY_EV_WATCHED;
        }
        return KEY_EV_NONE;

    case KEY_IDLE:
        break;
    }
    return KEY_EV_NONE;
}
