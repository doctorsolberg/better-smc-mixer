/*
 * The JieLi USB key: puts a JieLi chip into its ROM download mode ("UBOOT") when sent on the USB
 * data lines while the chip starts (docs/recovery.md).
 *
 * 16 bits, 0x16EF, most significant first: D+ is the clock, D- the data, the chip takes a bit on
 * each rising edge of D+. The key repeats until the chip answers by turning on its D+ pull-up,
 * which shows when D+ is released while the clock is low. Then both lines are let go at once.
 *
 * Hardware-independent: the lines, the delays and the clock come through struct key_io, so the
 * logic runs in host tests (test_key.c).
 */
#ifndef KEY_H
#define KEY_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_VALUE        0x16EFu
#define KEY_HALF_US      10u        /* half a clock period: about 50 kHz */
#define KEY_SENSE_US     3u         /* settling time of a released D+ before reading it */
#define KEY_ARM_LOW_US   10000u     /* D+ must stay low this long (no device on the bus) */
#define KEY_TIMEOUT_US   60000000u  /* an attempt gives up after a minute */
#define KEY_WATCH_US     3000000u   /* after the answer, D+ is watched this long */
#define KEY_DROP_US      200000u    /* D+ low this long after the answer: the chip left the bus */

enum key_line { KEY_DP, KEY_DM };

struct key_io {
    void (*drive)(enum key_line line, int level);  /* output at level */
    void (*release)(enum key_line line);           /* input, no pulls */
    int (*read)(enum key_line line);
    void (*delay_us)(uint32_t us);
    uint32_t (*now_us)(void);
};

enum key_state { KEY_IDLE, KEY_ARMING, KEY_KEYING, KEY_WATCH };

enum key_event {
    KEY_EV_NONE,
    KEY_EV_ARMED,       /* D+ stayed low: keying starts */
    KEY_EV_ANSWERED,    /* the chip took the bus; lines released */
    KEY_EV_TIMEOUT,     /* no answer within KEY_TIMEOUT_US; lines released */
    KEY_EV_WATCHED,     /* KEY_WATCH_US after the answer; see low_pulses, longest_low_us */
};

struct key_attempt {
    enum key_state state;
    uint32_t start_us;      /* attempt start */
    uint32_t low_since_us;  /* arming: since when D+ is low; watching: start of a low pulse */
    bool low;               /* D+ seen low (arming, watching) */
    uint32_t keys;          /* complete keys sent without an answer */
    uint32_t answer_us;     /* time from the start to the answer */
    uint32_t watch_start_us;
    uint32_t low_pulses;    /* watching: D+ low pulses (bus resets, or the chip leaving) */
    uint32_t longest_low_us;
    bool low_at_end;
};

/* Sends the key once with both lines driven. True when the chip answered (lines released). */
bool key_send_once(const struct key_io *io);

void key_start(struct key_attempt *a, const struct key_io *io);
void key_abort(struct key_attempt *a, const struct key_io *io);

/* One step of an attempt: call it as often as possible. While keying it sends one whole key
 * (about 0.3 ms) per call. */
enum key_event key_poll(struct key_attempt *a, const struct key_io *io);

/* True when the chip left the bus after answering (a watch result). */
bool key_dropped(const struct key_attempt *a);

#endif /* KEY_H */
