#ifndef _WIFI_DATA_H_
#define _WIFI_DATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/net/net_if.h>

bool wifi_data_acquire_buffer(uint8_t **data, size_t len);
void wifi_data_commit_data(size_t len);
void wifi_data_message_complete(size_t len);

/**
 * @brief Transmit a raw IEEE 802.11 frame on the air interface.
 *
 * The frame must be a complete MAC frame (frame control through payload);
 * the driver appends the FCS. This path bypasses the TCP/IP stack.
 *
 * @param iface  Wi-Fi network interface to transmit on.
 * @param frame  Pointer to the raw 802.11 frame bytes.
 * @param len    Frame length in bytes (24..2346, FCS not included).
 *
 * @return 0 on success, negative errno on failure.
 */
int wifi_data_raw_frame_tx(struct net_if *iface, const uint8_t *frame, size_t len);

/**
 * @brief Callback invoked when a raw 802.11 frame is received on the air
 *        interface (raw/promiscuous RX enabled).
 *
 * @param iface  Wi-Fi interface the frame was received on.
 * @param frame  Pointer to the raw received frame bytes (may include a
 *               driver prefix before the 802.11 header).
 * @param len    Frame length in bytes.
 */
typedef void (*wifi_data_raw_rx_cb)(struct net_if *iface, const uint8_t *frame,
				    size_t len);

/**
 * @brief Register the raw-RX receive callback.
 */
void wifi_data_set_raw_rx_cb(wifi_data_raw_rx_cb cb);

/**
 * @brief Enable raw 802.11 RX capture (monitor-mode sniffer) so the device can
 *        capture the frames it transmits (data connectivity loopback testing).
 *        Switches the radio to monitor mode on @p channel and passes all frames
 *        to the network stack. Idempotent.
 *
 * @param iface    Wi-Fi interface to capture on.
 * @param channel  2.4 GHz channel to listen on (0 = driver default).
 *
 * @return 0 on success, negative errno on failure.
 */
int wifi_data_raw_frame_rx_enable(struct net_if *iface, uint32_t channel);

/**
 * @brief Disable raw 802.11 RX. Idempotent.
 *
 * @param iface  Wi-Fi interface to stop capturing on.
 *
 * @return 0 on success, negative errno on failure.
 */
int wifi_data_raw_frame_rx_disable(struct net_if *iface);

/**
 * @brief Diagnostic state of the raw-RX capture path.
 */
struct wifi_data_raw_rx_diag {
	int socket_fd;        /* AF_PACKET socket fd (-1 = not created). */
	bool thread_running;  /* Whether the RX thread is active. */
	uint32_t frames_received; /* Cumulative frames since enable. */
	uint32_t current_mode;  /* Wi-Fi mode read-back after set (0=not read). */
	int start_zep_step;   /* Last step in nrf_wifi_if_start_zep (0=not started, 10=ok). */
	int tx_socket_fd;     /* TX AF_PACKET socket fd (-1 = not created). */
	uint32_t tx_frames_sent; /* Cumulative TX frames sent. */
	int tx_injection_ret; /* Last return from net_eth_txinjection_mode(). */
	int tx_setup_ret;     /* TX socket setup step (0=ok, 100+=socket-fail, 200+=bind-fail). */
	int tx_last_ret;      /* Last return from wifi_data_raw_frame_tx(). */
	uint32_t tx_last_len; /* Last frame length passed to wifi_data_raw_frame_tx(). */
	int tx_last_wifi;     /* net_if_is_wifi() result for last TX call. */
};
/**
 * @brief Get the current diagnostic state of the raw-RX capture path.
 */
void wifi_data_raw_rx_diag(struct wifi_data_raw_rx_diag *out);

#endif // _WIFI_DATA_H_
