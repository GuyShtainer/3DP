/* hw_dev_hooks.h -- the device hooks the patched NitroSDK files call (port-patches/nitrosdk-devices.patch). Plain C types, so a host test
 * can include it. Implemented by source/hw/hw_devs.c over the pure units hw_pxi / hw_dma / hw_cp. Registers are plain memory, so a
 * register with a side effect is a function the SDK calls after the write; the private design notes ("devices").
 */
#ifndef HW_DEV_HOOKS_H
#define HW_DEV_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PXI (pxi_fifo.c): reset = the ARM7 comes up (loopback, handle bits published); cnt_sync before the SDK reads IPCFIFOCNT; send = write
   to the send FIFO; recv = read from the receive FIFO. */
void HWDEV_PxiReset(void);
void HWDEV_PxiCntSync(void);
void HWDEV_PxiSend(uint32_t word);
/* PXI_SendWordByFifo: a data word that does not fit the 26-bit field (a pointer the 3DS placed above 0x04000000: DS-sized addresses are the only ones the
   FIFO can carry) is fatal, because the word would be truncated on the wire. */
void HWDEV_PxiCheckData(uint32_t tag, uint32_t data);
uint32_t HWDEV_PxiRecv(void);

/* DMA (mi_dma.c MIi_DmaSetParams*, MI_WaitDma, MI_StopDma): a CNT write on channel `dmaNo` just happened. */
void HWDEV_DmaStart(uint32_t dmaNo);

/* Divider and square root (cp/divider.h, cp/sqrt.h, cp_context.c): the operands or the control word were just written. */
void HWDEV_CpDivRun(void);
void HWDEV_CpSqrtRun(void);

/* Geometry engine registers (bet 2.4b, source/hw/gpu3d.c): the registers are plain memory, so the SDK calls HWG3_Sync right BEFORE it reads GXSTAT, POS/VEC_RESULT, CLIPMTX,
   VECMTX or the list/vertex counters (the engine runs what it was fed and publishes its registers into the I/O block), and HWG3_StatWrite right AFTER it writes GXSTAT
   or DISP3DCNT (write-1-to-clear flags, the FIFO-interrupt condition). */
void HWG3_Sync(void);
void HWG3_StatWrite(uint32_t off);   /* off = 0x600 (GXSTAT) or 0x060 (DISP3DCNT) */

/* Geometry engine INPUT (bet 2.1a, source/hw/hw_gx.c): every store the game makes into the GX FIFO or a command port
   (I/O block offsets 0x400..0x5FF) goes through here, in program order. `off` is the I/O block OFFSET (REG_xxx_OFFSET), never an absolute address (the host
   tests are 64-bit). HWG3_Port = one 32-bit store (a direct reg_G3_* / reg_G3X_GXFIFO assignment); HWG3_Words = a bulk sender's `words` words from `src`:
     HWG3_MODE_SEND  MI_CpuSend32: every word to the SAME address `off`           (path CPUSEND)
     HWG3_MODE_COPY  MI_CpuCopy32 / MI_CpuCopyFast: word i to off + 4 * i         (path CPUSEND)
     HWG3_MODE_FIFO  GX_SendFifo36B..128B, GXi_NopClearFifo128_: the FIFO mirror   (path SENDFIFO)
   The hook does NOT store into the I/O block (the canary range must stay untouched); the last command-port value is kept in a shadow. */
#define HWG3_MODE_SEND 0u
#define HWG3_MODE_COPY 1u
#define HWG3_MODE_FIFO 2u
void HWG3_Port(uint32_t off, uint32_t val);
void HWG3_Words(uint32_t off, const void *src, uint32_t words, uint32_t mode);
/* Bet 2.4a, trace-only: the game READ `words` (1..16) 32-bit registers from I/O offset `off` (GXSTAT 0x600, POS_RESULT 0x620, VEC_RESULT 0x630/0x632/0x634, CLIPMTX 0x640,
   VECMTX 0x680) and the port returned `vals`. Records an RDBK entry group (hw_gx.h) at this stream position; stores nothing, changes no counter. */
void HWG3_Readback(uint32_t off, uint32_t words, const void *vals);

/* VRAM banks (bet 2.1b, source/hw/hw_vram.c): HWVRAM_SetCnt is called by every VRAMCNT_A..I store of the SDK (the
   HWVRAM_STORE macro of ioreg_GX.h) right after the register byte is written: it copies the bank between the physical store and the CPU-visible window
   arrays so a plain CPU store to a window address stays truthful across a remap. `bank` 0..8 = A..I. HWVRAM_Touch: a bulk helper (MIi_Cpu*) is about to
   write `bytes` at `dst`; marks the 1 KB dirty pages, ignored for any address that is not VRAM. */
void HWVRAM_SetCnt(uint32_t bank, uint32_t value);
void HWVRAM_Touch(const void *dst, uint32_t bytes);

/* Card bus (card_rom.c): MCCNT1 control word write/read and the MCD1 data port. Over source/hw/hw_card.c. */
void HWDEV_CardCtrlWrite(uint32_t ctrl);
uint32_t HWDEV_CardCtrlRead(void);
uint32_t HWDEV_CardDataRead(void);

/* Backup lock (card_common.c CARD_LockBackup / CARD_UnlockBackup, bet 2.6b; source/hw/hw_devs.c over source/hw/hw_save.c): the save write-back's hook points. Lock = the SDK now holds the
   backup resource (a backup request may be in flight; nothing may snapshot the flash image). Unlock = the last request of the round has finished, and a flash write that landed since
   the previous unlock makes a file flush pending (debounced, written off the game thread). `lock_id` is the SDK's lock id (only counted). */
void HWDEV_BackupLock(uint32_t lock_id);
void HWDEV_BackupUnlock(uint32_t lock_id);

/* Overlays (fs_overlay.c): the overlay `id` is linked into the image; load restores its .data from the pristine copy and zeroes its .bss,
   start marks it loaded, unload marks it unloaded. Over source/hw/hw_ovl.c. */
void HWDEV_OverlayLoad(uint32_t id);
void HWDEV_OverlayStart(uint32_t id);
void HWDEV_OverlayUnload(uint32_t id);

/* Bet 2.8 widescreen (port-patches/port-wide-camera.patch, src/camera.c Camera_ComputeProjectionMatrix): the projection's sinFovY and aspect go through these (identity
   unless config.ini wide = 1; the aspect hook is told sinFovY so both decide alike), and the matrix the game just built is registered with the geometry engine, with the
   camera's own (unwidened) arguments -- ortho 0 = MTX_Perspective, 1 = the orthographic branch -- and the camera distance (the stereo zero-parallax depth). */
int32_t HWDEV_WideSin(int32_t sin_fovy);
int32_t HWDEV_WideAspect(int32_t aspect, int32_t sin_fovy);
void HWDEV_WideProjection(const void *proj_mtx, int32_t ortho, int32_t sin_fovy, int32_t cos_fovy, int32_t aspect, int32_t near_clip, int32_t far_clip, int32_t distance);

#ifdef __cplusplus
}
#endif

#endif
