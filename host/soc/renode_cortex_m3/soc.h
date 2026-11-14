/*
 * Renode virtual Cortex-M3 SoC (host application).
 *
 * Minimal CMSIS SoC header: no SoC-specific IRQs beyond the two PL011
 * UARTs (their numbers come from the devicetree).
 */

#ifndef _SOCS_RENODE_CORTEX_M3_SOC_H_
#define _SOCS_RENODE_CORTEX_M3_SOC_H_

#include <cmsis_core_m_defaults.h>
#include <zephyr/sys/util.h>

/* default system clock (matches the Renode machine systick frequency) */

#define SYSCLK_DEFAULT_HZ MHZ(100)

#endif /* _SOCS_RENODE_CORTEX_M3_SOC_H_ */
