# Hubbo

A standalone replacement for the Logitech Harmony Hub, running on an ESP32-S3. A Harmony
Companion remote talks to Hubbo over 2.4 GHz RF. Hubbo owns the activities, button mappings and
devices, and acts on them directly: Bluetooth HID to an Apple TV, and outbound calls to Home
Assistant (IR comes later). A web admin and LLM agents configure it through one API.

- **[ARCHITECTURE.md](ARCHITECTURE.md)**: the agreed design. Read it first.
- **[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)**: milestones and acceptance gates, in order.
- **[docs/development.md](docs/development.md)**: toolchain, commands, repo layout.

Primary board: Waveshare ESP32-S3-ETH. Hubbo builds on the RF protocol work in
[soaresbrun0/Harmonizer](https://github.com/soaresbrun0/harmonizer) and our own spike on top of it
(the local `Harmonizer/` directory; not committed). Licensed AGPL-3.0, like Harmonizer.

**Priority:** a working RF/BLE activity engine with measurable responsiveness comes before a
sophisticated web UI. The web admin and LLM clients use the same API.
