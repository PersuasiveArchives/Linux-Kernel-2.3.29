/* dec_esp.h: Defines and structures for the JAZZ SCSI driver.
 *
 * DECstation changes Copyright (C) 1998 Harald Koerfgen
 *
 * based on jazz_esp.h:
 * Copyright (C) 1997 Thomas Bogendoerfer (tsbogend@alpha.franken.de)
 */

#ifndef DEC_ESP_H
#define DEC_ESP_H

#define EREGS_PAD(n)     unchar n[3];

#include "NCR53C9x.h"


extern int dec_esp_detect(struct SHT *);
extern const char *esp_info(struct Scsi_Host *);
extern int esp_queue(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
extern int esp_command(Scsi_Cmnd *);
extern int esp_abort(Scsi_Cmnd *);
extern int esp_reset(Scsi_Cmnd *, unsigned int);
extern int esp_proc_info(char *buffer, char **start, off_t offset, int length,
			 int hostno, int inout);

#define SCSI_DEC_ESP {                                         \
		proc_name:      "esp",				\
		proc_info:      &esp_proc_info,			\
		name:           "PMAZ-AA",			\
		detect:         dec_esp_detect,			\
		info:           esp_info,			\
		command:        esp_command,			\
		queuecommand:   esp_queue,			\
		abort:          esp_abort,			\
		reset:          esp_reset,			\
		can_queue:      7,				\
		this_id:        7,				\
		sg_tablesize:   SG_ALL,				\
		cmd_per_lun:    1,				\
		use_clustering: DISABLE_CLUSTERING, }

#endif /* DEC_ESP_H */
