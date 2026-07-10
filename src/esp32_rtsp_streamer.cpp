#include "esp32_rtsp_streamer.h"

#include <Arduino.h>

#include "esp_camera.h"

Esp32RtspStreamer::Esp32RtspStreamer(
    unsigned short width,
    unsigned short height,
    JpegFrameObserver frameObserver)
    : CStreamer(width, height), frameObserver_(frameObserver) {}

void Esp32RtspStreamer::streamImage(uint32_t curMsec) {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    return;
  }

  if (frameObserver_ != nullptr) {
    frameObserver_(frame->buf, frame->len, frame->width, frame->height, curMsec);
  }
  streamFrame(frame->buf, frame->len, curMsec);
  esp_camera_fb_return(frame);
}
