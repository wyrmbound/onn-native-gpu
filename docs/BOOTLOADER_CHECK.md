# Bootloader check on another PC (read-only)

Goal: read what the onn 4K Pro's bootloader reports, without changing anything. Three commands, then reboot.
Nothing here unlocks or flashes. Unlocking is a separate decision: it wipes the box, including OnnShell's
Device Owner state, the debloat, Shizuku and the whole /data/local/tmp/pb driver stack.

## Cable
The box's **USB-A socket** is its device port. Use the A-to-A cable from the box's USB-A to the PC.
The box's USB-C is power only.

## Get the box into fastboot
From any computer that already has adb access to the box (Wi-Fi or USB):

```bash
adb reboot bootloader
```

or hold it in Android and run the same command from the other PC once adb sees the box. The TV shows only the
logo while in fastboot; that is normal. Do not unplug/replug USB or let anything reset the port while it is in
fastboot: this bootloader's USB dies on a reset and only a power-cycle brings it back.

## Fastboot tool
- **Windows x64:** download Android platform-tools (dl.google.com/android/repository/platform-tools-latest-windows.zip),
  unzip. Driver: run Zadig (github.com/pbatard/libwdi/releases, zadig-2.9.exe) as admin with the box in fastboot,
  pick "jarvis2" (Options > List All Devices if the list is empty), target WinUSB, Install Driver. On x64 this works;
  it fails on Windows on ARM64 because of ARM64 driver-signing rules.
- **Linux:** `sudo apt install fastboot` (or platform-tools zip). No driver needed. Run with sudo or add a udev rule.
- **macOS:** platform-tools zip. No driver needed.

## Commands (copy the whole output back)
```bash
fastboot devices
fastboot getvar all
fastboot flashing get_unlock_ability
fastboot oem device-info
fastboot reboot
```
`getvar all` prints to stderr on some versions; add `2>&1` if the terminal shows nothing.
If `get_unlock_ability` says "unknown command" that is itself an answer (no standard unlock path exposed).

## What to do with the output
Keep the output. The interesting lines are `unlocked`, `secure`, `unlock_ability`
(1 = the bootloader would accept `fastboot flashing unlock`, 0 = it would refuse even with the OEM-unlock
toggle on), anything mentioning `token`/`walmart`/`oem`, and the `oem device-info` block.

## Afterwards
`fastboot reboot` returns the box to Android. If it was unplugged while in fastboot and no longer answers,
power-cycle it.

## Result on our unit (2026-11-06)
Bootloader 01.01.260525, A/B, `unlocked: no`, `secure: yes`. Both `fastboot flashing get_unlock_ability` and
`fastboot flashing unlock` answer `locked device`; `oem unlock` and `oem device-info` are unrecognized. The unlock is
vendor-gated. Nothing in this repo needs root or an unlocked bootloader.
Note: Windows on ARM64 could not host fastboot for this device (WinUSB package signing), and USB/IP passthrough
resets the port, which this bootloader's USB stack does not survive. Use an x64 Windows, Linux or macOS host.
