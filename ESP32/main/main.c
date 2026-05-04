#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

// 使用 SPI2 驱动 ILI9488 模块屏。
#define LCD_HOST SPI2_HOST

// 当前按横屏使用，逻辑分辨率为 480x320。
#define LCD_H_RES 480
#define LCD_V_RES 320

// LVGL 渲染缓冲行数。
#define LVGL_DRAW_BUF_LINES 64
// 底层 SPI 分块发送行数。ESP32-S3 SPI master 单事务 DMA 上限 = 2^18 bit = 32 KB；
// 480*16*3 = 23040 字节 < 32 KB，留出余量；配合 ping/pong 两块缓冲让 CPU 展开和 DMA 发送重叠。
#define LCD_IO_LINES 16

// 下面这一组是当前项目假定的 LCD 接线。
// 如果实物接线不同，先改这里再烧录。
#define PIN_NUM_SCLK 12
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO -1

#define PIN_NUM_LCD_CS 10
#define PIN_NUM_LCD_DC 9
#define PIN_NUM_LCD_RST 14
#define PIN_NUM_LCD_BKLT 21

#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_SPI_MODE 0
#define LCD_BKLT_ON_LEVEL 1

// 通过 MADCTL 控制屏幕方向与颜色顺序。
#define LCD_MIRROR_X 0
#define LCD_MIRROR_Y 1
#define LCD_SWAP_XY 1
#define LCD_BGR_MODE 1
#define LCD_INVERT_COLOR 0

#define ILI9488_CMD_SLPOUT 0x11
#define ILI9488_CMD_DISPON 0x29
#define ILI9488_CMD_CASET 0x2A
#define ILI9488_CMD_PASET 0x2B
#define ILI9488_CMD_RAMWR 0x2C
#define ILI9488_CMD_MADCTL 0x36
#define ILI9488_CMD_COLMOD 0x3A
#define ILI9488_CMD_INVON 0x21
#define ILI9488_CMD_INVOFF 0x20

#define ILI9488_MADCTL_MY 0x80
#define ILI9488_MADCTL_MX 0x40
#define ILI9488_MADCTL_MV 0x20
#define ILI9488_MADCTL_BGR 0x08

static const char *TAG = "ili9488_lvgl";

static spi_device_handle_t s_lcd_spi;
// ILI9488 按 18-bit SPI 模式写像素，每像素 3 字节。两块 ping/pong DMA 缓冲：CPU 展开下一块时 DMA 读上一块。
// DRAM_ATTR 强制放在内部 SRAM 上，确保 SPI DMA 可达。
static DRAM_ATTR uint8_t s_lcd_io_buf[2][LCD_H_RES * LCD_IO_LINES * 3];
static spi_transaction_t s_pixel_trans[2];
// LVGL 用 RGB565 渲染，减少内存占用；flush 时再转换成 ILI9488 需要的 18-bit。
static uint16_t s_lvgl_buf[LCD_H_RES * LVGL_DRAW_BUF_LINES];

static lv_obj_t *s_arc;
static lv_obj_t *s_bar_temp;
static lv_obj_t *s_bar_humid;
static lv_obj_t *s_temp_val;
static lv_obj_t *s_humid_val;
static lv_obj_t *s_press_val;
static lv_obj_t *s_load_pct;
static lv_obj_t *s_frame_label;

static void lcd_send_cmd(uint8_t cmd)
{
    spi_transaction_t trans = {
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
    };

    trans.tx_data[0] = cmd;
    gpio_set_level(PIN_NUM_LCD_DC, 0);
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd_spi, &trans));
}

static void lcd_send_data(const void *data, size_t len)
{
    spi_transaction_t trans;

    if(len == 0) {
        return;
    }

    memset(&trans, 0, sizeof(trans));
    trans.length = len * 8;
    trans.tx_buffer = data;

    gpio_set_level(PIN_NUM_LCD_DC, 1);
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd_spi, &trans));
}

static void lcd_panel_reset(void)
{
    // 硬复位时序按模块例程处理，给控制器足够时间完成上电复位。
    gpio_set_level(PIN_NUM_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_NUM_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_NUM_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void lcd_set_rotation(void)
{
    // MADCTL 用来设置镜像、横竖屏切换和 RGB/BGR 顺序。
    uint8_t madctl = 0;

    if(LCD_MIRROR_Y) {
        madctl |= ILI9488_MADCTL_MY;
    }
    if(LCD_MIRROR_X) {
        madctl |= ILI9488_MADCTL_MX;
    }
    if(LCD_SWAP_XY) {
        madctl |= ILI9488_MADCTL_MV;
    }
    if(LCD_BGR_MODE) {
        madctl |= ILI9488_MADCTL_BGR;
    }

    lcd_send_cmd(ILI9488_CMD_MADCTL);
    lcd_send_data(&madctl, 1);
}

static void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    // 先设列地址，再设页地址，最后进入 RAM 写入状态。
    uint8_t data[4];

    lcd_send_cmd(ILI9488_CMD_CASET);
    data[0] = (uint8_t)(x1 >> 8);
    data[1] = (uint8_t)(x1 & 0xFF);
    data[2] = (uint8_t)(x2 >> 8);
    data[3] = (uint8_t)(x2 & 0xFF);
    lcd_send_data(data, sizeof(data));

    lcd_send_cmd(ILI9488_CMD_PASET);
    data[0] = (uint8_t)(y1 >> 8);
    data[1] = (uint8_t)(y1 & 0xFF);
    data[2] = (uint8_t)(y2 >> 8);
    data[3] = (uint8_t)(y2 & 0xFF);
    lcd_send_data(data, sizeof(data));

    lcd_send_cmd(ILI9488_CMD_RAMWR);
}

static size_t lcd_expand_rgb565_to_666_block(uint8_t *dst, const uint16_t *src, size_t pixels)
{
    for(size_t i = 0; i < pixels; ++i) {
        uint16_t color = src[i];
        dst[(i * 3) + 0] = (uint8_t)(color >> 8) & 0xF8;
        dst[(i * 3) + 1] = (uint8_t)(color >> 3) & 0xFC;
        dst[(i * 3) + 2] = (uint8_t)(color << 3);
    }

    return pixels * 3;
}

static void lcd_draw_bitmap_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t *pixels)
{
    if(width == 0 || height == 0 || pixels == NULL) {
        return;
    }

    // 整段像素流期间锁住总线，避免每个事务都重新仲裁 / 重压器件配置。
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_lcd_spi, portMAX_DELAY));

    lcd_set_window(x, y, x + width - 1, y + height - 1);

    // RAMWR 之后所有像素事务都是数据，DC 在循环外置一次即可。
    gpio_set_level(PIN_NUM_LCD_DC, 1);

    int ping = 0;
    int in_flight = 0;

    while(height > 0) {
        uint16_t chunk_lines = height > LCD_IO_LINES ? LCD_IO_LINES : height;
        size_t chunk_pixels = (size_t)width * chunk_lines;
        // CPU 在这里写 buf[ping]；如果上一块还在 DMA，那就是真正的并行发生处。
        size_t bytes_to_send = lcd_expand_rgb565_to_666_block(s_lcd_io_buf[ping], pixels, chunk_pixels);

        spi_transaction_t *trans = &s_pixel_trans[ping];
        memset(trans, 0, sizeof(*trans));
        trans->length = bytes_to_send * 8;
        trans->tx_buffer = s_lcd_io_buf[ping];

        ESP_ERROR_CHECK(spi_device_queue_trans(s_lcd_spi, trans, portMAX_DELAY));
        in_flight++;

        ping ^= 1;
        pixels += chunk_pixels;
        height -= chunk_lines;

        // 维持最多两块在飞，第三块来之前先收掉最早的那块，腾出缓冲槽。
        if(in_flight >= 2) {
            spi_transaction_t *done;
            ESP_ERROR_CHECK(spi_device_get_trans_result(s_lcd_spi, &done, portMAX_DELAY));
            in_flight--;
        }
    }

    while(in_flight > 0) {
        spi_transaction_t *done;
        ESP_ERROR_CHECK(spi_device_get_trans_result(s_lcd_spi, &done, portMAX_DELAY));
        in_flight--;
    }

    spi_device_release_bus(s_lcd_spi);
}

static void lcd_fill_screen(uint16_t color)
{
    for(size_t i = 0; i < LCD_H_RES * LCD_IO_LINES; ++i) {
        s_lvgl_buf[i] = color;
    }

    for(uint16_t y = 0; y < LCD_V_RES; y += LCD_IO_LINES) {
        uint16_t lines = (LCD_V_RES - y > LCD_IO_LINES) ? LCD_IO_LINES : (LCD_V_RES - y);
        lcd_draw_bitmap_rgb565(0, y, LCD_H_RES, lines, s_lvgl_buf);
    }
}

static void lcd_init(void)
{
    lcd_panel_reset();

    // 以下初始化序列参考厂家提供的 ILI9488 裸例程。
    lcd_send_cmd(0xF7);
    lcd_send_data((uint8_t[]){0xA9, 0x51, 0x2C, 0x82}, 4);

    lcd_send_cmd(0xC0);
    lcd_send_data((uint8_t[]){0x11, 0x09}, 2);

    lcd_send_cmd(0xC1);
    lcd_send_data((uint8_t[]){0x41}, 1);

    lcd_send_cmd(0xC5);
    lcd_send_data((uint8_t[]){0x00, 0x0A, 0x80}, 3);

    lcd_send_cmd(0xB1);
    lcd_send_data((uint8_t[]){0xB0, 0x11}, 2);

    lcd_send_cmd(0xB4);
    lcd_send_data((uint8_t[]){0x02}, 1);

    lcd_send_cmd(0xB6);
    lcd_send_data((uint8_t[]){0x02, 0x22}, 2);

    lcd_send_cmd(0xB7);
    lcd_send_data((uint8_t[]){0xC6}, 1);

    lcd_send_cmd(0xBE);
    lcd_send_data((uint8_t[]){0x00, 0x04}, 2);

    lcd_send_cmd(0xE9);
    lcd_send_data((uint8_t[]){0x00}, 1);

    lcd_send_cmd(0xE0);
    lcd_send_data((uint8_t[]){0x00, 0x07, 0x10, 0x09, 0x17, 0x0B, 0x41, 0x89, 0x4B, 0x0A, 0x0C, 0x0E, 0x18, 0x1B, 0x0F}, 15);

    lcd_send_cmd(0xE1);
    lcd_send_data((uint8_t[]){0x00, 0x17, 0x1A, 0x04, 0x0E, 0x06, 0x2F, 0x45, 0x43, 0x02, 0x0A, 0x09, 0x32, 0x36, 0x0F}, 15);

    // 0x66 表示 18-bit/pixel，后续 RAMWR 需要每像素发送 3 字节。
    lcd_send_cmd(ILI9488_CMD_COLMOD);
    lcd_send_data((uint8_t[]){0x66}, 1);

    lcd_set_rotation();

    lcd_send_cmd(ILI9488_CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_send_cmd(LCD_INVERT_COLOR ? ILI9488_CMD_INVON : ILI9488_CMD_INVOFF);
    lcd_send_cmd(ILI9488_CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 最后再开背光，避免初始化过程中看到闪屏。
    gpio_set_level(PIN_NUM_LCD_BKLT, LCD_BKLT_ON_LEVEL);
}

static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t height = (uint16_t)(area->y2 - area->y1 + 1);

    // 临时计时：每秒最多打一行 flush 耗时统计，用来确认 SPI 流是否接近理论极限。
    // 验证完性能后可移除这段（连同 g_flush_*  static）。
    static int64_t g_flush_last_log_us = 0;
    static uint32_t g_flush_count = 0;
    static uint64_t g_flush_pixels = 0;
    static uint64_t g_flush_total_us = 0;
    int64_t t0 = esp_timer_get_time();

    lcd_draw_bitmap_rgb565((uint16_t)area->x1, (uint16_t)area->y1, width, height, (const uint16_t *)px_map);

    int64_t dt = esp_timer_get_time() - t0;
    g_flush_count++;
    g_flush_pixels += (uint64_t)width * height;
    g_flush_total_us += (uint64_t)dt;
    if(t0 - g_flush_last_log_us > 1000000) {
        ESP_LOGI(TAG, "flush: %lu calls, %llu px, %llu us total, last %ux%u %lld us",
                 (unsigned long)g_flush_count, g_flush_pixels, g_flush_total_us,
                 width, height, dt);
        g_flush_last_log_us = t0;
        g_flush_count = 0;
        g_flush_pixels = 0;
        g_flush_total_us = 0;
    }

    lv_display_flush_ready(display);
}

static void ui_stats_timer_cb(lv_timer_t *timer)
{
    static int32_t phase = 0;
    int32_t load = (phase * 7) % 101;
    int32_t temp = 18 + ((phase * 3) % 18);
    int32_t humid = 40 + ((phase * 5) % 50);
    int32_t press = 1008 + ((phase * 2) % 20);

    lv_arc_set_value(s_arc, load);
    lv_bar_set_value(s_bar_temp, temp, LV_ANIM_OFF);
    lv_bar_set_value(s_bar_humid, humid, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_load_pct, "%ld%%", (long)load);
    lv_label_set_text_fmt(s_temp_val, "%ld C", (long)temp);
    lv_label_set_text_fmt(s_humid_val, "%ld%%", (long)humid);
    lv_label_set_text_fmt(s_press_val, "%ld hPa", (long)press);
    lv_label_set_text_fmt(s_frame_label, "frame %ld", (long)phase);

    phase++;
}

static void ui_create_sidebar_card(lv_obj_t *parent, const char *title, lv_obj_t **val_lbl, lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 136, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141a2e), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_grow(card, 0);

    lv_obj_t *accent_line = lv_obj_create(card);
    lv_obj_set_size(accent_line, 28, 3);
    lv_obj_align(accent_line, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(accent_line, accent, 0);
    lv_obj_set_style_radius(accent_line, 2, 0);
    lv_obj_set_style_border_width(accent_line, 0, 0);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, title);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0x6a7fa8), 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 10);

    *val_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(*val_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(*val_lbl, lv_color_hex(0xf0f4ff), 0);
    lv_obj_align(*val_lbl, LV_ALIGN_TOP_LEFT, 0, 32);
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0f1a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* ── Header bar ───────────────────────────────────────── */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 480, 34);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111728), 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_set_style_pad_ver(header, 0, 0);

    lv_obj_t *hdr_title = lv_label_create(header);
    lv_label_set_text(hdr_title, "MimiClaw");
    lv_obj_set_style_text_font(hdr_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hdr_title, lv_color_hex(0xf0f4ff), 0);
    lv_obj_align(hdr_title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *hdr_pill = lv_obj_create(header);
    lv_obj_set_size(hdr_pill, 88, 22);
    lv_obj_align(hdr_pill, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr_pill, lv_color_hex(0x0d2818), 0);
    lv_obj_set_style_radius(hdr_pill, 11, 0);
    lv_obj_set_style_border_width(hdr_pill, 1, 0);
    lv_obj_set_style_border_color(hdr_pill, lv_color_hex(0x1a5c30), 0);
    lv_obj_set_style_pad_all(hdr_pill, 0, 0);

    lv_obj_t *dot = lv_obj_create(hdr_pill);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_border_width(dot, 0, 0);

    lv_obj_t *pill_txt = lv_label_create(hdr_pill);
    lv_label_set_text(pill_txt, "ONLINE");
    lv_obj_set_style_text_color(pill_txt, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_text_font(pill_txt, LV_FONT_DEFAULT, 0);
    lv_obj_align(pill_txt, LV_ALIGN_LEFT_MID, 20, 0);

    /* ── Left column: sidebar stat cards ──────────────────── */
    lv_obj_t *left_col = lv_obj_create(scr);
    lv_obj_set_size(left_col, 140, 252);
    lv_obj_align(left_col, LV_ALIGN_TOP_LEFT, 8, 40);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_set_style_pad_all(left_col, 0, 0);
    lv_obj_set_layout(left_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(left_col, 4, 0);

    ui_create_sidebar_card(left_col, "TEMPERATURE", &s_temp_val, lv_color_hex(0xff8c42));
    lv_label_set_text(s_temp_val, "24 C");

    ui_create_sidebar_card(left_col, "HUMIDITY", &s_humid_val, lv_color_hex(0x22d3ee));
    lv_label_set_text(s_humid_val, "52%");

    ui_create_sidebar_card(left_col, "PRESSURE", &s_press_val, lv_color_hex(0xfacc15));
    lv_label_set_text(s_press_val, "1013 hPa");

    /* ── Center column: gauge + info boxes ────────────────── */
    lv_obj_t *ctr_col = lv_obj_create(scr);
    lv_obj_set_size(ctr_col, 174, 252);
    lv_obj_align(ctr_col, LV_ALIGN_TOP_LEFT, 156, 40);
    lv_obj_set_style_bg_opa(ctr_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctr_col, 0, 0);
    lv_obj_set_style_pad_all(ctr_col, 0, 0);

    lv_obj_t *ctr_panel = lv_obj_create(ctr_col);
    lv_obj_set_size(ctr_panel, 174, 170);
    lv_obj_align(ctr_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(ctr_panel, lv_color_hex(0x141a2e), 0);
    lv_obj_set_style_radius(ctr_panel, 20, 0);
    lv_obj_set_style_border_width(ctr_panel, 0, 0);
    lv_obj_set_style_pad_all(ctr_panel, 12, 0);

    s_arc = lv_arc_create(ctr_panel);
    lv_obj_set_size(s_arc, 146, 146);
    lv_obj_align(s_arc, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x818cf8), LV_PART_INDICATOR);

    lv_obj_t *ctr_ttl = lv_label_create(ctr_panel);
    lv_label_set_text(ctr_ttl, "CPU");
    lv_obj_set_style_text_color(ctr_ttl, lv_color_hex(0x6a7fa8), 0);
    lv_obj_align(ctr_ttl, LV_ALIGN_TOP_LEFT, 46, 42);

    s_load_pct = lv_label_create(ctr_panel);
    lv_label_set_text(s_load_pct, "0%");
    lv_obj_set_style_text_font(s_load_pct, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_load_pct, lv_color_hex(0xf0f4ff), 0);
    lv_obj_align(s_load_pct, LV_ALIGN_TOP_LEFT, 38, 66);

    /* ── Bottom info boxes ────────────────────────────────── */
    lv_obj_t *box_panel = lv_obj_create(ctr_col);
    lv_obj_set_size(box_panel, 174, 76);
    lv_obj_align(box_panel, LV_ALIGN_TOP_LEFT, 0, 176);
    lv_obj_set_style_bg_color(box_panel, lv_color_hex(0x141a2e), 0);
    lv_obj_set_style_radius(box_panel, 16, 0);
    lv_obj_set_style_border_width(box_panel, 0, 0);
    lv_obj_set_style_pad_all(box_panel, 0, 0);

    lv_obj_t *bx1 = lv_obj_create(box_panel);
    lv_obj_set_size(bx1, 43, 60);
    lv_obj_align(bx1, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(bx1, lv_color_hex(0x1a2240), 0);
    lv_obj_set_style_radius(bx1, 10, 0);
    lv_obj_set_style_border_width(bx1, 0, 0);
    lv_obj_set_style_pad_all(bx1, 4, 0);

    lv_obj_t *bx1t = lv_label_create(bx1);
    lv_label_set_text(bx1t, "NET");
    lv_obj_set_style_text_color(bx1t, lv_color_hex(0x6a7fa8), 0);
    lv_obj_set_style_text_font(bx1t, LV_FONT_DEFAULT, 0);
    lv_obj_align(bx1t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bx1v = lv_label_create(bx1);
    lv_label_set_text(bx1v, "12\nMbps");
    lv_obj_set_style_text_color(bx1v, lv_color_hex(0x22d3ee), 0);
    lv_obj_align(bx1v, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *bx2 = lv_obj_create(box_panel);
    lv_obj_set_size(bx2, 43, 60);
    lv_obj_align(bx2, LV_ALIGN_TOP_LEFT, 57, 8);
    lv_obj_set_style_bg_color(bx2, lv_color_hex(0x1a2240), 0);
    lv_obj_set_style_radius(bx2, 10, 0);
    lv_obj_set_style_border_width(bx2, 0, 0);
    lv_obj_set_style_pad_all(bx2, 4, 0);

    lv_obj_t *bx2t = lv_label_create(bx2);
    lv_label_set_text(bx2t, "FREQ");
    lv_obj_set_style_text_color(bx2t, lv_color_hex(0x6a7fa8), 0);
    lv_obj_set_style_text_font(bx2t, LV_FONT_DEFAULT, 0);
    lv_obj_align(bx2t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bx2v = lv_label_create(bx2);
    lv_label_set_text(bx2v, "240\nMHz");
    lv_obj_set_style_text_color(bx2v, lv_color_hex(0xfacc15), 0);
    lv_obj_align(bx2v, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *bx3 = lv_obj_create(box_panel);
    lv_obj_set_size(bx3, 43, 60);
    lv_obj_align(bx3, LV_ALIGN_TOP_LEFT, 106, 8);
    lv_obj_set_style_bg_color(bx3, lv_color_hex(0x1a2240), 0);
    lv_obj_set_style_radius(bx3, 10, 0);
    lv_obj_set_style_border_width(bx3, 0, 0);
    lv_obj_set_style_pad_all(bx3, 4, 0);

    lv_obj_t *bx3t = lv_label_create(bx3);
    lv_label_set_text(bx3t, "MEM");
    lv_obj_set_style_text_color(bx3t, lv_color_hex(0x6a7fa8), 0);
    lv_obj_set_style_text_font(bx3t, LV_FONT_DEFAULT, 0);
    lv_obj_align(bx3t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bx3v = lv_label_create(bx3);
    lv_label_set_text(bx3v, "64\nKB");
    lv_obj_set_style_text_color(bx3v, lv_color_hex(0xa78bfa), 0);
    lv_obj_align(bx3v, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ── Right column: bars + info ────────────────────────── */
    lv_obj_t *right_col = lv_obj_create(scr);
    lv_obj_set_size(right_col, 140, 252);
    lv_obj_align(right_col, LV_ALIGN_TOP_LEFT, 336, 40);
    lv_obj_set_style_bg_opa(right_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_col, 0, 0);
    lv_obj_set_style_pad_all(right_col, 0, 0);

    lv_obj_t *bar_panel = lv_obj_create(right_col);
    lv_obj_set_size(bar_panel, 140, 130);
    lv_obj_align(bar_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar_panel, lv_color_hex(0x141a2e), 0);
    lv_obj_set_style_radius(bar_panel, 20, 0);
    lv_obj_set_style_border_width(bar_panel, 0, 0);
    lv_obj_set_style_pad_all(bar_panel, 14, 0);

    lv_obj_t *bar_title = lv_label_create(bar_panel);
    lv_label_set_text(bar_title, "LEVELS");
    lv_obj_set_style_text_color(bar_title, lv_color_hex(0x6a7fa8), 0);
    lv_obj_align(bar_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *tmp_lbl = lv_label_create(bar_panel);
    lv_label_set_text(tmp_lbl, "Temp");
    lv_obj_set_style_text_color(tmp_lbl, lv_color_hex(0x9ca3af), 0);
    lv_obj_align(tmp_lbl, LV_ALIGN_TOP_LEFT, 0, 28);

    s_bar_temp = lv_bar_create(bar_panel);
    lv_obj_set_size(s_bar_temp, 98, 8);
    lv_obj_align(s_bar_temp, LV_ALIGN_TOP_LEFT, 32, 32);
    lv_bar_set_range(s_bar_temp, 0, 50);
    lv_bar_set_value(s_bar_temp, 24, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bar_temp, 4, 0);
    lv_obj_set_style_bg_color(s_bar_temp, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_temp, lv_color_hex(0xff8c42), LV_PART_INDICATOR);

    lv_obj_t *hmd_lbl = lv_label_create(bar_panel);
    lv_label_set_text(hmd_lbl, "Humid");
    lv_obj_set_style_text_color(hmd_lbl, lv_color_hex(0x9ca3af), 0);
    lv_obj_align(hmd_lbl, LV_ALIGN_TOP_LEFT, 0, 58);

    s_bar_humid = lv_bar_create(bar_panel);
    lv_obj_set_size(s_bar_humid, 88, 8);
    lv_obj_align(s_bar_humid, LV_ALIGN_TOP_LEFT, 42, 62);
    lv_bar_set_range(s_bar_humid, 0, 100);
    lv_bar_set_value(s_bar_humid, 52, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bar_humid, 4, 0);
    lv_obj_set_style_bg_color(s_bar_humid, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_humid, lv_color_hex(0x22d3ee), LV_PART_INDICATOR);

    lv_obj_t *spi_panel = lv_obj_create(right_col);
    lv_obj_set_size(spi_panel, 140, 56);
    lv_obj_align(spi_panel, LV_ALIGN_TOP_LEFT, 0, 136);
    lv_obj_set_style_bg_color(spi_panel, lv_color_hex(0x141a2e), 0);
    lv_obj_set_style_radius(spi_panel, 16, 0);
    lv_obj_set_style_border_width(spi_panel, 0, 0);
    lv_obj_set_style_pad_all(spi_panel, 12, 0);

    lv_obj_t *spi_ttl = lv_label_create(spi_panel);
    lv_label_set_text(spi_ttl, "DISPLAY");
    lv_obj_set_style_text_color(spi_ttl, lv_color_hex(0x6a7fa8), 0);
    lv_obj_align(spi_ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *spi_val = lv_label_create(spi_panel);
    lv_label_set_text(spi_val, "ILI9488  480x320");
    lv_obj_set_style_text_color(spi_val, lv_color_hex(0x818cf8), 0);
    lv_obj_align(spi_val, LV_ALIGN_TOP_LEFT, 0, 18);

    lv_obj_t *up_panel = lv_obj_create(right_col);
    lv_obj_set_size(up_panel, 140, 56);
    lv_obj_align(up_panel, LV_ALIGN_TOP_LEFT, 0, 198);
    lv_obj_set_style_bg_color(up_panel, lv_color_hex(0x141a2e), 0);
    lv_obj_set_style_radius(up_panel, 16, 0);
    lv_obj_set_style_border_width(up_panel, 0, 0);
    lv_obj_set_style_pad_all(up_panel, 12, 0);

    lv_obj_t *up_ttl = lv_label_create(up_panel);
    lv_label_set_text(up_ttl, "UPTIME");
    lv_obj_set_style_text_color(up_ttl, lv_color_hex(0x6a7fa8), 0);
    lv_obj_align(up_ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *up_val = lv_label_create(up_panel);
    lv_label_set_text(up_val, "00:14:32");
    lv_obj_set_style_text_color(up_val, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_text_font(up_val, &lv_font_montserrat_18, 0);
    lv_obj_align(up_val, LV_ALIGN_TOP_LEFT, 0, 18);

    /* ── Footer ───────────────────────────────────────────── */
    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_size(footer, 480, 24);
    lv_obj_align(footer, LV_ALIGN_TOP_LEFT, 0, 298);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x111728), 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_hor(footer, 12, 0);
    lv_obj_set_style_pad_ver(footer, 0, 0);

    lv_obj_t *ft_left = lv_label_create(footer);
    lv_label_set_text(ft_left, "LVGL 9.5  |  ESP32-S3");
    lv_obj_set_style_text_color(ft_left, lv_color_hex(0x4a5568), 0);
    lv_obj_align(ft_left, LV_ALIGN_LEFT_MID, 0, 0);

    s_frame_label = lv_label_create(footer);
    lv_label_set_text(s_frame_label, "frame 0");
    lv_obj_set_style_text_color(s_frame_label, lv_color_hex(0x4a5568), 0);
    lv_obj_align(s_frame_label, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_timer_create(ui_stats_timer_cb, 900, NULL);
}

static void lvgl_init_display(void)
{
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_lvgl_buf, NULL, sizeof(s_lvgl_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, lvgl_flush_cb);
}

void app_main(void)
{
    gpio_config_t output_io = {
        .pin_bit_mask = (1ULL << PIN_NUM_LCD_DC) | (1ULL << PIN_NUM_LCD_RST) | (1ULL << PIN_NUM_LCD_BKLT),
        .mode = GPIO_MODE_OUTPUT,
    };
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_lcd_io_buf),
    };
    spi_device_interface_config_t lcd_devcfg = {
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .mode = LCD_SPI_MODE,
        .spics_io_num = PIN_NUM_LCD_CS,
        .queue_size = 2,
    };

    ESP_ERROR_CHECK(gpio_config(&output_io));
    gpio_set_level(PIN_NUM_LCD_BKLT, !LCD_BKLT_ON_LEVEL);

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &lcd_devcfg, &s_lcd_spi));

    ESP_LOGI(TAG, "init ILI9488 SPI panel");
    lcd_init();
    lcd_fill_screen(0x0000);

    ESP_LOGI(TAG, "init LVGL");
    lvgl_init_display();
    ui_create();

    while(true) {
        uint32_t wait_ms = lv_timer_handler();
        if(wait_ms == LV_NO_TIMER_READY || wait_ms > 20) {
            wait_ms = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}
