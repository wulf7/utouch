KMOD=	utouch
SRCS=	opt_kbd.h opt_usb.h bus_if.h device_if.h usbdevs.h utouch.c

.include <bsd.kmod.mk>
