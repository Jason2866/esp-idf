/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdatomic.h>
#include "sdkconfig.h"
#include "esp_types.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_rom_gpio.h"
#include "esp_heap_caps.h"
#include "soc/spi_periph.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/spi_common_internal.h"
#include "esp_private/spi_share_hw_ctrl.h"
#include "esp_private/esp_cache_private.h"
#include "esp_private/sleep_retention.h"
#include "esp_dma_utils.h"
#include "hal/spi_hal.h"
#if CONFIG_IDF_TARGET_ESP32
#include "soc/dport_reg.h"
#endif

#if CONFIG_SPI_MASTER_ISR_IN_IRAM || CONFIG_SPI_SLAVE_ISR_IN_IRAM
#define SPI_COMMON_ISR_ATTR IRAM_ATTR
#else
#define SPI_COMMON_ISR_ATTR
#endif

#if CONFIG_SPI_MASTER_ISR_IN_IRAM || CONFIG_SPI_SLAVE_ISR_IN_IRAM
#define SPI_COMMON_MALLOC_CAPS    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#define SPI_COMMON_MALLOC_CAPS    (MALLOC_CAP_DEFAULT)
#endif

static const char *SPI_TAG = "spi_common";

#define SPI_CHECK(a, str, ret_val, ...)  ESP_RETURN_ON_FALSE(a, ret_val, SPI_TAG, str, ##__VA_ARGS__)
#define SPI_CHECK_PIN(pin_num, pin_name, check_output) if (check_output) { \
            SPI_CHECK(GPIO_IS_VALID_OUTPUT_GPIO(pin_num), pin_name" not valid", ESP_ERR_INVALID_ARG); \
        } else { \
            SPI_CHECK(GPIO_IS_VALID_GPIO(pin_num), pin_name" not valid", ESP_ERR_INVALID_ARG); \
        }
#define SPI_MAIN_BUS_DEFAULT() { \
        .host_id = 0, \
        .bus_attr = { \
            .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE, \
        }, \
    }

typedef struct {
    int host_id;
    _lock_t mutex;      // mutex for controller
    spi_destroy_func_t destroy_func;
    void* destroy_arg;
    spi_bus_attr_t bus_attr;
    spi_dma_ctx_t *dma_ctx;
} spicommon_bus_context_t;

static spicommon_bus_context_t s_mainbus = SPI_MAIN_BUS_DEFAULT();
static spicommon_bus_context_t* bus_ctx[SOC_SPI_PERIPH_NUM] = {&s_mainbus};

#if CONFIG_SPI_FLASH_SHARE_SPI1_BUS
/* The lock for the share SPI1 bus is registered here in a constructor due to need to access the context
   This way we are able to decouple the SPI-flash driver from the spi-master driver */
static __attribute__((constructor)) void spi_bus_lock_init_main_bus(void)
{
    /* Initialize bus context about the main SPI bus lock, called during chip startup. */
    bus_ctx[0]->bus_attr.lock = g_main_spi_bus_lock;
}
#endif

esp_err_t spicommon_bus_alloc(spi_host_device_t host_id, const char *name)
{
    SPI_CHECK(spicommon_periph_claim(host_id, name), "host already in use", ESP_ERR_INVALID_STATE);

    spicommon_bus_context_t *ctx = heap_caps_calloc(1, sizeof(spicommon_bus_context_t), SPI_COMMON_MALLOC_CAPS);
    if (!ctx) {
        spicommon_periph_free(host_id);
        return ESP_ERR_NO_MEM;
    }
    ctx->host_id = host_id;
    bus_ctx[host_id] = ctx;
    return ESP_OK;
}

esp_err_t spicommon_bus_free(spi_host_device_t host_id)
{
    assert(bus_ctx[host_id]);
    spicommon_periph_free(host_id);
    free(bus_ctx[host_id]);
    bus_ctx[host_id] = NULL;
    return ESP_OK;
}

#if SOC_GDMA_SUPPORTED
//NOTE!! If both A and B are not defined, '#if (A==B)' is true, because GCC use 0 stand for undefined symbol
#if defined(SOC_GDMA_BUS_AXI) && (SOC_GDMA_TRIG_PERIPH_SPI2_BUS == SOC_GDMA_BUS_AXI)
#define SPI_GDMA_NEW_CHANNEL     gdma_new_axi_channel
#elif defined(SOC_GDMA_BUS_AHB) && (SOC_GDMA_TRIG_PERIPH_SPI2_BUS == SOC_GDMA_BUS_AHB)
#define SPI_GDMA_NEW_CHANNEL    gdma_new_ahb_channel
#endif

#else
//Each bit stands for 1 dma channel, BIT(0) should be used for SPI1
static uint8_t spi_dma_chan_enabled = 0;
static portMUX_TYPE spi_dma_spinlock = portMUX_INITIALIZER_UNLOCKED;
#endif  //!SOC_GDMA_SUPPORTED

static inline bool is_valid_host(spi_host_device_t host)
{
#if (SOC_SPI_PERIPH_NUM == 2)
    return host >= SPI1_HOST && host <= SPI2_HOST;
#elif (SOC_SPI_PERIPH_NUM == 3)
    return host >= SPI1_HOST && host <= SPI3_HOST;
#endif
}

int spicommon_irqsource_for_host(spi_host_device_t host)
{
    return spi_periph_signal[host].irq;
}

int spicommon_irqdma_source_for_host(spi_host_device_t host)
{
    return spi_periph_signal[host].irq_dma;
}

//----------------------------------------------------------alloc dma periph-------------------------------------------------------//
#if !SOC_GDMA_SUPPORTED

#if SPI_LL_DMA_SHARED
static inline periph_module_t get_dma_periph(int dma_chan)
{
    assert(dma_chan >= 1 && dma_chan <= SOC_SPI_DMA_CHAN_NUM);
    if (dma_chan == 1) {
        return PERIPH_SPI2_DMA_MODULE;
    } else if (dma_chan == 2) {
        return PERIPH_SPI3_DMA_MODULE;
    } else {
        abort();
    }
}
#endif

static bool claim_dma_chan(int dma_chan, uint32_t *out_actual_dma_chan)
{
    bool ret = false;

    portENTER_CRITICAL(&spi_dma_spinlock);
    bool is_used = (BIT(dma_chan) & spi_dma_chan_enabled);
    if (!is_used) {
        spi_dma_chan_enabled |= BIT(dma_chan);
#if SPI_LL_DMA_SHARED
        PERIPH_RCC_ACQUIRE_ATOMIC(get_dma_periph(dma_chan), ref_count) {
            //esp32s2: dma_chan index is same as spi host_id, no matter dma_chan_auto or not
            if (ref_count == 0) {
                spi_dma_ll_enable_bus_clock(dma_chan, true);
                spi_dma_ll_reset_register(dma_chan);
            }
        }
#else
        SPI_COMMON_RCC_CLOCK_ATOMIC() {
            //esp32: have only one spi_dma
            spi_dma_ll_enable_bus_clock(dma_chan, true);
            spi_dma_ll_reset_register(dma_chan);
        }
#endif
        *out_actual_dma_chan = dma_chan;
        ret = true;
    }
    portEXIT_CRITICAL(&spi_dma_spinlock);

    return ret;
}

static void connect_spi_and_dma(spi_host_device_t host, int dma_chan)
{
#if CONFIG_IDF_TARGET_ESP32
    DPORT_SET_PERI_REG_BITS(DPORT_SPI_DMA_CHAN_SEL_REG, 3, dma_chan, (host * 2));
#elif CONFIG_IDF_TARGET_ESP32S2
    //On ESP32S2, each SPI controller has its own DMA channel. So there is no need to connect them.
#endif
}

static esp_err_t alloc_dma_chan(spi_host_device_t host_id, spi_dma_chan_t dma_chan, spi_dma_ctx_t *dma_ctx)
{
    assert(is_valid_host(host_id));
#if CONFIG_IDF_TARGET_ESP32
    assert(dma_chan > SPI_DMA_DISABLED && dma_chan <= SPI_DMA_CH_AUTO);
#elif CONFIG_IDF_TARGET_ESP32S2
    assert(dma_chan == (int)host_id || dma_chan == SPI_DMA_CH_AUTO);
#endif

    esp_err_t ret = ESP_OK;
    bool success = false;
    uint32_t actual_dma_chan = 0;

    if (dma_chan == SPI_DMA_CH_AUTO) {
#if CONFIG_IDF_TARGET_ESP32
        for (int i = 1; i < SOC_SPI_DMA_CHAN_NUM + 1; i++) {
            success = claim_dma_chan(i, &actual_dma_chan);
            if (success) {
                break;
            }
        }
#elif CONFIG_IDF_TARGET_ESP32S2
        //On ESP32S2, each SPI controller has its own DMA channel
        success = claim_dma_chan(host_id, &actual_dma_chan);
#endif  //#if CONFIG_IDF_TARGET_XXX
    } else {
        success = claim_dma_chan((int)dma_chan, &actual_dma_chan);
    }

    //On ESP32 and ESP32S2, actual_tx_dma_chan and actual_rx_dma_chan are always same
    dma_ctx->tx_dma_chan.chan_id = actual_dma_chan;
    dma_ctx->rx_dma_chan.chan_id = actual_dma_chan;
    dma_ctx->tx_dma_chan.host_id = host_id;
    dma_ctx->rx_dma_chan.host_id = host_id;
    dma_ctx->tx_dma_chan.dir = DMA_CHANNEL_DIRECTION_TX;
    dma_ctx->rx_dma_chan.dir = DMA_CHANNEL_DIRECTION_RX;

    if (!success) {
        SPI_CHECK(false, "no available dma channel", ESP_ERR_NOT_FOUND);
    }
    connect_spi_and_dma(host_id, actual_dma_chan);

    spi_dma_enable_burst(dma_ctx->tx_dma_chan, true, true);
    spi_dma_enable_burst(dma_ctx->rx_dma_chan, true, true);
    return ret;
}

#else //SOC_GDMA_SUPPORTED

static esp_err_t alloc_dma_chan(spi_host_device_t host_id, spi_dma_chan_t dma_chan, spi_dma_ctx_t *dma_ctx)
{
    assert(is_valid_host(host_id));
    assert(dma_chan == SPI_DMA_CH_AUTO);
    esp_err_t ret = ESP_OK;

    if (dma_chan == SPI_DMA_CH_AUTO) {
        gdma_channel_alloc_config_t tx_alloc_config = {
            .flags.reserve_sibling = 1,
#if CONFIG_SPI_MASTER_ISR_IN_IRAM
            .flags.isr_cache_safe = true,
#endif
            .direction = GDMA_CHANNEL_DIRECTION_TX,
        };
        ESP_RETURN_ON_ERROR(SPI_GDMA_NEW_CHANNEL(&tx_alloc_config, &dma_ctx->tx_dma_chan), SPI_TAG, "alloc gdma tx failed");

        gdma_channel_alloc_config_t rx_alloc_config = {
            .direction = GDMA_CHANNEL_DIRECTION_RX,
            .sibling_chan = dma_ctx->tx_dma_chan,
#if CONFIG_SPI_MASTER_ISR_IN_IRAM
            .flags.isr_cache_safe = true,
#endif
        };
        ESP_RETURN_ON_ERROR(SPI_GDMA_NEW_CHANNEL(&rx_alloc_config, &dma_ctx->rx_dma_chan), SPI_TAG, "alloc gdma rx failed");

        if (host_id == SPI2_HOST) {
            gdma_connect(dma_ctx->tx_dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
            gdma_connect(dma_ctx->rx_dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
        }
#if (SOC_SPI_PERIPH_NUM >= 3)
        else if (host_id == SPI3_HOST) {
            gdma_connect(dma_ctx->tx_dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3));
            gdma_connect(dma_ctx->rx_dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3));
        }
#endif
        // TODO: add support to allow SPI transfer PSRAM buffer
        gdma_transfer_config_t trans_cfg = {
            .max_data_burst_size = 16,
            .access_ext_mem = false,
        };
        ESP_RETURN_ON_ERROR(gdma_config_transfer(dma_ctx->tx_dma_chan, &trans_cfg), SPI_TAG, "config gdma tx transfer failed");
        ESP_RETURN_ON_ERROR(gdma_config_transfer(dma_ctx->rx_dma_chan, &trans_cfg), SPI_TAG, "config gdma rx transfer failed");
    }
    return ret;
}
#endif  //#if !SOC_GDMA_SUPPORTED

esp_err_t spicommon_dma_chan_alloc(spi_host_device_t host_id, spi_dma_chan_t dma_chan, spi_dma_ctx_t **out_dma_ctx)
{
    assert(is_valid_host(host_id));
#if CONFIG_IDF_TARGET_ESP32
    assert(dma_chan > SPI_DMA_DISABLED && dma_chan <= SPI_DMA_CH_AUTO);
#elif CONFIG_IDF_TARGET_ESP32S2
    assert(dma_chan == (int)host_id || dma_chan == SPI_DMA_CH_AUTO);
#endif

    esp_err_t ret = ESP_OK;
    spi_dma_ctx_t *dma_ctx = (spi_dma_ctx_t *)heap_caps_calloc(1, sizeof(spi_dma_ctx_t), SPI_COMMON_MALLOC_CAPS);
    if (!dma_ctx) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ret = alloc_dma_chan(host_id, dma_chan, dma_ctx);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    *out_dma_ctx = dma_ctx;
    return ret;

cleanup:
    free(dma_ctx);
    return ret;
}

esp_err_t spicommon_dma_desc_alloc(spi_dma_ctx_t *dma_ctx, int cfg_max_sz, int *actual_max_sz)
{
    int dma_desc_ct = (cfg_max_sz + DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED;
    if (dma_desc_ct == 0) {
        dma_desc_ct = 1;    //default to 4k when max is not given
    }

    dma_ctx->dmadesc_tx = heap_caps_aligned_calloc(DMA_DESC_MEM_ALIGN_SIZE, 1, sizeof(spi_dma_desc_t) * dma_desc_ct, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    dma_ctx->dmadesc_rx = heap_caps_aligned_calloc(DMA_DESC_MEM_ALIGN_SIZE, 1, sizeof(spi_dma_desc_t) * dma_desc_ct, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (dma_ctx->dmadesc_tx == NULL || dma_ctx->dmadesc_rx == NULL) {
        if (dma_ctx->dmadesc_tx) {
            free(dma_ctx->dmadesc_tx);
            dma_ctx->dmadesc_tx = NULL;
        }
        if (dma_ctx->dmadesc_rx) {
            free(dma_ctx->dmadesc_rx);
            dma_ctx->dmadesc_rx = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    dma_ctx->dma_desc_num = dma_desc_ct;
    *actual_max_sz = dma_desc_ct * DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED;
    return ESP_OK;
}

void SPI_COMMON_ISR_ATTR spicommon_dma_desc_setup_link(spi_dma_desc_t *dmadesc, const void *data, int len, bool is_rx)
{
    dmadesc = ADDR_DMA_2_CPU(dmadesc);
    int n = 0;
    while (len) {
        int dmachunklen = len;
        if (dmachunklen > DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED) {
            dmachunklen = DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED;
        }
        if (is_rx) {
            //Receive needs DMA length rounded to next 32-bit boundary
            dmadesc[n].dw0.size = (dmachunklen + 3) & (~3);
        } else {
            dmadesc[n].dw0.size = dmachunklen;
            dmadesc[n].dw0.length = dmachunklen;
        }
        dmadesc[n].buffer = (uint8_t *)data;
        dmadesc[n].dw0.suc_eof = 0;
        dmadesc[n].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        dmadesc[n].next = ADDR_CPU_2_DMA(&dmadesc[n + 1]);
        len -= dmachunklen;
        data += dmachunklen;
        n++;
    }
    dmadesc[n - 1].dw0.suc_eof = 1; //Mark last DMA desc as end of stream.
    dmadesc[n - 1].next = NULL;
}

//----------------------------------------------------------free dma periph-------------------------------------------------------//
esp_err_t spicommon_dma_chan_free(spi_dma_ctx_t *dma_ctx)
{
    assert(dma_ctx);

#if !SOC_GDMA_SUPPORTED
    //On ESP32S2, each SPI controller has its own DMA channel
    int dma_chan = dma_ctx->tx_dma_chan.chan_id;
    assert(spi_dma_chan_enabled & BIT(dma_chan));

    portENTER_CRITICAL(&spi_dma_spinlock);
    spi_dma_chan_enabled &= ~BIT(dma_chan);
#if SPI_LL_DMA_SHARED
    PERIPH_RCC_RELEASE_ATOMIC(get_dma_periph(dma_chan), ref_count) {
        if (ref_count == 0) {
            spi_dma_ll_enable_bus_clock(dma_ctx->tx_dma_chan.host_id, false);
        }
    }
#else
    SPI_COMMON_RCC_CLOCK_ATOMIC() {
        spi_dma_ll_enable_bus_clock(dma_ctx->tx_dma_chan.host_id, false);
    }
#endif
    portEXIT_CRITICAL(&spi_dma_spinlock);

#else //SOC_GDMA_SUPPORTED
    if (dma_ctx->rx_dma_chan) {
        gdma_disconnect(dma_ctx->rx_dma_chan);
        gdma_del_channel(dma_ctx->rx_dma_chan);
    }
    if (dma_ctx->tx_dma_chan) {
        gdma_disconnect(dma_ctx->tx_dma_chan);
        gdma_del_channel(dma_ctx->tx_dma_chan);
    }
#endif

    free(dma_ctx);
    return ESP_OK;
}

//----------------------------------------------------------IO general-------------------------------------------------------//
#if SOC_SPI_SUPPORT_OCT
static bool check_iomux_pins_oct(spi_host_device_t host, const spi_bus_config_t* bus_config)
{
    if (host != SPI2_HOST) {
        return false;
    }
    int io_nums[] = {bus_config->data0_io_num, bus_config->data1_io_num, bus_config->data2_io_num, bus_config->data3_io_num,
                     bus_config->sclk_io_num, bus_config->data4_io_num, bus_config->data5_io_num, bus_config->data6_io_num, bus_config->data7_io_num
                    };
    int io_mux_nums[] = {SPI2_IOMUX_PIN_NUM_MOSI_OCT, SPI2_IOMUX_PIN_NUM_MISO_OCT, SPI2_IOMUX_PIN_NUM_WP_OCT, SPI2_IOMUX_PIN_NUM_HD_OCT,
                         SPI2_IOMUX_PIN_NUM_CLK_OCT, SPI2_IOMUX_PIN_NUM_IO4_OCT, SPI2_IOMUX_PIN_NUM_IO5_OCT, SPI2_IOMUX_PIN_NUM_IO6_OCT, SPI2_IOMUX_PIN_NUM_IO7_OCT
                        };
    for (size_t i = 0; i < sizeof(io_nums) / sizeof(io_nums[0]); i++) {
        if (io_nums[i] >= 0 && io_nums[i] != io_mux_nums[i]) {
            return false;
        }
    }
    return true;
}
#endif

static bool check_iomux_pins_quad(spi_host_device_t host, const spi_bus_config_t* bus_config)
{
    if (bus_config->sclk_io_num >= 0 &&
            bus_config->sclk_io_num != spi_periph_signal[host].spiclk_iomux_pin) {
        return false;
    }
    if (bus_config->quadwp_io_num >= 0 &&
            bus_config->quadwp_io_num != spi_periph_signal[host].spiwp_iomux_pin) {
        return false;
    }
    if (bus_config->quadhd_io_num >= 0 &&
            bus_config->quadhd_io_num != spi_periph_signal[host].spihd_iomux_pin) {
        return false;
    }
    if (bus_config->mosi_io_num >= 0 &&
            bus_config->mosi_io_num != spi_periph_signal[host].spid_iomux_pin) {
        return false;
    }
    if (bus_config->miso_io_num >= 0 &&
            bus_config->miso_io_num != spi_periph_signal[host].spiq_iomux_pin) {
        return false;
    }
    return true;
}

static bool bus_uses_iomux_pins(spi_host_device_t host, const spi_bus_config_t* bus_config)
{
//Check if SPI pins could be routed to iomux.
#if SOC_SPI_SUPPORT_OCT
    //The io mux pins available for Octal mode is not the same as the ones we use for non-Octal mode.
    if ((bus_config->flags & SPICOMMON_BUSFLAG_OCTAL) == SPICOMMON_BUSFLAG_OCTAL) {
        return check_iomux_pins_oct(host, bus_config);
    }
#endif
    return check_iomux_pins_quad(host, bus_config);
}

#if SOC_SPI_SUPPORT_OCT
static void bus_iomux_pins_set_oct(spi_host_device_t host, const spi_bus_config_t* bus_config)
{
    assert(host == SPI2_HOST);
    int io_nums[] = {bus_config->data0_io_num, bus_config->data1_io_num, bus_config->data2_io_num, bus_config->data3_io_num,
                     bus_config->sclk_io_num, bus_config->data4_io_num, bus_config->data5_io_num, bus_config->data6_io_num, bus_config->data7_io_num
                    };
    int io_signals[] = {spi_periph_signal[host].spid_in, spi_periph_signal[host].spiq_in, spi_periph_signal[host].spiwp_in,
                        spi_periph_signal[host].spihd_in, spi_periph_signal[host].spiclk_in, spi_periph_signal[host].spid4_in,
                        spi_periph_signal[host].spid5_in, spi_periph_signal[host].spid6_in, spi_periph_signal[host].spid7_in
                       };
    for (size_t i = 0; i < sizeof(io_nums) / sizeof(io_nums[0]); i++) {
        if (io_nums[i] > 0) {
            // In Octal mode use function channel 2
            gpio_iomux_input(io_nums[i], SPI2_FUNC_NUM_OCT, io_signals[i]);
            gpio_iomux_output(io_nums[i], SPI2_FUNC_NUM_OCT);
        }
    }
}
#endif //SOC_SPI_SUPPORT_OCT

static void bus_iomux_pins_set_quad(spi_host_device_t host, const spi_bus_config_t* bus_config)
{
    if (bus_config->mosi_io_num >= 0) {
        gpio_iomux_input(bus_config->mosi_io_num, spi_periph_signal[host].func, spi_periph_signal[host].spid_in);
        gpio_iomux_output(bus_config->mosi_io_num, spi_periph_signal[host].func);
    }
    if (bus_config->miso_io_num >= 0) {
        gpio_iomux_input(bus_config->miso_io_num, spi_periph_signal[host].func, spi_periph_signal[host].spiq_in);
        gpio_iomux_output(bus_config->miso_io_num, spi_periph_signal[host].func);
    }
    if (bus_config->quadwp_io_num >= 0) {
        gpio_iomux_input(bus_config->quadwp_io_num, spi_periph_signal[host].func, spi_periph_signal[host].spiwp_in);
        gpio_iomux_output(bus_config->quadwp_io_num, spi_periph_signal[host].func);
    }
    if (bus_config->quadhd_io_num >= 0) {
        gpio_iomux_input(bus_config->quadhd_io_num, spi_periph_signal[host].func, spi_periph_signal[host].spihd_in);
        gpio_iomux_output(bus_config->quadhd_io_num, spi_periph_signal[host].func);
    }
    if (bus_config->sclk_io_num >= 0) {
        gpio_iomux_input(bus_config->sclk_io_num, spi_periph_signal[host].func, spi_periph_signal[host].spiclk_in);
        gpio_iomux_output(bus_config->sclk_io_num, spi_periph_signal[host].func);
    }
}

// check if the GPIO is already used by others
static void s_spi_common_gpio_check_reserve(gpio_num_t gpio_num)
{
    assert(GPIO_IS_VALID_GPIO(gpio_num));  //coverity check
    uint64_t orig_occupied_map = esp_gpio_reserve(BIT64(gpio_num));
    if (orig_occupied_map & BIT64(gpio_num)) {
        ESP_LOGW(SPI_TAG, "GPIO %d is conflict with others and be overwritten", gpio_num);
    }
}

static void s_spi_common_bus_via_gpio(gpio_num_t gpio_num, int in_sig, int out_sig, uint64_t *io_mask)
{
    assert(GPIO_IS_VALID_GPIO(gpio_num));  //coverity check
    if (in_sig != -1) {
        gpio_input_enable(gpio_num);
        esp_rom_gpio_connect_in_signal(gpio_num, in_sig, false);
    }
    if (out_sig != -1) {
        // For gpio_matrix, reserve output pins, see 'esp_gpio_reserve.h'
        *io_mask |= BIT64(gpio_num);
        s_spi_common_gpio_check_reserve(gpio_num);
        esp_rom_gpio_connect_out_signal(gpio_num, out_sig, false, false);
    }
    gpio_func_sel(gpio_num, PIN_FUNC_GPIO);
}

/*
Do the common stuff to hook up a SPI host to a bus defined by a bunch of GPIO pins. Feed it a host number and a
bus config struct and it'll set up the GPIO matrix and enable the device. If a pin is set to non-negative value,
it should be able to be initialized.
*/
esp_err_t spicommon_bus_initialize_io(spi_host_device_t host, const spi_bus_config_t *bus_config, uint32_t flags, uint32_t* flags_o, uint64_t *io_reserved)
{
#if SOC_SPI_SUPPORT_OCT
    // In the driver of previous version, spi data4 ~ spi data7 are not in spi_bus_config_t struct. So the new-added pins come as 0
    // if they are not really set. Add this boolean variable to check if the user has set spi data4 ~spi data7 pins .
    bool io4_7_is_blank = !bus_config->data4_io_num && !bus_config->data5_io_num && !bus_config->data6_io_num && !bus_config->data7_io_num;
    // This boolean variable specifies if user sets pins used for octal mode (users can set spi data4 ~ spi data7 to -1).
    bool io4_7_enabled = !io4_7_is_blank && bus_config->data4_io_num >= 0 && bus_config->data5_io_num >= 0 &&
                         bus_config->data6_io_num >= 0 && bus_config->data7_io_num >= 0;
    SPI_CHECK((flags & SPICOMMON_BUSFLAG_MASTER) || !((flags & SPICOMMON_BUSFLAG_OCTAL) == SPICOMMON_BUSFLAG_OCTAL), "Octal SPI mode / OPI mode only works when SPI is used as Master", ESP_ERR_INVALID_ARG);
    SPI_CHECK(host == SPI2_HOST || !((flags & SPICOMMON_BUSFLAG_OCTAL) == SPICOMMON_BUSFLAG_OCTAL), "Only SPI2 supports Octal SPI mode / OPI mode", ESP_ERR_INVALID_ARG);
#endif //SOC_SPI_SUPPORT_OCT

    uint32_t temp_flag = 0;
    uint64_t gpio_reserv = 0;
    //check pin capabilities
    if (bus_config->sclk_io_num >= 0) {
        temp_flag |= SPICOMMON_BUSFLAG_SCLK;
        SPI_CHECK_PIN(bus_config->sclk_io_num, "sclk", (flags & SPICOMMON_BUSFLAG_MASTER));
    }
    if (bus_config->quadwp_io_num >= 0) {
        SPI_CHECK_PIN(bus_config->quadwp_io_num, "wp", true);
    }
    if (bus_config->quadhd_io_num >= 0) {
        SPI_CHECK_PIN(bus_config->quadhd_io_num, "hd", true);
    }
#if SOC_SPI_SUPPORT_OCT
    // set flags for OCTAL mode according to the existence of spi data4 ~ spi data7
    if (io4_7_enabled) {
        temp_flag |= SPICOMMON_BUSFLAG_IO4_IO7;
        if (bus_config->data4_io_num >= 0) {
            SPI_CHECK_PIN(bus_config->data4_io_num, "spi data4", true);
        }
        if (bus_config->data5_io_num >= 0) {
            SPI_CHECK_PIN(bus_config->data5_io_num, "spi data5", true);
        }
        if (bus_config->data6_io_num >= 0) {
            SPI_CHECK_PIN(bus_config->data6_io_num, "spi data6", true);
        }
        if (bus_config->data7_io_num >= 0) {
            SPI_CHECK_PIN(bus_config->data7_io_num, "spi data7", true);
        }
    }
#endif //SOC_SPI_SUPPORT_OCT

    //set flags for QUAD mode according to the existence of wp and hd
    if (bus_config->quadhd_io_num >= 0 && bus_config->quadwp_io_num >= 0) {
        temp_flag |= SPICOMMON_BUSFLAG_WPHD;
    }
    if (bus_config->mosi_io_num >= 0) {
        temp_flag |= SPICOMMON_BUSFLAG_MOSI;
        SPI_CHECK_PIN(bus_config->mosi_io_num, "mosi", (flags & SPICOMMON_BUSFLAG_MASTER) || (temp_flag & SPICOMMON_BUSFLAG_DUAL));
    }
    if (bus_config->miso_io_num >= 0) {
        temp_flag |= SPICOMMON_BUSFLAG_MISO;
        SPI_CHECK_PIN(bus_config->miso_io_num, "miso", !(flags & SPICOMMON_BUSFLAG_MASTER) || (temp_flag & SPICOMMON_BUSFLAG_DUAL));
    }
    //set flags for DUAL mode according to output-capability of MOSI and MISO pins.
    if ((bus_config->mosi_io_num < 0 || GPIO_IS_VALID_OUTPUT_GPIO(bus_config->mosi_io_num)) &&
            (bus_config->miso_io_num < 0 || GPIO_IS_VALID_OUTPUT_GPIO(bus_config->miso_io_num))) {
        temp_flag |= SPICOMMON_BUSFLAG_DUAL;
    }

    //check if the selected pins correspond to the iomux pins of the peripheral
    bool use_iomux = !(flags & SPICOMMON_BUSFLAG_GPIO_PINS) && bus_uses_iomux_pins(host, bus_config);
    if (use_iomux) {
        temp_flag |= SPICOMMON_BUSFLAG_IOMUX_PINS;
    } else {
        temp_flag |= SPICOMMON_BUSFLAG_GPIO_PINS;
    }

    uint32_t missing_flag = flags & ~temp_flag;
    missing_flag &= ~SPICOMMON_BUSFLAG_MASTER;  //don't check this flag
    missing_flag &= ~SPICOMMON_BUSFLAG_SLP_ALLOW_PD;

    if (missing_flag != 0) {
        //check pins existence
        if (missing_flag & SPICOMMON_BUSFLAG_SCLK) {
            ESP_LOGE(SPI_TAG, "sclk pin required.");
        }
        if (missing_flag & SPICOMMON_BUSFLAG_MOSI) {
            ESP_LOGE(SPI_TAG, "mosi pin required.");
        }
        if (missing_flag & SPICOMMON_BUSFLAG_MISO) {
            ESP_LOGE(SPI_TAG, "miso pin required.");
        }
        if (missing_flag & SPICOMMON_BUSFLAG_DUAL) {
            ESP_LOGE(SPI_TAG, "not both mosi and miso output capable");
        }
        if (missing_flag & SPICOMMON_BUSFLAG_WPHD) {
            ESP_LOGE(SPI_TAG, "both wp and hd required.");
        }
        if (missing_flag & SPICOMMON_BUSFLAG_IOMUX_PINS) {
            ESP_LOGE(SPI_TAG, "not using iomux pins");
        }
#if SOC_SPI_SUPPORT_OCT
        if (missing_flag & SPICOMMON_BUSFLAG_IO4_IO7) {
            ESP_LOGE(SPI_TAG, "spi data4 ~ spi data7 are required.");
        }
#endif
        SPI_CHECK(false, "not all required capabilities satisfied.", ESP_ERR_INVALID_ARG);
    }

    ESP_LOGD(SPI_TAG, "SPI%d use %s.", host + 1, use_iomux ? "iomux pins" : "gpio matrix");
    if (use_iomux) {
        //All SPI iomux pin selections resolve to 1, so we put that here instead of trying to figure
        //out which PIN_FUNC_GPIOx_xSPIxx to grab; they all are defined to 1 anyway.
        for (uint8_t i = 0; i < sizeof(bus_config->iocfg) / sizeof(bus_config->iocfg[0]); i++) {
            if (!(flags & SPICOMMON_BUSFLAG_OCTAL) && (i >= 4)) {
                break;
            }
            // For iomux, reserve all configured pins
            if (GPIO_IS_VALID_GPIO(bus_config->iocfg[i])) {
                gpio_reserv |= BIT64(bus_config->iocfg[i]);
                s_spi_common_gpio_check_reserve(bus_config->iocfg[i]);
            }
        }
        bus_iomux_pins_set_quad(host, bus_config);
#if SOC_SPI_SUPPORT_OCT
        if (flags & SPICOMMON_BUSFLAG_OCTAL) {
            bus_iomux_pins_set_oct(host, bus_config);
        }
#endif
    } else {
        //Use GPIO matrix
        if (bus_config->mosi_io_num >= 0) {
            int in_sig = (!(flags & SPICOMMON_BUSFLAG_MASTER) || (temp_flag & SPICOMMON_BUSFLAG_DUAL)) ? spi_periph_signal[host].spid_in : -1;
            int out_sig = ((flags & SPICOMMON_BUSFLAG_MASTER) || (temp_flag & SPICOMMON_BUSFLAG_DUAL)) ? spi_periph_signal[host].spid_out : -1;
            s_spi_common_bus_via_gpio(bus_config->mosi_io_num, in_sig, out_sig, &gpio_reserv);
        }
        if (bus_config->miso_io_num >= 0) {
            int in_sig = ((flags & SPICOMMON_BUSFLAG_MASTER) || (temp_flag & SPICOMMON_BUSFLAG_DUAL)) ? spi_periph_signal[host].spiq_in : -1;
            int out_sig = (!(flags & SPICOMMON_BUSFLAG_MASTER) || (temp_flag & SPICOMMON_BUSFLAG_DUAL)) ? spi_periph_signal[host].spiq_out : -1;
            s_spi_common_bus_via_gpio(bus_config->miso_io_num, in_sig, out_sig, &gpio_reserv);
        }
        if (bus_config->sclk_io_num >= 0) {
            int in_sig = (flags & SPICOMMON_BUSFLAG_MASTER) ? -1 : spi_periph_signal[host].spiclk_in;
            int out_sig = (flags & SPICOMMON_BUSFLAG_MASTER) ? spi_periph_signal[host].spiclk_out : -1;
            s_spi_common_bus_via_gpio(bus_config->sclk_io_num, in_sig, out_sig, &gpio_reserv);
        }
        if (bus_config->quadwp_io_num >= 0) {
            s_spi_common_bus_via_gpio(bus_config->quadwp_io_num, spi_periph_signal[host].spiwp_in, spi_periph_signal[host].spiwp_out, &gpio_reserv);
        }
        if (bus_config->quadhd_io_num >= 0) {
            s_spi_common_bus_via_gpio(bus_config->quadhd_io_num, spi_periph_signal[host].spihd_in, spi_periph_signal[host].spihd_out, &gpio_reserv);
        }
#if SOC_SPI_SUPPORT_OCT
        if (flags & SPICOMMON_BUSFLAG_OCTAL) {
            int io_nums[] = {bus_config->data4_io_num, bus_config->data5_io_num, bus_config->data6_io_num, bus_config->data7_io_num};
            uint8_t io_signals[4][2] = {
                {spi_periph_signal[host].spid4_out, spi_periph_signal[host].spid4_in},
                {spi_periph_signal[host].spid5_out, spi_periph_signal[host].spid5_in},
                {spi_periph_signal[host].spid6_out, spi_periph_signal[host].spid6_in},
                {spi_periph_signal[host].spid7_out, spi_periph_signal[host].spid7_in}
            };
            for (size_t i = 0; i < sizeof(io_nums) / sizeof(io_nums[0]); i++) {
                if (io_nums[i] >= 0) {
                    s_spi_common_bus_via_gpio(io_nums[i], io_signals[i][1], io_signals[i][0], &gpio_reserv);
                }
            }
        }
#endif //SOC_SPI_SUPPORT_OCT
    }

    if (flags_o) {
        *flags_o = temp_flag;
    }
    if (io_reserved) {
        *io_reserved |= gpio_reserv;
    }
    return ESP_OK;
}

esp_err_t spicommon_bus_free_io_cfg(const spi_bus_config_t *bus_cfg, uint64_t *io_reserved)
{
    for (uint8_t i = 0; i < sizeof(bus_cfg->iocfg) / sizeof(bus_cfg->iocfg[0]); i++) {
#if !SOC_SPI_SUPPORT_OCT
        if (i > 4) {
            break;
        }
#endif
        if (GPIO_IS_VALID_GPIO(bus_cfg->iocfg[i]) && (*io_reserved & BIT64(bus_cfg->iocfg[i]))) {
            *io_reserved &= ~BIT64(bus_cfg->iocfg[i]);
            gpio_output_disable(bus_cfg->iocfg[i]);
            esp_gpio_revoke(BIT64(bus_cfg->iocfg[i]));
        }
    }
    return ESP_OK;
}

void spicommon_cs_initialize(spi_host_device_t host, int cs_io_num, int cs_id, int force_gpio_matrix, uint64_t *io_reserved)
{
    uint64_t out_mask = 0;
    if (!force_gpio_matrix && cs_io_num == spi_periph_signal[host].spics0_iomux_pin && cs_id == 0) {
        out_mask |= BIT64(cs_io_num);
        s_spi_common_gpio_check_reserve(cs_io_num);
        //The cs0s for all SPI peripherals map to pin mux source 1, so we use that instead of a define.
        gpio_iomux_input(cs_io_num, spi_periph_signal[host].func, spi_periph_signal[host].spics_in);
        gpio_iomux_output(cs_io_num, spi_periph_signal[host].func);
    } else {
        //Use GPIO matrix
        if (GPIO_IS_VALID_OUTPUT_GPIO(cs_io_num)) {
            out_mask |= BIT64(cs_io_num);
            s_spi_common_gpio_check_reserve(cs_io_num);
            esp_rom_gpio_connect_out_signal(cs_io_num, spi_periph_signal[host].spics_out[cs_id], false, false);
        }
        // cs_id 0 is always used by slave for input
        if (cs_id == 0) {
            gpio_input_enable(cs_io_num);
            esp_rom_gpio_connect_in_signal(cs_io_num, spi_periph_signal[host].spics_in, false);
        }
        gpio_func_sel(cs_io_num, PIN_FUNC_GPIO);
    }
    if (io_reserved) {
        *io_reserved |= out_mask;
    }
}

void spicommon_cs_free_io(int cs_gpio_num, uint64_t *io_reserved)
{
    if (GPIO_IS_VALID_GPIO(cs_gpio_num) && (*io_reserved & BIT64(cs_gpio_num))) {
        *io_reserved &= ~BIT64(cs_gpio_num);
        gpio_output_disable(cs_gpio_num);
        esp_gpio_revoke(BIT64(cs_gpio_num));
    }
}

spi_bus_lock_handle_t spi_bus_lock_get_by_id(spi_host_device_t host_id)
{
    return bus_ctx[host_id]->bus_attr.lock;
}

#if SOC_SPI_SUPPORT_SLEEP_RETENTION && CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
static esp_err_t s_bus_create_sleep_retention_cb(void *arg)
{
    spicommon_bus_context_t *ctx = arg;
    return sleep_retention_entries_create(spi_reg_retention_info[ctx->host_id - 1].entry_array,
                                          spi_reg_retention_info[ctx->host_id - 1].array_size,
                                          REGDMA_LINK_PRI_GPSPI,
                                          spi_reg_retention_info[ctx->host_id - 1].module_id);
}
#endif  // SOC_SPI_SUPPORT_SLEEP_RETENTION
//----------------------------------------------------------master bus init-------------------------------------------------------//
esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config, spi_dma_chan_t dma_chan)
{
    esp_err_t err = ESP_OK;
    SPI_CHECK(is_valid_host(host_id), "invalid host_id", ESP_ERR_INVALID_ARG);
    SPI_CHECK(bus_ctx[host_id] == NULL, "SPI bus already initialized.", ESP_ERR_INVALID_STATE);
#ifdef CONFIG_IDF_TARGET_ESP32
    SPI_CHECK(dma_chan >= SPI_DMA_DISABLED && dma_chan <= SPI_DMA_CH_AUTO, "invalid dma channel", ESP_ERR_INVALID_ARG);
#elif CONFIG_IDF_TARGET_ESP32S2
    SPI_CHECK(dma_chan == SPI_DMA_DISABLED || dma_chan == (int)host_id || dma_chan == SPI_DMA_CH_AUTO, "invalid dma channel", ESP_ERR_INVALID_ARG);
#elif SOC_GDMA_SUPPORTED
    SPI_CHECK(dma_chan == SPI_DMA_DISABLED || dma_chan == SPI_DMA_CH_AUTO, "invalid dma channel, chip only support spi dma channel auto-alloc", ESP_ERR_INVALID_ARG);
#endif
    SPI_CHECK((bus_config->intr_flags & (ESP_INTR_FLAG_HIGH | ESP_INTR_FLAG_EDGE | ESP_INTR_FLAG_INTRDISABLED)) == 0, "intr flag not allowed", ESP_ERR_INVALID_ARG);
#ifndef CONFIG_SPI_MASTER_ISR_IN_IRAM
    SPI_CHECK((bus_config->intr_flags & ESP_INTR_FLAG_IRAM) == 0, "ESP_INTR_FLAG_IRAM should be disabled when CONFIG_SPI_MASTER_ISR_IN_IRAM is not set.", ESP_ERR_INVALID_ARG);
#endif
#if CONFIG_IDF_TARGET_ESP32
    SPI_CHECK((bus_config->data_io_default_level == 0), "no support changing io default level ", ESP_ERR_INVALID_ARG);
#endif

    ESP_RETURN_ON_ERROR(spicommon_bus_alloc(host_id, "spi master"), SPI_TAG, "alloc host failed");
    spi_bus_attr_t *bus_attr = (spi_bus_attr_t *)spi_bus_get_attr(host_id);
    spicommon_bus_context_t *ctx = __containerof(bus_attr, spicommon_bus_context_t, bus_attr);
    assert(bus_attr && ctx);  //coverity check
    bus_attr->bus_cfg = *bus_config;

    if (dma_chan != SPI_DMA_DISABLED) {
        bus_attr->dma_enabled = 1;

        err = spicommon_dma_chan_alloc(host_id, dma_chan, &ctx->dma_ctx);
        if (err != ESP_OK) {
            goto cleanup;
        }
        err = spicommon_dma_desc_alloc(ctx->dma_ctx, bus_config->max_transfer_sz, &bus_attr->max_transfer_sz);
        if (err != ESP_OK) {
            goto cleanup;
        }
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
        esp_cache_get_alignment(MALLOC_CAP_DMA, (size_t *)&bus_attr->internal_mem_align_size);
#else
        bus_attr->internal_mem_align_size = 4;
#endif
    } else {
        bus_attr->dma_enabled = 0;
        bus_attr->max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE;
    }

    spi_bus_lock_config_t lock_config = {
        .host_id = host_id,
        .cs_num = SOC_SPI_PERIPH_CS_NUM(host_id),
    };
    err = spi_bus_init_lock(&bus_attr->lock, &lock_config);
    if (err != ESP_OK) {
        goto cleanup;
    }

#if SOC_SPI_SUPPORT_SLEEP_RETENTION && CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    sleep_retention_module_init_param_t init_param = {
        .cbs = {
            .create = {
                .handle = s_bus_create_sleep_retention_cb,
                .arg = ctx,
            },
        },
        .depends = RETENTION_MODULE_BITMAP_INIT(CLOCK_SYSTEM)
    };

    _lock_acquire(&ctx->mutex);
    if (sleep_retention_module_init(spi_reg_retention_info[host_id - 1].module_id, &init_param) == ESP_OK) {
        if ((bus_attr->bus_cfg.flags & SPICOMMON_BUSFLAG_SLP_ALLOW_PD) && (sleep_retention_module_allocate(spi_reg_retention_info[host_id - 1].module_id) != ESP_OK)) {
            // even though the sleep retention create failed, SPI driver should still work, so just warning here
            ESP_LOGW(SPI_TAG, "alloc sleep recover failed, peripherals may hold power on");
        }
    } else {
        // even the sleep retention init failed, SPI driver should still work, so just warning here
        ESP_LOGW(SPI_TAG, "init sleep recover failed, spi may offline after sleep");
    }
    _lock_release(&ctx->mutex);
#else
    if (bus_attr->bus_cfg.flags & SPICOMMON_BUSFLAG_SLP_ALLOW_PD) {
        ESP_LOGE(SPI_TAG, "power down peripheral in sleep is not enabled or not supported on your target");
    }
#endif  // SOC_SPI_SUPPORT_SLEEP_RETENTION

#ifdef CONFIG_PM_ENABLE
#if CONFIG_IDF_TARGET_ESP32P4
    // use CPU_MAX lock to ensure PSRAM bandwidth and usability during DFS
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "spi_master",
                             &bus_attr->pm_lock);
#else
    err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "spi_master",
                             &bus_attr->pm_lock);
#endif
    if (err != ESP_OK) {
        goto cleanup;
    }
#endif //CONFIG_PM_ENABLE

    err = spicommon_bus_initialize_io(host_id, bus_config, SPICOMMON_BUSFLAG_MASTER | bus_config->flags, &bus_attr->flags, &bus_attr->gpio_reserve);
    if (err != ESP_OK) {
        goto cleanup;
    }

    return ESP_OK;

cleanup:
    if (bus_attr) {
#ifdef CONFIG_PM_ENABLE
        esp_pm_lock_delete(bus_attr->pm_lock);
#endif
        if (bus_attr->lock) {
            spi_bus_deinit_lock(bus_attr->lock);
        }
        if (ctx->dma_ctx) {
            free(ctx->dma_ctx->dmadesc_tx);
            free(ctx->dma_ctx->dmadesc_rx);
            spicommon_dma_chan_free(ctx->dma_ctx);
            ctx->dma_ctx = NULL;
        }
    }
    spicommon_bus_free(host_id);
    return err;
}

void *spi_bus_dma_memory_alloc(spi_host_device_t host_id, size_t size, uint32_t extra_heap_caps)
{
    (void) host_id; //remain for extendability
    ESP_RETURN_ON_FALSE((extra_heap_caps & MALLOC_CAP_SPIRAM) == 0, NULL, SPI_TAG, "external memory is not supported now");

    size_t dma_requir = 16;  //TODO: IDF-10111, using max alignment temp, refactor to "gdma_get_alignment_constraints" instead
    return heap_caps_aligned_calloc(dma_requir, 1, size, extra_heap_caps | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

const spi_bus_attr_t* spi_bus_get_attr(spi_host_device_t host_id)
{
    if (bus_ctx[host_id] == NULL) {
        return NULL;
    }

    return &bus_ctx[host_id]->bus_attr;
}

const spi_dma_ctx_t* spi_bus_get_dma_ctx(spi_host_device_t host_id)
{
    if (bus_ctx[host_id] == NULL) {
        return NULL;
    }

    return bus_ctx[host_id]->dma_ctx;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    if (bus_ctx[host_id] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    spicommon_bus_context_t* ctx = bus_ctx[host_id];
    spi_bus_attr_t* bus_attr = &ctx->bus_attr;

    if (ctx->destroy_func) {
        err = ctx->destroy_func(ctx->destroy_arg);
        if (err != ESP_OK) {
            return err;
        }
    }
    spicommon_bus_free_io_cfg(&bus_attr->bus_cfg, &bus_attr->gpio_reserve);

#if SOC_SPI_SUPPORT_SLEEP_RETENTION && CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    const periph_retention_module_t retention_id = spi_reg_retention_info[host_id - 1].module_id;
    _lock_acquire(&ctx->mutex);
    if (sleep_retention_is_module_created(retention_id)) {
        assert(sleep_retention_is_module_inited(retention_id));
        sleep_retention_module_free(retention_id);
    }
    if (sleep_retention_is_module_inited(retention_id)) {
        sleep_retention_module_deinit(retention_id);
    }
    _lock_release(&ctx->mutex);
    _lock_close(&ctx->mutex);
#endif

#ifdef CONFIG_PM_ENABLE
    esp_pm_lock_delete(bus_attr->pm_lock);
#endif

    spi_bus_deinit_lock(bus_attr->lock);
    if (ctx->dma_ctx) {
        free(ctx->dma_ctx->dmadesc_tx);
        free(ctx->dma_ctx->dmadesc_rx);
        spicommon_dma_chan_free(ctx->dma_ctx);
        ctx->dma_ctx = NULL;
    }
    spicommon_bus_free(host_id);
    return err;
}

esp_err_t spi_bus_register_destroy_func(spi_host_device_t host_id,
                                        spi_destroy_func_t f, void *arg)
{
    bus_ctx[host_id]->destroy_func = f;
    bus_ctx[host_id]->destroy_arg = arg;
    return ESP_OK;
}

/*
Code for workaround for DMA issue in ESP32 v0/v1 silicon
*/
#if CONFIG_IDF_TARGET_ESP32
static volatile int dmaworkaround_channels_busy[2] = {0, 0};
static dmaworkaround_cb_t dmaworkaround_cb;
static void *dmaworkaround_cb_arg;
static portMUX_TYPE dmaworkaround_mux = portMUX_INITIALIZER_UNLOCKED;
static int dmaworkaround_waiting_for_chan = 0;

bool SPI_COMMON_ISR_ATTR spicommon_dmaworkaround_req_reset(int dmachan, dmaworkaround_cb_t cb, void *arg)
{

    int otherchan = (dmachan == 1) ? 2 : 1;
    bool ret;
    portENTER_CRITICAL_ISR(&dmaworkaround_mux);
    if (dmaworkaround_channels_busy[otherchan - 1]) {
        //Other channel is busy. Call back when it's done.
        dmaworkaround_cb = cb;
        dmaworkaround_cb_arg = arg;
        dmaworkaround_waiting_for_chan = otherchan;
        ret = false;
    } else {
        //Reset DMA
        SPI_COMMON_RCC_CLOCK_ATOMIC() {
            spi_dma_ll_reset_register(dmachan);
        }
        ret = true;
    }
    portEXIT_CRITICAL_ISR(&dmaworkaround_mux);
    return ret;
}

bool SPI_COMMON_ISR_ATTR spicommon_dmaworkaround_reset_in_progress(void)
{
    return (dmaworkaround_waiting_for_chan != 0);
}

void SPI_COMMON_ISR_ATTR spicommon_dmaworkaround_idle(int dmachan)
{
    portENTER_CRITICAL_ISR(&dmaworkaround_mux);
    dmaworkaround_channels_busy[dmachan - 1] = 0;
    if (dmaworkaround_waiting_for_chan == dmachan) {
        //Reset DMA
        SPI_COMMON_RCC_CLOCK_ATOMIC() {
            spi_dma_ll_reset_register(dmachan);
        }
        dmaworkaround_waiting_for_chan = 0;
        //Call callback
        dmaworkaround_cb(dmaworkaround_cb_arg);

    }
    portEXIT_CRITICAL_ISR(&dmaworkaround_mux);
}

void SPI_COMMON_ISR_ATTR spicommon_dmaworkaround_transfer_active(int dmachan)
{
    portENTER_CRITICAL_ISR(&dmaworkaround_mux);
    dmaworkaround_channels_busy[dmachan - 1] = 1;
    portEXIT_CRITICAL_ISR(&dmaworkaround_mux);
}
#endif //#if CONFIG_IDF_TARGET_ESP32
