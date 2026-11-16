#include "ble_scanner.h"

#include <cstring>

#include "config.h"

bool BleScanner::begin() {
  queue_ = xQueueCreate(4, sizeof(ScanObservation));
  scan_ = NimBLEDevice::getScan();
  if (!queue_ || !scan_) return false;
  scan_->setScanCallbacks(this, true);
  scan_->setActiveScan(true);
  scan_->setInterval(SCAN_INTERVAL_MS);
  scan_->setWindow(SCAN_WINDOW_MS);
  scan_->setDuplicateFilter(0);
  scan_->setMaxResults(0);
  return true;
}

bool BleScanner::start() {
  if (queue_) xQueueReset(queue_);
  seen_=0; rejected_=0;
  portENTER_CRITICAL(&mux_); count_=0; portEXIT_CRITICAL(&mux_);
  return scan_ && scan_->start(0);
}

void BleScanner::stop() {
  if (scan_ && scan_->isScanning()) scan_->stop();
}

bool BleScanner::active() const {
  return scan_ && scan_->isScanning();
}

bool BleScanner::pop(ScanObservation& observation) {
  return queue_ && xQueueReceive(queue_, &observation, 0) == pdTRUE;
}

void BleScanner::onResult(const NimBLEAdvertisedDevice* device) {
  if (!device) return;
  const String address(device->getAddress().toString().c_str());
  portENTER_CRITICAL(&mux_);
  size_t slot=0; for (;slot<count_;++slot) if(!strcasecmp(nearby_[slot].mac,address.c_str())) break;
  if(slot<count_ || count_<16) {
    if(slot==count_) ++count_;
    strncpy(nearby_[slot].mac,address.c_str(),17); nearby_[slot].mac[17]=0;
    nearby_[slot].rssi=device->getRSSI();
  }
  portEXIT_CRITICAL(&mux_);
  if (!address.equalsIgnoreCase(target_)) return;
  ++seen_;

  const auto& payload = device->getPayload();
  const size_t advLength = device->getAdvLength();
  if (advLength > MAX_ADV_BYTES || payload.size() < advLength ||
      payload.size() - advLength > MAX_ADV_BYTES) { ++rejected_; return; }

  ScanObservation report{};
  address.toCharArray(report.snapshot.deviceMac, sizeof(report.snapshot.deviceMac));
  report.snapshot.addressType = device->getAddressType();
  report.snapshot.advType = device->getAdvType();
  report.snapshot.advLength = advLength;
  report.snapshot.responseLength = payload.size() - advLength;
  if (advLength) memcpy(report.snapshot.advData, payload.data(), advLength);
  if (report.snapshot.responseLength) {
    memcpy(report.snapshot.responseData, payload.data() + advLength,
           report.snapshot.responseLength);
  }
  if (!validAdData(report.snapshot.advData, advLength) ||
      !validAdData(report.snapshot.responseData, report.snapshot.responseLength)) { ++rejected_; return; }
  report.rssi = device->getRSSI();
  // A full queue drops the oldest report while keeping the latest value.
  if (xQueueSend(queue_, &report, 0) != pdTRUE) {
    ScanObservation discarded;
    xQueueReceive(queue_, &discarded, 0);
    xQueueSend(queue_, &report, 0);
  }
}

String BleScanner::environment() const {
  Nearby copy[16]; size_t n;
  portENTER_CRITICAL(&mux_); n=count_; memcpy(copy,nearby_,n*sizeof(Nearby)); portEXIT_CRITICAL(&mux_);
  String result="[";
  for(size_t i=0;i<n;++i) { if(i)result+=","; result+="{\"mac\":\""+String(copy[i].mac)+"\",\"rssi\":"+String(copy[i].rssi)+"}"; }
  return result+"]";
}

