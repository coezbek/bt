# Browser Torero

A fork of [Browser Tamer](https://github.com/aloneguid/bt). Documentation for rules, scripting and the command line:
[Browser Tamer manual](https://www.aloneguid.uk/projects/bt/).

<img src="../resources/icons/icon128x128.png" width="64" align="right" alt="Browser Torero logo">

Browser Torero is a fork of [Browser Tamer](https://github.com/aloneguid/bt) by Ivan G (@aloneguid), under the same
Apache 2.0 licence. All the routing, rules, discovery and link cleanup come from Browser Tamer. The fork changes three things:

- **Native Win32.** Picker, toast and settings are plain Win32 windows. ImGui and DirectX are gone, and the build is Windows only.
- **Much faster.** The picker opens in about 35 ms instead of 160-300 ms (time from launching `bt.exe` to the picker on screen, measured on the same machine). Browser Torero stays resident in the background and each click only hands the link over.
- **Foreground fixes.** The picker comes to the front even while a modifier key is still held, and the browser a link opens in comes to the front instead of only flashing in the taskbar.

It installs over Browser Tamer and keeps its configuration (`%APPDATA%\Browser Tamer`) and its default-browser registration.

Command line:

- `bt.exe <url>`: route the link.
- `bt.exe pick <url>`: always show the picker.
- `bt.exe`: open settings.
- `bt.exe --background`: start the background instance only; `bt.exe --exit`: stop it.
- `bt.exe browser list | get default | set default <browser>.<profile>`: console commands.

See the [release notes](release-notes.md#700) for details.

## Raising an Issue

Feel free to [raise an issue here](https://github.com/aloneguid/bt/issues/new).

## Contributing

I value your interest in this open-source project, and your support through coffee donations directly contributes to the project's development and sustainability, allowing me to invest more resources into refining, addressing issues, and implementing new features. Your involvement, in any form, is greatly appreciated. Thank you for being a part of our journey!

> [!IMPORTANT]
> One of the best ways to contribute simply [donating](https://www.aloneguid.uk/sponsor/) any amount you want. This will keep the project alive.