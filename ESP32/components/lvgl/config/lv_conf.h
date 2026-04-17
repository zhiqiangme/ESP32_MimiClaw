#ifndef LV_CONF_H
#define LV_CONF_H

/*
 * Minimal LVGL 9.5 configuration for the current ESP32-S3 + ILI9488 SPI setup.
 * Options not overridden here continue to use LVGL defaults.
 */

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

#define LV_MEM_SIZE (96U * 1024U)

/* Fonts used by the current UI. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1

#endif
