#define DT_DRV_COMPAT your_vendor_your_uart_controller // Replace with your UART's compatible string from DTS

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h> // For MMIO macros if needed

// #include <your_soc_header.h> // Include SoC specific register definitions if any

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(my_uart_driver, CONFIG_UART_LOG_LEVEL);

/* Device config */
struct my_uart_cfg {
    uint32_t base; // MMIO base address
    uint32_t baud_rate;
    uint8_t irq_num;
    // Add other configuration parameters like clock source, pins, etc.
    void (*irq_config_func)(const struct device *dev);
};

/* Device data */
struct my_uart_data {
    struct uart_config current_config; // Store current UART configuration
    uart_irq_callback_user_data_t cb;  // User callback function
    void *cb_data;                     // User callback data
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    uart_irq_rx_ready_t rx_ready_cb;
    uart_irq_tx_ready_t tx_ready_cb;
    uart_irq_tx_empty_t tx_empty_cb; // If supported by HW
#endif
    // Add other runtime data like FIFO buffers, states, etc.
    // Example:
    // uint8_t rx_buffer[64];
    // uint8_t tx_buffer[64];
    // atomic_t tx_busy_flag;
};

// --- UART API Implementation ---

static int my_uart_poll_in(const struct device *dev, unsigned char *p_char)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;

    // TODO: Implement polling for a received character
    // 1. Check if a character is available in the UART RX register/FIFO
    // 2. If yes, read it into *p_char and return 0
    // 3. If no, return -1
    // Example (pseudo-code):
    // if (UART_REG(config->base, RX_STATUS) & RX_CHAR_AVAILABLE_FLAG) {
    //     *p_char = UART_REG(config->base, RX_DATA_REG);
    //     return 0;
    // }
    return -1;
}

static void my_uart_poll_out(const struct device *dev, unsigned char out_char)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;

    // TODO: Implement polling to send a character
    // 1. Wait until the UART TX register/FIFO is ready to accept a new character
    // 2. Write out_char to the UART TX data register
    // Example (pseudo-code):
    // while (!(UART_REG(config->base, TX_STATUS) & TX_READY_FLAG)) {
    //     k_busy_wait(1); // Or other yielding mechanism if appropriate
    // }
    // UART_REG(config->base, TX_DATA_REG) = out_char;
}

static int my_uart_err_check(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;
    int err = 0;

    // TODO: Implement error checking (parity, framing, overrun)
    // 1. Read the UART error status register
    // 2. Map hardware error flags to Zephyr UART error flags:
    //    UART_ERROR_OVERRUN, UART_ERROR_PARITY, UART_ERROR_FRAMING, UART_ERROR_BREAK
    // Example (pseudo-code):
    // uint32_t status_reg = UART_REG(config->base, ERROR_STATUS_REG);
    // if (status_reg & HW_OVERRUN_ERR_FLAG) err |= UART_ERROR_OVERRUN;
    // if (status_reg & HW_PARITY_ERR_FLAG) err |= UART_ERROR_PARITY;
    // if (status_reg & HW_FRAMING_ERR_FLAG) err |= UART_ERROR_FRAMING;
    // Clear error flags in hardware if necessary
    return err;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int my_uart_configure(const struct device *dev,
                             const struct uart_config *cfg)
{
    // const struct my_uart_cfg *config = dev->config; // Initial config from DTS
    struct my_uart_data *data = dev->data;

    // TODO: Implement runtime UART configuration
    // 1. Validate the requested cfg (baud rate, parity, stop bits, data bits, flow control)
    // 2. Apply the configuration to the UART hardware registers
    //    - Baud rate calculation and setting
    //    - Parity, stop bits, data bits setting
    //    - Flow control (if supported)
    // 3. Store the new configuration in data->current_config
    // Example (pseudo-code):
    // if (cfg->baudrate != 115200 && cfg->baudrate != 9600) return -ENOTSUP;
    // set_hw_baudrate(config->base, cfg->baudrate);
    // set_hw_parity(config->base, cfg->parity);
    // ...
    // data->current_config = *cfg;
    return 0;
}

static int my_uart_config_get(const struct device *dev, struct uart_config *cfg)
{
    struct my_uart_data *data = dev->data;
    *cfg = data->current_config;
    return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int my_uart_fifo_fill(const struct device *dev, const uint8_t *tx_data, int len)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;
    int bytes_written = 0;

    // TODO: Implement interrupt-driven TX: fill TX FIFO
    // 1. Check if TX is busy or FIFO is full (use atomic flags or HW status)
    // 2. Write up to 'len' bytes from tx_data to the TX FIFO
    // 3. Return the number of bytes actually written
    //    This function is typically called with TX IRQ disabled.
    // Example (pseudo-code):
    // for (bytes_written = 0; bytes_written < len; bytes_written++) {
    //    if (UART_TX_FIFO_IS_FULL(config->base)) break;
    //    UART_REG(config->base, TX_DATA_REG) = tx_data[bytes_written];
    // }
    return bytes_written;
}

static int my_uart_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;
    int bytes_read = 0;

    // TODO: Implement interrupt-driven RX: read from RX FIFO
    // 1. Read up to 'size' bytes from RX FIFO into rx_data
    // 2. Return the number of bytes actually read
    //    This function is typically called with RX IRQ disabled.
    // Example (pseudo-code):
    // for (bytes_read = 0; bytes_read < size; bytes_read++) {
    //    if (UART_RX_FIFO_IS_EMPTY(config->base)) break;
    //    rx_data[bytes_read] = UART_REG(config->base, RX_DATA_REG);
    // }
    return bytes_read;
}

static void my_uart_irq_tx_enable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Enable TX ready/empty interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) |= TX_IRQ_ENABLE_FLAG;
}

static void my_uart_irq_tx_disable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Disable TX ready/empty interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) &= ~TX_IRQ_ENABLE_FLAG;
}

static int my_uart_irq_tx_ready(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Check if TX interrupt is enabled and TX FIFO is ready for more data
    // Return 1 if ready, 0 otherwise
    // Example (pseudo-code):
    // bool tx_irq_enabled = (UART_REG(config->base, IRQ_ENABLE_REG) & TX_IRQ_ENABLE_FLAG);
    // bool tx_fifo_has_space = !UART_TX_FIFO_IS_FULL(config->base);
    // return tx_irq_enabled && tx_fifo_has_space;
    return 0;
}

static void my_uart_irq_rx_enable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Enable RX ready interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) |= RX_IRQ_ENABLE_FLAG;
}

static void my_uart_irq_rx_disable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Disable RX ready interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) &= ~RX_IRQ_ENABLE_FLAG;
}

static int my_uart_irq_rx_ready(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Check if RX interrupt is enabled and RX FIFO has data
    // Return 1 if data available, 0 otherwise
    // Example (pseudo-code):
    // bool rx_irq_enabled = (UART_REG(config->base, IRQ_ENABLE_REG) & RX_IRQ_ENABLE_FLAG);
    // bool rx_fifo_has_data = !UART_RX_FIFO_IS_EMPTY(config->base);
    // return rx_irq_enabled && rx_fifo_has_data;
    return 0;
}

static void my_uart_irq_err_enable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Enable error interrupts (overrun, parity, framing) in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) |= ERROR_IRQ_ENABLE_FLAG;
}

static void my_uart_irq_err_disable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Disable error interrupts in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) &= ~ERROR_IRQ_ENABLE_FLAG;
}

static int my_uart_irq_is_pending(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Check if any UART interrupt (TX or RX) is pending
    // Return 1 if an interrupt is pending and enabled, 0 otherwise.
    // This is used by the system to determine if the ISR should be called.
    // Example (pseudo-code):
    // uint32_t status = UART_REG(config->base, IRQ_STATUS_REG);
    // uint32_t enabled = UART_REG(config->base, IRQ_ENABLE_REG);
    // if ((status & enabled & RX_IRQ_PENDING_FLAG) || (status & enabled & TX_IRQ_PENDING_FLAG)) {
    //    return 1;
    // }
    return 0;
}

static int my_uart_irq_update(const struct device *dev)
{
    // TODO: Clear any latched interrupt status bits in the hardware.
    // This function is called after the ISR has processed the interrupts.
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_STATUS_REG) = UART_REG(config->base, IRQ_STATUS_REG); // Write-to-clear
    return 1; // Indicates that the interrupt controller should not do anything.
}

static void my_uart_irq_callback_set(const struct device *dev,
                                     uart_irq_callback_user_data_t cb,
                                     void *cb_data)
{
    struct my_uart_data *data = dev->data;
    data->cb = cb;
    data->cb_data = cb_data;
}

/* Main UART Interrupt Service Routine */
static void my_uart_isr(const struct device *dev)
{
    struct my_uart_data *data = dev->data;
    // const struct my_uart_cfg *config = dev->config;

    // TODO: Implement ISR logic
    // 1. Determine the source of the interrupt (RX, TX, Error)
    // 2. Call the registered callback (data->cb) if it's set.
    //    The callback will typically call fifo_read/fifo_fill.
    // 3. Clear the specific interrupt flags in the hardware.

    if (data->cb) {
        data->cb(dev, data->cb_data);
    }

    // Example of more direct handling if not using the generic callback for everything:
    // uint32_t status = UART_REG(config->base, IRQ_STATUS_REG);
    // if (status & RX_IRQ_PENDING_FLAG) {
    //    if (data->rx_ready_cb) data->rx_ready_cb(dev);
    //    UART_REG(config->base, IRQ_STATUS_REG) = RX_IRQ_PENDING_FLAG; // Clear RX IRQ
    // }
    // if (status & TX_IRQ_PENDING_FLAG) {
    //    if (data->tx_ready_cb) data->tx_ready_cb(dev);
    //    UART_REG(config->base, IRQ_STATUS_REG) = TX_IRQ_PENDING_FLAG; // Clear TX IRQ
    // }
    // ... handle errors ...
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

// --- Initialization ---

static int my_uart_init(const struct device *dev)
{
    const struct my_uart_cfg *config = dev->config;
    struct my_uart_data *data = dev->data;
    int ret;

    // TODO: Basic hardware initialization
    // 1. Enable UART peripheral clock
    // 2. Configure pins for UART TX/RX (usually done via pinctrl in DTS)
    // 3. Set default configuration (baud rate, parity, etc.) from 'config' or a default.
    //    This might involve calling my_uart_configure() if runtime config is not used for initial setup.

    // Example: Set initial baud rate from DTS config
    data->current_config.baudrate = config->baud_rate;
    // ... set other defaults for data->current_config ...

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
    ret = my_uart_configure(dev, &data->current_config);
    if (ret != 0) {
        LOG_ERR("Failed to apply initial UART configuration");
        return ret;
    }
#else
    // Apply fixed configuration from DTS directly to HW
    // set_hw_baudrate(config->base, config->baud_rate);
    // ...
#endif

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    if (config->irq_config_func) {
        config->irq_config_func(dev);
    } else {
        LOG_ERR("IRQ config function not set for %s", dev->name);
        return -EINVAL;
    }
#endif

    LOG_INF("Device %s initialized", dev->name);
    return 0;
}

/* UART Driver API structure */
static const struct uart_driver_api my_uart_driver_api = {
    .poll_in = my_uart_poll_in,
    .poll_out = my_uart_poll_out,
    .err_check = my_uart_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
    .configure = my_uart_configure,
    .config_get = my_uart_config_get,
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    .fifo_fill = my_uart_fifo_fill,
    .fifo_read = my_uart_fifo_read,
    .irq_tx_enable = my_uart_irq_tx_enable,
    .irq_tx_disable = my_uart_irq_tx_disable,
    .irq_tx_ready = my_uart_irq_tx_ready,
    .irq_rx_enable = my_uart_irq_rx_enable,
    .irq_rx_disable = my_uart_irq_rx_disable,
    .irq_rx_ready = my_uart_irq_rx_ready,
    .irq_err_enable = my_uart_irq_err_enable,
    .irq_err_disable = my_uart_irq_err_disable,
    .irq_is_pending = my_uart_irq_is_pending,
    .irq_update = my_uart_irq_update,
    .irq_callback_set = my_uart_irq_callback_set,
#endif
};


// --- Device Instantiation ---
// This uses DT_INST() macros, so your DTS entry needs a 'compatible' property
// that matches DT_DRV_COMPAT defined at the top of this file.
// Example DTS node:
// my_uart0: uart@address {
//      compatible = "your_vendor,your_uart_controller";
//      reg = <address size>;
//      interrupts = <irq_num irq_flags>;
//      current-speed = <115200>;
//      // other properties...
// };

#define MY_UART_INIT(inst) \
    IF_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN, ( \
        static void my_uart_irq_config_func_##inst(const struct device *dev) \
        { \
            IRQ_CONNECT(DT_INST_IRQN(inst), \
                        DT_INST_IRQ(inst, priority), \
                        my_uart_isr, DEVICE_DT_INST_GET(inst), \
                        DT_INST_IRQ(inst, flags)); \
            irq_enable(DT_INST_IRQN(inst)); \
        } \
    )) \
    static struct my_uart_data my_uart_data_##inst = { \
        /* Initialize data fields if necessary, e.g. with DTS properties */ \
    }; \
    static const struct my_uart_cfg my_uart_cfg_##inst = { \
        .base = DT_INST_REG_ADDR(inst), \
        .baud_rate = DT_INST_PROP_OR(inst, current_speed, 115200), \
        .irq_num = DT_INST_IRQN(inst), \
        IF_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN, ( \
            .irq_config_func = my_uart_irq_config_func_##inst, \
        )) \
    }; \
    DEVICE_DT_INST_DEFINE(inst, \
                          &my_uart_init, \
                          NULL, /* No PM support for now */ \
                          &my_uart_data_##inst, \
                          &my_uart_cfg_##inst, \
                          PRE_KERNEL_1, /* or POST_KERNEL, or APPLICATION */ \
                          CONFIG_SERIAL_INIT_PRIORITY, \
                          &my_uart_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MY_UART_INIT)
```
**Key things to customize:**

1.  **`DT_DRV_COMPAT`**: At the very top, change `"your_vendor_your_uart_controller"` to the `compatible` string you will use in your Device Tree Source (DTS) file for this UART peripheral.
2.  **`struct my_uart_cfg`**: Add any other static configuration your UART needs (e.g., clock source identifiers, pin configuration if not fully handled by pinctrl).
3.  **`struct my_uart_data`**: Add any runtime data, like software FIFOs if the hardware FIFO is small, or flags.
4.  **API Function Implementations**:
    *   Replace `// TODO:` comments with actual hardware register manipulations.
    *   Use `config->base` to access the memory-mapped I/O base address of your UART.
    *   You'll need to know the register offsets and bit definitions for your specific UART controller.
5.  **`my_uart_init()`**:
    *   Implement clock enabling for your UART peripheral.
    *   Perform any necessary reset or initial setup of the UART controller.
    *   Set the initial baud rate, parity, data bits, and stop bits based on `config` (from DTS) or sensible defaults.
6.  **Interrupt Handling (`#ifdef CONFIG_UART_INTERRUPT_DRIVEN`)**:
    *   The `my_uart_irq_config_func_##inst` generated by the macro sets up the interrupt connection.
    *   `my_uart_isr()`: This is your main interrupt handler. It needs to check the UART's interrupt status register to see what caused the interrupt (RX data available, TX buffer empty, error) and then call the appropriate Zephyr callback or handle it directly. Remember to clear the interrupt flags in the hardware.
7.  **DTS Entry**: You'll need a corresponding entry in your board's DTS file. For example:
    ```dts
    // In your_board.dts or an overlay
    &soc { // Or wherever your peripherals are defined
        my_uart0: uart@deadbeef { // Use actual address
            compatible = "your_vendor,your_uart_controller"; // Must match DT_DRV_COMPAT
            reg = <0xdeadbeef 0x1000>; // Base address and size
            interrupts = <10 1>;       // IRQ number and flags (e.g., level/edge)
            status = "okay";
            label = "MY_UART_0";
            current-speed = <115200>;
            // pinctrl-0 = <&uart0_tx_pc10 &uart0_rx_pc11>; // Example pinctrl
            // pinctrl-names = "default";
        };
    };
    ```
8.  **Kconfig**: You might need to add Kconfig options for your driver if it has special features or to enable it by default.

This template provides a solid starting point. The most work will be in correctly interacting with your specific UART hardware registers. Refer to your UART peripheral's datasheet extensively.// filepath: /Users/mikko/projects/cfg80211_serial/src/uart.c
#define DT_DRV_COMPAT your_vendor_your_uart_controller // Replace with your UART's compatible string from DTS

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h> // For MMIO macros if needed

// #include <your_soc_header.h> // Include SoC specific register definitions if any

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(my_uart_driver, CONFIG_UART_LOG_LEVEL);

/* Device config */
struct my_uart_cfg {
    uint32_t base; // MMIO base address
    uint32_t baud_rate;
    uint8_t irq_num;
    // Add other configuration parameters like clock source, pins, etc.
    void (*irq_config_func)(const struct device *dev);
};

/* Device data */
struct my_uart_data {
    struct uart_config current_config; // Store current UART configuration
    uart_irq_callback_user_data_t cb;  // User callback function
    void *cb_data;                     // User callback data
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    uart_irq_rx_ready_t rx_ready_cb;
    uart_irq_tx_ready_t tx_ready_cb;
    uart_irq_tx_empty_t tx_empty_cb; // If supported by HW
#endif
    // Add other runtime data like FIFO buffers, states, etc.
    // Example:
    // uint8_t rx_buffer[64];
    // uint8_t tx_buffer[64];
    // atomic_t tx_busy_flag;
};

// --- UART API Implementation ---

static int my_uart_poll_in(const struct device *dev, unsigned char *p_char)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;

    // TODO: Implement polling for a received character
    // 1. Check if a character is available in the UART RX register/FIFO
    // 2. If yes, read it into *p_char and return 0
    // 3. If no, return -1
    // Example (pseudo-code):
    // if (UART_REG(config->base, RX_STATUS) & RX_CHAR_AVAILABLE_FLAG) {
    //     *p_char = UART_REG(config->base, RX_DATA_REG);
    //     return 0;
    // }
    return -1;
}

static void my_uart_poll_out(const struct device *dev, unsigned char out_char)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;

    // TODO: Implement polling to send a character
    // 1. Wait until the UART TX register/FIFO is ready to accept a new character
    // 2. Write out_char to the UART TX data register
    // Example (pseudo-code):
    // while (!(UART_REG(config->base, TX_STATUS) & TX_READY_FLAG)) {
    //     k_busy_wait(1); // Or other yielding mechanism if appropriate
    // }
    // UART_REG(config->base, TX_DATA_REG) = out_char;
}

static int my_uart_err_check(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;
    int err = 0;

    // TODO: Implement error checking (parity, framing, overrun)
    // 1. Read the UART error status register
    // 2. Map hardware error flags to Zephyr UART error flags:
    //    UART_ERROR_OVERRUN, UART_ERROR_PARITY, UART_ERROR_FRAMING, UART_ERROR_BREAK
    // Example (pseudo-code):
    // uint32_t status_reg = UART_REG(config->base, ERROR_STATUS_REG);
    // if (status_reg & HW_OVERRUN_ERR_FLAG) err |= UART_ERROR_OVERRUN;
    // if (status_reg & HW_PARITY_ERR_FLAG) err |= UART_ERROR_PARITY;
    // if (status_reg & HW_FRAMING_ERR_FLAG) err |= UART_ERROR_FRAMING;
    // Clear error flags in hardware if necessary
    return err;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int my_uart_configure(const struct device *dev,
                             const struct uart_config *cfg)
{
    // const struct my_uart_cfg *config = dev->config; // Initial config from DTS
    struct my_uart_data *data = dev->data;

    // TODO: Implement runtime UART configuration
    // 1. Validate the requested cfg (baud rate, parity, stop bits, data bits, flow control)
    // 2. Apply the configuration to the UART hardware registers
    //    - Baud rate calculation and setting
    //    - Parity, stop bits, data bits setting
    //    - Flow control (if supported)
    // 3. Store the new configuration in data->current_config
    // Example (pseudo-code):
    // if (cfg->baudrate != 115200 && cfg->baudrate != 9600) return -ENOTSUP;
    // set_hw_baudrate(config->base, cfg->baudrate);
    // set_hw_parity(config->base, cfg->parity);
    // ...
    // data->current_config = *cfg;
    return 0;
}

static int my_uart_config_get(const struct device *dev, struct uart_config *cfg)
{
    struct my_uart_data *data = dev->data;
    *cfg = data->current_config;
    return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int my_uart_fifo_fill(const struct device *dev, const uint8_t *tx_data, int len)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;
    int bytes_written = 0;

    // TODO: Implement interrupt-driven TX: fill TX FIFO
    // 1. Check if TX is busy or FIFO is full (use atomic flags or HW status)
    // 2. Write up to 'len' bytes from tx_data to the TX FIFO
    // 3. Return the number of bytes actually written
    //    This function is typically called with TX IRQ disabled.
    // Example (pseudo-code):
    // for (bytes_written = 0; bytes_written < len; bytes_written++) {
    //    if (UART_TX_FIFO_IS_FULL(config->base)) break;
    //    UART_REG(config->base, TX_DATA_REG) = tx_data[bytes_written];
    // }
    return bytes_written;
}

static int my_uart_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
    // const struct my_uart_cfg *config = dev->config;
    // struct my_uart_data *data = dev->data;
    int bytes_read = 0;

    // TODO: Implement interrupt-driven RX: read from RX FIFO
    // 1. Read up to 'size' bytes from RX FIFO into rx_data
    // 2. Return the number of bytes actually read
    //    This function is typically called with RX IRQ disabled.
    // Example (pseudo-code):
    // for (bytes_read = 0; bytes_read < size; bytes_read++) {
    //    if (UART_RX_FIFO_IS_EMPTY(config->base)) break;
    //    rx_data[bytes_read] = UART_REG(config->base, RX_DATA_REG);
    // }
    return bytes_read;
}

static void my_uart_irq_tx_enable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Enable TX ready/empty interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) |= TX_IRQ_ENABLE_FLAG;
}

static void my_uart_irq_tx_disable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Disable TX ready/empty interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) &= ~TX_IRQ_ENABLE_FLAG;
}

static int my_uart_irq_tx_ready(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Check if TX interrupt is enabled and TX FIFO is ready for more data
    // Return 1 if ready, 0 otherwise
    // Example (pseudo-code):
    // bool tx_irq_enabled = (UART_REG(config->base, IRQ_ENABLE_REG) & TX_IRQ_ENABLE_FLAG);
    // bool tx_fifo_has_space = !UART_TX_FIFO_IS_FULL(config->base);
    // return tx_irq_enabled && tx_fifo_has_space;
    return 0;
}

static void my_uart_irq_rx_enable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Enable RX ready interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) |= RX_IRQ_ENABLE_FLAG;
}

static void my_uart_irq_rx_disable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Disable RX ready interrupt in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) &= ~RX_IRQ_ENABLE_FLAG;
}

static int my_uart_irq_rx_ready(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Check if RX interrupt is enabled and RX FIFO has data
    // Return 1 if data available, 0 otherwise
    // Example (pseudo-code):
    // bool rx_irq_enabled = (UART_REG(config->base, IRQ_ENABLE_REG) & RX_IRQ_ENABLE_FLAG);
    // bool rx_fifo_has_data = !UART_RX_FIFO_IS_EMPTY(config->base);
    // return rx_irq_enabled && rx_fifo_has_data;
    return 0;
}

static void my_uart_irq_err_enable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Enable error interrupts (overrun, parity, framing) in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) |= ERROR_IRQ_ENABLE_FLAG;
}

static void my_uart_irq_err_disable(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Disable error interrupts in UART hardware
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_ENABLE_REG) &= ~ERROR_IRQ_ENABLE_FLAG;
}

static int my_uart_irq_is_pending(const struct device *dev)
{
    // const struct my_uart_cfg *config = dev->config;
    // TODO: Check if any UART interrupt (TX or RX) is pending
    // Return 1 if an interrupt is pending and enabled, 0 otherwise.
    // This is used by the system to determine if the ISR should be called.
    // Example (pseudo-code):
    // uint32_t status = UART_REG(config->base, IRQ_STATUS_REG);
    // uint32_t enabled = UART_REG(config->base, IRQ_ENABLE_REG);
    // if ((status & enabled & RX_IRQ_PENDING_FLAG) || (status & enabled & TX_IRQ_PENDING_FLAG)) {
    //    return 1;
    // }
    return 0;
}

static int my_uart_irq_update(const struct device *dev)
{
    // TODO: Clear any latched interrupt status bits in the hardware.
    // This function is called after the ISR has processed the interrupts.
    // Example (pseudo-code):
    // UART_REG(config->base, IRQ_STATUS_REG) = UART_REG(config->base, IRQ_STATUS_REG); // Write-to-clear
    return 1; // Indicates that the interrupt controller should not do anything.
}

static void my_uart_irq_callback_set(const struct device *dev,
                                     uart_irq_callback_user_data_t cb,
                                     void *cb_data)
{
    struct my_uart_data *data = dev->data;
    data->cb = cb;
    data->cb_data = cb_data;
}

/* Main UART Interrupt Service Routine */
static void my_uart_isr(const struct device *dev)
{
    struct my_uart_data *data = dev->data;
    // const struct my_uart_cfg *config = dev->config;

    // TODO: Implement ISR logic
    // 1. Determine the source of the interrupt (RX, TX, Error)
    // 2. Call the registered callback (data->cb) if it's set.
    //    The callback will typically call fifo_read/fifo_fill.
    // 3. Clear the specific interrupt flags in the hardware.

    if (data->cb) {
        data->cb(dev, data->cb_data);
    }

    // Example of more direct handling if not using the generic callback for everything:
    // uint32_t status = UART_REG(config->base, IRQ_STATUS_REG);
    // if (status & RX_IRQ_PENDING_FLAG) {
    //    if (data->rx_ready_cb) data->rx_ready_cb(dev);
    //    UART_REG(config->base, IRQ_STATUS_REG) = RX_IRQ_PENDING_FLAG; // Clear RX IRQ
    // }
    // if (status & TX_IRQ_PENDING_FLAG) {
    //    if (data->tx_ready_cb) data->tx_ready_cb(dev);
    //    UART_REG(config->base, IRQ_STATUS_REG) = TX_IRQ_PENDING_FLAG; // Clear TX IRQ
    // }
    // ... handle errors ...
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

// --- Initialization ---

static int my_uart_init(const struct device *dev)
{
    const struct my_uart_cfg *config = dev->config;
    struct my_uart_data *data = dev->data;
    int ret;

    // TODO: Basic hardware initialization
    // 1. Enable UART peripheral clock
    // 2. Configure pins for UART TX/RX (usually done via pinctrl in DTS)
    // 3. Set default configuration (baud rate, parity, etc.) from 'config' or a default.
    //    This might involve calling my_uart_configure() if runtime config is not used for initial setup.

    // Example: Set initial baud rate from DTS config
    data->current_config.baudrate = config->baud_rate;
    // ... set other defaults for data->current_config ...

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
    ret = my_uart_configure(dev, &data->current_config);
    if (ret != 0) {
        LOG_ERR("Failed to apply initial UART configuration");
        return ret;
    }
#else
    // Apply fixed configuration from DTS directly to HW
    // set_hw_baudrate(config->base, config->baud_rate);
    // ...
#endif

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    if (config->irq_config_func) {
        config->irq_config_func(dev);
    } else {
        LOG_ERR("IRQ config function not set for %s", dev->name);
        return -EINVAL;
    }
#endif

    LOG_INF("Device %s initialized", dev->name);
    return 0;
}

/* UART Driver API structure */
static const struct uart_driver_api my_uart_driver_api = {
    .poll_in = my_uart_poll_in,
    .poll_out = my_uart_poll_out,
    .err_check = my_uart_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
    .configure = my_uart_configure,
    .config_get = my_uart_config_get,
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    .fifo_fill = my_uart_fifo_fill,
    .fifo_read = my_uart_fifo_read,
    .irq_tx_enable = my_uart_irq_tx_enable,
    .irq_tx_disable = my_uart_irq_tx_disable,
    .irq_tx_ready = my_uart_irq_tx_ready,
    .irq_rx_enable = my_uart_irq_rx_enable,
    .irq_rx_disable = my_uart_irq_rx_disable,
    .irq_rx_ready = my_uart_irq_rx_ready,
    .irq_err_enable = my_uart_irq_err_enable,
    .irq_err_disable = my_uart_irq_err_disable,
    .irq_is_pending = my_uart_irq_is_pending,
    .irq_update = my_uart_irq_update,
    .irq_callback_set = my_uart_irq_callback_set,
#endif
};


// --- Device Instantiation ---
// This uses DT_INST() macros, so your DTS entry needs a 'compatible' property
// that matches DT_DRV_COMPAT defined at the top of this file.
// Example DTS node:
// my_uart0: uart@address {
//      compatible = "your_vendor,your_uart_controller";
//      reg = <address size>;
//      interrupts = <irq_num irq_flags>;
//      current-speed = <115200>;
//      // other properties...
// };

#define MY_UART_INIT(inst) \
    IF_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN, ( \
        static void my_uart_irq_config_func_##inst(const struct device *dev) \
        { \
            IRQ_CONNECT(DT_INST_IRQN(inst), \
                        DT_INST_IRQ(inst, priority), \
                        my_uart_isr, DEVICE_DT_INST_GET(inst), \
                        DT_INST_IRQ(inst, flags)); \
            irq_enable(DT_INST_IRQN(inst)); \
        } \
    )) \
    static struct my_uart_data my_uart_data_##inst = { \
        /* Initialize data fields if necessary, e.g. with DTS properties */ \
    }; \
    static const struct my_uart_cfg my_uart_cfg_##inst = { \
        .base = DT_INST_REG_ADDR(inst), \
        .baud_rate = DT_INST_PROP_OR(inst, current_speed, 115200), \
        .irq_num = DT_INST_IRQN(inst), \
        IF_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN, ( \
            .irq_config_func = my_uart_irq_config_func_##inst, \
        )) \
    }; \
    DEVICE_DT_INST_DEFINE(inst, \
                          &my_uart_init, \
                          NULL, /* No PM support for now */ \
                          &my_uart_data_##inst, \
                          &my_uart_cfg_##inst, \
                          PRE_KERNEL_1, /* or POST_KERNEL, or APPLICATION */ \
                          CONFIG_SERIAL_INIT_PRIORITY, \
                          &my_uart_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MY_UART_INIT)
```
**Key things to customize:**

1.  **`DT_DRV_COMPAT`**: At the very top, change `"your_vendor_your_uart_controller"` to the `compatible` string you will use in your Device Tree Source (DTS) file for this UART peripheral.
2.  **`struct my_uart_cfg`**: Add any other static configuration your UART needs (e.g., clock source identifiers, pin configuration if not fully handled by pinctrl).
3.  **`struct my_uart_data`**: Add any runtime data, like software FIFOs if the hardware FIFO is small, or flags.
4.  **API Function Implementations**:
    *   Replace `// TODO:` comments with actual hardware register manipulations.
    *   Use `config->base` to access the memory-mapped I/O base address of your UART.
    *   You'll need to know the register offsets and bit definitions for your specific UART controller.
5.  **`my_uart_init()`**:
    *   Implement clock enabling for your UART peripheral.
    *   Perform any necessary reset or initial setup of the UART controller.
    *   Set the initial baud rate, parity, data bits, and stop bits based on `config` (from DTS) or sensible defaults.
6.  **Interrupt Handling (`#ifdef CONFIG_UART_INTERRUPT_DRIVEN`)**:
    *   The `my_uart_irq_config_func_##inst` generated by the macro sets up the interrupt connection.
    *   `my_uart_isr()`: This is your main interrupt handler. It needs to check the UART's interrupt status register to see what caused the interrupt (RX data available, TX buffer empty, error) and then call the appropriate Zephyr callback or handle it directly. Remember to clear the interrupt flags in the hardware.
7.  **DTS Entry**: You'll need a corresponding entry in your board's DTS file. For example:
    ```dts
    // In your_board.dts or an overlay
    &soc { // Or wherever your peripherals are defined
        my_uart0: uart@deadbeef { // Use actual address
            compatible = "your_vendor,your_uart_controller"; // Must match DT_DRV_COMPAT
            reg = <0xdeadbeef 0x1000>; // Base address and size
            interrupts = <10 1>;       // IRQ number and flags (e.g., level/edge)
            status = "okay";
            label = "MY_UART_0";
            current-speed = <115200>;
            // pinctrl-0 = <&uart0_tx_pc10 &uart0_rx_pc11>; // Example pinctrl
            // pinctrl-names = "default";
        };
    };
    ```
8.  **Kconfig**: You might need to add Kconfig options for your driver if it has special features or to enable it by default.

This template provides a solid starting point. The most work will be in correctly interacting with your specific UART hardware registers. Refer to your UART peripheral's datasheet extensively.