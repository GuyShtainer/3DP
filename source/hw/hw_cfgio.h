/* hw_cfgio.h -- the ONE writer of config.ini (release RF2): every rewrite of the file goes through hwcfgio_write, and every launch runs hwcfgio_recover before the file is first read.
 * The SD's rename cannot replace an existing file (hw_ctr's hwctr_file_rename removes its target first), so a replace is: write config.ini.tmp, read it back, move config.ini to
 * config.ini.bak, move the .tmp into place, read it back, drop the .bak. A power cut or card pop between the two moves leaves no config.ini but a complete .tmp and the old
 * .bak; hwcfgio_recover puts one back at the next launch. No libctru here: it works over the hw_ctr.h seam, so the host tests run it as it is. */
#ifndef HW_CFGIO_H
#define HW_CFGIO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWCFGIO_MAX 4096u   /* the largest config.ini text it writes (the game reads at most 4095 bytes of it) */

/* config.ini absent: promote config.ini.tmp (newest) or else config.ini.bak. Returns 1 when a file was restored, else 0. Logs one line when it does. */
int hwcfgio_recover(void);

/* text[0..len) becomes config.ini, whole or not at all. 0 on success; -1 on any failure, with the old config.ini back in place (or, if even that restore failed, config.ini absent
 * and .tmp + .bak holding both versions for hwcfgio_recover). len is 1..HWCFGIO_MAX. */
int hwcfgio_write(const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif
