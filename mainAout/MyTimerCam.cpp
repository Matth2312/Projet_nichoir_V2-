#include "MyTimerCam.h"

// ---- Pinout spécifique M5Stack TimerCAM (OV3660) ----
// D’après la doc M5Stack et un exemple Reddit pour TimerCAM X :contentReference[oaicite:0]{index=0}
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  15
#define XCLK_GPIO_NUM   27
#define SIOD_GPIO_NUM   25
#define SIOC_GPIO_NUM   23

#define Y9_GPIO_NUM     19
#define Y8_GPIO_NUM     36
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     39
#define Y5_GPIO_NUM     5
#define Y4_GPIO_NUM     34
#define Y3_GPIO_NUM     35
#define Y2_GPIO_NUM     32

#define VSYNC_GPIO_NUM  22
#define HREF_GPIO_NUM   26
#define PCLK_GPIO_NUM   21

bool MyTimerCam::begin(framesize_t frameSize,
                       pixformat_t pixelFormat,
                       uint8_t fbCount,
                       uint8_t jpegQuality)
{
    camera_config_t config;

    // Toujours bien tout initialiser
    memset(&config, 0, sizeof(config));

    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;   // ⚠️ Si erreur de compilation, essayer pin_sscb_sda
    config.pin_sccb_scl = SIOC_GPIO_NUM;   // idem

    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d0       = Y2_GPIO_NUM;

    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;

    config.xclk_freq_hz = 20000000;            // 20 MHz classique
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    config.pixel_format = pixelFormat;
    config.frame_size   = frameSize;
    config.jpeg_quality = jpegQuality;         // 10 = bonne qualité
    config.fb_count     = fbCount;             // 1 ou 2 buffers

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("esp_camera_init failed: 0x%x\n", err);
        return false;
    }

    return true;
}

camera_fb_t* MyTimerCam::capture()
{
    return esp_camera_fb_get();
}

void MyTimerCam::freeFrame(camera_fb_t* fb)
{
    if (fb != nullptr) {
        esp_camera_fb_return(fb);
    }
}