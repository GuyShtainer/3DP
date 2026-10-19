/* hw_cfgio.c -- the atomic config.ini writer and its recovery (release RF2). See hw_cfgio.h. Our own code. */
#include <assert.h>
#include <string.h>

#include "hw_cfg.h"
#include "hw_cfgio.h"
#include "hw_ctr.h"
#include "hw_log.h"

#define CFG_TMP HWCFG_FILE ".tmp"
#define CFG_BAK HWCFG_FILE ".bak"

int hwcfgio_recover(void)
{
    if (hwctr_file_size(HWCFG_FILE) >= 0) {
        return 0;
    }
    if (hwctr_file_size(CFG_TMP) > 0 && hwctr_file_rename(CFG_TMP, HWCFG_FILE) == 0) {
        HWLOG("config.ini was missing: restored from config.ini.tmp");
        return 1;
    }
    if (hwctr_file_size(CFG_BAK) > 0 && hwctr_file_rename(CFG_BAK, HWCFG_FILE) == 0) {
        HWLOG("config.ini was missing: restored from config.ini.bak");
        return 1;
    }
    return 0;
}

static int file_is(const char *path, const char *want, size_t len)
{
    static char back[HWCFGIO_MAX + 1u];
    int n;

    assert(path != NULL && want != NULL && len <= HWCFGIO_MAX);
    n = hwctr_file_read(path, back, HWCFGIO_MAX);
    return n == (int)len && memcmp(back, want, len) == 0;
}

/* tmp becomes config.ini without a moment where no complete copy exists. 0 or -1; on -1 config.ini is the old file again (or absent with .tmp + .bak both complete). */
static int replace_from_tmp(void)
{
    int had = hwctr_file_size(HWCFG_FILE) >= 0;

    if (had && hwctr_file_rename(HWCFG_FILE, CFG_BAK) != 0) {
        return -1;
    }
    if (hwctr_file_rename(CFG_TMP, HWCFG_FILE) != 0) {
        if (had) {
            (void)hwctr_file_rename(CFG_BAK, HWCFG_FILE);
        }
        return -1;
    }
    return 0;
}

int hwcfgio_write(const char *text, size_t len)
{
    if (text == NULL || len == 0u || len > HWCFGIO_MAX) {
        return -1;
    }
    hwctr_mkdirs(HWCTR_APP_DIR);
    if (hwctr_file_write(CFG_TMP, text, (uint32_t)len) != (int)len || !file_is(CFG_TMP, text, len)) {
        (void)hwctr_file_remove(CFG_TMP);   /* a half-written .tmp must never be promoted by hwcfgio_recover */
        return -1;
    }
    if (replace_from_tmp() != 0) {
        return -1;
    }
    if (!file_is(HWCFG_FILE, text, len)) {   /* the new file does not read back: the old one (still in .bak) goes back */
        (void)hwctr_file_rename(CFG_BAK, HWCFG_FILE);
        return -1;
    }
    (void)hwctr_file_remove(CFG_BAK);   /* a leftover .bak is harmless: it is only used when config.ini is absent */
    return 0;
}
