<img src="assets/icon.svg" width="64" height="64" alt="">

# waterforce_kernel_driver

A fork of the mainline Linux `gigabyte_waterforce` hwmon driver for the
**Gigabyte AORUS WATERFORCE X240 / X280 / X360** (USB `1044:7a4d`), with
**fan and pump control** added. The mainline driver can only read the sensors.

It keeps the same module name (`gigabyte_waterforce`) and hwmon name
(`waterforce`), so CoolerControl and `sensors` see the same device as
before. The difference is that Fan speed and Pump speed can now be controlled.

## How control works

The cooler has no working "set exact speed" command. The fixed-RPM command
(`0xE6`) used by earlier drivers is ignored by the firmware. Testing on an X360
showed that neither the pump nor the fan reacted to it.

What does work is choosing one of the cooler's **built-in profiles** (commands
`0xE5` + `0xB6`, from
[omarchy-aorus-fan](https://github.com/D05Col/omarchy-aorus-fan)):

| Channel | Profile | Duty at idle | `pwm` range that selects it |
|---|---|---|---|
| Fan | quiet | ~56 % | 0–143 |
| Fan | balance | ~72 % | 144–184 |
| Fan | performance | ~88 % | 185–224 |
| Fan | max | 100 % | 225–255 |
| Pump | balance | ~81 % (~2670 RPM) | 0–207 |
| Pump | max | 100 % (~3280 RPM) | 208–255 |

When `pwmN` is written, the driver picks the **lowest profile that is at least
as fast as the requested duty**, so the cooler never runs slower than asked.
That makes the control steps coarse: 4 steps for the fan and 2 for the pump.
The profiles are themselves temperature curves inside the cooler, so the real
duty rises with coolant temperature.

## sysfs files

| File | Access | Meaning |
|---|---|---|
| `pwm1` / `pwm2` | rw | Read: duty the cooler reports. Write 0–255: picks a profile from the table above (manual mode) |
| `pwm1_enable` / `pwm2_enable` | rw | `1` = manual (`pwmN` writes pick the profile), `2` = use the profile named in `fan_profile` / `pump_profile` |
| `fan_profile` | rw | `zero quiet balance performance max default`, active one in `[brackets]` |
| `pump_profile` | rw | `balance max` |

`zero` (fan stop) and `default` (same as max on the tested unit) can only be
chosen through `fan_profile`, never through `pwm1`. A profile is only sent when
it changes, so the frequent `pwm` writes from CoolerControl do not keep sending
commands to the cooler.

The existing read-only files (`fan1_input`, `fan2_input`, `temp1_input`,
labels) behave as before.

## Install (DKMS, survives kernel updates)

CachyOS / Arch:

```sh
sudo pacman -S --needed dkms base-devel linux-cachyos-headers   # headers for your kernel
sudo pacman -S --needed clang llvm lld   # CachyOS kernels are built with clang; the Makefile detects it
sudo mkdir -p /usr/src/gigabyte_waterforce-1.1.0
sudo cp gigabyte_waterforce.c Makefile dkms.conf /usr/src/gigabyte_waterforce-1.1.0/
sudo dkms install gigabyte_waterforce/1.1.0

# swap the running module for the new one (or just reboot)
sudo modprobe -r gigabyte_waterforce && sudo modprobe gigabyte_waterforce
modinfo gigabyte_waterforce | grep version    # should print 1.1.0-control
```

DKMS installs into `updates/`, which takes priority over the in-kernel module.
If an older version was installed, remove it first with
`sudo dkms remove gigabyte_waterforce/1.0.0 --all`.

### Quick test without installing

```sh
make load      # builds, unloads the stock module, loads this one until reboot
sudo ./test-pump.sh
```

`make load` does not install anything. After a reboot the stock driver is back.

## Try it by hand

```sh
cd $(grep -l '^waterforce$' /sys/class/hwmon/*/name | xargs dirname)

echo max     | sudo tee pump_profile   # pump to max
echo quiet   | sudo tee fan_profile    # quiet fan curve
echo 255     | sudo tee pwm2           # same as pump max, manual mode
cat fan_profile                        # zero [quiet] balance performance max default
```

Allow a couple of seconds after a change before reading `fanN_input`. The
reading is cached for 2 s.

## CoolerControl

After the module is loaded, restart the daemon
(`sudo systemctl restart coolercontrold`). Fan speed and Pump speed then show
as controllable. A fixed speed or a curve works, but the result snaps to the
profile steps in the table above. For example, any pump setting up to 81 % gives
"balance" and anything above gives "max".

## Notes and known gaps

* Only the first-generation X series (`1044:7a4d`) is supported. The WATERFORCE
  II (`1044:7a5e`) and the non-X models use a different protocol.
* After a module load the driver assumes `balance` on both channels. It cannot
  ask the device which profile is actually active. The first profile request is
  always sent.
* LCD and RGB are not handled here. Use userspace tools for them.
