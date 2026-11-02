# Bouffalo SDK patches

Apply from the SDK folder (`$BL_SDK_BASE`):

```sh
git -C "$BL_SDK_BASE" apply /path/to/fpga_firmware-bl616/patches/cherryusb-hub-nports-clamp.patch
```

## cherryusb-hub-nports-clamp.patch

Fixes a crash when a USB hub has more ports than `CONFIG_USBHOST_MAX_EHPORTS`
(7, set in `usb/usb_config.h`).

The SDK's CherryUSB hub driver (`components/usb/cherryusb/class/hub/usbh_hub.c`)
stores the hub's port count in `hub->nports`, then checks it against
`CONFIG_USBHOST_MAX_EHPORTS` and returns an error. `usbh_hub_disconnect()` then
loops over `hub->nports` and releases hubports past the end of `child[]`. On the
BL616 this ends in a FreeRTOS assert in `xQueueSemaphoreTake`, called from the
`usbh_hub0` task, and the firmware stops.

The patch clamps the port count instead of failing, so a bigger hub works on its
first `CONFIG_USBHOST_MAX_EHPORTS` ports. The hub status read is 1 byte, so only
ports 1-7 are ever reported anyway. Upstream CherryUSB master avoids the crash
differently: it rejects the hub.

Same patch as in [inindev/fpga_companion](https://github.com/inindev/fpga_companion)
(`src/bl616/patches/`), tested on SDK commit `237ed9378`.
