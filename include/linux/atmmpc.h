#ifndef _ATMMPC_H_
#define _ATMMPC_H_

#include <linux/atmioc.h>
#include <linux/atm.h>

#define ATMMPC_CTRL _IO('a', ATMIOC_MPOA)
#define ATMMPC_DATA _IO('a', ATMIOC_MPOA+1)

#define MPC_SOCKET_INGRESS 1
#define MPC_SOCKET_EGRESS  2

struct atmmpc_ioc {
        int dev_num;
        uint32_t ipaddr;              /* the IP address of the shortcut    */
        int type;                     /* ingress or egress                 */
};

typedef struct in_ctrl_info {
        uint8_t   Last_NHRP_CIE_code;
        uint8_t   Last_Q2931_cause_value;     
        uint8_t   eg_MPC_ATM_addr[ATM_ESA_LEN];
        uint32_t  tag;
        uint32_t  in_dst_ip;      /* IP address this ingress MPC sends packets to */
        uint16_t  holding_time;
        uint32_t  request_id;
} in_ctrl_info;

typedef struct eg_ctrl_info {
        uint8_t   DLL_header[256];
        uint8_t   DH_length;
        uint32_t  cache_id;
        uint32_t  tag;
        uint32_t  mps_ip;
        uint32_t  eg_dst_ip;      /* IP address to which ingress MPC sends packets */
        uint8_t   in_MPC_data_ATM_addr[ATM_ESA_LEN];
        uint16_t  holding_time;
} eg_ctrl_info;

struct mpc_parameters{
        uint16_t mpc_p1;   /* Shortcut-Setup Frame Count    */
        uint16_t mpc_p2;   /* Shortcut-Setup Frame Time     */
        uint8_t mpc_p3[8]; /* Flow-detection Protocols      */
        uint16_t mpc_p4;   /* MPC Initial Retry Time        */
        uint16_t mpc_p5;   /* MPC Retry Time Maximum        */
        uint16_t mpc_p6;   /* Hold Down Time                */      
};

struct k_message{
        uint16_t type;
        uint32_t ip_mask;
        uint8_t  MPS_ctrl[ATM_ESA_LEN];
        union {
                in_ctrl_info in_info;
                eg_ctrl_info eg_info;
                struct mpc_parameters params;
        } content;
        struct atm_qos qos;       
};

struct llc_snap_hdr { /* RFC 1483 LLC/SNAP encapsulation for routed IP PDUs */
        uint8_t  dsap;    /* Destination Service Access Point (0xAA)     */
        uint8_t  ssap;    /* Source Service Access Point      (0xAA)     */
        uint8_t  ui;      /* Unnumbered Information           (0x03)     */
        uint8_t  org[3];  /* Organizational identification    (0x000000) */
        uint8_t  type[2]; /* Ether type (for IP)              (0x0800)   */
};

/* TLVs this MPC recognizes */
#define TLV_MPOA_DEVICE_TYPE         0x00a03e2a  

/* MPOA device types in MPOA Device Type TLV */
#define NON_MPOA    0
#define MPS         1
#define MPC         2
#define MPS_AND_MPC 3


/* MPC parameter defaults */

#define MPC_P1 10  /* Shortcut-Setup Frame Count  */ 
#define MPC_P2 1   /* Shortcut-Setup Frame Time   */
#define MPC_P3 0   /* Flow-detection Protocols    */
#define MPC_P4 5   /* MPC Initial Retry Time      */
#define MPC_P5 40  /* MPC Retry Time Maximum      */
#define MPC_P6 160 /* Hold Down Time              */
#define HOLDING_TIME_DEFAULT 1200 /* same as MPS-p7 */

/* MPC constants */

#define MPC_C1 2   /* Retry Time Multiplier       */
#define MPC_C2 60  /* Initial Keep-Alive Lifetime */

/* Message types - to MPOA daemon */

#define SND_MPOA_RES_RQST    201
#define SET_MPS_CTRL_ADDR    202
#define SND_MPOA_RES_RTRY    203 /* Different type in a retry due to req id         */
#define STOP_KEEP_ALIVE_SM   204
#define EGRESS_ENTRY_REMOVED 205
#define SND_EGRESS_PURGE     206
#define DIE                  207 /* tell the daemon to exit()                       */
#define DATA_PLANE_PURGE     208 /* Data plane purge because of egress cache hit miss or dead MPS */
#define OPEN_INGRESS_SVC     209

/* Message types - from MPOA daemon */

#define MPOA_TRIGGER_RCVD     101
#define MPOA_RES_REPLY_RCVD   102
#define INGRESS_PURGE_RCVD    103
#define EGRESS_PURGE_RCVD     104
#define MPS_DEATH             105
#define CACHE_IMPOS_RCVD      106
#define SET_MPC_CTRL_ADDR     107 /* Our MPC's control ATM address   */
#define SET_MPS_MAC_ADDR      108
#define CLEAN_UP_AND_EXIT     109
#define SET_MPC_PARAMS        110 /* MPC configuration parameters    */

/* Message types - bidirectional */       

#define RELOAD                301 /* kill -HUP the daemon for reload */

#endif /* _ATMMPC_H_ */

