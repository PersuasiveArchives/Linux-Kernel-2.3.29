/****************************************************************************
 *	Defines for the Multi-KISS driver.
 ****************************************************************************/

#define AX25_MAXDEV	16		/* MAX number of AX25 channels;
					   This can be overridden with
					   insmod -oax25_maxdev=nnn	*/
#define AX_MTU		236	

/* SLIP/KISS protocol characters. */
#define END             0300		/* indicates end of frame	*/
#define ESC             0333		/* indicates byte stuffing	*/
#define ESC_END         0334		/* ESC ESC_END means END 'data'	*/
#define ESC_ESC         0335		/* ESC ESC_ESC means ESC 'data'	*/

struct ax_disp {
	int                magic;

	/* Various fields. */
	struct tty_struct  *tty;		/* ptr to TTY structure		*/
	struct net_device      *dev;		/* easy for intr handling	*/
	struct ax_disp     *mkiss;		/* mkiss txport if mkiss channel*/

	/* These are pointers to the malloc()ed frame buffers. */
	unsigned char      *rbuff;		/* receiver buffer		*/
	int                rcount;		/* received chars counter       */
	unsigned char      *xbuff;		/* transmitter buffer		*/
	unsigned char      *xhead;		/* pointer to next byte to XMIT */
	int                xleft;		/* bytes left in XMIT queue     */

	/* SLIP interface statistics. */
	unsigned long      rx_packets;		/* inbound frames counter	*/
	unsigned long      tx_packets;		/* outbound frames counter      */
	unsigned long      rx_errors;		/* Parity, etc. errors          */
	unsigned long      tx_errors;		/* Planned stuff                */
	unsigned long      rx_dropped;		/* No memory for skb            */
	unsigned long      tx_dropped;		/* When MTU change              */
	unsigned long      rx_over_errors;	/* Frame bigger then SLIP buf.  */

	/* Detailed SLIP statistics. */
	int                 mtu;		/* Our mtu (to spot changes!)   */
	int                 buffsize;		/* Max buffers sizes            */


	unsigned char       flags;		/* Flag values/ mode etc	*/
#define AXF_INUSE	0		/* Channel in use               */
#define AXF_ESCAPE	1               /* ESC received                 */
#define AXF_ERROR	2               /* Parity, etc. error           */
#define AXF_KEEPTEST	3		/* Keepalive test flag		*/
#define AXF_OUTWAIT	4		/* is outpacket was flag	*/

	int                 mode;
        int                 crcmode;    /* MW: for FlexNet, SMACK etc.  */ 
#define CRC_MODE_NONE   0
#define CRC_MODE_FLEX   1
#define CRC_MODE_SMACK  2
};

#define AX25_MAGIC		0x5316
#define MKISS_DRIVER_MAGIC	1215
