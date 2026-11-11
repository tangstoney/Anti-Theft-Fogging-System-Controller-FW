# RD91 N32WB436 M4 J-Link configuration

Status: prepared for hardware verification. No probe or board connected/tested.

Vendor SDK source: D:\Tangshi\Project\N32Demo\_sdk\Nsingtech.N32WB43x_Library.2.2.0\utilities\dfu\JLink\JLink_V632\Devices\Nationstech\N32L43x.FLM
SHA256: 11e8f4ecacd0631830d3e7bfc94a6f1f0fb0eb820822fe0ba8509696f2d162b7

SDK JLINKProgrammingM4.bat selects N32L43X. This local alias RD91_N32WB436_M4 uses its unchanged N32L43x.FLM (128KiB Flash), but corrects WorkRAMSize from vendor generic 144KiB to actual M4 32KiB. Installed SEGGER software and SDK were not modified.

Cortex-Debug passes this directory using the legacy -JLinkDevicesXMLPath option supported by the installed J-Link 7.54. Recheck device-pack setup if changing J-Link versions. The flash loader's hardware operation still needs validation.

Launch downloads the current build/rd91_controller.elf, resets M4 and runs to main. It is not passive attach. This is not the BLE M0 configuration. Do not run SDK JLINKProgrammingM4.bat directly: its downloadMCU.jlink performs erase and writes the OTA Boot/settings/APP layout; this project is a standalone app at 0x08000000.

Wire by signal names: VTref to target 3.3V reference, GND, SWDIO, SWCLK, optional nRESET. Power the board separately; verify the actual J-Link connector pinout. Disconnect/disable any other on-board debug probe driving SWD.

Open this repository as the VS Code workspace, choose RD91 M4 - J-Link SWD (download and debug), and press F5. The previous launch.json templates were retained and backed up in .vscode/launch.before-rd91.jsonc. RTT OS startup/behavior and J-Link programming remain hardware-unverified.
