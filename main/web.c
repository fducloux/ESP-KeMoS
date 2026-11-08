// Wi-Fi setup page. The ESP32 opens its own access point "ESP-KVM-Setup";
// join it with a phone and open http://192.168.4.1 (login: admin / web password).
//
// Wi-Fi shares the radio with BLE, so the access point is not left on forever:
// it switches off ap_minutes after the last phone left. Press BOOT to switch it on again.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "kvm.h"

static const char *TAG = "web";

#define AP_SSID "ESP-KVM-Setup"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t srv;
static volatile bool ap_on;
static volatile bool stop_req;
static int64_t idle_since;     // last time a phone was connected (or the AP started), in us

// ---------------------------------------------------------------- login (HTTP Basic, user "admin")

static bool authorized(httpd_req_t *req)
{
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[100];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = 0;
            char want[48];
            snprintf(want, sizeof(want), "admin:%s", kvm_cfg.web_pass);
            // compare without bailing out at the first different byte
            size_t wl = strlen(want);
            unsigned diff = (n != wl);
            for (size_t i = 0; i < wl; i++) diff |= (i < n ? dec[i] : 0) ^ (unsigned char)want[i];
            if (!diff) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP KVM\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Login needed", HTTPD_RESP_USE_STRLEN);
    return false;
}

static esp_err_t bad(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------- handlers

static esp_err_t index_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    kvm_status_t s;
    kvm_get_status(&s);
    int left = -1;
    if (kvm_cfg.ap_minutes) {
        left = (int)((int64_t)kvm_cfg.ap_minutes * 60 - (esp_timer_get_time() - idle_since) / 1000000);
        if (left < 0) left = 0;
    }
    char b[1000];
    int n = snprintf(b, sizeof(b), "{\"active\":%d,\"learning\":%d,\"pairing\":%d,\"pair_left\":%d,\"passkey\":%ld,\"passkey_for\":\"%s\",\"hosts\":[",
                     s.active, s.learning, s.pairing, s.pair_left, (long)s.passkey, s.passkey_for);
    for (int i = 0; i < 2; i++)
        n += snprintf(b + n, sizeof(b) - n, "%s{\"known\":%d,\"connected\":%d,\"addr\":\"%s\"}",
                      i ? "," : "", s.host[i].known, s.host[i].connected, s.host[i].addr);
    n += snprintf(b + n, sizeof(b) - n, "],\"inputs\":[");
    for (int i = 0; i < 2; i++)
        n += snprintf(b + n, sizeof(b) - n, "%s{\"known\":%d,\"connected\":%d,\"ready\":%d,\"addr\":\"%s\"}",
                      i ? "," : "", s.input[i].known, s.input[i].connected, s.input[i].ready, s.input[i].addr);
    n += snprintf(b + n, sizeof(b) - n,
                  "],\"cfg\":{\"prefix\":\"%s\",\"apmin\":%d,\"modswap\":%d,\"swbtn\":%d},"
                  "\"default_web_pass\":%d,\"ap_left\":%d,\"heap\":%u,\"uptime\":%d}",
                  kvm_cfg.pair_prefix, kvm_cfg.ap_minutes, kvm_cfg.mod_swap, kvm_cfg.sw_btn,
                  strcmp(kvm_cfg.web_pass, "admin1234") == 0, left,
                  (unsigned)esp_get_free_heap_size(), (int)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, n);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));   // let the reply reach the phone first
    esp_restart();
}

static void restart_soon(void) { xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL); }

static esp_err_t action_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char q[64], name[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK)
        return bad(req, "name?");

    if (!strcmp(name, "switch0"))              kvm_switch(0);
    else if (!strcmp(name, "switch1"))         kvm_switch(1);
    else if (!strcmp(name, "pair_host0"))      { if (!kvm_pair(1)) return bad(req, "Already paired: forget it first"); }
    else if (!strcmp(name, "pair_host1"))      { if (!kvm_pair(2)) return bad(req, "Already paired: forget it first"); }
    else if (!strcmp(name, "pair_in0"))        { if (!kvm_pair(3)) return bad(req, "Already paired: forget it first"); }
    else if (!strcmp(name, "pair_in1"))        { if (!kvm_pair(4)) return bad(req, "Already paired: forget it first"); }
    else if (!strcmp(name, "pair_cancel"))     kvm_pair(0);
    else if (!strcmp(name, "learn_btn"))       kvm_learn_button(true);
    else if (!strcmp(name, "clear_btn"))       kvm_learn_button(false);
    else if (!strcmp(name, "swap_hosts"))      kvm_swap_hosts();
    else if (!strcmp(name, "forget_host0"))    kvm_forget_host(0);
    else if (!strcmp(name, "forget_host1"))    kvm_forget_host(1);
    else if (!strcmp(name, "forget_in0"))      kvm_forget_input(0);
    else if (!strcmp(name, "forget_in1"))      kvm_forget_input(1);
    else if (!strcmp(name, "wipe"))            { kvm_forget_all(); restart_soon(); }
    else if (!strcmp(name, "reset_settings"))  { kvm_cfg_reset(); restart_soon(); }
    else if (!strcmp(name, "reboot"))          restart_soon();
    else if (!strcmp(name, "ap_off"))          stop_req = true;
    else return bad(req, "unknown action");

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

static bool printable(const char *s, size_t min, size_t max)
{
    size_t n = strlen(s);
    if (n < min || n > max) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) return false;
    return true;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (req->content_len == 0 || req->content_len > 512) return bad(req, "bad body");
    char body[513];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body);
    if (!j) return bad(req, "bad json");

    kvm_cfg_t c = kvm_cfg;
    bool restart_ap = false;
    const cJSON *v;
    const char *err = NULL;

    if ((v = cJSON_GetObjectItem(j, "prefix")) && cJSON_IsString(v)) {
        if (!printable(v->valuestring, 0, sizeof(c.pair_prefix) - 1) ||
            strpbrk(v->valuestring, "\"\\")) err = "prefix: up to 15 plain characters";
        else strcpy(c.pair_prefix, v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(j, "modswap")) && cJSON_IsNumber(v)) c.mod_swap = v->valueint & 3;
    if ((v = cJSON_GetObjectItem(j, "apmin")) && cJSON_IsNumber(v)) {
        if (v->valueint < 0 || v->valueint > 240) err = "Wi-Fi timeout: 0 to 240 minutes";
        else c.ap_minutes = v->valueint;
    }
    // Passwords: empty or missing = leave unchanged
    if ((v = cJSON_GetObjectItem(j, "appass")) && cJSON_IsString(v) && v->valuestring[0]) {
        if (!printable(v->valuestring, 8, sizeof(c.ap_pass) - 1)) err = "Wi-Fi password: 8 to 31 characters";
        else { strcpy(c.ap_pass, v->valuestring); restart_ap = true; }
    }
    if ((v = cJSON_GetObjectItem(j, "webpass")) && cJSON_IsString(v) && v->valuestring[0]) {
        if (!printable(v->valuestring, 6, sizeof(c.web_pass) - 1) || strchr(v->valuestring, ':'))
            err = "Login password: 6 to 31 characters, no colon";
        else strcpy(c.web_pass, v->valuestring);
    }
    cJSON_Delete(j);
    if (err) return bad(req, err);

    kvm_cfg = c;
    kvm_cfg_save();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, restart_ap ? "ok-ap" : "ok", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------- "has internet" answers
// Phones test every Wi-Fi network with a small web request. If it fails they say "no internet"
// and drop the network when the screen sleeps. The board answers those tests (and every DNS
// lookup) itself, so the phone keeps the connection. Side effect: the phone has no real
// internet while joined to this network.

static esp_err_t probe_get(httpd_req_t *req)
{
    const char *u = req->uri;
    if (strstr(u, "generate_204") || strstr(u, "gen_204")) {          // Android
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/plain");
    if (strstr(u, "connecttest")) return httpd_resp_sendstr(req, "Microsoft Connect Test");   // Windows
    if (strstr(u, "ncsi"))        return httpd_resp_sendstr(req, "Microsoft NCSI");
    httpd_resp_set_type(req, "text/html");                              // Apple and others
    return httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

static volatile bool dns_running;

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    if (sock >= 0 && bind(sock, (struct sockaddr *)&a, sizeof(a)) == 0) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t buf[300];
        while (ap_on) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
            if (n < 12) continue;
            int i = 12;
            while (i < n && buf[i]) i += buf[i] + 1;                  // skip the question name
            if (i + 5 > n) continue;
            int qtype = (buf[i + 1] << 8) | buf[i + 2];
            int len = i + 5;                                           // end of the question
            buf[2] = 0x81; buf[3] = 0x80;                              // response, recursion available
            buf[4] = 0; buf[5] = 1;                                    // 1 question
            buf[6] = 0; buf[7] = (qtype == 1);                         // 1 answer for A queries only
            buf[8] = buf[9] = buf[10] = buf[11] = 0;
            if (qtype == 1) {
                static const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
                memcpy(buf + len, ans, sizeof(ans));
                len += sizeof(ans);
            }
            sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
        }
    }
    if (sock >= 0) close(sock);
    dns_running = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- access point

static void ap_start(void)
{
    idle_since = esp_timer_get_time();
    stop_req = false;
    if (ap_on) return;

    wifi_config_t wc = {0};
    strcpy((char *)wc.ap.ssid, AP_SSID);
    wc.ap.ssid_len = strlen(AP_SSID);
    strcpy((char *)wc.ap.password, kvm_cfg.ap_pass);
    wc.ap.channel = 1;
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (esp_wifi_start() != ESP_OK) { ESP_LOGE(TAG, "wifi start failed"); return; }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.max_open_sockets = 4;
    hc.max_uri_handlers = 16;
    hc.lru_purge_enable = true;
    if (httpd_start(&srv, &hc) == ESP_OK) {
        static const httpd_uri_t uris[] = {
            { .uri = "/",            .method = HTTP_GET,  .handler = index_get },
            { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
            { .uri = "/api/action",  .method = HTTP_POST, .handler = action_post },
            { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post },
            { .uri = "/generate_204",           .method = HTTP_GET, .handler = probe_get },
            { .uri = "/gen_204",                .method = HTTP_GET, .handler = probe_get },
            { .uri = "/hotspot-detect.html",    .method = HTTP_GET, .handler = probe_get },
            { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = probe_get },
            { .uri = "/connecttest.txt",        .method = HTTP_GET, .handler = probe_get },
            { .uri = "/ncsi.txt",               .method = HTTP_GET, .handler = probe_get },
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    }
    ap_on = true;
    if (!dns_running) {
        dns_running = true;
        xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    }
    ESP_LOGI(TAG, "Setup Wi-Fi on: join \"%s\", open http://192.168.4.1", AP_SSID);
}

static void ap_stop(void)
{
    if (!ap_on) return;
    if (srv) { httpd_stop(srv); srv = NULL; }
    esp_wifi_stop();
    ap_on = false;
    ESP_LOGI(TAG, "Setup Wi-Fi off (press BOOT to switch it on)");
}

static void ap_task(void *arg)
{
    bool was_pressed = false;
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !was_pressed) ap_start();
        was_pressed = pressed;

        if (++tick < 10) continue;
        tick = 0;
        if (!ap_on) continue;
        if (stop_req) { ap_stop(); continue; }
        wifi_sta_list_t l;
        if (esp_wifi_ap_get_sta_list(&l) == ESP_OK && l.num > 0) idle_since = esp_timer_get_time();
        else if (kvm_cfg.ap_minutes &&
                 esp_timer_get_time() - idle_since > (int64_t)kvm_cfg.ap_minutes * 60 * 1000000) ap_stop();
    }
}

void web_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    {   // hand out our own address as the DNS server, so lookups reach dns_task
        esp_netif_ip_info_t ip;
        esp_netif_dns_info_t dns = {0};
        uint8_t offer = 2;                                             // OFFER_DNS
        esp_netif_get_ip_info(ap, &ip);
        dns.ip.type = IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ip.ip.addr;
        esp_netif_dhcps_stop(ap);
        esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
        esp_netif_dhcps_start(ap);
    }
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ap_start();
    xTaskCreate(ap_task, "ap_task", 3072, NULL, 3, NULL);
}
