# Browser Tamer [![Downloads](https://img.shields.io/github/downloads/aloneguid/bt/total)](https://github.com/aloneguid/bt/releases) [![WinGet Package Version](https://img.shields.io/winget/v/aloneguid.bt)](https://www.aloneguid.uk/projects/bt/#installing)


[Home + Documentation](https://www.aloneguid.uk/projects/bt/) 👈 No "AI". Guaranteed **Human**.


[![](https://www.aloneguid.uk/projects/bt/one.png)](https://www.aloneguid.uk/projects/bt/)


## This fork: Win32 build

This branch is a Windows-only rework built for speed. Browser Tamer stays resident in the background
and shows a native Win32 picker in well under 80 ms after a click. See [release notes](release-notes.md#700).

- `bt.exe` with a URL: route the link (forwarded to the background instance).
- `bt.exe pick <url>`: always show the picker.
- `bt.exe`: open settings.
- `bt.exe --background`: start the background instance only; `bt.exe --exit`: stop it.
- `bt.exe browser list | get default | set default <browser>.<profile>`: console commands.

## Raising an Issue

Feel free to [raise an issue here](https://github.com/aloneguid/bt/issues/new).

## Contributing

I value your interest in this open-source project, and your support through coffee donations directly contributes to the project's development and sustainability, allowing me to invest more resources into refining, addressing issues, and implementing new features. Your involvement, in any form, is greatly appreciated. Thank you for being a part of our journey!

> [!IMPORTANT]
> One of the best ways to contribute simply [donating](https://www.aloneguid.uk/sponsor/) any amount you want. This will keep the project alive.