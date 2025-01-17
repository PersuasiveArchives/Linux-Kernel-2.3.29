

/* Minimal serial functions needed to send messages out the serial
 * port on the MBX console.
 *
 * The MBX uxes SMC1 for the serial port.  We reset the port and use
 * only the first BD that EPPC-Bug set up as a character FIFO.
 *
 * Later versions (at least 1.4, maybe earlier) of the MBX EPPC-Bug
 * use COM1 instead of SMC1 as the console port.  This kinda sucks
 * for the rest of the kernel, so here we force the use of SMC1 again.
 */
#include <linux/config.h>
#include <linux/types.h>
#include <asm/uaccess.h>
#include <asm/mpc8xx.h>
#include "../8xx_io/commproc.h"

#ifdef CONFIG_MBX
#define MBX_CSR1	((volatile u_char *)0xfa100000)
#define CSR1_COMEN	(u_char)0x02
#endif

static cpm8xx_t	*cpmp = (cpm8xx_t *)&(((immap_t *)IMAP_ADDR)->im_cpm);

void
serial_init(bd_t *bd)
{
	volatile smc_t		*sp;
	volatile smc_uart_t	*up;
	volatile cbd_t	*tbdf, *rbdf;
	volatile cpm8xx_t	*cp;
	uint	dpaddr, memaddr;

	cp = cpmp;
	sp = (smc_t*)&(cp->cp_smc[0]);
	up = (smc_uart_t *)&cp->cp_dparam[PROFF_SMC1];

	/* Disable transmitter/receiver.
	*/
	sp->smc_smcmr &= ~(SMCMR_REN | SMCMR_TEN);

#ifndef CONFIG_MBX
	{
	/* Initialize SMC1 and use it for the console port.
	 */

	/* Enable SDMA.
	*/
	((immap_t *)IMAP_ADDR)->im_siu_conf.sc_sdcr = 1;

	/* Use Port B for SMCs instead of other functions.
	*/
	cp->cp_pbpar |= 0x00000cc0;
	cp->cp_pbdir &= ~0x00000cc0;
	cp->cp_pbodr &= ~0x00000cc0;

	/* Allocate space for two buffer descriptors in the DP ram.
	 * For now, this address seems OK, but it may have to
	 * change with newer versions of the firmware.
	 */
	dpaddr = 0x0800;

	/* Grab a few bytes from the top of memory.  EPPC-Bug isn't
	 * running any more, so we can do this.
	 */
	memaddr = (bd->bi_memsize - 32) & ~15;

	/* Set the physical address of the host memory buffers in
	 * the buffer descriptors.
	 */
	rbdf = (cbd_t *)&cp->cp_dpmem[dpaddr];
	rbdf->cbd_bufaddr = memaddr;
	rbdf->cbd_sc = 0;
	tbdf = rbdf + 1;
	tbdf->cbd_bufaddr = memaddr+4;
	tbdf->cbd_sc = 0;

	/* Set up the uart parameters in the parameter ram.
	*/
	up->smc_rbase = dpaddr;
	up->smc_tbase = dpaddr+sizeof(cbd_t);
	up->smc_rfcr = SMC_EB;
	up->smc_tfcr = SMC_EB;

	/* Set UART mode, 8 bit, no parity, one stop.
	 * Enable receive and transmit.
	 */
	sp->smc_smcmr = smcr_mk_clen(9) |  SMCMR_SM_UART;

	/* Mask all interrupts and remove anything pending.
	*/
	sp->smc_smcm = 0;
	sp->smc_smce = 0xff;

	/* Set up the baud rate generator.
	 * See 8xx_io/commproc.c for details.
	 */
	cp->cp_simode = 0x10000000;
	cp->cp_brgc1 =
		((((bd->bi_intfreq * 1000000)/16) / bd->bi_baudrate) << 1) | CPM_BRG_EN;

#else /* CONFIG_MBX */
	if (*MBX_CSR1 & CSR1_COMEN) {
		/* COM1 is enabled.  Initialize SMC1 and use it for
		 * the console port.
		 */

		/* Enable SDMA.
		*/
		((immap_t *)IMAP_ADDR)->im_siu_conf.sc_sdcr = 1;

		/* Use Port B for SMCs instead of other functions.
		*/
		cp->cp_pbpar |= 0x00000cc0;
		cp->cp_pbdir &= ~0x00000cc0;
		cp->cp_pbodr &= ~0x00000cc0;

		/* Allocate space for two buffer descriptors in the DP ram.
		 * For now, this address seems OK, but it may have to
		 * change with newer versions of the firmware.
		 */
		dpaddr = 0x0800;

		/* Grab a few bytes from the top of memory.  EPPC-Bug isn't
		 * running any more, so we can do this.
		 */
		memaddr = (bd->bi_memsize - 32) & ~15;

		/* Set the physical address of the host memory buffers in
		 * the buffer descriptors.
		 */
		rbdf = (cbd_t *)&cp->cp_dpmem[dpaddr];
		rbdf->cbd_bufaddr = memaddr;
		rbdf->cbd_sc = 0;
		tbdf = rbdf + 1;
		tbdf->cbd_bufaddr = memaddr+4;
		tbdf->cbd_sc = 0;

		/* Set up the uart parameters in the parameter ram.
		*/
		up->smc_rbase = dpaddr;
		up->smc_tbase = dpaddr+sizeof(cbd_t);
		up->smc_rfcr = SMC_EB;
		up->smc_tfcr = SMC_EB;

		/* Set UART mode, 8 bit, no parity, one stop.
		 * Enable receive and transmit.
		 */
		sp->smc_smcmr = smcr_mk_clen(9) |  SMCMR_SM_UART;

		/* Mask all interrupts and remove anything pending.
		*/
		sp->smc_smcm = 0;
		sp->smc_smce = 0xff;

		/* Set up the baud rate generator.
		 * See 8xx_io/commproc.c for details.
		 */
		cp->cp_simode = 0x10000000;
		cp->cp_brgc1 =
			((((bd->bi_intfreq * 1000000)/16) / 9600) << 1) | CPM_BRG_EN;

		/* Enable SMC1 for console output.
		*/
		*MBX_CSR1 &= ~CSR1_COMEN;
	}
	else {
#endif /* ndef CONFIG_MBX */
		/* SMC1 is used as console port.
		*/
		tbdf = (cbd_t *)&cp->cp_dpmem[up->smc_tbase];
		rbdf = (cbd_t *)&cp->cp_dpmem[up->smc_rbase];

		/* Issue a stop transmit, and wait for it.
		*/
		cp->cp_cpcr = mk_cr_cmd(CPM_CR_CH_SMC1,
					CPM_CR_STOP_TX) | CPM_CR_FLG;
		while (cp->cp_cpcr & CPM_CR_FLG);
	}

	/* Make the first buffer the only buffer.
	*/
	tbdf->cbd_sc |= BD_SC_WRAP;
	rbdf->cbd_sc |= BD_SC_EMPTY | BD_SC_WRAP;

	/* Single character receive.
	*/
	up->smc_mrblr = 1;
	up->smc_maxidl = 0;

	/* Initialize Tx/Rx parameters.
	*/
	cp->cp_cpcr = mk_cr_cmd(CPM_CR_CH_SMC1, CPM_CR_INIT_TRX) | CPM_CR_FLG;
	while (cp->cp_cpcr & CPM_CR_FLG);

	/* Enable transmitter/receiver.
	*/
	sp->smc_smcmr |= SMCMR_REN | SMCMR_TEN;
}

void
serial_putchar(const char c)
{
	volatile cbd_t		*tbdf;
	volatile char		*buf;
	volatile smc_uart_t	*up;

	up = (smc_uart_t *)&cpmp->cp_dparam[PROFF_SMC1];
	tbdf = (cbd_t *)&cpmp->cp_dpmem[up->smc_tbase];

	/* Wait for last character to go.
	*/
	buf = (char *)tbdf->cbd_bufaddr;
	while (tbdf->cbd_sc & BD_SC_READY);

	*buf = c;
	tbdf->cbd_datlen = 1;
	tbdf->cbd_sc |= BD_SC_READY;
}

char
serial_getc()
{
	volatile cbd_t		*rbdf;
	volatile char		*buf;
	volatile smc_uart_t	*up;
	char			c;

	up = (smc_uart_t *)&cpmp->cp_dparam[PROFF_SMC1];
	rbdf = (cbd_t *)&cpmp->cp_dpmem[up->smc_rbase];

	/* Wait for character to show up.
	*/
	buf = (char *)rbdf->cbd_bufaddr;
	while (rbdf->cbd_sc & BD_SC_EMPTY);
	c = *buf;
	rbdf->cbd_sc |= BD_SC_EMPTY;

	return(c);
}

int
serial_tstc()
{
	volatile cbd_t		*rbdf;
	volatile smc_uart_t	*up;

	up = (smc_uart_t *)&cpmp->cp_dparam[PROFF_SMC1];
	rbdf = (cbd_t *)&cpmp->cp_dpmem[up->smc_rbase];

	return(!(rbdf->cbd_sc & BD_SC_EMPTY));
}
