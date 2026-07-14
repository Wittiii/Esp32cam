#include "esp32_rtsp_streamer.h"

#include <Arduino.h>

#include "esp_camera.h"

Esp32RtspStreamer::Esp32RtspStreamer(unsigned short width, unsigned short height)
    : CStreamer(width, height) {}

void Esp32RtspStreamer::streamImage(uint32_t curMsec) {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    lastFrameSucceeded_ = false;
    ++consecutiveCaptureFailures_;
    return;
  }

  lastFrameSucceeded_ = true;
  consecutiveCaptureFailures_ = 0;
  streamFrame(frame->buf, frame->len, curMsec);
  esp_camera_fb_return(frame);
}
