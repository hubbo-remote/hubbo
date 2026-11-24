#pragma once

// Board profile: Waveshare ESP32-S3-ETH (ESP32-S3, 16 MB flash, 8 MB OPI
// PSRAM, onboard W5500, optional PoE module) with an nRF24L01+ on jumpers.
//
// The only board Hubbo supports for the MVP. Pins were verified on the
// Harmonizer spike boards (see Harmonizer/HANDOFF.md). The W5500 and the
// nRF24 sit on separate SPI hosts, so they never share a bus or a lock.
//
// Hardware notes that firmware cannot fix:
//   - The nRF24 needs a 10 uF capacitor across VCC/GND. Without it, on PoE
//     power, most release packets are lost.
//   - Never connect USB and PoE at the same time.

#include <hal/spi_types.h>

#ifndef HUBBO_BOARD_WAVESHARE_ESP32_S3_ETH
#error "Only the Waveshare ESP32-S3-ETH board profile exists. Build env waveshare_esp32_s3_eth."
#endif

// Macros because the image marker (image_marker.h) is one string literal.
#define HUBBO_BOARD_NAME "waveshare_esp32_s3_eth"
// The partition table's version (platformio.ini: default_16MB.csv). An OTA
// update can't change the table, so an image for another layout is refused.
// Bump it when the table changes.
#define HUBBO_PARTITION_LAYOUT 1

namespace hubbo::fw::board {

constexpr const char *kName = HUBBO_BOARD_NAME;
constexpr unsigned kLayout = HUBBO_PARTITION_LAYOUT;

// Onboard W5500 Ethernet, driven by ESP-IDF's W5500 driver on SPI3.
namespace w5500 {
constexpr int kSck = 13;
constexpr int kMiso = 12;
constexpr int kMosi = 11;
constexpr int kCs = 14;
constexpr int kInt = 10;
constexpr int kRst = 9;
constexpr int kPhyAddr = 1;
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
}  // namespace w5500

// nRF24L01+ on the Arduino global SPI (FSPI = SPI2), its own pins. No IRQ
// line is wired; the radio is polled.
namespace nrf24 {
constexpr int kSck = 15;
constexpr int kMiso = 16;
constexpr int kMosi = 17;
constexpr int kCsn = 18;
constexpr int kCe = 38;
}  // namespace nrf24

// IR emitter (docs/design/ir.md): an active-high signal into the emitter's
// N-FET (e.g. Adafruit 5639), V+ on VSYS. Header pin 25. Driven by RMT TX.
namespace ir {
constexpr int kPin = 1;
}  // namespace ir

}  // namespace hubbo::fw::board
