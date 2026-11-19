/*
 * Renode virtual Cortex-M33 SoC (host application).
 */

#ifndef __RENODE_CORTEX_M33_SOC_H_
#define __RENODE_CORTEX_M33_SOC_H_

#include <zephyr/arch/arm/cortex_m/nvic.h>

/* CMSIS required values */
typedef enum {
	Reset_IRQn            = -15,
	NonMaskableInt_IRQn   = -14,
	HardFault_IRQn        = -13,
	MemoryManagement_IRQn = -12,
	BusFault_IRQn         = -11,
	UsageFault_IRQn       = -10,
	SVCall_IRQn           =  -5,
	DebugMonitor_IRQn     =  -4,
	PendSV_IRQn           =  -2,
	SysTick_IRQn          = -1,
} IRQn_Type;

#define __CM33_REV_S           0
#define __MPU_PRESENT          0
#define __NVIC_PRIO_BITS       NUM_IRQ_PRIO_BITS
#define __Vendor_SysTickConfig 0
#define __FPU_PRESENT          0

#include <core_cm33.h>

#endif /* __RENODE_CORTEX_M33_SOC_H_ */
