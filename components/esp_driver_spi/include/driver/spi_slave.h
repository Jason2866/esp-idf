/*
 * SPDX-FileCopyrightText: 2010-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DRIVER_SPI_SLAVE_H_
#define _DRIVER_SPI_SLAVE_H_

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SPI_SLAVE_TXBIT_LSBFIRST          (1<<0)  ///< Transmit command/address/data LSB first instead of the default MSB first
#define SPI_SLAVE_RXBIT_LSBFIRST          (1<<1)  ///< Receive data LSB first instead of the default MSB first
#define SPI_SLAVE_BIT_LSBFIRST            (SPI_SLAVE_TXBIT_LSBFIRST|SPI_SLAVE_RXBIT_LSBFIRST) ///< Transmit and receive LSB first
#define SPI_SLAVE_NO_RETURN_RESULT        (1<<2)  ///< Don't return the descriptor to the host on completion (use `post_trans_cb` to notify instead)

/** @cond */
typedef struct spi_slave_transaction_t spi_slave_transaction_t;
/** @endcond */
typedef void(*slave_transaction_cb_t)(spi_slave_transaction_t *trans);

/**
 * @brief This is a configuration for a SPI host acting as a slave device.
 */
typedef struct {
    int spics_io_num;               ///< CS GPIO pin for this device
    uint32_t flags;                 ///< Bitwise OR of SPI_SLAVE_* flags
    int queue_size;                 ///< Transaction queue size. This sets how many transactions can be 'in the air' (queued using spi_slave_queue_trans but not yet finished using spi_slave_get_trans_result) at the same time
    uint8_t mode;                   /**< SPI mode, representing a pair of (CPOL, CPHA) configuration:
                                         - 0: (0, 0)
                                         - 1: (0, 1)
                                         - 2: (1, 0)
                                         - 3: (1, 1)
                                     */
    slave_transaction_cb_t post_setup_cb;  /**< Callback called after the SPI registers are loaded with new data.
                                             *
                                             *  This callback is called within interrupt
                                             *  context should be in IRAM for best
                                             *  performance, see "Transferring Speed"
                                             *  section in the SPI Master documentation for
                                             *  full details. If not, the callback may crash
                                             *  during flash operation when the driver is
                                             *  initialized with ESP_INTR_FLAG_IRAM.
                                             */
    slave_transaction_cb_t post_trans_cb;  /**< Callback called after a transaction is done.
                                             *
                                             *  This callback is called within interrupt
                                             *  context should be in IRAM for best
                                             *  performance, see "Transferring Speed"
                                             *  section in the SPI Master documentation for
                                             *  full details. If not, the callback may crash
                                             *  during flash operation when the driver is
                                             *  initialized with ESP_INTR_FLAG_IRAM.
                                             */
} spi_slave_interface_config_t;

#define SPI_SLAVE_TRANS_DMA_BUFFER_ALIGN_AUTO   (1<<0)    ///< Automatically re-malloc dma buffer if user buffer doesn't meet hardware alignment or dma_capable, this process may loss some memory and performance

/**
 * This structure describes one SPI transaction
 */
struct spi_slave_transaction_t {
    uint32_t flags;                 ///< Bitwise OR of SPI_SLAVE_TRANS_* flags
    size_t length;                  ///< Total data length, in bits
    size_t trans_len;               ///< Transaction data length, in bits
    const void *tx_buffer;          ///< Pointer to transmit buffer, or NULL for no MOSI phase
    void *rx_buffer;                /**< Pointer to receive buffer, or NULL for no MISO phase.
                                     * When the DMA is enabled, must start at WORD boundary (``rx_buffer%4==0``),
                                     * and has length of a multiple of 4 bytes.
                                     */
    void *user;                     ///< User-defined variable. Can be used to store eg transaction ID.
};

/**
 * @brief Initialize a SPI bus as a slave interface and enable it by default
 *
 * @warning SPI0/1 is not supported
 *
 * @param host          SPI peripheral to use as a SPI slave interface
 * @param bus_config    Pointer to a spi_bus_config_t struct specifying how the host should be initialized
 * @param slave_config  Pointer to a spi_slave_interface_config_t struct specifying the details for the slave interface
 * @param dma_chan      - Selecting a DMA channel for an SPI bus allows transactions on the bus with size only limited by the amount of internal memory.
 *                      - Selecting SPI_DMA_DISABLED limits the size of transactions.
 *                      - Set to SPI_DMA_DISABLED if only the SPI flash uses this bus.
 *                      - Set to SPI_DMA_CH_AUTO to let the driver to allocate the DMA channel.
 *
 * @warning If a DMA channel is selected, any transmit and receive buffer used should be allocated in
 *          DMA-capable memory.
 *
 * @warning The ISR of SPI is always executed on the core which calls this
 *          function. Never starve the ISR on this core or the SPI transactions will not
 *          be handled.
 *
 * @return
 *         - ESP_ERR_INVALID_ARG   if configuration is invalid
 *         - ESP_ERR_INVALID_STATE if host already is in use
 *         - ESP_ERR_NOT_FOUND     if there is no available DMA channel
 *         - ESP_ERR_NO_MEM        if out of memory
 *         - ESP_OK                on success
 */
esp_err_t spi_slave_initialize(spi_host_device_t host, const spi_bus_config_t *bus_config, const spi_slave_interface_config_t *slave_config, spi_dma_chan_t dma_chan);

/**
 * @brief Free a SPI bus claimed as a SPI slave interface
 *
 * @param host SPI peripheral to free
 * @return
 *         - ESP_ERR_INVALID_ARG   if parameter is invalid
 *         - ESP_ERR_INVALID_STATE if not all devices on the bus are freed
 *         - ESP_OK                on success
 */
esp_err_t spi_slave_free(spi_host_device_t host);

/**
 * @brief Enable the spi slave function for an initialized spi host
 * @note No need to call this function additionally after `spi_slave_initialize`,
 *       because it has been enabled already during the initialization.
 *
 * @param host SPI peripheral to be enabled
 * @return
 *         - ESP_OK                 On success
 *         - ESP_ERR_INVALID_ARG    Unsupported host
 *         - ESP_ERR_INVALID_STATE  Peripheral already enabled
 */
esp_err_t spi_slave_enable(spi_host_device_t host);

/**
 * @brief Disable the spi slave function for an initialized spi host
 *
 * @param host SPI peripheral to be disabled
 * @return
 *         - ESP_OK                 On success
 *         - ESP_ERR_INVALID_ARG    Unsupported host
 *         - ESP_ERR_INVALID_STATE  Peripheral already disabled
 */
esp_err_t spi_slave_disable(spi_host_device_t host);

/**
 * @brief Queue a SPI transaction for execution
 *
 * @note On esp32, if trans length not WORD aligned, the rx buffer last word memory will still overwritten by DMA HW
 *
 * Queues a SPI transaction to be executed by this slave device. (The transaction queue size was specified when the slave
 * device was initialised via spi_slave_initialize.) This function may block if the queue is full (depending on the
 * ticks_to_wait parameter). No SPI operation is directly initiated by this function, the next queued transaction
 * will happen when the master initiates a SPI transaction by pulling down CS and sending out clock signals.
 *
 * This function hands over ownership of the buffers in ``trans_desc`` to the SPI slave driver; the application is
 * not to access this memory until ``spi_slave_queue_trans`` is called to hand ownership back to the application.
 *
 * @param host SPI peripheral that is acting as a slave
 * @param trans_desc Description of transaction to execute. Not const because we may want to write status back
 *                   into the transaction description.
 * @param ticks_to_wait Ticks to wait until there's room in the queue; use portMAX_DELAY to
 *                      never time out.
 * @return
 *         - ESP_ERR_INVALID_ARG   if parameter is invalid
 *         - ESP_ERR_NO_MEM        if set flag `SPI_SLAVE_TRANS_DMA_BUFFER_ALIGN_AUTO` but there is no free memory
 *         - ESP_ERR_INVALID_STATE if sync data between Cache and memory failed
 *         - ESP_OK                on success
 */
esp_err_t spi_slave_queue_trans(spi_host_device_t host, const spi_slave_transaction_t *trans_desc, TickType_t ticks_to_wait);

/**
 * @brief Get the result of a SPI transaction queued earlier
 *
 * This routine will wait until a transaction to the given device (queued earlier with
 * spi_slave_queue_trans) has successfully completed. It will then return the description of the
 * completed transaction so software can inspect the result and e.g. free the memory or
 * reuse the buffers.
 *
 * It is mandatory to eventually use this function for any transaction queued by ``spi_slave_queue_trans``.
 *
 * @param host SPI peripheral to that is acting as a slave
 * @param[out] trans_desc Pointer to variable able to contain a pointer to the description of the
 *                   transaction that is executed
 * @param ticks_to_wait Ticks to wait until there's a returned item; use portMAX_DELAY to never time
 *                      out.
 * @return
 *         - ESP_ERR_INVALID_ARG   if parameter is invalid
 *         - ESP_ERR_NOT_SUPPORTED if flag `SPI_SLAVE_NO_RETURN_RESULT` is set
 *         - ESP_OK                on success
 */
esp_err_t spi_slave_get_trans_result(spi_host_device_t host, spi_slave_transaction_t **trans_desc, TickType_t ticks_to_wait);

/**
 * @brief Do a SPI transaction
 *
 * Essentially does the same as spi_slave_queue_trans followed by spi_slave_get_trans_result. Do
 * not use this when there is still a transaction queued that hasn't been finalized
 * using spi_slave_get_trans_result.
 *
 * @param host SPI peripheral to that is acting as a slave
 * @param trans_desc Pointer to variable able to contain a pointer to the description of the
 *                   transaction that is executed. Not const because we may want to write status back
 *                   into the transaction description.
 * @param ticks_to_wait Ticks to wait until there's a returned item; use portMAX_DELAY to never time
 *                      out.
 * @return
 *         - ESP_ERR_INVALID_ARG   if parameter is invalid
 *         - ESP_OK                on success
 */
esp_err_t spi_slave_transmit(spi_host_device_t host, spi_slave_transaction_t *trans_desc, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif

#endif
