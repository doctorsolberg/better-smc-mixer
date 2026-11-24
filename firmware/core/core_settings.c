/* Core settings by name (core_settings.h). */
#include "core_settings.h"

#include <string.h>

enum what {
    W_LAYER_MODE, W_PICKUP, W_GESTURE_MS, W_HYBRID_MS, W_ENC_ACCEL, W_FINE14,
    W_SHIFT_MODE, W_NOTE_OFF, W_CHANNEL, W_PARAM_NULL
};

static const struct {
    const char *name;
    uint8_t what;
    bool per_preset;
} settings[] = {
    {"layer_mode", W_LAYER_MODE, false}, {"pickup", W_PICKUP, false},
    {"gesture_ms", W_GESTURE_MS, false}, {"hybrid_ms", W_HYBRID_MS, false},
    {"enc_accel", W_ENC_ACCEL, false},   {"fine14", W_FINE14, false},
    {"shift_mode", W_SHIFT_MODE, true},  {"note_off", W_NOTE_OFF, true},
    {"channel", W_CHANNEL, true},        {"param_null", W_PARAM_NULL, true},
};

static const struct {
    uint8_t what;
    const char *word;
    uint8_t value;
} words[] = {
    {W_LAYER_MODE, "gesture", CORE_LAYER_GESTURE}, {W_LAYER_MODE, "immediate", CORE_LAYER_IMMEDIATE},
    {W_PICKUP, "jump", CORE_JUMP},                 {W_PICKUP, "pickup", CORE_PICKUP},
    {W_PICKUP, "scale", CORE_SCALE},               {W_SHIFT_MODE, "hold", CORE_SHIFT_HOLD},
    {W_SHIFT_MODE, "latch", CORE_SHIFT_LATCH},     {W_SHIFT_MODE, "hybrid", CORE_SHIFT_HYBRID},
    {W_NOTE_OFF, "9n", CORE_NOTE_OFF_9N},          {W_NOTE_OFF, "8n", CORE_NOTE_OFF_8N},
    {W_FINE14, "range", CORE_FINE14_RANGE},        {W_FINE14, "lsb", CORE_FINE14_LSB},
    {W_PARAM_NULL, "off", CORE_PARAM_NULL_OFF},    {W_PARAM_NULL, "gesture", CORE_PARAM_NULL_GESTURE},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void core_settings_init(struct core_settings *s, unsigned config)
{
    memset(s, 0, sizeof *s);
    s->config = (uint8_t)config;
}

/* "1".."max" -> the number, else -1. */
static long number(const char *v, long max)
{
    long n = 0;
    if (!*v)
        return -1;
    for (; *v; v++) {
        if (*v < '0' || *v > '9' || n > max)
            return -1;
        n = n * 10 + (*v - '0');
    }
    return n >= 1 && n <= max ? n : -1;
}

bool core_settings_add(struct core_settings *s, const char *name, const char *value)
{
    if (!strcmp(name, "config")) {
        if (s->count)
            return false; /* it would undo the settings before it */
        if (!strcmp(value, "factory"))
            s->config = CORE_CONFIG_FACTORY;
        else if (!strcmp(value, "test"))
            s->config = CORE_CONFIG_TEST;
        else
            return false;
        return true;
    }
    if (s->count >= CORE_SETTINGS_MAX)
        return false;
    for (size_t i = 0; i < COUNT(settings); i++) {
        size_t n = strlen(settings[i].name);
        uint8_t presets = (1u << CORE_PRESETS) - 1;
        if (strncmp(name, settings[i].name, n))
            continue;
        if (name[n]) { /* NAME_pK */
            if (!settings[i].per_preset || name[n] != '_' || name[n + 1] != 'p' ||
                name[n + 2] < '1' || name[n + 2] > '0' + CORE_PRESETS || name[n + 3])
                continue;
            presets = (uint8_t)(1u << (name[n + 2] - '1'));
        }
        long v = -1;
        if (settings[i].what == W_GESTURE_MS || settings[i].what == W_HYBRID_MS) {
            v = number(value, 60000);
        } else if (settings[i].what == W_ENC_ACCEL) {
            v = number(value, 200);
            if (v < 50)
                v = -1; /* 50-200 percent, as the configuration (encAccel) */
        } else if (settings[i].what == W_CHANNEL) {
            v = number(value, 16);
            if (v > 0)
                v--; /* the device's 0-15 */
        } else {
            for (size_t k = 0; k < COUNT(words); k++)
                if (words[k].what == settings[i].what && !strcmp(words[k].word, value))
                    v = words[k].value;
        }
        if (v < 0)
            return false;
        s->item[s->count].what = settings[i].what;
        s->item[s->count].presets = presets;
        s->item[s->count].value = (uint16_t)v;
        s->count++;
        return true;
    }
    return false;
}

static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* The next word of text[*pos..len) into w (cap bytes); false at the end. A word too long for w
 * comes back as "?", which no setting matches. */
static bool word(const char *text, size_t len, size_t *pos, char *w, size_t cap)
{
    size_t i = *pos, n = 0;
    bool cut = false;
    while (i < len && space(text[i]))
        i++;
    if (i == len || !text[i]) {
        *pos = len; /* a NUL ends the text */
        return false;
    }
    while (i < len && !space(text[i]) && text[i]) {
        if (n + 1 < cap)
            w[n++] = text[i];
        else
            cut = true;
        i++;
    }
    w[n] = 0;
    if (cut)
        strcpy(w, "?"); /* no setting or value is called that */
    *pos = i;
    return true;
}

unsigned core_settings_parse(struct core_settings *s, const char *text, size_t len)
{
    char name[24], value[16];
    size_t pos = 0;
    unsigned pair = 0;
    if (len > CORE_SETTINGS_TEXT_MAX)
        return 1;
    while (word(text, len, &pos, name, sizeof name)) {
        pair++;
        if (!word(text, len, &pos, value, sizeof value) || !core_settings_add(s, name, value))
            return pair;
    }
    return 0;
}

void core_settings_apply(const struct core_settings *s, struct core_config *cfg)
{
    if (s->config == CORE_CONFIG_TEST)
        core_config_test(cfg);
    else
        core_config_factory(cfg);
    struct core_global *g = &cfg->global;
    for (unsigned i = 0; i < s->count; i++) {
        uint16_t v = s->item[i].value;
        switch (s->item[i].what) {
        case W_LAYER_MODE: g->layer_mode = (uint8_t)v; break;
        case W_PICKUP: g->pickup = (uint8_t)v; break;
        case W_GESTURE_MS: g->gesture_ms = v; break;
        case W_HYBRID_MS: g->hybrid_ms = v; break;
        case W_ENC_ACCEL: g->enc_accel = (uint8_t)v; break;
        case W_FINE14: g->fine14 = (uint8_t)v; break;
        default:
            for (unsigned k = 0; k < CORE_PRESETS; k++) {
                if (!(s->item[i].presets >> k & 1))
                    continue;
                if (s->item[i].what == W_SHIFT_MODE)
                    cfg->preset[k].shift_mode = (uint8_t)v;
                else if (s->item[i].what == W_CHANNEL)
                    cfg->preset[k].channel = (uint8_t)v;
                else if (s->item[i].what == W_PARAM_NULL)
                    cfg->preset[k].param_null = (uint8_t)v;
                else
                    cfg->preset[k].note_off = (uint8_t)v;
            }
        }
    }
}
