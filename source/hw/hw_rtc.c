/* hw_rtc.c -- the DS real-time-clock chip behind the RTC PXI tag (bet 1.6b). Pure C, host-tested.
 * The ARM9 side (NitroSDK rtc/src/external.c) sends one command word per request and reads/writes the 8-byte RTCRawData image in system work;
 * the ARM7 side (rtc/src/ARM7/control.c) does the SPI-like chip access and answers 0x8000 | command << 8 | result. This module is that ARM7 side
 * with the host clock as the chip: BCD date/time from `now`, status/alarm/pulse/adjust/free registers kept in a small register file, and a
 * date/time write moves an offset so the game's "set clock" screen works (it is not persisted: config.ini's rtc_offset is the persistent one). */
#include <assert.h>
#include <string.h>

#include "hw_pure.h"

#define SECS_PER_DAY 86400
#define STATUS1_FORMAT24 0x0002u   /* RTCRawStatus1.format */

int64_t hwrtc_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era, yoe, doy, doe;

    assert(m >= 1u && m <= 12u);
    assert(d >= 1u && d <= 31u);
    y -= m <= 2u ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * ((int64_t)m + (m > 2u ? -3 : 9)) + 2) / 5 + (int64_t)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 - 10957;   /* 10957 = days from 1970-01-01 to 2000-01-01: the result is relative to 2000 */
}

void hwrtc_civil_from_days(int64_t days, int64_t *y, unsigned *m, unsigned *d)
{
    int64_t z, era, doe, yoe, doy, mp;

    assert(y != NULL && m != NULL && d != NULL);
    z = days + 10957 + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2u ? 1 : 0);
}

static uint32_t to_bcd(uint32_t v)
{
    assert(v < 100u);
    return ((v / 10u) << 4) | (v % 10u);
}

static int from_bcd(uint32_t b, uint32_t max)
{
    uint32_t hi = (b >> 4) & 0xfu, lo = b & 0xfu;

    if (hi > 9u || lo > 9u || (b >> 8) != 0u || hi * 10u + lo > max) {
        return -1;
    }
    return (int)(hi * 10u + lo);
}

void hwrtc_raw_from_secs(int64_t secs, uint32_t *date_word, uint32_t *time_word)
{
    int64_t days = secs / SECS_PER_DAY, y;
    int64_t rem = secs % SECS_PER_DAY;
    unsigned m, d, hour, min, sec, week;

    assert(date_word != NULL && time_word != NULL);
    if (rem < 0) {
        rem += SECS_PER_DAY;
        days -= 1;
    }
    hwrtc_civil_from_days(days, &y, &m, &d);
    hour = (unsigned)(rem / 3600);
    min = (unsigned)(rem % 3600 / 60);
    sec = (unsigned)(rem % 60);
    week = (unsigned)(((days % 7) + 7 + 6) % 7);   /* 2000-01-01 was a Saturday (6), Sunday = 0 */
    *date_word = to_bcd((unsigned)(((y - 2000) % 100 + 100) % 100)) | (to_bcd(m) << 8) | (to_bcd(d) << 16) | ((uint32_t)week << 24);
    *time_word = to_bcd(hour) | (hour >= 12u ? 0x40u : 0u) | (to_bcd(min) << 8) | (to_bcd(sec) << 16);
}

int64_t hwrtc_fixed_secs(int64_t epoch, uint64_t vblanks)
{
    assert(vblanks < (uint64_t)INT64_MAX / 2u);
    return epoch - HWRTC_UNIX_2000 + (int64_t)(vblanks / 60u);
}

void hwrtc_fields_from_secs(int64_t secs, HwRtcFields *f)
{
    int64_t days = secs / SECS_PER_DAY, rem = secs % SECS_PER_DAY;

    assert(f != NULL);
    if (rem < 0) {
        rem += SECS_PER_DAY;
        days -= 1;
    }
    hwrtc_civil_from_days(days, &f->year, &f->mon, &f->day);
    f->hour = (unsigned)(rem / 3600);
    f->min = (unsigned)(rem % 3600 / 60);
    f->sec = (unsigned)(rem % 60);
    f->wday = (unsigned)(((days % 7) + 7 + 6) % 7);   /* 2000-01-01 was a Saturday (6) */
}

int hwrtc_secs_from_raw(uint32_t date_word, uint32_t time_word, int64_t *secs)
{
    int y = from_bcd(date_word & 0xffu, 99u), mo = from_bcd((date_word >> 8) & 0x1fu, 12u), d = from_bcd((date_word >> 16) & 0x3fu, 31u);
    int h = from_bcd(time_word & 0x3fu, 23u), mi = from_bcd((time_word >> 8) & 0x7fu, 59u), s = from_bcd((time_word >> 16) & 0x7fu, 59u);

    assert(secs != NULL);
    if (y < 0 || mo < 1 || d < 1 || h < 0 || mi < 0 || s < 0) {
        return -1;
    }
    *secs = hwrtc_days_from_civil(2000 + y, (unsigned)mo, (unsigned)d) * SECS_PER_DAY + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
    return 0;
}

void hwrtc_init(HwRtc *r, int64_t offset)
{
    assert(r != NULL);
    memset(r, 0, sizeof *r);
    r->offset = offset;
    r->status1 = STATUS1_FORMAT24;
}

static void put16(uint8_t *b, unsigned off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1u] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *b, unsigned off, uint32_t v)
{
    put16(b, off, (uint16_t)v);
    put16(b, off + 2u, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *b, unsigned off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1u] << 8) | ((uint32_t)b[off + 2u] << 16) | ((uint32_t)b[off + 3u] << 24);
}

static uint16_t get16(const uint8_t *b, unsigned off)
{
    return (uint16_t)(b[off] | (b[off + 1u] << 8));
}

/* the READ_* commands: fill the system-work image */
static unsigned do_read(HwRtc *r, unsigned cmd, uint8_t *buf, int64_t now)
{
    uint32_t dw, tw;

    hwrtc_raw_from_secs(now + r->offset, &dw, &tw);
    switch (cmd) {
    case HWRTC_CMD_READ_DT:
        put32(buf, 0, dw);
        put32(buf, 4, tw);
        break;
    case HWRTC_CMD_READ_DATE:
        put32(buf, 0, dw);
        break;
    case HWRTC_CMD_READ_TIME:
        put32(buf, 4, tw);   /* RTCRawData.t.time is the second word */
        break;
    case HWRTC_CMD_READ_STAT1:
        put16(buf, 0, r->status1);
        break;
    case HWRTC_CMD_READ_STAT2:
        put16(buf, 2, r->status2);
        break;
    case HWRTC_CMD_READ_PULSE:
        put32(buf, 4, r->pulse);
        break;
    case HWRTC_CMD_READ_ALARM1:
    case HWRTC_CMD_READ_ALARM2:
        put32(buf, 4, r->alarm[cmd == HWRTC_CMD_READ_ALARM2 ? 1 : 0]);
        break;
    case HWRTC_CMD_READ_ADJUST:
        put32(buf, 4, r->adjust);
        break;
    case HWRTC_CMD_READ_FREE:
        put32(buf, 4, r->freereg);
        break;
    default:
        return HWRTC_RESULT_INVALID;
    }
    r->reads++;
    return HWRTC_RESULT_SUCCESS;
}

/* WRITE_DATE keeps the clock's time of day and WRITE_TIME its date, as the chip does; an invalid BCD field is refused. */
static unsigned do_write_clock(HwRtc *r, unsigned cmd, const uint8_t *buf, int64_t now)
{
    uint32_t cur_d, cur_t, dw, tw;
    int64_t target;

    hwrtc_raw_from_secs(now + r->offset, &cur_d, &cur_t);
    dw = cmd == HWRTC_CMD_WRITE_TIME ? cur_d : get32(buf, 0);
    tw = cmd == HWRTC_CMD_WRITE_DATE ? cur_t : get32(buf, 4);
    if (hwrtc_secs_from_raw(dw, tw, &target) != 0) {
        r->invalid++;
        return HWRTC_RESULT_INVALID;
    }
    r->offset = target - now;
    r->writes++;
    return HWRTC_RESULT_SUCCESS;
}

static unsigned do_write_reg(HwRtc *r, unsigned cmd, const uint8_t *buf)
{
    switch (cmd) {
    case HWRTC_CMD_WRITE_STAT1:
        r->status1 = (uint16_t)(get16(buf, 0) & 0x00ffu);
        break;
    case HWRTC_CMD_WRITE_STAT2:
        r->status2 = (uint16_t)(get16(buf, 2) & 0x00ffu);
        break;
    case HWRTC_CMD_WRITE_PULSE:
        r->pulse = (uint8_t)(get32(buf, 4) & 0x1fu);
        break;
    case HWRTC_CMD_WRITE_ALARM1:
    case HWRTC_CMD_WRITE_ALARM2:
        r->alarm[cmd == HWRTC_CMD_WRITE_ALARM2 ? 1 : 0] = get32(buf, 4) & 0x00ffffffu;
        break;
    case HWRTC_CMD_WRITE_ADJUST:
        r->adjust = (uint8_t)get32(buf, 4);
        break;
    case HWRTC_CMD_WRITE_FREE:
        r->freereg = (uint8_t)get32(buf, 4);
        break;
    default:
        return HWRTC_RESULT_INVALID;
    }
    r->writes++;
    return HWRTC_RESULT_SUCCESS;
}

unsigned hwrtc_command(HwRtc *r, unsigned cmd, uint8_t *buf, int64_t now)
{
    assert(r != NULL && buf != NULL);
    r->commands++;
    if (cmd == HWRTC_CMD_RESET) {
        r->status1 = STATUS1_FORMAT24;   /* the SDK resets and then re-selects the 24-hour format; the registers keep their values otherwise */
        return HWRTC_RESULT_SUCCESS;
    }
    if (cmd == HWRTC_CMD_SET_FORMAT) {
        r->status1 = (uint16_t)(r->status1 | (get16(buf, 0) & STATUS1_FORMAT24));
        return HWRTC_RESULT_SUCCESS;
    }
    if (cmd >= HWRTC_CMD_READ_DT && cmd <= HWRTC_CMD_READ_FREE) {
        return do_read(r, cmd, buf, now);
    }
    if (cmd == HWRTC_CMD_WRITE_DT || cmd == HWRTC_CMD_WRITE_DATE || cmd == HWRTC_CMD_WRITE_TIME) {
        return do_write_clock(r, cmd, buf, now);
    }
    if (cmd >= HWRTC_CMD_WRITE_PULSE && cmd <= HWRTC_CMD_WRITE_FREE) {
        return do_write_reg(r, cmd, buf);
    }
    r->invalid++;
    return HWRTC_RESULT_INVALID;
}
