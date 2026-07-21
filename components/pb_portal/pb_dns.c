// SPDX-License-Identifier: MIT
// Captive-portal DNS responder (adapted from OpenVent pv_dns; generic).
#include "pb_dns.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <string.h>

static const char *TAG = "pb_dns";

#define DNS_PORT    53
#define DNS_MAX_LEN 512

static TaskHandle_t s_task = NULL;
static int          s_sock = -1;
static uint32_t     s_redirect_ip = 0;

typedef struct __attribute__((packed)) {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} dns_hdr_t;

static size_t qname_length(const uint8_t *buf, size_t buf_len)
{
    size_t i = 0;
    while (i < buf_len) {
        uint8_t l = buf[i];
        if (l == 0) return i + 1;
        if (l >= 64) return 0;
        i += l + 1;
    }
    return 0;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_MAX_LEN];
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DNS_PORT);

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "socket/bind failed");
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS on UDP :%d", DNS_PORT);

    for (;;) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < (int)sizeof(dns_hdr_t)) continue;

        dns_hdr_t *hdr = (dns_hdr_t *)buf;
        if ((ntohs(hdr->flags) & 0x8000) != 0) continue;   // ignore responses
        if (ntohs(hdr->qdcount) == 0) continue;

        size_t off = sizeof(dns_hdr_t);
        size_t qn = qname_length(buf + off, n - off);
        if (qn == 0 || off + qn + 4 > (size_t)n) continue;

        uint16_t qtype  = (buf[off + qn] << 8) | buf[off + qn + 1];
        uint16_t qclass = (buf[off + qn + 2] << 8) | buf[off + qn + 3];
        if (qtype != 1 || qclass != 1) continue;           // A / IN only

        hdr->flags = htons(0x8180);
        hdr->ancount = htons(1);
        hdr->nscount = 0;
        hdr->arcount = 0;

        size_t resp_len = off + qn + 4;
        uint8_t ans[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
            (uint8_t)(s_redirect_ip), (uint8_t)(s_redirect_ip >> 8),
            (uint8_t)(s_redirect_ip >> 16), (uint8_t)(s_redirect_ip >> 24),
        };
        if (resp_len + sizeof(ans) > sizeof(buf)) continue;
        memcpy(buf + resp_len, ans, sizeof(ans));
        resp_len += sizeof(ans);
        sendto(s_sock, buf, resp_len, 0, (struct sockaddr *)&src, src_len);
    }
}

esp_err_t pb_dns_start(uint32_t redirect_ip)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_redirect_ip = redirect_ip;
    return xTaskCreate(dns_task, "pb_dns", 4096, NULL, 4, &s_task) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void pb_dns_stop(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
}
