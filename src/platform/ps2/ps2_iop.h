#ifndef PS2_IOP_H_
#define PS2_IOP_H_

/*
 * Clean IOP bring-up for real hardware.
 *
 * A raw-ELF boot inherits whatever IOP state the loader left behind. That
 * state is not trustworthy after the loader returns: MCMAN/MCSERV, PADMAN,
 * USBD and even FILEIO can be half-initialized or bound to the old RPC
 * generation. The port therefore performs one explicit IOP reset and loads
 * the base stack itself. Host-based development (PCSX2/ps2link) and an
 * explicit NO_IOP_RESET marker keep the loader's I/O instead.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Call after ps2_exception_init() and before ps2_storage_init(). */
void ps2_iop_bringup(void);

/* Non-zero when this boot actually reset the IOP (useful for logs). */
int ps2_iop_did_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_IOP_H_ */
