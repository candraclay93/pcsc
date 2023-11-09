/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sys/param.h"
#include "usb/usb_host.h"

#define CLIENT_NUM_EVENT_MSG 5

typedef enum
{
    ACTION_CCID_ON = 0x00,
    ACTION_OPEN_DEV = 0x01,
    ACTION_GET_DEV_INFO = 0x02,
    ACTION_GET_DEV_DESC = 0x04,
    ACTION_GET_CONFIG_DESC = 0x08,
    ACTION_GET_STR_DESC = 0x10,
    ACTION_CLOSE_DEV = 0x20,
    ACTION_EXIT = 0x40,
    ACTION_RECONNECT = 0x80,
} action_t;

usb_device_info_t dev_info;
const usb_device_desc_t *dev_desc;
const usb_config_desc_t *config_desc;

uint8_t bufferData = {0};

typedef struct
{
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint32_t actions;
} class_driver_t;

static const char *TAG = "CLASS";
static class_driver_t *s_driver_obj;

static void ccid_cb(usb_transfer_t *transfer)
{
    printf("Transfer status %d, actual number of bytes transferred %d\n", transfer->status, transfer->actual_num_bytes);
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event)
    {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (driver_obj->dev_addr == 0)
        {
            driver_obj->dev_addr = event_msg->new_dev.address;
            // Open the device next
            driver_obj->actions |= ACTION_OPEN_DEV;
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (driver_obj->dev_hdl != NULL)
        {
            // Cancel any other actions and close the device next
            driver_obj->actions = ACTION_CLOSE_DEV;
        }
        break;
    default:
        // Should never occur
        abort();
    }
}

static void action_ccid_on(class_driver_t *driver_obj)
{
    usb_print_device_descriptor(dev_desc);
    usb_print_config_descriptor(config_desc, NULL);

    usb_transfer_t *ccid_on = NULL;
    typedef union
    {
        struct
        {
            uint8_t bmRequestType; 
            uint8_t bRequest;      
            uint8_t dIndex;       
            uint8_t dType;
            uint16_t dLang;       
            uint16_t wLength;      
        } __attribute__((packed));
        uint8_t val[USB_SETUP_PACKET_SIZE]; /**< Descriptor value */
    } usb_setup_custom_t;
    ESP_STATIC_ASSERT(sizeof(usb_setup_custom_t) == USB_SETUP_PACKET_SIZE, "Size of usb_setup_packet_t incorrect");

    usb_setup_custom_t data;
    data.bmRequestType = 0x80;
    data.bRequest = 0x06;
    data.dIndex = 0x01;
    data.dType = 0x03;
    data.dLang = 0x0309;
    data.wLength = 0xff00;

    usb_host_transfer_alloc(MAX(sizeof(usb_transfer_t), sizeof(usb_setup_custom_t)), 0, &ccid_on);
    memcpy(ccid_on->data_buffer, (const void*)&data, sizeof(data));
    ccid_on->num_bytes = sizeof(usb_setup_packet_t);
    ccid_on->device_handle = driver_obj->dev_hdl;
    ccid_on->bEndpointAddress = 0x81;
    ccid_on->callback = NULL;
    ccid_on->context = (void*)&driver_obj;
    
    esp_err_t err = usb_host_interface_claim(driver_obj->client_hdl, driver_obj->dev_hdl, 0, 0);
    if (err)
        printf("\nclaim fail");
    err = usb_host_transfer_submit(ccid_on);
    if(err)
        printf("\nsubmit fail");
    
    printf("\nccid on packet : ");
    for(uint8_t i = 0; i < sizeof(usb_setup_packet_t); i++)
        printf(" %i,", ccid_on->data_buffer[i]);
    printf("| num bytes : (%d)\n", ccid_on->num_bytes);        
    printf("t\nransfer status : %s",(ccid_on->status == USB_TRANSFER_STATUS_ERROR ? "error" : "ok"));
    
    // if(usb_host_interface_release(driver_obj->client_hdl, driver_obj->dev_hdl, 1) != ESP_OK)
    //     printf("release fail");
    // if(usb_host_endpoint_clear(driver_obj->dev_hdl,0x81))
    //     printf("flush ep fail");

}

static void action_open_dev(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_addr != 0);
    printf("\nOpening device at address %d", driver_obj->dev_addr);
    esp_err_t err = usb_host_device_open(driver_obj->client_hdl, driver_obj->dev_addr, &driver_obj->dev_hdl);
    if(err)
        printf("open fail"); 
    driver_obj->actions &= ~ACTION_OPEN_DEV;
    driver_obj->actions |= ACTION_GET_DEV_INFO;
}

static void action_get_info(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    printf("\nGetting device information \n");
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    printf("\n%s speed", (dev_info.speed == USB_SPEED_LOW) ? "Low" : "Full \n");
    printf("\nbConfigurationValue %d", dev_info.bConfigurationValue);
    driver_obj->actions &= ~ACTION_GET_DEV_INFO;
    driver_obj->actions |= ACTION_GET_DEV_DESC;
}

static void action_get_dev_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    printf("\nGetting device descriptor \n");
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(driver_obj->dev_hdl, &dev_desc));
    driver_obj->actions &= ~ACTION_GET_DEV_DESC;
    driver_obj->actions |= ACTION_GET_CONFIG_DESC;
}

static void action_get_config_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    printf("\nGetting config descriptor \n");
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));
    driver_obj->actions &= ~ACTION_GET_CONFIG_DESC;
    driver_obj->actions |= ACTION_GET_STR_DESC;
}

static void action_get_str_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    driver_obj->actions &= ~ACTION_GET_STR_DESC;
    driver_obj->actions |= ACTION_CCID_ON;
    printf("\nCCID : (%d)", driver_obj->actions);
}

static void action_close_dev(class_driver_t *driver_obj)
{
    printf("\nclose device \n");
    ESP_ERROR_CHECK(usb_host_device_close(driver_obj->client_hdl, driver_obj->dev_hdl));
    driver_obj->dev_hdl = NULL;
    driver_obj->dev_addr = 0;
    driver_obj->actions &= ~ACTION_CLOSE_DEV;
    driver_obj->actions |= ACTION_RECONNECT;
}

void class_driver_task(void *arg)
{
    class_driver_t driver_obj = {0};

    printf("\nRegistering Client \n");
    usb_host_client_config_t client_config = {
        .is_synchronous = false, // Synchronous clients currently not supported. Set this to false
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = (void *)&driver_obj,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &driver_obj.client_hdl));
    s_driver_obj = &driver_obj;

    while (1)
    {
        if (driver_obj.actions == 0)
        {
            usb_host_client_handle_events(driver_obj.client_hdl, portMAX_DELAY);
        }
        else
        {
            if (driver_obj.actions & ACTION_OPEN_DEV)
            {
                action_open_dev(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_DEV_INFO)
            {
                action_get_info(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_DEV_DESC)
            {
                action_get_dev_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_CONFIG_DESC)
            {
                action_get_config_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_STR_DESC)
            {
                action_get_str_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_CLOSE_DEV)
            {
                action_close_dev(&driver_obj);
            }
            if (driver_obj.actions == ACTION_CCID_ON)
            {
                action_ccid_on(&driver_obj);
            }
            if (driver_obj.actions & ACTION_EXIT)
            {
                break;
            }
            if (driver_obj.actions & ACTION_RECONNECT)
            {
                driver_obj.actions = 0;
            }
        }
    }

    printf("\nDeregistering Client \n");
    ESP_ERROR_CHECK(usb_host_client_deregister(driver_obj.client_hdl));
    vTaskSuspend(NULL);
}

void class_driver_client_deregister(void)
{
    if (s_driver_obj->dev_hdl != NULL)
    {
        s_driver_obj->actions = ACTION_CLOSE_DEV;
    }
    s_driver_obj->actions |= ACTION_EXIT;
    ESP_ERROR_CHECK(usb_host_client_unblock(s_driver_obj->client_hdl));
}