/* 
 * The MIT License (MIT)
 * 
 * Copyright (c) 2016 Johan Kanflo (github.com/kanflo)
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <espressif/esp_common.h>
#include <esp/uart.h> 
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <esp8266.h>
#include <esp/spi.h>
#include <ssid_config.h>
#include <Adafruit_GFX.hpp>
#include <Adafruit_ILI9341.hpp>
#include <SPI.hpp>

extern "C" {
  #include <ota-tftp.h>
  #include <cli.h>
  #include "c64.h"
}

#define TFT_CS 4
#define TFT_DC 2
#define TFT_LED 0

#define ADC_BUTTON_PRESSED (1000) // adc > ADC_BUTTON_PRESSED when button pressed

SemaphoreHandle_t gTFTSemaphore;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);

#define delay_ms(ms) vTaskDelay(ms / portTICK_PERIOD_MS)

extern "C" {

// Convert a 24-bit RGB color to an equivalent 16-bit RGB565 value
static __inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
//    return ((r / 8) << 11) | ((g / 4) << 5) | (b / 8);
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static __inline uint16_t read16(uint8_t *bmp, uint32_t *pos)
{
    uint16_t result;
    ((uint8_t *)&result)[0] = bmp[(*pos)++];  // LSB
    ((uint8_t *)&result)[1] = bmp[(*pos)++];  // MSB
    return result;
}

static __inline uint32_t read32(uint8_t *bmp, uint32_t *pos)
{
    uint32_t result;
    ((uint8_t *)&result)[0] = bmp[(*pos)++];  // LSB
    ((uint8_t *)&result)[1] = bmp[(*pos)++];
    ((uint8_t *)&result)[2] = bmp[(*pos)++];
    ((uint8_t *)&result)[3] = bmp[(*pos)++];  // MSB
    return result;
}

static void bmp_draw(uint8_t *bmp, uint16_t x, uint16_t y)
{
    uint32_t bmp_pos = 0;
    int      bmp_width, bmp_height;   // W+H in pixels
    uint8_t  bmp_depth;               // Bit depth (currently must be 24)
    uint32_t img_offset;              // Start of image data in file
    uint32_t row_size;                // Not always = bmp_width; may have padding
    boolean  good_bmp = false;        // Set to true on valid header parse
    boolean  flip    = true;          // BMP is stored bottom-to-top
    int      w, h, row, col;
    uint8_t  r, g, b;
    uint32_t pos = 0;
    uint32_t img_size;

    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    if((x >= tft.width()) || (y >= tft.height())) return;


    // Parse BMP header
    if(read16(bmp, &bmp_pos) == 0x4D42) { // BMP signature
        img_size = read32(bmp, &bmp_pos);
        (void) read32(bmp, &bmp_pos); // Read & ignore creator bytes
        img_offset = read32(bmp, &bmp_pos); // Start of image data
        // Read DIB header
        (void) read32(bmp, &bmp_pos); // Header size
        bmp_width  = read32(bmp, &bmp_pos);
        bmp_height = read32(bmp, &bmp_pos);
        if(read16(bmp, &bmp_pos) == 1) { // # planes -- must be '1'
            bmp_depth = read16(bmp, &bmp_pos); // bits per pixel
            if((bmp_depth == 24) && (read32(bmp, &bmp_pos) == 0)) { // 0 = uncompressed
                good_bmp = true; // Supported BMP format -- proceed!
                printf("Image dimensions: %d x %d  %d bit  %d bytes\n", bmp_width, bmp_height, bmp_depth, img_size);
                // BMP rows are padded (if needed) to 4-byte boundary
                row_size = (bmp_width * 3 + 3) & ~3;

                // If bmp_height is negative, image is in top-down order.
                // This is not canon but has been observed in the wild.
                if(bmp_height < 0) {
                    bmp_height = -bmp_height;
                    flip = false;
                }

                // Crop area to be loaded
                w = bmp_width;
                h = bmp_height;
                if((x+w-1) >= tft.width())  w = tft.width()  - x;
                if((y+h-1) >= tft.height()) h = tft.height() - y;

                // Set TFT address window to clipped image bounds
                tft.setAddrWindow(x, y, x+w-1, y+h-1);

                gpio_write(TFT_DC, true); // @todo: should be done in setAddrWindow imho
                gpio_write(TFT_CS, false);
                for (row=0; row<h; row++) { // For each scanline...

                    // Seek to start of scan line.  It might seem labor-
                    // intensive to be doing this on every line, but this
                    // method covers a lot of gritty details like cropping
                    // and scanline padding.  Also, the seek only takes
                    // place if the file position actually needs to change
                    // (avoids a lot of cluster math in SD library).
                    if(flip) // Bitmap is stored bottom-to-top order (normal BMP)
                        pos = img_offset + (bmp_height - 1 - row) * row_size;
                    else     // Bitmap is stored top-to-bottom
                        pos = img_offset + row * row_size;
                    for (col=0; col<w; col++) { // For each pixel...
                        // Convert pixel from BMP to TFT format, push to display
                        b = bmp[pos++];
                        g = bmp[pos++];
                        r = bmp[pos++];
                        uint16_t temp = ((r / 8) << 11) | ((g / 4) << 5) | (b / 8);
                        temp = temp >> 8 | temp << 8;
                        (void) spi_transfer_16(1, temp); // Faster than tft.pushColor(rgb565(r,g,b));
                    }
                }
                gpio_write(TFT_CS, true);
            }
        }
    }

    if(!good_bmp) {
        char msg[64];
        snprintf((char*) msg, sizeof(msg), "No BMP found at 0x%08x", (uint32_t) bmp);
        tft.fillScreen(ILI9341_BLACK);
        tft.setCursor(50, 100);
        tft.setTextColor(ILI9341_RED);
        tft.print((char*) msg);
        printf("BMP format not recognized.\n");
    }
    xSemaphoreGive(gTFTSemaphore);
}


void init_cmd(uint32_t argc, char *argv[])
{
    uint8_t num_attemtps = 3;
    uint8_t diag;
    spi_init(1, (spi_mode_t) SPI_MODE0, SPI_FREQ_DIV_20M, true, SPI_LITTLE_ENDIAN, true);

    /*
    SPI_FREQ_DIV_2M   < 2MHz
    SPI_FREQ_DIV_4M   < 4MHz
    SPI_FREQ_DIV_8M   < 8MHz
    SPI_FREQ_DIV_10M  < 10MHz
    SPI_FREQ_DIV_20M  < 20MHz
    */

    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    do {
        tft.begin();
        tft.setCursor(0, 0);
        tft.setTextColor(ILI9341_WHITE);
        diag = tft.readcommand8(ILI9341_RDSELFDIAG);
        num_attemtps--;        
    } while(diag != 0xc0 && num_attemtps); // Sometimes the tft init fails and diag reads 0

    if (diag != 0xc0) {
        printf("Error! ILI9341 init failed.\n");
    } else {
        // Read diagnostics (optional but can help debug problems)
        printf("Display Power Mode: 0x%02x\n", tft.readcommand8(ILI9341_RDMODE));
        printf("MADCTL Mode:        0x%02x\n", tft.readcommand8(ILI9341_RDMADCTL));
        printf("Pixel Format:       0x%02x\n", tft.readcommand8(ILI9341_RDPIXFMT));
        printf("Image Format:       0x%02x\n", tft.readcommand8(ILI9341_RDIMGFMT));
        printf("Self Diagnostic:    0x%02x\n", tft.readcommand8(ILI9341_RDSELFDIAG));
    }
    xSemaphoreGive(gTFTSemaphore);
}

void draw_bmp_cmd(uint32_t argc, char *argv[])
{
    bmp_draw((uint8_t*) c64_bmp, 0, 0);
}

void fill_cmd(uint32_t argc, char *argv[])
{
    char temp[3] = {0, 0, 0};
    char *end;
    uint8_t r, g, b;
    if (strlen(argv[1]) != 7) {
        printf("Express color as #rrggbb\n");
    } else {
        if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
            printf("Timeout acquiring TFT semaphore\n");
            return;
        }
        temp[0] = argv[1][1];
        temp[1] = argv[1][2];
        r = (uint8_t) strtoul((char*) &temp, &end, 16);
        temp[0] = argv[1][3];
        temp[1] = argv[1][4];
        g = (uint8_t) strtoul((char*) &temp, &end, 16);
        temp[0] = argv[1][5];
        temp[1] = argv[1][6];
        b = (uint8_t) strtoul((char*) &temp, &end, 16);
        tft.fillScreen(rgb565(r, g, b));
    }
    xSemaphoreGive(gTFTSemaphore);
}

void text_cmd(uint32_t argc, char *argv[])
{
    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    if (argc == 1) {
       tft.println((char*) "");
    }
    for (uint32_t i = 1; i < argc; i++) {
        tft.print(argv[i]);
        tft.print((char*) " ");
    }
    xSemaphoreGive(gTFTSemaphore);
}

void text_size_cmd(uint32_t argc, char *argv[])
{
    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    tft.setTextSize(atoi(argv[1]));
    xSemaphoreGive(gTFTSemaphore);
}

void cls_cmd(uint32_t argc, char *argv[])
{
    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    tft.fillScreen(ILI9341_BLACK);
    tft.setCursor(0, 0);
    xSemaphoreGive(gTFTSemaphore);
}

void on_cmd(uint32_t argc, char *argv[])
{
    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    for (uint32_t i=1; i<argc; i++) {
        uint32_t gpio = atoi(argv[i]);
        printf(" Turning on GPIO %d\n", gpio);
        gpio_enable(gpio, GPIO_OUTPUT);
        gpio_write(gpio, true);
    }
    xSemaphoreGive(gTFTSemaphore);
}

void off_cmd(uint32_t argc, char *argv[])
{
    if (xSemaphoreTake(gTFTSemaphore, 500 / portTICK_PERIOD_MS) == pdFALSE) {
        printf("Timeout acquiring TFT semaphore\n");
        return;
    }
    for (uint32_t i=1; i<argc; i++) {
        uint32_t gpio = atoi(argv[i]);
        printf(" Turning off GPIO %d\n", gpio);
        gpio_enable(gpio, GPIO_OUTPUT);
        gpio_write(gpio, false);
    }
    xSemaphoreGive(gTFTSemaphore);
}

void button_task(void *pvParameters)
{
    struct ip_info ipconfig;
    char msg[32];
    while(1) {
        while (sdk_system_adc_read() < ADC_BUTTON_PRESSED) {
            delay(100);
        }

        if (!sdk_wifi_get_ip_info(STATION_IF, &ipconfig)) {
            printf("Failed to read my own IP address...\r\n");
            snprintf((char*) msg, sizeof(msg), "No IP");
        } else {

        }
        snprintf((char*) msg, sizeof(msg), "%d.%d.%d.%d", ip4_addr1(&ipconfig), ip4_addr2(&ipconfig), ip4_addr3(&ipconfig), ip4_addr4(&ipconfig));
        xSemaphoreTake(gTFTSemaphore, portMAX_DELAY);
        tft.fillRect(0, 0, 320, 25, ILI9341_BLACK);
        tft.setTextSize(2);
        tft.setCursor(5, 5);
        tft.print((char*) msg);
        xSemaphoreGive(gTFTSemaphore);

        while (sdk_system_adc_read() >= ADC_BUTTON_PRESSED) {
            delay(250);
        }
    }
}

void cursor_task(void *pvParameters)
{
    uint16_t light = rgb565(165, 165, 175);
    uint16_t dark  = rgb565( 66,  66, 231);
    while(1) {
        xSemaphoreTake(gTFTSemaphore, portMAX_DELAY);
        tft.fillRect(56, 304, 8, 8, light);
        xSemaphoreGive(gTFTSemaphore);

        delay(200);

        xSemaphoreTake(gTFTSemaphore, portMAX_DELAY);
        tft.fillRect(56, 304, 8, 8, dark);
        xSemaphoreGive(gTFTSemaphore);

        delay(200);        
    }
}

void cli_task(void *pvParameters)
{
    const command_t cmds[] = {
        { .cmd = "init",  .handler = &init_cmd,        .min_arg = 0, .max_arg = 0,   .help = "ILI9341 init" },
        { .cmd = "fill",  .handler = &fill_cmd,        .min_arg = 1, .max_arg = 1,   .help = "Fill screen with specified color", .usage = "#rrggbb" },
        { .cmd = "bmp",   .handler = &draw_bmp_cmd,    .min_arg = 0, .max_arg = 0,   .help = "Draw BMP on screen" },
        { .cmd = "cls",   .handler = &cls_cmd,         .min_arg = 0, .max_arg = 0,   .help = "Clear screen" },
        { .cmd = "t",     .handler = &text_cmd,        .min_arg = 0, .max_arg = 16,  .help = "Draw text on screen. eg. 't Hello World!' or 't' for newline" },
        { .cmd = "size",  .handler = &text_size_cmd,   .min_arg = 1, .max_arg = 11,  .help = "Set text size",  .usage = "<1...>" },
        { .cmd = "on",    .handler = &on_cmd,          .min_arg = 1, .max_arg = 16,  .help = "Turn on one or more GPIOs", .usage = "<gpio> [<gpio>]*" },
        { .cmd = "off",   .handler = &off_cmd,         .min_arg = 1, .max_arg = 16,  .help = "Turn off one or more GPIOs", .usage = "<gpio> [<gpio>]*" },
    };
    delay_ms(250); // Seem to run into problems when initing the TFT too soon
    init_cmd(0, 0);
    draw_bmp_cmd(0, 0);
    gpio_write(TFT_LED, false);
    xTaskCreate(cursor_task, "cursor_task", 512, NULL, 2, NULL);
    xTaskCreate(button_task, "button_task", 512, NULL, 2, NULL);
    cli_run(cmds, sizeof(cmds) / sizeof(command_t), "Commadorable 64");
}

void user_init(void)
{
    gpio_enable(TFT_LED, GPIO_OUTPUT);
    gpio_write(TFT_LED, true);
    uart_set_baud(0, 115200);
    vSemaphoreCreateBinary(gTFTSemaphore);

#ifndef CONFIG_NO_WIFI
  // Wifi not necessary for the CLI demo but I use OTA for flashing
    ota_tftp_init_server(TFTP_PORT);
    struct sdk_station_config config;
    strcpy((char*) &config.ssid, (char*) WIFI_SSID);
    strcpy((char*) &config.password, (char*) WIFI_PASS);
    // required to call wifi_set_opmode before station_set_config
    sdk_wifi_set_opmode(STATION_MODE);
    sdk_wifi_station_set_config(&config);
#endif // CONFIG_NO_WIFI

    xTaskCreate(cli_task, "cli_task", 512, NULL, 2, NULL);
}

} // extern "C"
