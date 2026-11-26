<p align="center">
  <a href="https://hubbo.tv"><img src="https://hubbo.tv/static/og.png" alt="Hubbo: keep your remote, replace the hub" width="760"></a>
</p>

<p align="center">
  <a href="https://hubbo.tv">hubbo.tv</a> ·
  <a href="https://hubbo.tv/build/">Build your hub</a> ·
  <a href="https://hubbo.tv/devices/">Device catalog</a> ·
  <a href="https://hubbo.tv/tour/">Tour</a> ·
  <a href="https://github.com/hubbo-remote/hubbo/releases">Releases</a>
</p>

# Hubbo

Hubbo is open-source software that runs on a hub you build yourself. Keep the Harmony remote you
already love, and use it to control your TV, receiver, streaming boxes and lights.

- **Infrared, Bluetooth and Home Assistant.** Control IR devices, pair as a remote and keyboard
  with streaming devices, and use your Home Assistant devices right next to them.
- **Activities and smart buttons.** Press Watch TV and the right devices turn on, the right inputs
  are picked, and every button on the remote does the right thing.
- **Managed in a browser.** Add devices, build activities and map your remote from any browser on
  your network. Nothing to install, no account, no cloud.
- **Built for agents.** Give an AI agent a token and your hub's address, then ask in plain words:
  "Make a Watch TV activity with the TV and the Apple TV, and put the volume on the receiver." It
  builds the activity and your remote's buttons, and they show up in Hubbo like anything you made.
- **Hubbo shows up in Home Assistant.** Start and stop activities from your automations and
  dashboards.
- **Thousands of devices.** Search the [device catalog](https://hubbo.tv/devices/) for your
  brand and model, and add it in a couple of clicks.
- **Updates itself.** New releases install from the browser, and a bad update rolls back on its own.

## What you need

- A Waveshare ESP32-S3-ETH board (the PoE version if you want one cable for power and network).
- An nRF24L01+ radio module, or the long-range PA+LNA version.
- An IR emitter, if you have devices that use infrared.
- Some female-to-female jumper wires.
- Your Harmony Companion remote, and your old Harmony Hub for a minute to pair it.

The [build guide](https://hubbo.tv/build/) has the full parts list with links, the wiring for each
part, and every step.

## Getting started

1. **Build your hub.** Wire the radio (and the IR emitter, if you're adding one) to the ESP32
   board, following the [build guide](https://hubbo.tv/build/).
2. **Install Hubbo.** Download `hubbo-install.bin` from the
   [latest release](https://github.com/hubbo-remote/hubbo/releases/latest) and flash it from Chrome
   or Edge, following the [install steps](https://hubbo.tv/build/#step-install). After that,
   updates install from Hubbo itself.
3. **Open Hubbo.** Plug the hub into your network and open `http://hubbo-xxxx.local/` in a browser,
   where `xxxx` matches your hub's name (for example `hubbo-7f8d.local`), or use the address your
   router gave it. Create your username and password.
4. **Pair your remote.** In Hubbo, start pairing, then press the pair button on your old Harmony
   Hub. That's the last time you need it.
5. **Add your devices.** Search the catalog, import a profile, or add your Home Assistant devices.
6. **Make activities.** Pick the devices, the inputs and what each button does. Then pick up the
   remote and press Watch TV.

## For developers

Hubbo is a portable C++ core (the engine, devices and activities) with firmware for the ESP32-S3,
a React web app, and one HTTP API that the web app, Home Assistant and scripts all use. Everything
builds and tests in Docker.

- [docs/development.md](docs/development.md): the toolchain, commands, repo layout and releases.
- [ARCHITECTURE.md](ARCHITECTURE.md): how Hubbo is put together.
- [docs/api.md](docs/api.md): the API.
- [hubbo-remote/hubbo-profiles](https://hubbo.tv/spec/): the open device profile format.

Hubbo builds on the RF protocol work in
[soaresbrun0/Harmonizer](https://github.com/soaresbrun0/harmonizer).

## License

AGPL-3.0, like Harmonizer. Hubbo isn't affiliated with Logitech. Harmony is a trademark of its
owner.
