#include "net/dns_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "dns";
static TaskHandle_t s_task = NULL;
static int s_sock = -1;
static volatile bool s_run = false;

#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;
#pragma pack(pop)

static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed (need port 53 free)");
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS captive started on :53");

    uint8_t rx[512];
    uint8_t tx[512];

    while (s_run) {
        struct sockaddr_in from = {0};
        socklen_t flen = sizeof(from);
        int r = recvfrom(s_sock, rx, sizeof(rx), 0, (struct sockaddr*)&from, &flen);
        if (r <= (int)sizeof(dns_hdr_t)) continue;

        dns_hdr_t *h = (dns_hdr_t*)rx;

        // Only handle standard queries
        // Build response: copy query, set flags, set ancount=1
        memset(tx, 0, sizeof(tx));
        memcpy(tx, rx, r);
        dns_hdr_t *rh = (dns_hdr_t*)tx;

        rh->flags = htons(0x8180); // standard query response, NoError
        rh->qdcount = htons(1);
        rh->ancount = htons(1);
        rh->nscount = 0;
        rh->arcount = 0;

        // Find end of question (QNAME ... 0x00 + QTYPE(2) + QCLASS(2))
        int idx = sizeof(dns_hdr_t);
        while (idx < r && tx[idx] != 0) idx++;
        if (idx + 5 >= r) continue;
        idx += 1 + 4; // zero + qtype + qclass

        // Answer section:
        // Name pointer to QNAME: 0xC00C
        tx[idx++] = 0xC0; tx[idx++] = 0x0C;
        // Type A, Class IN
        tx[idx++] = 0x00; tx[idx++] = 0x01;
        tx[idx++] = 0x00; tx[idx++] = 0x01;
        // TTL 60s
        tx[idx++] = 0x00; tx[idx++] = 0x00; tx[idx++] = 0x00; tx[idx++] = 0x3C;
        // RDLENGTH 4
        tx[idx++] = 0x00; tx[idx++] = 0x04;
        // RDATA = 192.168.4.1
        tx[idx++] = 192; tx[idx++] = 168; tx[idx++] = 4; tx[idx++] = 1;

        sendto(s_sock, tx, idx, 0, (struct sockaddr*)&from, flen);
    }

    ESP_LOGI(TAG, "DNS captive stopped");
    close(s_sock);
    s_sock = -1;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void)
{
    if (s_task) return ESP_OK;
    s_run = true;
    xTaskCreate(dns_task, "dns_srv", 4096, NULL, 5, &s_task);
    return ESP_OK;
}

void dns_server_stop(void)
{
    s_run = false;
    s_task = NULL;
}
