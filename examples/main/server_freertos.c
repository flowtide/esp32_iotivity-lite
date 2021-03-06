/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <pthread.h>
#include <stdio.h>

#define OC_SERVER 1

#include "/ws/esp32_iotivity-lite/components/iotivity/adapter/include/config.h"
#include "debug_print.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"

#include "oc_api.h"
#include "port/oc_clock.h"
#include "lightbulb.h"
#include "debug_print.h"

#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

static EventGroupHandle_t wifi_event_group;

static const int IPV4_CONNECTED_BIT = BIT0;
static const int IPV6_CONNECTED_BIT = BIT1;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static struct timespec ts;
static int quit = 0;
static bool light_state = false;

static const char *TAG = "ocf-svr";

#if 1
#define DBG(...) printf("server_freertos.c:%d ", __LINE__), printf(__VA_ARGS__), printf("\n")
#else
#define DBG(...)
#endif

static void
set_device_custom_property(void *data)
{
  (void)data;
  oc_set_custom_device_property(purpose, "desk lamp");
}

static int
app_init(void)
{
  DBG("app_init+");
  int ret = oc_init_platform("Intel", NULL, NULL);
  ret |= oc_add_device("/oic/d", "oic.d.light", "Kishen's light", "ocf.1.0.0",
                       "ocf.res.1.0.0", set_device_custom_property, NULL);
  DBG("app_init-");

  return ret;
}

static void
get_light(oc_request_t *request, oc_interface_mask_t interface, void *user_data)
{
  (void)user_data;
  PRINT("GET_light:\n");
  oc_rep_start_root_object();
  switch (interface) {
  case OC_IF_BASELINE:
    oc_process_baseline_interface(request->resource);
  /* fall through */
  case OC_IF_RW:
    oc_rep_set_boolean(root, state, light_state);
    break;
  default:
    break;
  }
  oc_rep_end_root_object();
  oc_send_response(request, OC_STATUS_OK);
  PRINT("Light state %d\n", light_state);
}

static void
post_light(oc_request_t *request, oc_interface_mask_t interface, void *user_data)
{
  (void)user_data;
  (void)interface;
  PRINT("POST_light:\n");
  bool state = false;
  oc_rep_t *rep = request->request_payload;
  while (rep != NULL) {
    PRINT("key: %s ", oc_string(rep->name));
    switch (rep->type) {
    case OC_REP_BOOL:
      state = rep->value.boolean;
      PRINT("value: %d\n", state);
      break;
    case OC_REP_INT:
        PRINT("value: %d\n", rep->value.integer);
        // process light info...
        notify_lightbulb_state(rep->value.integer, 0);
        vTaskDelay(50);     // keep light state ticks
        break;

    // case ...
    default:
      oc_send_response(request, OC_STATUS_BAD_REQUEST);
      return;
      break;
    }
    rep = rep->next;
  }
  oc_send_response(request, OC_STATUS_CHANGED);
  light_state = state;
}

static void
put_light(oc_request_t *request, oc_interface_mask_t interface,
           void *user_data)
{
  post_light(request, interface, user_data);
}

static void
register_resources(void)
{
  DBG("register_resources+");
  oc_resource_t *res = oc_new_resource("lightbulb", "/light/1", 1, 0);
  oc_resource_bind_resource_type(res, "oic.r.light");
  oc_resource_bind_resource_interface(res, OC_IF_RW);
  oc_resource_set_default_interface(res, OC_IF_RW);
  oc_resource_set_discoverable(res, true);
  oc_resource_set_periodic_observable(res, 1);
  oc_resource_set_request_handler(res, OC_GET, get_light, NULL);
  oc_resource_set_request_handler(res, OC_POST, post_light, NULL);
  oc_resource_set_request_handler(res, OC_PUT, put_light, NULL);
  oc_add_resource(res);
  DBG("register_resource-");
}

static void
signal_event_loop(void)
{
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mutex);
}

static void
handle_signal(int signal)
{
  (void)signal;
  DBG("handle_signal: signal=%d", signal);
  signal_event_loop();
  quit = 1;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, IPV4_CONNECTED_BIT);
        break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, IPV4_CONNECTED_BIT);
#ifndef OC_IPV4
        xEventGroupClearBits(wifi_event_group, IPV6_CONNECTED_BIT);
#endif
        break;

    case SYSTEM_EVENT_STA_CONNECTED:
#ifndef OC_IPV4
        tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
#endif
        break;

    case SYSTEM_EVENT_AP_STA_GOT_IP6:
        xEventGroupSetBits(wifi_event_group, IPV6_CONNECTED_BIT);
        break;

    default:
        break;
    }

    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void server_main(void* pvParameter)
{
  int init;
  tcpip_adapter_ip_info_t ip4_info = { 0 };
#ifndef OC_IPV4
  struct ip6_addr if_ipaddr_ip6 = { 0 };
#endif
  ESP_LOGI(TAG, "iotivity server task started");
  // wait to fetch IPv4 && ipv6 address
#ifdef OC_IPV4
  xEventGroupWaitBits(wifi_event_group, IPV4_CONNECTED_BIT, false, true, portMAX_DELAY);
#else
  xEventGroupWaitBits(wifi_event_group, IPV4_CONNECTED_BIT | IPV6_CONNECTED_BIT, false, true, portMAX_DELAY);
#endif

  if ( tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip4_info) != ESP_OK) {
      print_error("get IPv4 address failed");
  } else {
      ESP_LOGI(TAG, "got IPv4 addr:%s", ip4addr_ntoa(&(ip4_info.ip)));
  }

#ifndef OC_IPV4
  if ( tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr_ip6) != ESP_OK) {
      print_error("get IPv6 address failed");
  } else {
      ESP_LOGI(TAG, "got IPv6 addr:%s", ip6addr_ntoa(&if_ipaddr_ip6));
  }
#endif

  static const oc_handler_t handler = {.init = app_init,
                                       .signal_event_loop = signal_event_loop,
                                       .register_resources = register_resources };

  oc_clock_time_t next_event;

#ifdef OC_SECURITY
  oc_storage_config("./server_creds");
#endif /* OC_SECURITY */

  DBG("oc_main_init+");
  init = oc_main_init(&handler);
  DBG("oc_main_init: %d", init);
  if (init < 0) {
    print_error("oc_main_init() failed\n");
    return;
  }

  while (quit != 1) {
    printf("oc_main_poll+\n");
    next_event = oc_main_poll();
    printf("oc_main_poll=%lld\n", next_event);
    pthread_mutex_lock(&mutex);
    if (next_event == 0) {
#if 0
/* avoid following error:
task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog in time:
task_wdt:  - IDLE1 (CPU 1)
task_wdt: Tasks currently running:
task_wdt: CPU 0: IDLE0
task_wdt: CPU 1: pthread
*/
      ts.tv_sec = 1;
      ts.tv_nsec = 0;
      pthread_cond_timedwait(&cv, &mutex, &ts);
      printf("delay to avoid watchdog warning\n");
      vTaskDelay(100);
#else
      pthread_cond_wait(&cv, &mutex);
#endif
    } else {
      ts.tv_sec = (next_event / OC_CLOCK_SECOND);
      ts.tv_nsec = (next_event % OC_CLOCK_SECOND) * 1.e09 / OC_CLOCK_SECOND;
      pthread_cond_timedwait(&cv, &mutex, &ts);
    }
    pthread_mutex_unlock(&mutex);
  }

  oc_main_shutdown();
}


void app_main(void)
{
    if (nvs_flash_init() != ESP_OK){
        print_error("nvs_flash_init failed");
    }

    pthread_cond_init(&cv, NULL);

    print_macro_info();

    initialise_wifi();

#if 1
    if ( xTaskCreate(server_main, "server_main", 15*1024, NULL, 5, NULL) != pdPASS ) {
        print_error("task create failed");
    }
#endif
#if 0
    if ( xTaskCreate(lightbulb_damon_task, "lightbulb_damon_task", 8192, NULL, 5, NULL) != pdPASS ) {
        print_error("task create failed");
    }
#endif
}

