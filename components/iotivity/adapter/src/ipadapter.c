/*
// Copyright (c) 2018 Intel Corporation
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

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <freertos/FreeRTOS.h>
#include "freertos/semphr.h"
#include "lwip/mld6.h"
#include "lwip/sockets.h"
#include "esp_system.h"
#include <assert.h>
#include "oc_buffer.h"
#include "oc_endpoint.h"
#include "port/oc_assert.h"
#include "port/oc_connectivity.h"
#include "debug_print.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include <lwip/netdb.h>
#include "esp_log.h"
#include "tcpip_adapter.h"

static const char* TAG = "ipadapter";

// {{ ESP32 OCF PORTING
#define SUPPORT_IPV6 0
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
// }}

// most of function declaration is under iotivity-constrained/port/oc_connectivity.h and oc_network_events_mutex.h
#ifndef IFA_MULTICAST
#define IFA_MULTICAST 7
#endif

/* Some outdated toolchains do not define IFA_FLAGS.
   Note: Requires Linux kernel 3.14 or later. */
#ifndef IFA_FLAGS
#define IFA_FLAGS (IFA_MULTICAST+1)
#endif

#define OCF_PORT_UNSECURED (5683)
static const uint8_t ALL_OCF_NODES_LL[] = {
  0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x58
};
static const uint8_t ALL_OCF_NODES_RL[] = {
  0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x58
};
static const uint8_t ALL_OCF_NODES_SL[] = {
  0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x58
};
#define ALL_COAP_NODES_V4 0xe00001bb

static pthread_mutex_t mutex;

typedef struct ip_context_t {
  struct ip_context_t *next;
  struct sockaddr_storage mcast;
  struct sockaddr_storage server;
  int mcast_sock;
  int server_sock;
  uint16_t port;
#ifdef OC_SECURITY
  struct sockaddr_storage secure;
  int secure_sock;
  uint16_t dtls_port;
#endif /* OC_SECURITY */
#ifdef OC_IPV4
  struct sockaddr_storage mcast4;
  struct sockaddr_storage server4;
  int mcast4_sock;
  int server4_sock;
  uint16_t port4;
#ifdef OC_SECURITY
  struct sockaddr_storage secure4;
  int secure4_sock;
  uint16_t dtls4_port;
#endif /* OC_SECURITY */
#endif /* OC_IPV4 */
  pthread_t event_thread;
  int terminate;
  int device;
} ip_context_t;

#ifdef OC_DYNAMIC_ALLOCATION
OC_LIST(ip_contexts);
#else /* OC_DYNAMIC_ALLOCATION */
static ip_context_t devices[OC_MAX_NUM_DEVICES];
#endif /* !OC_DYNAMIC_ALLOCATION */

void
oc_network_event_handler_mutex_init(void)
{
  if (pthread_mutex_init(&mutex, NULL) != 0) {
    oc_abort("error initializing network event handler mutex\n");
  }
}

void
oc_network_event_handler_mutex_lock(void)
{
  pthread_mutex_lock(&mutex);
}

void
oc_network_event_handler_mutex_unlock(void)
{
  pthread_mutex_unlock(&mutex);
}

void oc_network_event_handler_mutex_destroy(void) {
  pthread_mutex_destroy(&mutex);
}

static ip_context_t *get_ip_context_for_device(int device) {
#ifdef OC_DYNAMIC_ALLOCATION
  ip_context_t *dev = oc_list_head(ip_contexts);
  while (dev != NULL && dev->device != device) {
    dev = dev->next;
  }
  if (!dev) {
    return NULL;
  }
#else  /* OC_DYNAMIC_ALLOCATION */
  ip_context_t *dev = &devices[device];
#endif /* !OC_DYNAMIC_ALLOCATION */
  return dev;
}

#ifdef OC_IPV4
static int add_mcast_sock_to_ipv4_mcast_group(int mcast_sock,
                                              const struct in_addr *local,
                                              int interface_index) {
    struct ip_mreq imreq = { 0 };
    int err = 0;
    // Configure source interface
    memset(&imreq, 0, sizeof(struct ip_mreq));
    tcpip_adapter_ip_info_t ip_info = { 0 };
    err = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
    if (err != ESP_OK) {
        print_error("get ip4 ret:%d\n", err);
    }

    inet_addr_from_ip4addr(&imreq.imr_interface, &ip_info.ip);
    imreq.imr_multiaddr.s_addr = htonl(ALL_COAP_NODES_V4);
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));

    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        print_error("not a valid multicast address");
    }

    err = setsockopt(mcast_sock, IPPROTO_IP, IP_MULTICAST_IF, &imreq.imr_interface, sizeof(struct in_addr));
    if (err < 0) {
        print_error("setsockopt IP_MULTICAST_IF ret:%d", err);
    }

#ifdef OC_LEAVE_GROUP
    err = setsockopt(mcast_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        print_error("setsockopt IP_DROP_MEMBERSHIP ret:%d", err);
    }
#endif

    err = setsockopt(mcast_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,  &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        print_error("setsockopt IP_ADD_MEMBERSHIP ret:%d", err);
    }
  return 0;
}
#endif /* OC_IPV4 */

#if SUPPORT_IPV6
static int add_mcast_sock_to_ipv6_mcast_group(int mcast_sock, int interface_index)
{
    int err = 0;
    struct ip6_mreq v6imreq = { 0 };
    struct ip6_addr if_ipaddr = { 0 };
    err = tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr);
    if (err != ESP_OK) {
        print_error("got ip6 addr ret:%d\n", err);
    }
    /* Link-local scope */
    memset(&v6imreq, 0, sizeof(struct ip6_mreq));
    // interface
     inet6_addr_from_ip6addr(&v6imreq.ipv6mr_interface, &if_ipaddr);
     // copy ipv6
     memcpy(v6imreq.ipv6mr_multiaddr.s6_addr, ALL_OCF_NODES_LL, 16);
     err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &v6imreq.ipv6mr_interface, sizeof(struct in6_addr));
     if (err < 0) {
         print_error("setsockopt IPV6_MULTICAST_IF ret:%d\n", err);
     }

#ifdef OC_LEAVE_GROUP
    err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        print_error("set IPV6_DROP_MEMBERSHIP ret:%d\n",err);
    }
#endif
    err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        print_error("set IPV6_ADD_MEMBERSHIP ret:%d\n",err);
    }

    /* Realm-local scope */
    memset(&v6imreq, 0, sizeof(struct ip6_mreq));
    inet6_addr_from_ip6addr(&v6imreq.ipv6mr_interface, &if_ipaddr);
    memcpy(v6imreq.ipv6mr_multiaddr.s6_addr, ALL_OCF_NODES_RL, 16);

#ifdef OC_LEAVE_GROUP
    err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        print_error("set IPV6_DROP_MEMBERSHIP ret:%d\n",err);
    }
#endif
    err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        print_error("set IPV6_ADD_MEMBERSHIP ret:%d\n",err);
    }

      /* Site-local scope */
    memset(&v6imreq, 0, sizeof(struct ip6_mreq));
    inet6_addr_from_ip6addr(&v6imreq.ipv6mr_interface, &if_ipaddr);
    memcpy(v6imreq.ipv6mr_multiaddr.s6_addr, ALL_OCF_NODES_SL, 16);

#ifdef OC_LEAVE_GROUP
    err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        print_error("set IPV6_DROP_MEMBERSHIP ret:%d\n",err);
    }
#endif

    err = setsockopt(mcast_sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        print_error("set IPV6_ADD_MEMBERSHIP ret:%d\n",err);
    }

  return 0;
}
#endif

static int configure_mcast_socket(int mcast_sock, int sa_family) {
    int ret = 0;

#if SUPPORT_IPV6
    /* Accordingly handle IPv6/IPv4 addresses */
    if (sa_family == AF_INET6) {
        ret += add_mcast_sock_to_ipv6_mcast_group(mcast_sock, NULL);
    }
#endif

#ifdef OC_IPV4
    if (sa_family == AF_INET) {
      ret += add_mcast_sock_to_ipv4_mcast_group(mcast_sock, NULL,
                                                0);
    }
#endif /* OC_IPV4 */

  return ret;
}

static void *network_event_thread(void *data) {
  struct sockaddr_storage client;
  memset(&client, 0, sizeof(struct sockaddr_storage));
  struct sockaddr_in6 *c = (struct sockaddr_in6 *)&client;
  socklen_t len = sizeof(client);

#ifdef OC_IPV4
  struct sockaddr_in *c4 = (struct sockaddr_in *)&client;
#endif

  ip_context_t *dev = (ip_context_t *)data;

  fd_set rfds, setfds;
  FD_ZERO(&rfds);
  FD_SET(dev->server_sock, &rfds);
  FD_SET(dev->mcast_sock, &rfds);
#ifdef OC_SECURITY
  FD_SET(dev->secure_sock, &rfds);
#endif /* OC_SECURITY */

#ifdef OC_IPV4
  FD_SET(dev->server4_sock, &rfds);
  FD_SET(dev->mcast4_sock, &rfds);
#ifdef OC_SECURITY
  FD_SET(dev->secure4_sock, &rfds);
#endif /* OC_SECURITY */
#endif /* OC_IPV4 */

  int i, n;

  while (dev->terminate != 1) {
    len = sizeof(client);
    setfds = rfds;
#ifdef OC_IPV4
    int maxfd = (dev->server4_sock > dev->mcast4_sock) ? dev->server4_sock : dev->mcast4_sock;
#else
    int maxfd = (dev->server_sock > dev->mcast_sock) ? dev->server_sock : dev->mcast_sock;
#endif
    n = select(maxfd + 1, &setfds, NULL, NULL, NULL);
    for (i = 0; i < n; i++) {
      len = sizeof(client);
      oc_message_t *message = oc_allocate_message();

      if (!message) {
        break;
      }

      if (FD_ISSET(dev->server_sock, &setfds)) {
        int count = recvfrom(dev->server_sock, message->data, OC_PDU_SIZE, 0,
                             (struct sockaddr *)&client, &len);
        if (count < 0) {
          oc_message_unref(message);
          continue;
        }
        message->length = count;
        message->endpoint.flags = IPV6;
        message->endpoint.device = dev->device;
        FD_CLR(dev->server_sock, &setfds);
        goto common;
      }

      if (FD_ISSET(dev->mcast_sock, &setfds)) {
        int count = recvfrom(dev->mcast_sock, message->data, OC_PDU_SIZE, 0,
                             (struct sockaddr *)&client, &len);
        if (count < 0) {
          oc_message_unref(message);
          continue;
        }
        message->length = count;
        message->endpoint.flags = IPV6 | MULTICAST;
        message->endpoint.device = dev->device;
        FD_CLR(dev->mcast_sock, &setfds);
        goto common;
      }

#ifdef OC_IPV4
      if (FD_ISSET(dev->server4_sock, &setfds)) {
        int count = recvfrom(dev->server4_sock, message->data, OC_PDU_SIZE, 0,
                             (struct sockaddr *)&client, &len);
        if (count < 0) {
          oc_message_unref(message);
          continue;
        }
        message->length = count;
        message->endpoint.flags = IPV4;
        message->endpoint.device = dev->device;
        FD_CLR(dev->server4_sock, &setfds);
        goto common;
      }

      if (FD_ISSET(dev->mcast4_sock, &setfds)) {
        int count = recvfrom(dev->mcast4_sock, message->data, OC_PDU_SIZE, 0,
                             (struct sockaddr *)&client, &len);
        if (count < 0) {
          oc_message_unref(message);
          continue;
        }
        message->length = count;
        message->endpoint.flags = IPV4 | MULTICAST;
        message->endpoint.device = dev->device;
        FD_CLR(dev->mcast4_sock, &setfds);
        goto common;
      }
#endif /* OC_IPV4 */

#ifdef OC_SECURITY
      if (FD_ISSET(dev->secure_sock, &setfds)) {
        int count = recvfrom(dev->secure_sock, message->data, OC_PDU_SIZE, 0,
                             (struct sockaddr *)&client, &len);
        if (count < 0) {
          oc_message_unref(message);
          continue;
        }
        message->length = count;
        message->endpoint.flags = IPV6 | SECURED;
        message->endpoint.device = dev->device;
        FD_CLR(dev->secure_sock, &setfds);
      }
#ifdef OC_IPV4
      if (FD_ISSET(dev->secure4_sock, &setfds)) {
        int count = recvfrom(dev->secure4_sock, message->data, OC_PDU_SIZE, 0,
                             (struct sockaddr *)&client, &len);
        if (count < 0) {
          oc_message_unref(message);
          continue;
        }
        message->length = count;
        message->endpoint.flags = IPV4 | SECURED;
        message->endpoint.device = dev->device;
        FD_CLR(dev->secure4_sock, &setfds);
      }
#endif /* OC_IPV4 */
#endif /* OC_SECURITY */
    common:
#ifdef OC_IPV4
      if (message->endpoint.flags & IPV4) {
        memcpy(message->endpoint.addr.ipv4.address, &c4->sin_addr.s_addr,
               sizeof(c4->sin_addr.s_addr));
        message->endpoint.addr.ipv4.port = ntohs(c4->sin_port);
      } else if (message->endpoint.flags & IPV6) {
#else  /* OC_IPV4 */
      if (message->endpoint.flags & IPV6) {
#endif /* !OC_IPV4 */
        memcpy(message->endpoint.addr.ipv6.address, c->sin6_addr.s6_addr,
               sizeof(c->sin6_addr.s6_addr));
        message->endpoint.addr.ipv6.scope = IPADDR_ANY;
        message->endpoint.addr.ipv6.port = ntohs(c->sin6_port);
      }

#ifdef OC_DEBUG
      PRINT("Incoming message of size %d bytes from ", message->length);
      PRINTipaddr(message->endpoint);
      PRINT("\n\n");
#endif /* OC_DEBUG */

      oc_network_event(message);
    }
  }
  vTaskDelete(NULL);
  return NULL;
}

oc_endpoint_t *
oc_connectivity_get_endpoints(int device)
{
    (void)device;
    oc_init_endpoint_list();
    oc_endpoint_t ep;
    memset(&ep, 0, sizeof(oc_endpoint_t));
    int err = 0;
#ifdef OC_IPV4
    ep.flags = IPV4;
    ep.addr.ipv4.port = OCF_PORT_UNSECURED;
    tcpip_adapter_ip_info_t sta_ip;
    err = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &sta_ip);
    if (err != ESP_OK) {
        print_error("get ipv4 failed,ret:%d\n", err);
    }
    memcpy(ep.addr.ipv4.address, &sta_ip.ip, 4);
#else   // IPv6
    ep.flags = IPV6;
    ep.addr.ipv6.port = OCF_PORT_UNSECURED;
    struct ip6_addr if_ipaddr = { 0 };
    err = tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr);
    if (err != ESP_OK) {
        print_error("get ipv6 failed,ret:%d\n", err);
    }
    memcpy(ep.addr.ipv6.address, if_ipaddr.addr, 16);
#endif
    ep.device = 0;
    oc_add_endpoint_to_list(&ep);
    return oc_get_endpoint_list();
}

void oc_send_buffer(oc_message_t *message) {
#ifdef OC_DEBUG
  PRINT("Outgoing message of size %d bytes to ", message->length);
  PRINTipaddr(message->endpoint);
  PRINT("\n\n");
#endif /* OC_DEBUG */

  struct sockaddr_storage receiver;
  memset(&receiver, 0, sizeof(struct sockaddr_storage));
#ifdef OC_IPV4
  if (message->endpoint.flags & IPV4) {
    struct sockaddr_in *r = (struct sockaddr_in *)&receiver;
    memcpy(&r->sin_addr.s_addr, message->endpoint.addr.ipv4.address,
           sizeof(r->sin_addr.s_addr));
    r->sin_family = AF_INET;
    r->sin_port = htons(message->endpoint.addr.ipv4.port);
  } else {
#else
  {
#endif
    struct sockaddr_in6 *r = (struct sockaddr_in6 *)&receiver;
    memcpy(r->sin6_addr.s6_addr, message->endpoint.addr.ipv6.address,
           sizeof(r->sin6_addr.s6_addr));
    r->sin6_family = AF_INET6;
    r->sin6_port = htons(message->endpoint.addr.ipv6.port);
    r->sin6_scope_id = IPADDR_ANY;
  }
  int send_sock = -1;

  ip_context_t *dev = get_ip_context_for_device(message->endpoint.device);
#ifdef OC_SECURITY
  if (message->endpoint.flags & SECURED) {
#ifdef OC_IPV4
    if (message->endpoint.flags & IPV4) {
      send_sock = dev->secure4_sock;
    } else {
      send_sock = dev->secure_sock;
    }
#else  /* OC_IPV4 */
    send_sock = dev->secure_sock;
#endif /* !OC_IPV4 */
  } else
#endif /* OC_SECURITY */
#ifdef OC_IPV4
    if (message->endpoint.flags & IPV4) {
    send_sock = dev->server4_sock;
  } else {
    send_sock = dev->server_sock;
  }
#else  /* OC_IPV4 */
  {
    send_sock = dev->server_sock;
  }
#endif /* !OC_IPV4 */

  int bytes_sent = 0, x;
  while (bytes_sent < (int)message->length) {
    x = sendto(send_sock, message->data + bytes_sent,
        message->length - bytes_sent, 0, (struct sockaddr *)&receiver,
        sizeof(receiver));
    if (x < 0) {
      OC_WRN("sendto() returned errno %d\n", errno);
      return;
    }
    bytes_sent += x;
  }
  OC_DBG("Sent %d bytes\n", bytes_sent);
}

void oc_send_discovery_request(oc_message_t *message)
{
  ip_context_t *dev = get_ip_context_for_device(message->endpoint.device);
  struct in6_addr if_inaddr = { 0 };
  struct ip6_addr if_ipaddr = { 0 };
  int err = 0;
#ifndef OC_IPV4
    err = tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr);
    inet6_addr_from_ip6addr(&if_inaddr, &if_ipaddr);
    if (err != ESP_OK) {
        print_error("tcpip_adapter_get_ip6_linklocal ret:%d\n", err);
    }
    // Assign the multicast source interface, via its IP
    err = setsockopt(dev->server_sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &if_inaddr, sizeof(struct in6_addr));
    if (err < 0) {
        print_error("set opt ret:%d\n", err);
    }
    oc_send_buffer(message);
#else
  tcpip_adapter_ip_info_t ip_info = { 0 };
  struct in_addr iaddr = { 0 };
  err = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
  if (err != ESP_OK) {
      print_error("get ip ret:%d\n", err);
  }
  inet_addr_from_ip4addr(&iaddr, &ip_info.ip);

  // Assign the IPv4 multicast source interface, via its IP
  // (only necessary if this socket is IPV4 only)
  err = setsockopt(dev->server4_sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr, sizeof(struct in_addr));
  if (err < 0) {
      print_error("set opt ret:%d\n", err);
  }
  oc_send_buffer(message);
#endif
}

#ifdef OC_IPV4
static int
connectivity_ipv4_init(ip_context_t *dev)
{
  OC_DBG("Initializing IPv4 connectivity for device %d\n", dev->device);
  memset(&dev->mcast4, 0, sizeof(struct sockaddr_storage));
  memset(&dev->server4, 0, sizeof(struct sockaddr_storage));

  struct sockaddr_in *m = (struct sockaddr_in *)&dev->mcast4;
  m->sin_family = AF_INET;
  m->sin_port = htons(OCF_PORT_UNSECURED);
  m->sin_addr.s_addr = INADDR_ANY;

  struct sockaddr_in *l = (struct sockaddr_in *)&dev->server4;
  l->sin_family = AF_INET;
//  l->sin_addr.s_addr = INADDR_ANY;
  int err = 0;
  tcpip_adapter_ip_info_t ip_info = { 0 };
  err = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
  if (err != ESP_OK) {
      print_error("get ip4 ret:%d\n", err);
  }
  inet_addr_from_ip4addr(&l->sin_addr, &ip_info.ip);
  l->sin_port = 0;

#ifdef OC_SECURITY
  memset(&dev->secure4, 0, sizeof(struct sockaddr_storage));
  struct sockaddr_in *sm = (struct sockaddr_in *)&dev->secure4;
  sm->sin_family = AF_INET;
  sm->sin_port = 0;
  sm->sin_addr.s_addr = INADDR_ANY;

  dev->secure4_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (dev->secure4_sock < 0) {
    OC_ERR("creating secure IPv4 socket\n");
    return -1;
  }
#endif /* OC_SECURITY */

  dev->server4_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  dev->mcast4_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (dev->server4_sock < 0 || dev->mcast4_sock < 0) {
    OC_ERR("creating IPv4 server sockets\n");
    return -1;
  }

  if (bind(dev->server4_sock, (struct sockaddr *)&dev->server4,
           sizeof(dev->server4)) == -1) {
    OC_ERR("binding server4 socket %d\n", errno);
    return -1;
  }

  socklen_t socklen = sizeof(dev->server4);
  if (getsockname(dev->server4_sock, (struct sockaddr *)&dev->server4,
                  &socklen) == -1) {
    OC_ERR("obtaining server4 socket information %d\n", errno);
    return -1;
  }

  dev->port4 = ntohs(l->sin_port);

  if (configure_mcast_socket(dev->mcast4_sock, AF_INET) < 0) {
    return -1;
  }

  int reuse = 1;
  if (setsockopt(dev->mcast4_sock, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) == -1) {
    OC_ERR("setting reuseaddr IPv4 option %d\n", errno);
    return -1;
  }
  if (bind(dev->mcast4_sock, (struct sockaddr *)&dev->mcast4,
           sizeof(dev->mcast4)) == -1) {
    OC_ERR("binding mcast IPv4 socket %d\n", errno);
    return -1;
  }

#ifdef OC_SECURITY
  if (setsockopt(dev->secure4_sock, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) == -1) {
    OC_ERR("setting reuseaddr IPv4 option %d\n", errno);
    return -1;
  }

  if (bind(dev->secure4_sock, (struct sockaddr *)&dev->secure4,
           sizeof(dev->secure4)) == -1) {
    OC_ERR("binding IPv4 secure socket %d\n", errno);
    return -1;
  }

  socklen = sizeof(dev->secure4);
  if (getsockname(dev->secure4_sock, (struct sockaddr *)&dev->secure4,
                  &socklen) == -1) {
    OC_ERR("obtaining DTLS4 socket information %d\n", errno);
    return -1;
  }

  dev->dtls4_port = ntohs(sm->sin_port);
#endif /* OC_SECURITY */

  OC_DBG("Successfully initialized IPv4 connectivity for device %d\n",
         dev->device);

  return 0;
}
#endif

int oc_connectivity_init(int device) {
  OC_DBG("Initializing connectivity for device %d\n", device);
#ifdef OC_DYNAMIC_ALLOCATION
  ip_context_t *dev = (ip_context_t *)calloc(1, sizeof(ip_context_t));
  if (!dev) {
    oc_abort("Insufficient memory");
  }
  oc_list_add(ip_contexts, dev);
#else  /* OC_DYNAMIC_ALLOCATION */
  ip_context_t *dev = &devices[device];
#endif /* !OC_DYNAMIC_ALLOCATION */
  dev->device = device;
#ifndef OC_IPV4
  memset(&dev->mcast, 0, sizeof(struct sockaddr_storage));
  memset(&dev->server, 0, sizeof(struct sockaddr_storage));

  struct sockaddr_in6 *m = (struct sockaddr_in6 *)&dev->mcast;
  m->sin6_family = AF_INET6;
  m->sin6_port = htons(OCF_PORT_UNSECURED);
  m->sin6_addr = in6addr_any;
  int err = 0;
  struct ip6_addr if_ipaddr = { 0 };

  struct sockaddr_in6 *l = (struct sockaddr_in6 *)&dev->server;
  l->sin6_family = AF_INET6;

  err = tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr);
  if (err != ESP_OK) {
      print_error("get ip6 ret:%d\n", err);
  }
  inet6_addr_from_ip6addr(&l->sin6_addr, &if_ipaddr);

  l->sin6_port = 0;

#ifdef OC_SECURITY
  memset(&dev->secure, 0, sizeof(struct sockaddr_storage));
  struct sockaddr_in6 *sm = (struct sockaddr_in6 *)&dev->secure;
  sm->sin6_family = AF_INET6;
  sm->sin6_port = 0;
  sm->sin6_addr = in6addr_any;
#endif /* OC_SECURITY */

  dev->server_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  dev->mcast_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

  if (dev->server_sock < 0 || dev->mcast_sock < 0) {
    OC_ERR("creating server sockets\n");
    return -1;
  }

#ifdef OC_SECURITY
  dev->secure_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (dev->secure_sock < 0) {
    OC_ERR("creating secure socket\n");
    return -1;
  }
#endif /* OC_SECURITY */

  int opt = 1;
  if (setsockopt(dev->server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt,
                 sizeof(opt)) == -1) {
    OC_ERR("setting sock option %d\n", errno);
    return -1;
  }

  if (bind(dev->server_sock, (struct sockaddr *)&dev->server,
           sizeof(dev->server)) == -1) {
    OC_ERR("binding server socket %d\n", errno);
    return -1;
  }

  socklen_t socklen = sizeof(dev->server);
  if (getsockname(dev->server_sock, (struct sockaddr *)&dev->server,
                  &socklen) == -1) {
    OC_ERR("obtaining server socket information %d\n", errno);
    return -1;
  }

  dev->port = ntohs(l->sin6_port);

  if (configure_mcast_socket(dev->mcast_sock, AF_INET6) < 0) {
    return -1;
  }

  int reuse = 1;
  if (setsockopt(dev->mcast_sock, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) == -1) {
    OC_ERR("setting reuseaddr option %d\n", errno);
    return -1;
  }
  if (bind(dev->mcast_sock, (struct sockaddr *)&dev->mcast,
           sizeof(dev->mcast)) == -1) {
    OC_ERR("binding mcast socket %d\n", errno);
    return -1;
  }

#ifdef OC_SECURITY
  if (setsockopt(dev->secure_sock, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) == -1) {
    OC_ERR("setting reuseaddr option %d\n", errno);
    return -1;
  }
  if (bind(dev->secure_sock, (struct sockaddr *)&dev->secure,
           sizeof(dev->secure)) == -1) {
    OC_ERR("binding IPv6 secure socket %d\n", errno);
    return -1;
  }

  socklen = sizeof(dev->secure);
  if (getsockname(dev->secure_sock, (struct sockaddr *)&dev->secure,
                  &socklen) == -1) {
    OC_ERR("obtaining secure socket information %d\n", errno);
    return -1;
  }

  dev->dtls_port = ntohs(sm->sin6_port);
#endif /* OC_SECURITY */
#endif

#ifdef OC_IPV4
  if (connectivity_ipv4_init(dev) != 0) {
    OC_ERR("Could not initialize IPv4\n");
  }
#endif /* OC_IPV4 */

  if (pthread_create(&dev->event_thread, NULL, &network_event_thread, dev) !=
      0) {
    OC_ERR("creating network polling thread\n");
    return -1;
  }

  OC_DBG("Successfully initialized connectivity for device %d\n", device);

  return 0;
}

void
oc_connectivity_shutdown(int device)
{
  ip_context_t *dev = get_ip_context_for_device(device);
  dev->terminate = 1;

  close(dev->server_sock);
  close(dev->mcast_sock);

#ifdef OC_IPV4
  close(dev->server4_sock);
  close(dev->mcast4_sock);
#endif /* OC_IPV4 */

#ifdef OC_SECURITY
  close(dev->secure_sock);
#ifdef OC_IPV4
  close(dev->secure4_sock);
#endif /* OC_IPV4 */
#endif /* OC_SECURITY */

  pthread_cancel(dev->event_thread);
  pthread_join(dev->event_thread, NULL);

#ifdef OC_DYNAMIC_ALLOCATION
  oc_list_remove(ip_contexts, dev);
  free(dev);
#endif /* OC_DYNAMIC_ALLOCATION */

  OC_DBG("oc_connectivity_shutdown for device %d\n", device);
}
