#pragma once

#include <Arduino.h>
#include "esp_camera.h"

class MyTimerCam {
public:
    // frameSize : FRAMESIZE_QVGA, SVGA, UXGA, QXGA, etc.
    // pixelFormat : PIXFORMAT_JPEG, GRAYSCALE, ...
    bool begin(framesize_t frameSize = FRAMESIZE_UXGA,
               pixformat_t pixelFormat = PIXFORMAT_JPEG,
               uint8_t fbCount = 1,
               uint8_t jpegQuality = 10);

    // Capture une image
    camera_fb_t* capture();

    // Libère le buffer
    void freeFrame(camera_fb_t* fb);
};