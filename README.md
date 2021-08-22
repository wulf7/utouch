# utouch - FreeBSD driver for absolute USB HID mices

utouch - driver for absolute USB HID mices emulated by some virtualization
systems like Virtual Box, BHyve etc. It uses evdev protocol to communicate
with userland applications like libinput and xf86-input-evdev. The driver
should be installed in to the guest FreeBSD system. Host system should be
configured to emulate mouse as single-touch USB tablet.

System requirements:	FreeBSD 11.2+

To build driver, cd in to extracted archive directory and type **make**

To install driver already built type "**make install**" as root.
/boot/modules/utouch.ko file will be created after that.

To load installed driver in to the kernel: "**kldload 
/boot/modules/utouch.ko**".
It may be necessary to unload conflicting uhid driver with "**kldunload uhid**"
**after** utouch.ko has been loaded.

To load driver automaticaly at the boot time add **utouch_load="YES"** string
to **/boot/loader.conf** file.

**Note:** This driver is deprecated on FreeBSD 13+. Please use **hms(4)**
bundled with base system. It is disabled by default and can be enabled with
adding of following lines to **/boot/loader.conf**:
```
hw.usb.usbhid.enable=1
usbhid_load="YES"
```
