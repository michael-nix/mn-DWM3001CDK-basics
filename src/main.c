#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "deca_device_api.h"
#include "deca_interface.h"

// Set which device kicks off transmissions.  Device ID is printed out after
// DW3000 initialization, and can be added here before you recompile.  All other
// devices will respond to messages sent by this device:
#define INITIATOR_DEVICE_ID 0x5056474e0a12c8ba // 0x5056474d4a134211

#define DW3000_NODE DT_NODELABEL(dw3000)

// Maximum time to wait for the DW3000 to enter IDLE_RC mode after reset:
#define DW_MAX_WAIT_UNTIL_IDLE_MS 100

// Maximum number of events in the dw_event_queue, tracking DW3000 RX events:
#define DW_MAX_NUM_EVENTS 4

// default antenna delay, from DW3000 User Manual:
#define DW_ANTENNA_DELAY 0x4015

//  UWB microsecond (uus) to device time unit (dtu, around 15.65 ps) conversion
//  factor. 1 uus = 512 / 499.2 µs and 1 µs = 499.2 * 128 dtu.
#define UUS_TO_DWT_TIME (63898)

// This was the fastest I could get it:
#define DW_TRANSMIT_TIME_DELAY_UUS (2250)

// Speed of light... (m / s)
#define SPEED_OF_LIGHT (299702547.0)

LOG_MODULE_REGISTER(uwb_test, LOG_LEVEL_INF);

// In order to support both slow and fast SPI rates, we define two
// `spi_dt_spec`s, and switch between them by changing the pointer `dw3000_spi`
// which is used in the SPI communication functions below.  I saw this
// recommended by Zephyr somewhere; something about in case you update the SPI
// config while it's doing something else, since it needs to be a global pointer
// for the DW3000 driver to be able to use the platform specific SPI functions.
static struct spi_dt_spec dw_spi_slow =
    SPI_DT_SPEC_GET(DW3000_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static struct spi_dt_spec dw_spi_fast =
    SPI_DT_SPEC_GET(DW3000_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);

// Device tree specification for the SPI peripheral connected to the DW3000.
// Initialized to the slow frequency by default.
static const struct spi_dt_spec* dw3000_spi = &dw_spi_slow;

static const struct gpio_dt_spec dw3000_reset_pin =
    GPIO_DT_SPEC_GET(DW3000_NODE, reset_gpios);

static const struct gpio_dt_spec dw3000_irq_pin =
    GPIO_DT_SPEC_GET(DW3000_NODE, irq_gpios);

struct gpio_callback dw3000_irq_callback = {0};

static dwt_config_t dw3000_config = {0};

// defined in dw3000_device.c, the only driver needed for the DWM3001CDK.
extern const struct dwt_driver_s dw3000_driver;

// needed to populate the `dw_probe` struct used to initialize the DW3000 driver
// with `dwt_probe()`.
static const struct dwt_driver_s* dw_driver_list[] = {
    &dw3000_driver,
};

/* ----------------------------------------------------------------------------
    Platform specific function definitions to be used by the DW3000 driver to
    communicate over SPI, and tracked in the `struct dwt_spi_s`
    `dw_spi_functions`, or the `struct dwt_probe_s` `dw_probe`:
    - dw_readfromspi,
    - dw_writetospi,
    - dw_writetospiwithcrc,
    - dw_setslowrate,
    - dw_setfastrate,
    - dw_wakeup_device_with_io.
*/

int dw_readfromspi(uint16_t headerLength, uint8_t* headerBuffer,
    uint16_t readLength, uint8_t* readBuffer)
{
    struct spi_buf tx_buffers[1] = {{
        .buf = (void*)headerBuffer,
        .len = headerLength,
    }};
    struct spi_buf_set tx_buffer_set = {
        .buffers = tx_buffers,
        .count = 1,
    };

    struct spi_buf rx_buffers[2] = {
        {
            .buf = NULL,
            .len = headerLength,
        },
        {
            .buf = (void*)readBuffer,
            .len = readLength,
        },
    };

    struct spi_buf_set rx_buffer_set = {
        .buffers = rx_buffers,
        .count = 2,
    };

    return spi_transceive_dt(dw3000_spi, &tx_buffer_set, &rx_buffer_set);
}

int dw_writetospi(uint16_t headerLength, const uint8_t* headerBuffer,
    uint16_t bodyLength, const uint8_t* bodyBuffer)
{
    struct spi_buf tx_buffers[2] = {
        {
            .buf = (void*)headerBuffer,
            .len = headerLength,
        },
        {
            .buf = (void*)bodyBuffer,
            .len = bodyLength,
        },
    };

    struct spi_buf_set tx_buffer_set = {
        .buffers = tx_buffers,
        .count = 2,
    };

    return spi_write_dt(dw3000_spi, &tx_buffer_set);
}

int dw_writetospiwithcrc(uint16_t headerLength, const uint8_t* headerBuffer,
    uint16_t bodyLength, const uint8_t* bodyBuffer, uint8_t crc8)
{
    ARG_UNUSED(crc8);
    return dw_writetospi(headerLength, headerBuffer, bodyLength, bodyBuffer);
}

static void dw_setslowrate(void) { dw3000_spi = &dw_spi_slow; }

static void dw_setfastrate(void) { dw3000_spi = &dw_spi_fast; }

static void dw_wakeup_device_with_io(void)
{
    gpio_pin_set_dt(&(dw3000_spi->config.cs.gpio), GPIO_OUTPUT_ACTIVE);
    k_busy_wait(500);

    gpio_pin_set_dt(
        &(dw3000_spi->config.cs.gpio), GPIO_OUTPUT_INACTIVE); /* deassert   */
    k_msleep(4);
}

/*
    DW3000 function definitions for SPI communication, which are passed to the
    DW3000 driver via the `struct dwt_probe_s` `dw_probe` by calling
    `dwt_probe()`.
*/
static const struct dwt_spi_s dw_spi_functions = {
    .readfromspi = dw_readfromspi,
    .writetospi = dw_writetospi,
    .writetospiwithcrc = dw_writetospiwithcrc,
    .setslowrate = dw_setslowrate,
    .setfastrate = dw_setfastrate,
};

/*
    DW3000 probe structure to be passed to `dwt_probe()` on initialization,
    which tracks the SPI function definitions in `dw_spi_functions`, and the
    list of available DW drivers in `dw_driver_list`.
*/
static struct dwt_probe_s dw_probe = {
    .dw = NULL,
    .spi = (void*)&dw_spi_functions,
    .wakeup_device_with_io = dw_wakeup_device_with_io,
    .driver_list = (struct dwt_driver_s**)dw_driver_list,
    .dw_driver_num = ARRAY_SIZE(dw_driver_list),
};

// ----------------------------------------------------------------------------

/* ----------------------------------------------------------------------------
    Platform specific function definitions required by deca_device_api.h:
     - decamutexon,
     - decamutexoff,
     - deca_sleep,
     - deca_usleep.
*/

decaIrqStatus_t decamutexon(void)
{
    gpio_pin_interrupt_configure_dt(&dw3000_irq_pin, GPIO_INT_DISABLE);

    return 1;
}

void decamutexoff(decaIrqStatus_t s)
{
    gpio_pin_interrupt_configure_dt(&dw3000_irq_pin, GPIO_INT_ENABLE);
}

void deca_sleep(unsigned int time_ms) { k_msleep(time_ms); }

void deca_usleep(unsigned long time_us) { k_usleep(time_us); }

// ----------------------------------------------------------------------------

/* ----------------------------------------------------------------------------
    Functions and structures for managing interrupt requests on the
    `dw3000_irq_pin`.
*/

/*
    Called when an interrupt is triggered on the `dw3000_irq_pin`, and only
    serves as a platform specific wrapper around the `dwt_isr()` function
    provided by the DW3000 driver.
*/
void dw3000_irq_handler(
    const struct device* port, struct gpio_callback* cb, uint32_t pins)
{
    dwt_isr();
}

enum dw3000_msg_type
{
    DW3000_INIT_TX = 0xC5,
    DW3000_RESP_TX = 0xC6,
    DW3000_RX_ERR,
};

// Structure passed from ISR to main loop
struct dw3000_msg_data
{
    uint8_t msg_type;
    uint8_t seq_num;
    uint64_t device_id;
    uint64_t reply_time;
};

#define DW_MSG_DATA_TXRX_LEN (sizeof(struct dw3000_msg_data))

// Message queue for managing DW3000 RX-OK events.
K_MSGQ_DEFINE(
    dw_event_queue, sizeof(struct dw3000_msg_data), DW_MAX_NUM_EVENTS, 4);

/*
    Callback function for the DW3000 driver to call when a RX good frame event
    is triggered.
*/
void dw3000_rxok_callback(const dwt_cb_data_t* data)
{
    LOG_INF("RX OK callback triggered, reading data from DW3000.");

    struct dw3000_msg_data rx_message = {0};
    dwt_readrxdata((uint8_t*)&rx_message, DW_MSG_DATA_TXRX_LEN, 0);

    if (0 != k_msgq_put(&dw_event_queue, &rx_message, K_NO_WAIT))
    {
        LOG_WRN("DW event queue full, dropping message.");
    }
}

void dw3000_rxerr_callback(const dwt_cb_data_t* data)
{
    LOG_ERR("Error receiving DW3000 frame.");

    struct dw3000_msg_data rx_message = {0};
    rx_message.msg_type = DW3000_RX_ERR;

    if (0 != k_msgq_put(&dw_event_queue, &rx_message, K_NO_WAIT))
    {
        LOG_WRN("DW event queue full, dropping message.");
    }
}

static dwt_callbacks_s dw3000_irq_callbacks = {
    .cbRxOk = dw3000_rxok_callback,
    .cbRxTo = dw3000_rxerr_callback,
    .cbRxErr = dw3000_rxerr_callback,
    .devErr = dw3000_rxerr_callback,
};

// ----------------------------------------------------------------------------

void dw3000_reset(void)
{
    gpio_pin_set_dt(&dw3000_reset_pin, 1);
    k_msleep(2);
    gpio_pin_set_dt(&dw3000_reset_pin, 0);
    k_msleep(2);
}

int dw3000_initialize(uint64_t* device_id)
{
    if (!spi_is_ready_dt(dw3000_spi))
    {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&dw3000_reset_pin) ||
        !gpio_is_ready_dt(&dw3000_irq_pin))
    {
        LOG_ERR("GPIOs not ready");
        return -ENODEV;
    }

    dw_spi_slow.config.frequency = DT_PROP(DW3000_NODE, spi_min_frequency);
    dw_spi_fast.config.frequency = DT_PROP(DW3000_NODE, spi_max_frequency);

    gpio_pin_configure_dt(&dw3000_reset_pin, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&dw3000_spi->config.cs.gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&dw3000_irq_pin, GPIO_INPUT);

    gpio_init_callback(
        &dw3000_irq_callback, dw3000_irq_handler, BIT(dw3000_irq_pin.pin));
    gpio_add_callback(dw3000_irq_pin.port, &dw3000_irq_callback);
    gpio_pin_interrupt_configure_dt(&dw3000_irq_pin, GPIO_INT_EDGE_TO_ACTIVE);

    LOG_INF("SPI peripheral for DW3000 initialized.");

    dw3000_reset();

    LOG_INF("DW3000 reset complete.");

    if (dwt_probe(&dw_probe) != DWT_SUCCESS)
    {
        LOG_ERR("dwt_probe failed.");
        return -ENODEV;
    }

    LOG_INF("DW3000 probe success.");

    int i = 0;
    for (; i < DW_MAX_WAIT_UNTIL_IDLE_MS; i++)
    {
        if (1 == dwt_checkidlerc())
        {
            break;
        }

        k_msleep(1);
    }

    if (DW_MAX_WAIT_UNTIL_IDLE_MS == i)
    {
        LOG_ERR("DW3000 not in IDLE_RC.");

        return -ENODEV;
    }

    LOG_INF("DW3000 in IDLE_RC, proceeding with initialise.");

    dw_setfastrate();

    uint32_t dev_id = dwt_readdevid();
    LOG_INF("DW3000 DEV_ID = 0x%08x", dev_id);

    if (DWT_ERROR == dwt_initialise(DWT_DW_INIT))
    {
        LOG_ERR("dwt_initialise failed.");

        return -ENODEV;
    }

    LOG_INF("DW3000 initialized.");

    dw3000_config.chan = DT_PROP(DW3000_NODE, chan);
    dw3000_config.dataRate = DT_PROP(DW3000_NODE, datarate);
    dw3000_config.pdoaMode = DT_PROP(DW3000_NODE, pdoamode);
    dw3000_config.phrMode = DT_PROP(DW3000_NODE, phrmode);
    dw3000_config.phrRate = DT_PROP(DW3000_NODE, phrrate);
    dw3000_config.rxCode = DT_PROP(DW3000_NODE, rxcode);
    dw3000_config.rxPAC = DT_PROP(DW3000_NODE, rxpac);
    dw3000_config.stsLength = DT_PROP(DW3000_NODE, stslength);
    dw3000_config.stsMode = DT_PROP(DW3000_NODE, stsmode);
    dw3000_config.txCode = DT_PROP(DW3000_NODE, txcode);
    dw3000_config.txPreambLength = DT_PROP(DW3000_NODE, txpreamblength);
    dw3000_config.sfdType = DT_PROP(DW3000_NODE, sfdtype);

    uint16_t sfd_length = 8;
    switch (dw3000_config.sfdType)
    {
    case DWT_SFD_DW_16:
    case DWT_SFD_LEN16:
        sfd_length = 16;
        break;
    default:
        break;
    }

    uint16_t pac_length = 16;
    switch (dw3000_config.rxPAC)
    {
    case DWT_PAC8:
        pac_length = 8;
        break;
    case DWT_PAC32:
        pac_length = 32;
        break;
    case DWT_PAC4:
        pac_length = 4;
        break;
    default:
        break;
    }

    dw3000_config.sfdTO =
        dw3000_config.txPreambLength + 1 + sfd_length - pac_length;

    if (DWT_SUCCESS != dwt_configure(&dw3000_config))
    {
        LOG_ERR("dwt_configure failed.");

        return -ENODEV;
    }

    LOG_INF("DW3000 configured.");

    uint64_t lot_id = dwt_getlotid();   // 48 bits
    uint32_t part_id = dwt_getpartid(); // 32 bits
    uint64_t full_id = (lot_id << 16) + (uint64_t)part_id;
    *device_id = full_id;

    LOG_INF("DW3000 Full ID: 0x%llx", full_id);

    // From Table 7 of the DW3000 Software API Guide:
    dwt_txconfig_t tx_config = {0};
    if (5 == DT_PROP(DW3000_NODE, chan))
    {
        tx_config.PGcount = 0;
        tx_config.PGdly = 0x34;
        tx_config.power = 0xfdfdfdfd;
    }
    else
    {
        tx_config.PGcount = 0;
        tx_config.PGdly = 0x34;
        tx_config.power = 0xfefefefe;
    };

    dwt_configuretxrf(&tx_config);

    LOG_INF("DW3000 TX spectrum configured.");

    dwt_setcallbacks(&dw3000_irq_callbacks);
    dwt_setinterrupt(DWT_INT_RX, 0, DWT_ENABLE_INT);

    LOG_INF("DW3000 interrupts and callback set.");

    dwt_settxantennadelay(DW_ANTENNA_DELAY);
    dwt_setrxantennadelay(DW_ANTENNA_DELAY);

    LOG_INF("DW3000 antenna delays set.");

    dwt_setleds(DWT_LEDS_BLINK_TIME_DEF | DWT_LEDS_ENABLE);

    return 0;
}

int main(void)
{
    uint64_t device_id = 0;
    if (dw3000_initialize(&device_id) != 0)
    {
        LOG_ERR("HW init failed");

        return -1;
    }

    // IEEE standard message type for a blink is 0xC5, followed by sequence
    // number, then eight byte device ID:
    struct dw3000_msg_data tx_message = {0};
    tx_message.msg_type = INITIATOR_DEVICE_ID == device_id ? DW3000_INIT_TX :
                                                             DW3000_RESP_TX;
    tx_message.device_id = device_id;

    LOG_INF("");

    if (INITIATOR_DEVICE_ID != device_id)
    {
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        while (true)
        {
            struct dw3000_msg_data rx_message = {0};
            if (0 != k_msgq_get(&dw_event_queue, &rx_message, K_FOREVER))
            {
                LOG_WRN("DW event queue purged.");
                dwt_rxenable(DWT_START_RX_IMMEDIATE);

                continue;
            }

            switch ((enum dw3000_msg_type)rx_message.msg_type)
            {
            case DW3000_RX_ERR:
                LOG_ERR("DW3000 responder failed to receive a message, "
                        "resetting receiver.");
                dwt_rxenable(DWT_START_RX_IMMEDIATE);

                continue;
            default:
            }

            uint64_t rx_timestamp = 0;
            dwt_readrxtimestamp((uint8_t*)&rx_timestamp, DWT_COMPAT_NONE);

            // transmit time only uses the top 31 bits of the 40 bit timestamps,
            // which is why you see it converted to a uint32 by shifting down
            // eight bits, then convert it back and lopping off the LSB.  This
            // way the actual time spent between reception and transmission is
            // accurate to when the actual message is sent:
            uint32_t transmit_time =
                (rx_timestamp +
                    (DW_TRANSMIT_TIME_DELAY_UUS * UUS_TO_DWT_TIME)) >>
                8;

            dwt_setdelayedtrxtime(transmit_time);

            tx_message.reply_time =
                (((uint64_t)(transmit_time & 0xFFFFFFFEUL)) << 8) +
                DW_ANTENNA_DELAY - rx_timestamp;

            dwt_writetxdata(DW_MSG_DATA_TXRX_LEN, (uint8_t*)&tx_message, 0);
            dwt_writetxfctrl(
                DW_MSG_DATA_TXRX_LEN + 2, 0, 1); // + 2 bytes for CRC

            int32_t error =
                dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
            if (DWT_SUCCESS != error)
            {
                LOG_WRN("DW3000 transmission was cancelled, transmit time has "
                        "passed!\n");

                dwt_rxenable(DWT_START_RX_IMMEDIATE);

                continue;
            }

            uint64_t tx_timestamp = {0};
            dwt_readtxtimestamp((uint8_t*)&tx_timestamp);

            LOG_INF("DW3000 rx event device_id = 0x%llx", rx_message.device_id);
            LOG_INF("DW3000 rx timestamp = %llu", rx_timestamp);
            LOG_INF("Sending message, device_id = 0x%llx", device_id);
            LOG_INF("Transmit time: %llu", (uint64_t)transmit_time << 8);
            LOG_INF("Reply time: %llu\n", rx_message.reply_time);
        }

        return 0;
    }

    while (true)
    {
        LOG_INF("Sending message, device_id = 0x%llx", device_id);
        dwt_forcetrxoff();

        dwt_writetxdata(DW_MSG_DATA_TXRX_LEN, (uint8_t*)&tx_message, 0);
        dwt_writetxfctrl(DW_MSG_DATA_TXRX_LEN + 2, 0, 1); // + 2 bytes for CRC
        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

        struct dw3000_msg_data rx_message = {0};
        int error = k_msgq_get(&dw_event_queue, &rx_message, K_MSEC(100));
        if (0 != error)
        {
            LOG_INF("Expected DW3000 rx event timeout!\n");

            k_sleep(K_MSEC(900));

            continue;
        }

        switch ((enum dw3000_msg_type)rx_message.msg_type)
        {
        case DW3000_RX_ERR:
            LOG_ERR("DW3000 initiator failed to receive a message, resetting "
                    "receiver.");

            k_sleep(K_MSEC(900));

            continue;
        default:
        }

        uint64_t tx_timestamp = 0;
        dwt_readtxtimestamp((uint8_t*)&tx_timestamp);

        uint64_t rx_timestamp = 0;
        dwt_readrxtimestamp((uint8_t*)&rx_timestamp, DWT_COMPAT_NONE);

        LOG_INF("TX timestamp from raw: %llu\n", tx_timestamp);

        LOG_INF("DW3000 rx event device_id = 0x%llx", rx_message.device_id);
        LOG_INF("DW3000 rx timestamp = %llu\n", rx_timestamp);

        double offset = ((float)dwt_readclockoffset()) / (uint32_t)(1 << 26);
        double range = (rx_timestamp - tx_timestamp -
                           rx_message.reply_time * (1.0 - offset)) /
                       2.0 * DWT_TIME_UNITS * SPEED_OF_LIGHT;

        LOG_INF("Calculated range: %f m\n", range);

        k_sleep(K_SECONDS(1));
    }

    return 0;
}
