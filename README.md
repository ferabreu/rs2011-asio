# RS2011-ASIO

This project aims to add ASIO support to Ubisoft's **Rocksmith (2011)**, allowing the game to be run on Linux, using Steam/Proton, Pipewire (or JACK), and WineASIO.

## How to use

### Requirements

- The original Ubisoft/Hercules *Real Tone Cable* (also known as *Rocksmith USB Guitar Adapter*).
- Steam's version of Rocksmith 2011 installed and set up according to [**Nizo's "Rocksmith 2014 on Linux"**](https://codeberg.org/nizo/linux-rocksmith) guide.

### Installation and setup

- Copy the files `avrt.dll`, `RS_ASIO.dll` and `RS_ASIO.ini` of [latest release](https://github.com/ferabreu/rs2011-asio/releases/latest) (zip archive rs2011-asio-<version>.zip ) to the game folder.
  - Only the Steam version of Rocksmith 2011 is currently supported. You can find the local folder of the game by right clicking on it in your Steam library, and selecting menu "Manage" -> "Browse local files"
- The `RS_ASIO.ini` file is pre-configured to be used with the game running on Linux, with the usual Proton stack and WineASIO.
- Make sure `Rocksmith.ini` is set to run with `ExclusiveMode=1`. If in doubt, use default settings.
- Make sure your interface clock is set to 48kHz. RS2011-ASIO will try to request 48kHz mode, but you need to set it manually in Pipewire or JACK.
- An `RS_ASIO.log` file is generated inside the game directory which may help diagnosing issues.

### Removal

- Remove the files `avrt.dll`, `RS_ASIO.dll` and `RS_ASIO.ini` from the game folder.

---

## Linux setup instructions

[**Nizo's "Rocksmith 2014 on Linux"**](https://codeberg.org/nizo/linux-rocksmith) guide covers setting up Rocksmith 2014 on Arch, Debian, Fedora, SteamOS and NixOS-derived distributions, using PipeWire or JACK as the audio system. The guide includes instructions for Steam/Proton, troubleshooting tips and performance optimizations. Check it out at [nizo/linux-rocksmith](https://codeberg.org/nizo/linux-rocksmith) (CC-BY-SA-4.0).

The same instructions should work for Rocksmith 2011 as well.

---

## Known issues

- Only works with the original Ubisoft/Hercules *Real Tone Cable* (also known as *Rocksmith USB Guitar Adapter*).
- Both Wine and WineASIO must support 32-bit.
- Hardware hotplugging while the game is running won't be noticed by the game.
- Sometimes, the game will not start. That's probably a momentary issue with WineASIO or the way the game initializes audio devices. Just try again and it should work.

---

## Additional technical details

Additional information about how RS2011-ASIO works, and how to configure it, can be found in the [technical details document](docs/tech-details.md). This includes information about how the WASAPI redirect works and how to match the `WasapiDevice` setting.

---

## Credits

Developed with the assistance of GitHub Copilot, Claude Sonnet 4.6 and Claude Haiku 4.5, based on my specs.

This project is a fork of [RS ASIO](https://github.com/mdias/rs_asio), by Micael Dias, without which this project would not be possible. RS ASIO is still being maintained and developed, and I recommend checking it out if you want to use ASIO with Rocksmith 2014.
