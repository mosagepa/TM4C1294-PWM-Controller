# Sudo-less flashing with `lm4flash`

Your TM4C1294XL ICDI shows up as:

- `1cbe:00fd` **Luminary Micro Inc. In-Circuit Debug Interface**

`lm4flash` talks to the USB device under `/dev/bus/usb/...`, which is typically owned by `root` and not writable by normal users. The preferred fix is a **udev rule**.

## Option A (recommended): udev rule

1) Copy the provided rule into `/etc/udev/rules.d/`:

```bash
sudo cp tools/udev/49-lm4flash-icdi.rules /etc/udev/rules.d/
```

2) Reload udev rules and re-trigger:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

3) Unplug/replug the LaunchPad USB.

4) Ensure your user is in `plugdev` (some distros don’t have it; if so, pick `dialout` or create a dedicated group and update the rule):

```bash
groups
sudo usermod -aG plugdev "$USER"
# log out/in (or reboot) for group change to apply
```

5) Test sudo-less flashing:

```bash
lm4flash integr_v02.bin
```

If it still fails, check permissions:

```bash
ls -l /dev/bus/usb/001/015  # bus/device numbers may differ
```

## Option B: sudoers NOPASSWD (fallback)

If you *must* keep root permissions, you can allow `lm4flash` without a password.

Run `sudo visudo` and add (adjust username/path if needed):

```
mosagepa ALL=(root) NOPASSWD: /usr/bin/lm4flash
```

This is less safe than a udev rule (it grants root flashing capability), but it’s simple.

## Using Makefile without sudo

The `Makefile` supports a `SUDO` variable.

Once udev is set up:

```bash
make flash SUDO=
make auto SUDO= DURATION=8 CMD='PSYN 44\r'
```
