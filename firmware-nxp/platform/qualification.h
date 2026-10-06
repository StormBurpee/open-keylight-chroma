#ifndef NXP_QUALIFICATION_H
#define NXP_QUALIFICATION_H

/* Never set these from an assumed chip family or a passing mock test.
 * Populate only after recording physical/board-specific qualification. */
#define NXP_REFERENCE_PART_ID 0x0000bc40u /* Observed ROM IAP54: LPC11U35/501. */
#ifndef NXP_SPI_ONLY_TRIAL
#define NXP_SPI_ONLY_TRIAL 0
#endif
#ifndef NXP_APPROVED_PART_ID
#define NXP_APPROVED_PART_ID 0u
#endif
#ifndef NXP_BOARD_QUALIFICATIONS
#define NXP_BOARD_QUALIFICATIONS 0u
#endif
#ifndef NXP_QUALIFIED_SPI_MODE
#define NXP_QUALIFIED_SPI_MODE 0u
#endif

#endif
