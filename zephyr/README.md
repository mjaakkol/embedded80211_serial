# Zephyr subsystem for the project

## Required NCS SDK patch (Zephyr CDC ACM TX drain)

The app relies on `uart_fifo_fill()` (see `src/serial/serial.c:serial_data_tx()`),
which for the USB CDC ACM backend maps to `cdc_acm_fifo_fill()`. The stock NCS
v3.4.1 driver sets the `tx_fifo.altered` flag but does **not** schedule the
`tx_fifo_work` drain work item, so data written from a work context (after the
USB completion that triggered the response) is never enqueued to the bulk IN
endpoint and sits in the TX ring forever. The device then appears to boot and
enumerate but never answers the serial link protocol.

Patch applied to the installed SDK (2026-11-01):

- File: `zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c` (Zephyr submodule
  of the NCS west workspace, e.g. `/home/mikko/ncs/v3.4.1/zephyr/...`)
- Change: in `cdc_acm_fifo_fill()`, after `data->tx_fifo.altered = true;`,
  add `cdc_acm_work_schedule(&data->tx_fifo_work, K_NO_WAIT);` — the same
  scheduling `cdc_acm_poll_out()` already performs.

After any SDK upgrade/`west update`, re-apply this patch or the scan
link will go silent again. Verify with:

    git -C <ncs>/v3.4.1/zephyr diff subsys/usb/device_next/class/usbd_cdc_acm.c
