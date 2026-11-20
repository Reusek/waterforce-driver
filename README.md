<img src="assets/icon.svg" width="64" height="64" alt="">

# waterforce_kernel_driver

A fork of the mainline Linux `gigabyte_waterforce` hwmon driver for the
**Gigabyte AORUS WATERFORCE X240 / X280 / X360** (USB `1044:7a4d`), with
**fan and pump control** added. The mainline driver can only read the sensors.

It keeps the same module name (`gigabyte_waterforce`) and hwmon name
(`waterforce`), so CoolerControl and `sensors` see the same device as
before. The difference is that Fan speed and Pump speed can now be controlled.

## How control works

Two kinds of control, both confirmed on an X360:

**Fixed speed (custom mode).** Writing `pwmN` or `fanN_target` sends a flat
custom curve (`0xE6`) and switches that channel to custom mode (profile code
`0x01`, applied with `0xB6`). The `0xE6` command alone is ignored. It only
takes effect in custom mode. This allows speeds below every built-in profile,
for example fan 1000 RPM (~40 %) and pump 2000 RPM (~62 %).

* `pwmN` 0–254 maps linearly to 0 – `fan_max_rpm` / `pump_max_rpm`, raised
  to the minimum (`fan_min_rpm` 750, `pump_min_rpm` 1500).
* The cooler leaves custom mode at 2800 RPM or more, so fixed speeds stop at
  2799 RPM. That limits the pump to about 85 %. `pwmN = 255` selects the `max`
  profile for full speed instead.

**Built-in profiles.** `fan_profile` / `pump_profile` pick one of the cooler's
own temperature curves (`0xE5` + `0xB6`). Duty measured at 26 °C coolant:

| Fan profile | Duty | Pump profile | Duty |
|---|---|---|---|
| zero | fan stop | silent | ~63 % (~2100 RPM) |
| quiet | ~56 % | quiet | ~66 % (~2140 RPM) |
| balance | ~72 % | balance | ~81 % (~2650 RPM) |
| performance | ~88 % | high | ~93 % (~2980 RPM) |
| max / default | 100 % | max | 100 % (~3250 RPM) |

A command is only sent when the setting changes, so the frequent `pwm` writes
from CoolerControl do not keep sending commands to the cooler.

## sysfs files

| File | Access | Meaning |
|---|---|---|
| `pwm1` / `pwm2` | rw | Read: duty the cooler reports. Write 0–255: fixed speed (manual mode) |
| `fan1_target` / `fan2_target` | rw | Fixed speed in RPM (manual mode). Reads 0 when a profile is active |
| `pwm1_enable` / `pwm2_enable` | rw | `1` = manual, `2` = use the profile named in `fan_profile` / `pump_profile` |
| `fan_profile` | rw | `zero quiet balance performance max default`, active one in `[brackets]` |
| `pump_profile` | rw | `silent quiet balance high max` |

The existing read-only files (`fan1_input`, `fan2_input`, `temp1_input`,
labels) behave as before.

Module parameters (also writable at runtime in
`/sys/module/gigabyte_waterforce/parameters/`): `fan_min_rpm=750`,
`fan_max_rpm=2600`, `pump_min_rpm=1500`, `pump_max_rpm=3300`.

## Install (DKMS, survives kernel updates)

CachyOS / Arch:

```sh
sudo pacman -S --needed dkms base-devel linux-cachyos-headers   # headers for your kernel
sudo pacman -S --needed clang llvm lld   # CachyOS kernels are built with clang; the Makefile detects it

make install
```

`make install`:

1. removes every version already installed through DKMS (`dkms status` lists them),
2. installs this version with DKMS, which rebuilds it automatically for new kernels,
3. reloads the driver and restarts CoolerControl if it is running.

Run it again after pulling a new version. It asks for the sudo password.
Check what is running with `cat /sys/module/gigabyte_waterforce/version`.

`make uninstall` removes it again and goes back to the stock read-only driver.

DKMS installs into `updates/`, which takes priority over the in-kernel module.

### Quick test without installing

```sh
make load      # builds (add LLVM=1 on clang kernels if the Makefile misses it),, unloads the stock module, loads this one until reboot
sudo ./test-pump.sh
```

`make load` does not install anything. After a reboot the stock driver is back.

## Try it by hand

```sh
cd $(grep -l '^waterforce$' /sys/class/hwmon/*/name | xargs dirname)

echo 1000    | sudo tee fan1_target    # fan fixed at ~1000 RPM
echo 1800    | sudo tee fan2_target    # pump fixed at ~1800 RPM
echo 120     | sudo tee pwm1           # fan ~47 %
echo quiet   | sudo tee fan_profile    # back to a built-in fan curve
cat pump_profile                       # silent quiet balance high max [custom 1800 rpm]
```

Allow a couple of seconds after a change before reading `fanN_input`. The
reading is cached for 2 s.

## CoolerControl

After the module is loaded, restart the daemon
(`sudo systemctl restart coolercontrold`). Fan speed and Pump speed then show
as controllable, across the full range. Pump settings between ~85 % and
99 % run at the 2799 RPM cap; 100 % switches to the max profile.

## Notes and known gaps

* Only the first-generation X series (`1044:7a4d`) is supported. The WATERFORCE
  II (`1044:7a5e`) and the non-X models use a different protocol.
* After a module load the driver assumes `balance` on both channels. It cannot
  ask the device which profile is actually active. The first profile request is
  always sent.
* LCD and RGB are not handled here. Use userspace tools for them.
