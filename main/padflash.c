// Flashes the pad (second ESP32) from the hub over a serial link, using the pad's ROM boot loader.
// The firmware to write sits in the hub's "padfw" partition (see tools/make_padfw.py).
// Wires (hub GPIO -> pad pin): 26 -> RXD (GPIO3), 27 <- TXD (GPIO1), 32 -> EN, 33 -> BOOT (GPIO0).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "kvm.h"
#include "driver/gpio.h"

#define FL_UART     2
#define FL_TX_GPIO  26
#define FL_RX_GPIO  27
#define FL_EN_GPIO  32
#define FL_IO0_GPIO 33
#define BLOCK       1024
#define FL_FAST_BAUD 0            // 0 = stay at 115200 (460800 over jumper wires gave garbled replies)

static const char *TAG = "padflash";
static volatile bool running;

static bool flash_pad(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "padfw");
    if (!part) { ESP_LOGE(TAG, "no padfw partition"); return false; }
    char magic[4];
    if (esp_partition_read(part, 0, magic, 4) != ESP_OK || memcmp(magic, "PFW1", 4)) {
        ESP_LOGE(TAG, "no pad firmware stored in the hub (run tools/make_padfw.py and write padfw.blob)");
        return false;
    }

    loader_esp32_config_t cfg = {
        .baud_rate = 115200, .uart_port = FL_UART, .uart_rx_pin = FL_RX_GPIO, .uart_tx_pin = FL_TX_GPIO,
        .reset_trigger_pin = FL_EN_GPIO, .gpio0_trigger_pin = FL_IO0_GPIO,
    };
    if (loader_port_esp32_init(&cfg) != ESP_LOADER_SUCCESS) { ESP_LOGE(TAG, "serial init failed"); return false; }

    bool ok = false;
    esp_loader_connect_args_t ca = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t e = esp_loader_connect(&ca);
    if (e != ESP_LOADER_SUCCESS) { ESP_LOGE(TAG, "cannot reach the pad boot loader (%d): check the 4 flash wires", e); goto out; }
    ESP_LOGI(TAG, "pad found (chip %d)", esp_loader_get_target());
    if (FL_FAST_BAUD && esp_loader_change_transmission_rate(FL_FAST_BAUD) == ESP_LOADER_SUCCESS)
        loader_port_change_transmission_rate(FL_FAST_BAUD);

    static uint8_t buf[BLOCK];
    size_t pos = 4;
    for (;;) {
        uint32_t hdr[2];
        if (esp_partition_read(part, pos, hdr, 8) != ESP_OK) goto out;
        pos += 8;
        uint32_t off = hdr[0], len = hdr[1];
        if (len == 0) break;
        ESP_LOGI(TAG, "writing %u bytes at 0x%x", (unsigned)len, (unsigned)off);
        e = esp_loader_flash_start(off, len, BLOCK);
        if (e != ESP_LOADER_SUCCESS) { ESP_LOGE(TAG, "flash start failed (%d)", e); goto out; }
        for (uint32_t done = 0; done < len;) {
            uint32_t n = len - done < BLOCK ? len - done : BLOCK;
            if (esp_partition_read(part, pos + done, buf, n) != ESP_OK) goto out;
            e = esp_loader_flash_write(buf, n);
            if (e != ESP_LOADER_SUCCESS) { ESP_LOGE(TAG, "flash write failed (%d) at 0x%x", e, (unsigned)(off + done)); goto out; }
            done += n;
        }
        e = esp_loader_flash_verify();                // MD5 of what the pad now holds (the ROM rejects FLASH_END)
        if (e != ESP_LOADER_SUCCESS) { ESP_LOGE(TAG, "verify failed (%d) at 0x%x", e, (unsigned)off); goto out; }
        ESP_LOGI(TAG, "0x%x verified", (unsigned)off);
        pos += len;
    }
    ok = true;
out:
    esp_loader_reset_target();                  // leave boot mode in every case: the pad runs again
    loader_port_esp32_deinit();
    return ok;
}

static void flash_task(void *arg)
{
    ESP_LOGW(TAG, "Flashing the pad from the hub: the Mac link drops for about 30 s");
    bool ok = flash_pad();
    if (ok) ESP_LOGW(TAG, "Pad flashed, it reboots now");
    else    ESP_LOGW(TAG, "Pad flash FAILED");
    running = false;
    vTaskDelete(NULL);
}

bool padflash_start(void)
{
    if (running) return false;
    running = true;
    if (xTaskCreate(flash_task, "padflash", 6144, NULL, 4, NULL) != pdPASS) { running = false; return false; }
    return true;
}


// Wire test (type T on the console): which hub pin reaches the pad's EN, which one its BOOT pin.
static void pin_out(int pin, int lvl) { gpio_reset_pin(pin); gpio_set_direction(pin, GPIO_MODE_OUTPUT); gpio_set_level(pin, lvl); }
static void pin_free(int pin) { gpio_reset_pin(pin); gpio_set_direction(pin, GPIO_MODE_INPUT); }
static void wait_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void diag_task(void *arg)
{
    ESP_LOGW(TAG, "DIAG start, pad alive=%d", kvm_pad_alive());
    for (int k = 0; k < 2; k++) {
        int pin = k ? FL_IO0_GPIO : FL_EN_GPIO;
        pin_out(pin, 0); wait_ms(5000);
        ESP_LOGW(TAG, "DIAG GPIO%d held low 5 s: pad alive=%d (0 means this pin reaches EN)", pin, kvm_pad_alive());
        pin_free(pin); wait_ms(5000);
        ESP_LOGW(TAG, "DIAG GPIO%d released 5 s: pad alive=%d", pin, kvm_pad_alive());
    }
    for (int k = 0; k < 2; k++) {
        int boot = k ? FL_EN_GPIO : FL_IO0_GPIO, en = k ? FL_IO0_GPIO : FL_EN_GPIO;
        pin_out(boot, 0); pin_out(en, 0); wait_ms(150); gpio_set_level(en, 1); wait_ms(300); pin_free(boot);
        wait_ms(5000);
        ESP_LOGW(TAG, "DIAG GPIO%d low while GPIO%d pulsed: pad alive=%d (0 means GPIO%d is BOOT, GPIO%d is EN)", boot, en, kvm_pad_alive(), boot, en);
        for (int j = 0; j < 2; j++) { int q = j ? FL_IO0_GPIO : FL_EN_GPIO; pin_out(q, 0); wait_ms(150); pin_free(q); wait_ms(1500); }
    }
    ESP_LOGW(TAG, "DIAG done, pad alive=%d", kvm_pad_alive());
    vTaskDelete(NULL);
}

// Wire probe (type P): a pin with the hub's pull-down reads 1 only if the pad's pull-up is attached to the wire.
static void probe_task(void *arg)
{
    static const struct { int pin; const char *what; } w[] = {
        {FL_EN_GPIO, "EN"}, {FL_IO0_GPIO, "BOOT/G0"}, {FL_RX_GPIO, "pad TXD"},
    };
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(w[i].pin); gpio_set_direction(w[i].pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(w[i].pin, GPIO_PULLDOWN_ONLY); wait_ms(100);
        int dn = gpio_get_level(w[i].pin);
        gpio_set_pull_mode(w[i].pin, GPIO_PULLUP_ONLY); wait_ms(100);
        int up = gpio_get_level(w[i].pin);
        gpio_set_pull_mode(w[i].pin, GPIO_FLOATING);
        ESP_LOGW(TAG, "PROBE GPIO%d (%s): pull-down reads %d, pull-up reads %d -> %s", w[i].pin, w[i].what, dn, up,
                 dn ? "wire reaches a pulled-up pad pin" : "NOT connected (or pad pin not pulled up)");
    }
    // Does the BOOT wire reach the pad's G0? The pad opens its pairing window when its BOOT pin is low.
    pin_out(FL_IO0_GPIO, 0); wait_ms(1500);
    int low = kvm_pad_pair_open();
    pin_free(FL_IO0_GPIO); wait_ms(1500);
    ESP_LOGW(TAG, "PROBE BOOT held low: pad pairing window open=%d (1 = the wire reaches G0), after release=%d", low, kvm_pad_pair_open());
    vTaskDelete(NULL);
}

// Typing F on the hub's USB serial console starts it too (for development).
static void console_task(void *arg)
{
    for (;;) {
        int c = fgetc(stdin);
        if (c == 'F') padflash_start();
        else if (c == 'P') xTaskCreate(probe_task, "padflash_probe", 3072, NULL, 3, NULL);
        else if (c == 'T') xTaskCreate(diag_task, "padflash_diag", 4096, NULL, 3, NULL);
        else if (c == EOF) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void padflash_init(void)
{
    xTaskCreate(console_task, "padflash_con", 3072, NULL, 1, NULL);
}
