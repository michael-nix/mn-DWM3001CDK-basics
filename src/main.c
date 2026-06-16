#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include "dw3000_thread.h"

LOG_MODULE_REGISTER(main_thread, LOG_LEVEL_INF);

#define DW3000_THREAD_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(dw3000_thread_stack, DW3000_THREAD_STACK_SIZE)

struct k_event usb_event = {0};

enum usb_events
{
    DW3000_USB_CONNECTED = 1,
};

void usb_state_callback(enum usb_dc_status_code cb_status, const uint8_t* param)
{
    switch (cb_status)
    {
    case USB_DC_CONNECTED:
        LOG_INF("USB connected, this DW3000 is an initiator.");
        k_event_post(&usb_event, DW3000_USB_CONNECTED);

        break;

    default:
        break;
    }
}

int usb_initialize(bool* is_initiator)
{
    k_event_init(&usb_event);
    if (usb_enable(usb_state_callback) != 0)
    {
        LOG_ERR("USB enable failed");

        return -1;
    }

    uint32_t event_match =
        k_event_wait(&usb_event, DW3000_USB_CONNECTED, false, K_MSEC(500));
    *is_initiator = event_match == DW3000_USB_CONNECTED ? true : false;

    return 0;
}

int main(void)
{
    bool is_initiator = false;
    if (usb_initialize(&is_initiator) != 0)
    {
        return -1;
    }

    struct k_thread dw3000_thread_handle = {0};
    k_thread_create(&dw3000_thread_handle, dw3000_thread_stack,
        DW3000_THREAD_STACK_SIZE, dw3000_thread, (void*)&is_initiator, NULL,
        NULL, 1, 0, K_NO_WAIT);

    while (true)
    {
        k_sleep(K_FOREVER);
    }

    return 0;
}
