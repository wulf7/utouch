/*-
 * Copyright (c) 2014, Jakub Wojciech Klama <jceel@FreeBSD.org>
 * Copyright (c) 2018, Vladimir Kondratyev <wulf@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/stddef.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#if __FreeBSD_version >= 1300134
#include <dev/hid/hid.h>
#endif

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbhid.h>
#include "usbdevs.h"

#define	USB_DEBUG_VAR utouch_debug
#include <dev/usb/usb_debug.h>

#include <dev/usb/quirk/usb_quirk.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

static int utouch_debug = 0;
static SYSCTL_NODE(_hw_usb, OID_AUTO, utouch, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "USB touch");
SYSCTL_INT(_hw_usb_utouch, OID_AUTO, debug, CTLFLAG_RWTUN, &utouch_debug, 0,
    "Debug level");

enum {
	UTOUCH_INTR_DT,
	UTOUCH_N_TRANSFER,
};

struct utouch_absinfo {
	int32_t min;
	int32_t max;
	int32_t res;
};

struct utouch_softc
{
	device_t sc_dev;
	struct evdev_dev *sc_evdev;
	struct mtx sc_mtx;
	struct usb_xfer *sc_xfer[UTOUCH_N_TRANSFER];
	struct hid_location sc_loc_x;
	struct hid_location sc_loc_y;
	struct hid_location sc_loc_z;
#define	UTOUCH_BUTTON_MAX	8
	struct hid_location sc_loc_btn[UTOUCH_BUTTON_MAX];
	struct utouch_absinfo sc_ai_x;
	struct utouch_absinfo sc_ai_y;
	uint8_t	sc_iid_x;
	uint8_t	sc_iid_y;
	uint8_t	sc_iid_z;
	uint8_t	sc_iid_btn[UTOUCH_BUTTON_MAX];
	uint8_t	sc_nbuttons;
	uint32_t sc_flags;
#define	UTOUCH_FLAG_X_AXIS	0x0001
#define	UTOUCH_FLAG_Y_AXIS	0x0002
#define	UTOUCH_FLAG_Z_AXIS	0x0004
#define	UTOUCH_FLAG_OPENED	0x0008

	uint8_t	sc_temp[64];
};

static usb_callback_t utouch_intr_callback;

static device_probe_t utouch_probe;
static device_attach_t utouch_attach;
static device_detach_t utouch_detach;

static int utouch_hid_test(const void *, uint16_t);
static void utouch_hid_parse(struct utouch_softc *, const void *, uint16_t);

#if __FreeBSD_version >= 1200077
static evdev_open_t utouch_ev_open;
static evdev_close_t utouch_ev_close;
#else
static evdev_open_t utouch_ev_open_11;
static evdev_close_t utouch_ev_close_11;
#endif

static const struct evdev_methods utouch_evdev_methods = {
#if __FreeBSD_version >= 1200077
	.ev_open = &utouch_ev_open,
	.ev_close = &utouch_ev_close,
#else
	.ev_open = &utouch_ev_open_11,
	.ev_close = &utouch_ev_close_11,
#endif
};

static const struct usb_config utouch_config[UTOUCH_N_TRANSFER] = {

	[UTOUCH_INTR_DT] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.flags = {.pipe_bof = 1,.short_xfer_ok = 1,},
		.bufsize = 0,	/* use wMaxPacketSize */
		.callback = &utouch_intr_callback,
	},
};

static int
utouch_probe(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	void *d_ptr;
	uint16_t d_len;
	int err;

	if (uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);

	if (uaa->info.bInterfaceClass != UICLASS_HID)
		return (ENXIO);

	err = usbd_req_get_hid_desc(uaa->device, NULL,
	    &d_ptr, &d_len, M_TEMP, uaa->info.bIfaceIndex);
	if (err != USB_ERR_NORMAL_COMPLETION)
		return (ENXIO);

	if (utouch_hid_test(d_ptr, d_len))
		err = BUS_PROBE_DEFAULT;
	else
		err = ENXIO;

	free(d_ptr, M_TEMP);
	return (err);
}

static int
utouch_attach(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct utouch_softc *sc = device_get_softc(dev);
	void *d_ptr = NULL;
	uint16_t d_len;
	int i, err;

	device_set_usb_desc(dev);
	sc->sc_dev = dev;

	mtx_init(&sc->sc_mtx, "utouch lock", NULL, MTX_DEF | MTX_RECURSE);

	err = usbd_transfer_setup(uaa->device,
	    &uaa->info.bIfaceIndex, sc->sc_xfer, utouch_config,
	    UTOUCH_N_TRANSFER, sc, &sc->sc_mtx);
	if (err != USB_ERR_NORMAL_COMPLETION)
		goto detach;

	err = usbd_req_get_hid_desc(uaa->device, NULL, &d_ptr,
	    &d_len, M_TEMP, uaa->info.bIfaceIndex);
	if (err != USB_ERR_NORMAL_COMPLETION)
		goto detach;

	utouch_hid_parse(sc, d_ptr, d_len);
	free(d_ptr, M_TEMP);

	sc->sc_evdev = evdev_alloc();
	evdev_set_name(sc->sc_evdev, device_get_desc(dev));
	evdev_set_phys(sc->sc_evdev, device_get_nameunit(dev));
	evdev_set_id(sc->sc_evdev, BUS_USB, uaa->info.idVendor,
	    uaa->info.idProduct, 0);
	evdev_set_serial(sc->sc_evdev, usb_get_serial(uaa->device));
	evdev_set_methods(sc->sc_evdev, sc, &utouch_evdev_methods);
	evdev_support_prop(sc->sc_evdev, INPUT_PROP_DIRECT);
	evdev_support_event(sc->sc_evdev, EV_SYN);
	evdev_support_event(sc->sc_evdev, EV_ABS);
	evdev_support_event(sc->sc_evdev, EV_REL);
	evdev_support_event(sc->sc_evdev, EV_KEY);

	/* Report absolute axes information */
	if (sc->sc_flags & UTOUCH_FLAG_X_AXIS)
#if __FreeBSD_version >= 1300134
		evdev_support_abs(sc->sc_evdev, ABS_X, sc->sc_ai_x.min,
		    sc->sc_ai_x.max, 0, 0, sc->sc_ai_x.res);
#else
		evdev_support_abs(sc->sc_evdev, ABS_X, 0, sc->sc_ai_x.min,
		    sc->sc_ai_x.max, 0, 0, sc->sc_ai_x.res);
#endif
	if (sc->sc_flags & UTOUCH_FLAG_Y_AXIS)
#if __FreeBSD_version >= 1300134
		evdev_support_abs(sc->sc_evdev, ABS_Y, sc->sc_ai_y.min,
		    sc->sc_ai_y.max, 0, 0, sc->sc_ai_y.res);
#else
		evdev_support_abs(sc->sc_evdev, ABS_Y, 0, sc->sc_ai_y.min,
		    sc->sc_ai_y.max, 0, 0, sc->sc_ai_y.res);
#endif

	if (sc->sc_flags & UTOUCH_FLAG_Z_AXIS)
		evdev_support_rel(sc->sc_evdev, REL_WHEEL);

	for (i = 0; i < sc->sc_nbuttons; i++)
		evdev_support_key(sc->sc_evdev, BTN_MOUSE + i);

	err = evdev_register_mtx(sc->sc_evdev, &sc->sc_mtx);
	if (err)
		goto detach;

	return (0);

detach:
	utouch_detach(dev);
	return (ENXIO);
}

static int
utouch_detach(device_t dev)
{
	struct utouch_softc *sc = device_get_softc(dev);

	evdev_free(sc->sc_evdev);
	usbd_transfer_unsetup(sc->sc_xfer, UTOUCH_N_TRANSFER);
	mtx_destroy(&sc->sc_mtx);
	return (0);
}

static void
utouch_intr_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct utouch_softc *sc = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;
	uint8_t *buf = sc->sc_temp;
	uint8_t id;
	int len, i;

	usbd_xfer_status(xfer, &len, NULL, NULL, NULL);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		if (len > (int)sizeof(sc->sc_temp)) {
			DPRINTFN(6, "truncating large packet to %zu bytes\n",
			    sizeof(sc->sc_temp));
			len = sizeof(sc->sc_temp);
		}
		if (len == 0)
			goto tr_setup;

		pc = usbd_xfer_get_frame(xfer, 0);
		usbd_copy_out(pc, 0, buf, len);

		id = 0;
		if (sc->sc_iid_x > 0 || sc->sc_iid_y > 0) {
			id = *buf;
			len--;
			buf++;
                }

		if (sc->sc_flags & UTOUCH_FLAG_X_AXIS && id == sc->sc_iid_x)
			evdev_push_abs(sc->sc_evdev, ABS_X,
			    hid_get_data(buf, len, &sc->sc_loc_x));

		if (sc->sc_flags & UTOUCH_FLAG_Y_AXIS && id == sc->sc_iid_y)
			evdev_push_abs(sc->sc_evdev, ABS_Y,
			    hid_get_data(buf, len, &sc->sc_loc_y));

		if (sc->sc_flags & UTOUCH_FLAG_Z_AXIS && id == sc->sc_iid_z)
			evdev_push_rel(sc->sc_evdev, REL_WHEEL,
			    hid_get_data(buf, len, &sc->sc_loc_z));

		for (i = 0; i < sc->sc_nbuttons; i++)
			if (id == sc->sc_iid_btn[i])
				evdev_push_key(sc->sc_evdev, BTN_MOUSE + i,
				    hid_get_data(buf, len, &sc->sc_loc_btn[i]));

		evdev_sync(sc->sc_evdev);

	case USB_ST_SETUP:
tr_setup:
		usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);
		break;
	default:
		if (error != USB_ERR_CANCELLED) {
			/* try clear stall first */
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		break;
	}
}

static void
utouch_ev_close_11(struct evdev_dev *evdev, void *ev_softc)
{
	struct utouch_softc *sc = ev_softc;

	mtx_assert(&sc->sc_mtx, MA_OWNED);
	usbd_transfer_stop(sc->sc_xfer[UTOUCH_INTR_DT]);
}

static int
utouch_ev_open_11(struct evdev_dev *evdev, void *ev_softc)
{
	struct utouch_softc *sc = ev_softc;

        mtx_assert(&sc->sc_mtx, MA_OWNED);
	usbd_transfer_start(sc->sc_xfer[UTOUCH_INTR_DT]);

        return (0);
}

#if __FreeBSD_version >= 1200077
static int
utouch_ev_close(struct evdev_dev *evdev)
{
	struct utouch_softc *sc = evdev_get_softc(evdev);

	utouch_ev_close_11(evdev, sc);

	return (0);
}

static int
utouch_ev_open(struct evdev_dev *evdev)
{
	struct utouch_softc *sc = evdev_get_softc(evdev);

	return (utouch_ev_open_11(evdev, sc));
}
#endif

static int
utouch_hid_test(const void *d_ptr, uint16_t d_len)
{
	struct hid_data *hd;
	struct hid_item hi;
	int mdepth;
	int found;

	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	if (hd == NULL)
		return (0);

	mdepth = 0;
	found = 0;

	while (hid_get_item(hd, &hi)) {
		switch (hi.kind) {
		case hid_collection:
			if (mdepth != 0)
				mdepth++;
			else if (hi.collection == 1 &&
			     hi.usage ==
			      HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_MOUSE))
				mdepth++;
			break;
		case hid_endcollection:
			if (mdepth != 0)
				mdepth--;
			break;
		case hid_input:
			if (mdepth == 0)
				break;
			if (hi.usage ==
			     HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X) &&
			    (hi.flags & (HIO_CONST|HIO_VARIABLE|HIO_RELATIVE)) == HIO_VARIABLE)
				found++;
			if (hi.usage ==
			     HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y) &&
			    (hi.flags & (HIO_CONST|HIO_VARIABLE|HIO_RELATIVE)) == HIO_VARIABLE)
				found++;
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);
	return (found);
}

static void
utouch_hid_parse(struct utouch_softc *sc, const void *buf, uint16_t len)
{
	struct hid_data *hd;
	struct hid_item hi;
	int mdepth;
	uint32_t flags;
	uint8_t i;

	hd = hid_start_parse(buf, len, 1 << hid_input);
	if (hd == NULL)
		return;

	mdepth = 0;

	while (hid_get_item(hd, &hi)) {
		switch (hi.kind) {
		case hid_collection:
			if (mdepth != 0)
				mdepth++;
			else if (hi.collection == 1 &&
			     hi.usage ==
			      HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_MOUSE))
				mdepth++;
			break;
		case hid_endcollection:
			if (mdepth != 0)
				mdepth--;
			break;
		case hid_input:
			if (mdepth == 0)
				break;
			if (hi.usage ==
			     HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X) &&
			    (hi.flags & (HIO_CONST|HIO_VARIABLE|HIO_RELATIVE)) == HIO_VARIABLE) {
				sc->sc_flags |= UTOUCH_FLAG_X_AXIS;
				sc->sc_loc_x = hi.loc;
				sc->sc_iid_x = hi.report_ID;
				sc->sc_ai_x = (struct utouch_absinfo) {
					.max = hi.logical_maximum,
					.min = hi.logical_minimum,
					.res = hid_item_resolution(&hi),
				};
			}
			if (hi.usage ==
			     HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y) &&
			    (hi.flags & (HIO_CONST|HIO_VARIABLE|HIO_RELATIVE)) == HIO_VARIABLE) {
				sc->sc_flags |= UTOUCH_FLAG_Y_AXIS;
				sc->sc_loc_y = hi.loc;
				sc->sc_iid_y = hi.report_ID;
				sc->sc_ai_y = (struct utouch_absinfo) {
					.max = hi.logical_maximum,
					.min = hi.logical_minimum,
					.res = hid_item_resolution(&hi),
				};
			}
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);

	/* Try the wheel first as the Z activator since it's tradition. */
	if (hid_locate(buf, len,
	    HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_WHEEL),
	    hid_input, 0, &sc->sc_loc_z, &flags, &sc->sc_iid_z) ||
	    hid_locate(buf, len,
	    HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_TWHEEL),
	    hid_input, 0, &sc->sc_loc_z, &flags, &sc->sc_iid_z)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= UTOUCH_FLAG_Z_AXIS;
	}

	/* figure out the number of buttons */
	for (i = 0; i < UTOUCH_BUTTON_MAX; i++) {
		if (!hid_locate(buf, len, HID_USAGE2(HUP_BUTTON, (i + 1)),
		    hid_input, 0, &sc->sc_loc_btn[i], NULL,
		    &sc->sc_iid_btn[i])) {
			break;
		}
	}

	sc->sc_nbuttons = i;

	if (sc->sc_flags == 0)
		return;

	/* announce information about the mouse */
	device_printf(sc->sc_dev, "%d buttons and [%s%s%s] axes\n",
	    (sc->sc_nbuttons),
	    (sc->sc_flags & UTOUCH_FLAG_X_AXIS) ? "X" : "",
	    (sc->sc_flags & UTOUCH_FLAG_Y_AXIS) ? "Y" : "",
	    (sc->sc_flags & UTOUCH_FLAG_Z_AXIS) ? "Z" : "");
}

static const STRUCT_USB_HOST_ID utouch_devs[] = {
	/* generic HID class w/o boot interface */
	{USB_IFACE_CLASS(UICLASS_HID),
	 USB_IFACE_SUBCLASS(0),},
};

static devclass_t utouch_devclass;

static device_method_t utouch_methods[] = {
	DEVMETHOD(device_probe, utouch_probe),
	DEVMETHOD(device_attach, utouch_attach),
	DEVMETHOD(device_detach, utouch_detach),

	DEVMETHOD_END
};

static driver_t utouch_driver = {
	.name = "utouch",
	.methods = utouch_methods,
	.size = sizeof(struct utouch_softc),
};

DRIVER_MODULE(utouch, uhub, utouch_driver, utouch_devclass, NULL, 0);
MODULE_DEPEND(utouch, usb, 1, 1, 1);
#if __FreeBSD_version >= 1300134
MODULE_DEPEND(utouch, hid, 1, 1, 1);
#endif
MODULE_DEPEND(utouch, evdev, 1, 1, 1);
MODULE_VERSION(utouch, 1);
USB_PNP_HOST_INFO(utouch_devs);
