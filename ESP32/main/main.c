#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
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

// 底层 SPI 分块发送行数。适中取值兼顾刷新速度和稳定性。
#define LCD_IO_LINES 20
// LVGL 渲染缓冲行数。
#define LVGL_DRAW_BUF_LINES 32

// 下面这一组是当前项目假定的 LCD 接线。
// 如果实物接线不同，先改这里再烧录。
#define PIN_NUM_SCLK 12
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO -1

#define PIN_NUM_LCD_CS 10
#define PIN_NUM_LCD_DC 9
#define PIN_NUM_LCD_RST 14
#define PIN_NUM_LCD_BKLT 21

#define LCD_SPI_CLOCK_HZ (26 * 1000 * 1000)
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
// ILI9488 按 18-bit SPI 模式写像素，因此底层发送缓冲按 3 字节/像素预留。
static uint8_t s_lcd_io_buf[LCD_H_RES * LCD_IO_LINES * 3];
// LVGL 用 RGB565 渲染，减少内存占用；flush 时再转换成 ILI9488 需要的 18-bit。
static uint16_t s_lvgl_buf[LCD_H_RES * LVGL_DRAW_BUF_LINES];

static lv_obj_t *s_status_value_label;
static lv_obj_t *s_temp_value_label;
static lv_obj_t *s_arc;
static lv_obj_t *s_bar;

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

static size_t lcd_expand_rgb565_to_666_block(const uint16_t *src, size_t pixels)
{
    for(size_t i = 0; i < pixels; ++i) {
        uint16_t color = src[i];
        s_lcd_io_buf[(i * 3) + 0] = (uint8_t)(color >> 8) & 0xF8;
        s_lcd_io_buf[(i * 3) + 1] = (uint8_t)(color >> 3) & 0xFC;
        s_lcd_io_buf[(i * 3) + 2] = (uint8_t)(color << 3);
    }

    return pixels * 3;
}

static void lcd_draw_bitmap_rgb565(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t *pixels)
{
    if(width == 0 || height == 0 || pixels == NULL) {
        return;
    }

    lcd_set_window(x, y, x + width - 1, y + height - 1);

    while(height > 0) {
        uint16_t chunk_lines = height > LCD_IO_LINES ? LCD_IO_LINES : height;
        size_t chunk_pixels = (size_t)width * chunk_lines;
        size_t bytes_to_send = lcd_expand_rgb565_to_666_block(pixels, chunk_pixels);
        lcd_send_data(s_lcd_io_buf, bytes_to_send);

        pixels += chunk_pixels;
        height -= chunk_lines;
    }
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

    lcd_draw_bitmap_rgb565((uint16_t)area->x1, (uint16_t)area->y1, width, height, (const uint16_t *)px_map);
    lv_display_flush_ready(display);
}

static void ui_action_btn_event_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_user_data(e);

    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_label_set_text(label, "Sync queued");
    }
}

static void ui_stats_timer_cb(lv_timer_t *timer)
{
    static int32_t phase = 0;
    lv_obj_t *label = lv_timer_get_user_data(timer);
    int32_t load = (phase * 7) % 101;
    int32_t temp = 24 + ((phase * 3) % 12);

    lv_arc_set_value(s_arc, load);
    lv_bar_set_value(s_bar, temp, LV_ANIM_ON);
    lv_label_set_text_fmt(s_status_value_label, "%ld%%", (long)load);
    lv_label_set_text_fmt(s_temp_value_label, "%ld C", (long)temp);
    lv_label_set_text_fmt(label, "Render loop stable  |  frame %ld", (long)phase);

    phase++;
}

static lv_obj_t *ui_create_metric_card(lv_obj_t *parent, const char *title, lv_obj_t **value_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 146, 92);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1d2436), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x8da0c7), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    *value_label = lv_label_create(card);
    lv_obj_set_style_text_font(*value_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(*value_label, lv_color_hex(0xf5f7ff), 0);
    lv_obj_align(*value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return card;
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0f1c), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x16213a), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 448, 54);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "MimiClaw Display");
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf3f6ff), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, -8);

    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "ESP32-S3  |  LVGL 9.5  |  ILI9488 SPI");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8ea2cc), 0);
    lv_obj_align(subtitle, LV_ALIGN_LEFT_MID, 2, 14);

    lv_obj_t *status_pill = lv_obj_create(header);
    lv_obj_set_size(status_pill, 110, 34);
    lv_obj_align(status_pill, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(status_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_pill, lv_color_hex(0x193c2c), 0);
    lv_obj_set_style_border_width(status_pill, 0, 0);
    lv_obj_set_style_pad_all(status_pill, 0, 0);

    lv_obj_t *status_text = lv_label_create(status_pill);
    lv_label_set_text(status_text, "DISPLAY OK");
    lv_obj_set_style_text_color(status_text, lv_color_hex(0x85f0b1), 0);
    lv_obj_center(status_text);

    lv_obj_t *left_card = lv_obj_create(scr);
    lv_obj_set_size(left_card, 448, 118);
    lv_obj_align(left_card, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_radius(left_card, 26, 0);
    lv_obj_set_style_bg_color(left_card, lv_color_hex(0x11192b), 0);
    lv_obj_set_style_border_width(left_card, 0, 0);
    lv_obj_set_style_pad_all(left_card, 14, 0);

    lv_obj_t *metrics_row = lv_obj_create(left_card);
    lv_obj_set_size(metrics_row, 420, 92);
    lv_obj_center(metrics_row);
    lv_obj_set_style_bg_opa(metrics_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metrics_row, 0, 0);
    lv_obj_set_style_pad_all(metrics_row, 0, 0);
    lv_obj_set_layout(metrics_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(metrics_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_create_metric_card(metrics_row, "GPU Load", &s_status_value_label);
    lv_label_set_text(s_status_value_label, "0%");

    ui_create_metric_card(metrics_row, "Panel Temp", &s_temp_value_label);
    lv_label_set_text(s_temp_value_label, "24 C");

    lv_obj_t *mini_card = lv_obj_create(metrics_row);
    lv_obj_set_size(mini_card, 98, 92);
    lv_obj_set_style_radius(mini_card, 20, 0);
    lv_obj_set_style_bg_color(mini_card, lv_color_hex(0x26314d), 0);
    lv_obj_set_style_border_width(mini_card, 0, 0);

    lv_obj_t *mini_label = lv_label_create(mini_card);
    lv_label_set_text(mini_label, "SPI\n26 MHz");
    lv_obj_set_style_text_align(mini_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(mini_label, lv_color_hex(0xdce6ff), 0);
    lv_obj_center(mini_label);

    lv_obj_t *main_grid = lv_obj_create(scr);
    lv_obj_set_size(main_grid, 448, 112);
    lv_obj_align(main_grid, LV_ALIGN_TOP_MID, 0, 198);
    lv_obj_set_style_bg_opa(main_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_grid, 0, 0);
    lv_obj_set_style_pad_all(main_grid, 0, 0);

    static int32_t col_dsc[] = {220, 212, LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[] = {112, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(main_grid, col_dsc, row_dsc);

    lv_obj_t *gauge_panel = lv_obj_create(main_grid);
    lv_obj_set_grid_cell(gauge_panel, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_radius(gauge_panel, 26, 0);
    lv_obj_set_style_bg_color(gauge_panel, lv_color_hex(0x11192b), 0);
    lv_obj_set_style_border_width(gauge_panel, 0, 0);

    s_arc = lv_arc_create(gauge_panel);
    lv_obj_set_size(s_arc, 126, 126);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2b3754), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x4ee3b8), LV_PART_INDICATOR);

    lv_obj_t *arc_text = lv_label_create(gauge_panel);
    lv_label_set_text(arc_text, "Render\nLoad");
    lv_obj_set_style_text_align(arc_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(arc_text, lv_color_hex(0xcae0ff), 0);
    lv_obj_center(arc_text);

    lv_obj_t *info_panel = lv_obj_create(main_grid);
    lv_obj_set_grid_cell(info_panel, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_radius(info_panel, 26, 0);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0x11192b), 0);
    lv_obj_set_style_border_width(info_panel, 0, 0);
    lv_obj_set_style_pad_all(info_panel, 16, 0);

    lv_obj_t *info_title = lv_label_create(info_panel);
    lv_label_set_text(info_title, "System Preview");
    lv_obj_set_style_text_font(info_title, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(info_title, lv_color_hex(0xf6f8ff), 0);
    lv_obj_align(info_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *info_sub = lv_label_create(info_panel);
    lv_label_set_text(info_sub, "LVGL partial refresh over SPI");
    lv_obj_set_style_text_color(info_sub, lv_color_hex(0x8ea2cc), 0);
    lv_obj_align(info_sub, LV_ALIGN_TOP_LEFT, 0, 28);

    s_bar = lv_bar_create(info_panel);
    lv_obj_set_size(s_bar, 176, 12);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_bar_set_range(s_bar, 20, 40);
    lv_bar_set_value(s_bar, 24, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x26314d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xff8c42), LV_PART_INDICATOR);

    lv_obj_t *info_note = lv_label_create(info_panel);
    lv_label_set_text(info_note, "Touch is disabled for now.\nThis screen verifies LVGL + panel flush.");
    lv_obj_set_style_text_color(info_note, lv_color_hex(0xc8d5f2), 0);
    lv_obj_align(info_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_size(footer, 448, 44);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);

    lv_obj_t *action_btn = lv_button_create(footer);
    lv_obj_set_size(action_btn, 118, 40);
    lv_obj_align(action_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(action_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(action_btn, lv_color_hex(0x3d68ff), 0);
    lv_obj_set_style_border_width(action_btn, 0, 0);

    lv_obj_t *action_label = lv_label_create(action_btn);
    lv_label_set_text(action_label, "Trigger Sync");
    lv_obj_center(action_label);

    lv_obj_t *footer_status = lv_label_create(footer);
    lv_label_set_text(footer_status, "Render loop stable  |  frame 0");
    lv_obj_set_style_text_color(footer_status, lv_color_hex(0x90a3c8), 0);
    lv_obj_align(footer_status, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(action_btn, ui_action_btn_event_cb, LV_EVENT_CLICKED, footer_status);
    lv_timer_create(ui_stats_timer_cb, 900, footer_status);
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
