#pragma once

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "storage.h"

struct ScanObservation {
  Snapshot snapshot;
  int8_t rssi = 0;
};

class BleScanner : public NimBLEScanCallbacks {
 public:
  bool begin();
  bool start();
  void stop();
  bool pop(ScanObservation& observation);
  bool active() const;
  void target(const char* mac) { strncpy(target_, mac, sizeof(target_)-1); target_[17]=0; }
  uint32_t seen() const { return seen_; }
  uint32_t rejected() const { return rejected_; }
  String environment() const;

 private:
  void onResult(const NimBLEAdvertisedDevice* device) override;
  NimBLEScan* scan_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  char target_[18]{};
  volatile uint32_t seen_ = 0, rejected_ = 0;
  struct Nearby { char mac[18]; int rssi; };
  Nearby nearby_[16]{};
  size_t count_=0;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

