<img src="assets/icon.svg" width="64" height="64" alt="">

# waterforce_kernel_driver

A fork of the mainline Linux `gigabyte_waterforce` hwmon driver for the
**Gigabyte AORUS WATERFORCE X240 / X280 / X360** (USB `1044:7a4d`), with
**fan and pump control** added. The mainline driver can only read the sensors.

It keeps the same module name (`gigabyte_waterforce`) and hwmon name
(`waterforce`), so CoolerControl and `sensors` see the same device as
before. The difference is that Fan speed and Pump speed can now be controlled.

> ⚠️ The control commands are community reverse-engineered, not documented by
> Gigabyte. The fixed-speed command comes from Aleksa Savic's original WIP driver
> and the profile command from
> [omarchy-aorus-fan](https://github.com/D05Col/omarchy-aorus-fan), which was
> measured on an X360. Watch temperatures the first time you use it.

## What it adds

| sysfs file | Access | Meaning |
|---|---|---|
| `pwm1` / `pwm2` | rw | Fan / pump duty, 0–255. Writing sets a fixed speed and switches to manual mode |
| `pwm1_enable` / `pwm2_enable` | rw | `1` = manual (fixed speed), `2` = cooler's own curve (the selected profile) |
| `fan1_target` / `fan2_target` | rw | Fixed speed in RPM. Kept within 750 to `max_rpm`−1 |
| `fan_profile` | rw | Firmware fan curve: `zero quiet balance default performance max` |
| `pump_profile` | rw | Firmware pump curve: `balance max` |

The existing read-only files (`fan1_input`, `fan2_input`, `temp1_input`,
labels) behave as before.

Module parameters:

* `max_rpm=2800`: the RPM that `pwm=255` maps to. The device takes back control
  at 2800 RPM or more, so targets stay just below it.
* `keepalive_ms=0`: if the cooler falls back to its own curve after a while,
  set this to, for example, `5000` to re-send the manual target every 5 s. It
  can be changed at runtime in `/sys/module/gigabyte_waterforce/parameters/keepalive_ms`.

## Install (DKMS, survives kernel updates)

CachyOS / Arch:

```sh
sudo pacman -S --needed dkms base-devel linux-cachyos-headers   # headers for your kernel
sudo mkdir -p /usr/src/gigabyte_waterforce-1.0.0
sudo cp gigabyte_waterforce.c Makefile dkms.conf /usr/src/gigabyte_waterforce-1.0.0/
sudo dkms install gigabyte_waterforce/1.0.0

# swap the running module for the new one (or just reboot)
sudo modprobe -r gigabyte_waterforce && sudo modprobe gigabyte_waterforce
modinfo gigabyte_waterforce | grep version    # should print 1.0.0-control
```

DKMS installs into `updates/`, which takes priority over the in-kernel module.

To remove it: `sudo dkms remove gigabyte_waterforce/1.0.0 --all`. The stock
driver comes back after a reboot or a `modprobe -r` / `modprobe`.

### Quick test without installing

```sh
make load      # builds, unloads the stock module, loads this one until reboot
```

## Try it by hand

```sh
cd /sys/class/hwmon/$(grep -l waterforce /sys/class/hwmon/*/name | xargs dirname | xargs basename)

cat fan1_input fan2_input              # current RPM
echo 1500 | sudo tee fan1_target       # fan fixed at ~1500 RPM
echo 180  | sudo tee pwm2              # pump ~70 %
echo 2    | sudo tee pwm1_enable       # fan back to the cooler's curve
echo quiet | sudo tee fan_profile      # pick which curve that is
cat fan_profile                        # zero [quiet] balance default performance max
```

Allow a couple of seconds after a change before reading `fanN_input`. The
reading is cached for 2 s.

## CoolerControl

After the module is loaded, restart the daemon
(`sudo systemctl restart coolercontrold`). Fan speed and Pump speed then show as
controllable. Apply a profile (a fixed speed or a curve on CPU temperature).
CoolerControl writes `pwmN`, which the driver turns into an RPM target.

## Notes and known gaps

* Only the first-generation X series (`1044:7a4d`) is supported. The WATERFORCE
  II (`1044:7a5e`) and the non-X models use a different protocol.
* `pwmN` reads back the duty the cooler *reports*, so after a write it can differ
  a little from what was written.
* After a module load the driver reports `pwmN_enable = 2` and the `balance`
  profiles. It cannot ask the device which profile is actually active.
* One tool reported that the `0xE6` fixed-speed command can make the firmware
  stick at about 88 %. If manual speeds don't take effect on this unit, use
  `fan_profile` / `pump_profile` (the `0xE5` + `0xB6` commands), which are known
  to work on the X360.
* LCD and RGB are not handled here. Use userspace tools for them.
