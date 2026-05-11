![rocksmith-on-linux](assets/rocksmith-logo-terminal.png)[^1]

This project aims to add ASIO support to Ubisoft's **Rocksmith**, allowing the game to be run on Linux, using Steam/Proton, Pipewire (or JACK), and WineASIO.

Rocksmith is different from Rocksmith 2014 in how it handles audio, and the original RS ASIO mod doesn't work with it. This project is a fork of RS ASIO, with changes to make it compatible with Rocksmith.

Using Ubisoft's Real Tone Cable is still required. Rocksmith does not work without it. Because of that, the only practical purpose of this mod is to allow the game to run on Linux with WineASIO, which should improve audio latency and performance compared to using Wine's built-in audio drivers.

## How to use

### Requirements

- The original Ubisoft/Hercules *Real Tone Cable* (also known as *Rocksmith USB Guitar Adapter*).
- Steam's version of Rocksmith installed and set up according to [**Nizo's "Rocksmith 2014 on Linux"**](https://codeberg.org/nizo/linux-rocksmith) guide.

### Installation and setup

- Copy the files `avrt.dll`, `RS_ASIO.dll` and `RS_ASIO.ini` of [latest release](https://github.com/ferabreu/rocksmith-on-linux/releases/latest) (zip archive rocksmith-on-linux-\<VERSION\>.zip ) to the game folder.
  - This project will follow the upstream RS ASIO versioning scheme, with an additional sub-version number to indicate changes specific to Rocksmith. For example, if the latest RS ASIO release is 0.7.4, the corresponding rocksmith-on-linux version will be 0.7.4-0, 0.7.4-1 and so on.
  - Only the Steam version of Rocksmith is currently supported. You can find the local folder of the game by right clicking on it in your Steam library, and selecting menu "Manage" -> "Browse local files"
- The `RS_ASIO.ini` file is pre-configured to be used with the game running on Linux, with the usual Proton stack and WineASIO.
- Make sure `Rocksmith.ini` is set to run with `ExclusiveMode=1`. If in doubt, use default settings.
- Make sure your interface clock is set to 48kHz. rocksmith-on-linux will try to request 48kHz mode, but you need to set it manually in Pipewire or JACK.
- An `RS_ASIO.log` file is generated inside the game directory which may help diagnosing issues.

### Removal

- Remove the files `avrt.dll`, `RS_ASIO.dll` and `RS_ASIO.ini` from the game folder.

---

## Linux setup instructions

[**Nizo's "Rocksmith 2014 on Linux"**](https://codeberg.org/nizo/linux-rocksmith) guide covers setting up Rocksmith 2014 on Arch, Debian, Fedora, SteamOS and NixOS-derived distributions, using PipeWire or JACK as the audio system. The guide includes instructions for Steam/Proton, troubleshooting tips and performance optimizations. Check it out at [nizo/linux-rocksmith](https://codeberg.org/nizo/linux-rocksmith) (CC-BY-SA-4.0).

The same instructions should work for Rocksmith as well.

---

## Known issues

- Only works with the original Ubisoft/Hercules *Real Tone Cable* (also known as *Rocksmith USB Guitar Adapter*).
  - The branch `asio-support` contains an ongoing attempt to add support for other ASIO interfaces, but it's not working yet. If you want to help with that, check the branch and the files:
    - [asio-support-report.md](https://github.com/ferabreu/rocksmith-on-linux/blob/asio-support/docs/asio-support-report.md)
    - [failed-asio-support-chat.md](https://github.com/ferabreu/rocksmith-on-linux/blob/asio-support/docs/failed-asio-support-chat.md)
      - It's not really a "failure" - I was just upset. 😅 Adding support for other ASIO interfaces is still a goal, but it will require significant changes to the code and a better understanding of how ASIO works in Wine.
- Both Wine and WineASIO must support 32-bit.
- Hardware hotplugging while the game is running won't be noticed by the game.
- Sometimes, the game will not start. That's probably a momentary issue with WineASIO or the way the game initializes audio devices. Just try again and it should work.

---

## Disclaimer

**Testing and Limitations:**
- This software has not been exhaustively tested.
- Basic functionality has been verified:
  - with a single Real Tone Cable
  - in a single Linux environment based on Manjaro Linux, using Steam/Proton, PipeWire, and WineASIO.
- It may not work in all possible environments and configurations.

**No Warranty:**
- This software is provided as-is without any warranty of any kind, express or implied.
- The authors are not responsible for any problems, damages, data loss, or other issues arising from the use of this software.
- Use at your own risk.

**Legal Notice:**
- Rocksmith and Rocksmith 2014 are trademarks and properties of Ubisoft.
- This project is an independent effort and is not affiliated with, endorsed by, or approved by Ubisoft.

---

## Additional technical details

Additional information about how rocksmith-on-linux works, and how to configure it, can be found in the [technical details document](https://github.com/ferabreu/rocksmith-on-linux/blob/main/docs/tech-details.md). This includes information about how the WASAPI redirect works and how to match the `WasapiDevice` setting.

I have also included a good portion of my "development conversation" with GitHub Copilot in the [copilot-chat.md](https://github.com/ferabreu/rocksmith-on-linux/blob/main/docs/copilot-chat.md). This includes the initial specifications I gave to Copilot, and the subsequent conversation where I asked for help with implementation details, debugging and testing. It may be interesting for those who want to understand how the project was developed, or how to use GitHub Copilot for similar projects. The file also includes a section on trying to replicate full ASIO support in Wine, which was ultimately unsuccessful.

---

## Credits

Developed with the assistance of GitHub Copilot, Claude Sonnet 4.6 and Claude Haiku 4.5, based on my specs.

This project is a fork of [RS ASIO](https://github.com/mdias/rs_asio), by Micael Dias, without which this project would not be possible. RS ASIO is still being maintained and developed, and I recommend checking it out if you want to use ASIO with Rocksmith 2014.

[^1]: The logo is a modified version of the original Rocksmith logo, created by Ubisoft. The logo and branding are protected, meaning they cannot be used for commercial purposes without authorization from Ubisoft. This project is an independent effort and is not affiliated with, endorsed by, or approved by Ubisoft. The logo is used here for informational purposes only, to indicate that the project is related to Rocksmith.