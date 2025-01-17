/*
 * cisreg.h 1.14 1999/10/25 20:23:17
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License
 * at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and
 * limitations under the License. 
 *
 * The initial developer of the original code is David A. Hinds
 * <dhinds@pcmcia.sourceforge.org>.  Portions created by David A. Hinds
 * are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU Public License version 2 (the "GPL"), in which
 * case the provisions of the GPL are applicable instead of the
 * above.  If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use
 * your version of this file under the MPL, indicate your decision by
 * deleting the provisions above and replace them with the notice and
 * other provisions required by the GPL.  If you do not delete the
 * provisions above, a recipient may use your version of this file
 * under either the MPL or the GPL.
 */

#ifndef _LINUX_CISREG_H
#define _LINUX_CISREG_H

/* Offsets from ConfigBase for CIS registers */
#define CISREG_COR		0x00
#define CISREG_CCSR		0x02
#define CISREG_PRR		0x04
#define CISREG_SCR		0x06
#define CISREG_ESR		0x08
#define CISREG_IOBASE_0		0x0a
#define CISREG_IOBASE_1		0x0c
#define CISREG_IOBASE_2		0x0e
#define CISREG_IOBASE_3		0x10
#define CISREG_IOSIZE		0x12

/*
 * Configuration Option Register
 */
#define COR_CONFIG_MASK		0x3f
#define COR_MFC_CONFIG_MASK	0x38
#define COR_FUNC_ENA		0x01
#define COR_ADDR_DECODE		0x02
#define COR_IREQ_ENA		0x04
#define COR_LEVEL_REQ		0x40
#define COR_SOFT_RESET		0x80

/*
 * Card Configuration and Status Register
 */
#define CCSR_INTR_ACK		0x01
#define CCSR_INTR_PENDING	0x02
#define CCSR_POWER_DOWN		0x04
#define CCSR_AUDIO_ENA		0x08
#define CCSR_IOIS8		0x20
#define CCSR_SIGCHG_ENA		0x40
#define CCSR_CHANGED		0x80

/*
 * Pin Replacement Register
 */
#define PRR_WP_STATUS		0x01
#define PRR_READY_STATUS	0x02
#define PRR_BVD2_STATUS		0x04
#define PRR_BVD1_STATUS		0x08
#define PRR_WP_EVENT		0x10
#define PRR_READY_EVENT		0x20
#define PRR_BVD2_EVENT		0x40
#define PRR_BVD1_EVENT		0x80

/*
 * Socket and Copy Register
 */
#define SCR_SOCKET_NUM		0x0f
#define SCR_COPY_NUM		0x70

/*
 * Extended Status Register
 */
#define ESR_REQ_ATTN_ENA	0x01
#define ESR_REQ_ATTN		0x10

/*
 * CardBus Function Status Registers
 */
#define CBFN_EVENT		0x00
#define CBFN_MASK		0x04
#define CBFN_STATE		0x08
#define CBFN_FORCE		0x0c

/*
 * These apply to all the CardBus function registers
 */
#define CBFN_WP			0x0001
#define CBFN_READY		0x0002
#define CBFN_BVD2		0x0004
#define CBFN_BVD1		0x0008
#define CBFN_GWAKE		0x0010
#define CBFN_INTR		0x8000

/*
 * Extra bits in the Function Event Mask Register
 */
#define FEMR_BAM_ENA		0x0020
#define FEMR_PWM_ENA		0x0040
#define FEMR_WKUP_MASK		0x4000

#endif /* _LINUX_CISREG_H */
