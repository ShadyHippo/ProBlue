// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2009  Bastien Nocera <hadess@hadess.net>
 *  Copyright (C) 2011  Antonio Ospite <ospite@studenti.unina.it>
 *  Copyright (C) 2013  Szymon Janc <szymon.janc@gmail.com>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <glib.h>
#include <libudev.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/hci.h"
#include "bluetooth/sdp.h"
#include "bluetooth/uuid.h"

#include "profiles/input/procon.h"

#include "src/adapter.h"
#include "src/device.h"
#include "src/agent.h"
#include "src/plugin.h"
#include "src/log.h"
#include "src/shared/util.h"
#include "profiles/input/server.h"
#include "profiles/input/sixaxis.h"

struct authentication_closure {
	guint auth_id;
	char *sysfs_path;
	struct btd_adapter *adapter;
	struct btd_device *device;
	int fd;
	bdaddr_t bdaddr; /* device bdaddr */
	CablePairingType type;
	bool existing; /* device record pre-existed; never remove it */
};

struct authentication_destroy_closure {
	struct authentication_closure *closure;
	bool remove_device;
};

static struct udev *ctx = NULL;
static struct udev_monitor *monitor = NULL;
static guint watch_id = 0;
/* key = sysfs_path (const str), value = auth_closure */
static GHashTable *pending_auths = NULL;

#define SIXAXIS_HID_SDP_RECORD "3601920900000A000100000900013503191124090004"\
	"350D35061901000900113503190011090006350909656E09006A090100090009350"\
	"8350619112409010009000D350F350D350619010009001335031900110901002513"\
	"576972656C65737320436F6E74726F6C6C65720901012513576972656C657373204"\
	"36F6E74726F6C6C6572090102251B536F6E7920436F6D707574657220456E746572"\
	"7461696E6D656E74090200090100090201090100090202080009020308210902042"\
	"8010902052801090206359A35980822259405010904A101A1028501750895011500"\
	"26FF00810375019513150025013500450105091901291381027501950D0600FF810"\
	"3150026FF0005010901A10075089504350046FF0009300931093209358102C00501"\
	"75089527090181027508953009019102750895300901B102C0A1028502750895300"\
	"901B102C0A10285EE750895300901B102C0A10285EF750895300901B102C0C00902"\
	"07350835060904090901000902082800090209280109020A280109020B090100090"\
	"20C093E8009020D280009020E2800"

/* Make sure to unset auth_id if already handled */
static void auth_closure_destroy(struct authentication_closure *closure,
				bool remove_device)
{
	if (closure->auth_id)
		btd_cancel_authorization(closure->auth_id);

	if (remove_device)
		btd_adapter_remove_device(closure->adapter, closure->device);
	close(closure->fd);
	g_free(closure->sysfs_path);
	g_free(closure);
}

static int sixaxis_get_device_bdaddr(int fd, bdaddr_t *bdaddr)
{
	uint8_t buf[18];
	int ret;

	memset(buf, 0, sizeof(buf));

	buf[0] = 0xf2;

	ret = ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf);
	if (ret < 0) {
		error("sixaxis: failed to read device address (%s)",
							strerror(errno));
		return ret;
	}

	baswap(bdaddr, (bdaddr_t *) (buf + 4));

	return 0;
}

static int ds4_get_device_bdaddr(int fd, bdaddr_t *bdaddr)
{
	uint8_t buf[7];
	int ret;

	memset(buf, 0, sizeof(buf));

	buf[0] = 0x81;

	ret = ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf);
	if (ret < 0) {
		error("sixaxis: failed to read DS4 device address (%s)",
		      strerror(errno));
		return ret;
	}

	/* address is little-endian on DS4 */
	bacpy(bdaddr, (bdaddr_t*) (buf + 1));

	return 0;
}

static int get_device_bdaddr(int fd, bdaddr_t *bdaddr, CablePairingType type)
{
	if (type == CABLE_PAIRING_SIXAXIS)
		return sixaxis_get_device_bdaddr(fd, bdaddr);
	else if (type == CABLE_PAIRING_DS4)
		return ds4_get_device_bdaddr(fd, bdaddr);
	else if (type == CABLE_PAIRING_PROCON)
		return procon_get_device_bdaddr(fd, bdaddr);
	return -1;
}

static int sixaxis_get_central_bdaddr(int fd, bdaddr_t *bdaddr)
{
	uint8_t buf[8];
	int ret;

	memset(buf, 0, sizeof(buf));

	buf[0] = 0xf5;

	ret = ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf);
	if (ret < 0) {
		error("sixaxis: failed to read central address (%s)",
							strerror(errno));
		return ret;
	}

	baswap(bdaddr, (bdaddr_t *) (buf + 2));

	return 0;
}

static int ds4_get_central_bdaddr(int fd, bdaddr_t *bdaddr)
{
	uint8_t buf[16];
	int ret;

	memset(buf, 0, sizeof(buf));

	buf[0] = 0x12;

	ret = ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf);
	if (ret < 0) {
		error("sixaxis: failed to read DS4 central address (%s)",
		      strerror(errno));
		return ret;
	}

	/* address is little-endian on DS4 */
	bacpy(bdaddr, (bdaddr_t*) (buf + 10));

	return 0;
}

static int get_central_bdaddr(int fd, bdaddr_t *bdaddr, CablePairingType type)
{
	if (type == CABLE_PAIRING_SIXAXIS)
		return sixaxis_get_central_bdaddr(fd, bdaddr);
	else if (type == CABLE_PAIRING_DS4)
		return ds4_get_central_bdaddr(fd, bdaddr);
	/* CABLE_PAIRING_PROCON: the stored-central read is done by read-then-decide
	 * (subcmd 0x10 on x2000, in procon.c); this signature cannot return a key,
	 * so the dispatcher here never asks. */
	return -1;
}

static int sixaxis_set_central_bdaddr(int fd, const bdaddr_t *bdaddr)
{
	uint8_t buf[8];
	int ret;

	buf[0] = 0xf5;
	buf[1] = 0x01;

	baswap((bdaddr_t *) (buf + 2), bdaddr);

	ret = ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf);
	if (ret < 0)
		error("sixaxis: failed to write central address (%s)",
							strerror(errno));

	return ret;
}

static int ds4_set_central_bdaddr(int fd, const bdaddr_t *bdaddr)
{
	uint8_t buf[23];
	int ret;

	buf[0] = 0x13;
	bacpy((bdaddr_t*) (buf + 1), bdaddr);
	/* TODO: we could put the key here but
	   there is no way to force a re-loading
	   of link keys to the kernel from here. */
	memset(buf + 7, 0, 16);

	ret = ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf);
	if (ret < 0)
		error("sixaxis: failed to write DS4 central address (%s)",
		      strerror(errno));

	return ret;
}

static int set_central_bdaddr(int fd, const bdaddr_t *bdaddr,
					CablePairingType type)
{
	if (type == CABLE_PAIRING_SIXAXIS)
		return sixaxis_set_central_bdaddr(fd, bdaddr);
	else if (type == CABLE_PAIRING_DS4)
		return ds4_set_central_bdaddr(fd, bdaddr);
	/* CABLE_PAIRING_PROCON: handled in agent_auth_cb via procon_pair() / the
	 * read-then-decide path (it returns the LTK, which this signature cannot). */
	return -1;
}

static bool is_auth_pending(struct authentication_closure *closure)
{
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, pending_auths);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		struct authentication_closure *c = value;
		if (c == closure)
			return true;
	}
	return false;
}

static gboolean auth_closure_destroy_idle(gpointer user_data)
{
	struct authentication_destroy_closure *destroy = user_data;

	auth_closure_destroy(destroy->closure, destroy->remove_device);
	g_free(destroy);

	return false;
}

static void agent_auth_cb(DBusError *derr, void *user_data)
{
	struct authentication_closure *closure = user_data;
	struct authentication_destroy_closure *destroy;
	char central_addr[18], adapter_addr[18], device_addr[18];
	bdaddr_t central_bdaddr;
	const bdaddr_t *adapter_bdaddr;
	/* Only remove a device created in this flow; a pre-existing record (a
	 * known controller being re-paired) must survive a failed repair. */
	bool remove_device = !closure->existing;

	if (!is_auth_pending(closure))
		return;

	/* Don't try to remove this auth, we're handling it already */
	closure->auth_id = 0;

	if (derr != NULL) {
		DBG("Agent replied negatively, removing temporary device");
		goto out;
	}

	adapter_bdaddr = btd_adapter_get_address(closure->adapter);

	if (closure->type == CABLE_PAIRING_PROCON) {
		uint8_t ltk[16];

		/*
		 * Wired pairing: read-then-decide on x2000, falling back to the
		 * 3-step when the controller is not paired to us yet. Register
		 * the resulting key with bluetoothd and the kernel before the
		 * controller connects — the controller authenticates with the
		 * stored key, no over-the-air SSP on reconnect.
		 */
		if (procon_acquire_ltk(closure->fd, adapter_bdaddr, ltk) < 0)
			goto out;

		btd_adapter_store_link_key(closure->adapter, closure->device,
					ltk, HCI_LK_UNAUTH_COMBINATION, 0);
		info("procon: link key stored");

		/* No stored-central comparison: read-then-decide owns that, and
		 * the 3-step rewrote the pairing to us. */
		bacpy(&central_bdaddr, adapter_bdaddr);
	} else {
		if (get_central_bdaddr(closure->fd, &central_bdaddr,
							closure->type) < 0)
			goto out;

		if (bacmp(adapter_bdaddr, &central_bdaddr)) {
			if (set_central_bdaddr(closure->fd, adapter_bdaddr,
								closure->type) < 0)
				goto out;
		}
	}

	remove_device = false;
	btd_device_set_temporary(closure->device, false);

	if (closure->type == CABLE_PAIRING_SIXAXIS) {
		btd_device_set_record(closure->device, HID_UUID,
						 SIXAXIS_HID_SDP_RECORD);
	} else if (closure->type == CABLE_PAIRING_PROCON) {
		btd_device_set_record(closure->device, HID_UUID,
						 PROCON_HID_SDP_RECORD);

		/* A cable-paired Pro Controller has a stored BR/EDR link key
		 * (btd_adapter_store_link_key above) — mark it exactly like a
		 * device loaded from storage with a key (adapter.c:
		 * device_create_from_storage → device_set_paired + device_set_bonded).
		 * Without this the device stays Paired: no / Bonded: no — UIs
		 * keep showing "Pair", and the power-on accept-list re-add (bonded
		 * BR/EDR devices only) skips it, so a wake page isn't heard after
		 * a power cycle. Do not apply to SIXAXIS/DS4: those bond over the
		 * air and must not be pre-marked.
		 */
		device_set_paired(closure->device, BDADDR_BREDR);
		device_set_bonded(closure->device, BDADDR_BREDR);
	}

	device_set_cable_pairing(closure->device, true);

	server_set_cable_pairing(adapter_bdaddr, true);

	ba2str(&closure->bdaddr, device_addr);
	ba2str(&central_bdaddr, central_addr);
	ba2str(adapter_bdaddr, adapter_addr);
	info("sixaxis: cable pairing complete (remote %s old_central %s "
	     "new_central %s)", device_addr, central_addr, adapter_addr);

out:
	g_hash_table_steal(pending_auths, closure->sysfs_path);

	/* btd_adapter_remove_device() cannot be called in this
	 * callback or it would lead to a double-free in while
	 * trying to cancel the authentication that's being processed,
	 * so clean up in an idle */
	destroy = g_new0(struct authentication_destroy_closure, 1);
	destroy->closure = closure;
	destroy->remove_device = remove_device;
	g_idle_add(auth_closure_destroy_idle, destroy);
}

static bool setup_device(int fd, const char *sysfs_path,
			const struct cable_pairing *cp,
			struct btd_adapter *adapter)
{
	bdaddr_t device_bdaddr;
	const bdaddr_t *adapter_bdaddr;
	struct btd_device *device;
	struct authentication_closure *closure;
	bool existing;

	if (cp->type == CABLE_PAIRING_PROCON) {
		bdaddr_t conn_bdaddr;

		/*
		 * Find out which controller this is before touching the wired
		 * session. USB command 0x01 is a plain status query: it returns
		 * the controller's address without starting the UART session,
		 * so a controller that is already connected over Bluetooth
		 * keeps its link. Starting the session instead (0x02/0x03/0x02)
		 * makes the controller commit to USB and terminate that link,
		 * which is what used to happen when a working controller was
		 * plugged in to charge.
		 *
		 * A controller that does not answer the query falls through to
		 * the session below, exactly as before.
		 */
		if (procon_get_conn_status(fd, &conn_bdaddr) == 0) {
			struct btd_device *conn_device;

			conn_device = btd_adapter_find_device(adapter,
						&conn_bdaddr, BDADDR_BREDR);
			if (conn_device &&
				btd_device_has_uuid(conn_device, HID_UUID) &&
				btd_device_is_connected(conn_device)) {
				char conn_addr[18];

				ba2str(&conn_bdaddr, conn_addr);
				info("procon: %s is connected over Bluetooth, "
					"leaving it alone", conn_addr);
				return false;
			}
		}

		/*
		 * The controller is not connected, so the wired session is
		 * safe: there is no Bluetooth link left to lose. hid-nintendo's
		 * passive probe never starts it itself.
		 */
		if (procon_usb_session_init(fd) < 0)
			return false;
	}

	/* Console order: device info first, then the wired arm. */
	if (get_device_bdaddr(fd, &device_bdaddr, cp->type) < 0)
		return false;

	if (cp->type == CABLE_PAIRING_PROCON) {
		/* Re-arm on every plug-in, like the Switch. A failure here is
		 * not fatal: the controller is already paired, and the
		 * connectable update below has to run either way. */
		if (procon_arm_wired(fd) < 0)
			error("procon: wired arm failed (non-fatal)");

		/* A controller woken by a button press pages the host, so the
		 * host must keep page-scanning; the GUI only makes it
		 * connectable while discoverable. */
		btd_adapter_set_connectable(adapter, true);
	}

	/*
	 * A controller that is connected to us right now needs nothing: it is
	 * already running on the stored key. Anything else is re-checked over
	 * USB. That matters for the Pro Controller in particular: its flash has
	 * a single pairing slot, so another host may have replaced our record
	 * since the last dock. read-then-decide reuses the key when it is still
	 * ours and runs the 3-step when it is not, so a trusted-but-idle Pro
	 * Controller must not be skipped (Sony controllers keep the old
	 * trusted-skip behaviour).
	 */
	device = btd_adapter_find_device(adapter, &device_bdaddr,
							BDADDR_BREDR);
	if (device && btd_device_has_uuid(device, HID_UUID) &&
			(btd_device_is_connected(device) ||
			 (cp->type != CABLE_PAIRING_PROCON &&
			  btd_device_is_trusted(device)))) {
		char device_addr[18];
		ba2str(&device_bdaddr, device_addr);
		DBG("device %s already known, skipping", device_addr);
		return false;
	}

	existing = device != NULL;

	device = btd_adapter_get_device(adapter, &device_bdaddr, BDADDR_BREDR);

	if (!device) {
		error("sixaxis: unable to set up a new device");
		return false;
	}

	info("sixaxis: %s device",
			existing ? "re-checking known" : "setting up new");

	btd_device_device_set_name(device, cp->name);
	btd_device_set_pnpid(device, cp->source, cp->vid, cp->pid, cp->version);

	/* A record we already have stays permanent: if the repair fails, the
	 * user's pairing must survive (see remove_device in agent_auth_cb). */
	if (!existing)
		btd_device_set_temporary(device, true);

	/*
	 * Physical access to the cable is the authorization, so trust the
	 * device before asking for cable authorization: the request is then
	 * auto-approved and no agent prompt appears. Without this every
	 * connection fails as "without agent".
	 */
	btd_device_set_trusted(device, true);

	closure = g_new0(struct authentication_closure, 1);
	if (!closure) {
		if (!existing)
			btd_adapter_remove_device(adapter, device);
		return false;
	}
	closure->adapter = adapter;
	closure->device = device;
	closure->sysfs_path = g_strdup(sysfs_path);
	closure->fd = fd;
	bacpy(&closure->bdaddr, &device_bdaddr);
	closure->type = cp->type;
	closure->existing = existing;
	adapter_bdaddr = btd_adapter_get_address(adapter);
	closure->auth_id = btd_request_authorization_cable_configured(
					adapter_bdaddr, &device_bdaddr,
					HID_UUID, agent_auth_cb, closure);

	if (closure->auth_id == 0) {
		error("sixaxis: could not request cable authorization");
		auth_closure_destroy(closure, !existing);
		return false;
	}

	g_hash_table_insert(pending_auths, closure->sysfs_path, closure);

	return true;
}

static const struct cable_pairing *
get_pairing_type_for_device(struct udev_device *udevice, uint16_t *bus,
						char **sysfs_path)
{
	struct udev_device *hid_parent;
	const char *hid_name;
	const char *hid_id;
	const struct cable_pairing *cp;
	uint16_t vid, pid;

	hid_parent = udev_device_get_parent_with_subsystem_devtype(udevice,
								"hid", NULL);
	if (!hid_parent)
		return NULL;

	hid_id = udev_device_get_property_value(hid_parent, "HID_ID");

	if (!hid_id || sscanf(hid_id, "%hx:%hx:%hx", bus, &vid, &pid) != 3)
		return NULL;

	hid_name = udev_device_get_property_value(hid_parent, "HID_NAME");

	cp = get_pairing(vid, pid, hid_name);
	if (!cp)
		cp = get_nintendo_pairing(vid, pid, hid_name);
	*sysfs_path = g_strdup(udev_device_get_syspath(udevice));

	return cp;
}

static void device_added(struct udev_device *udevice)
{
	struct btd_adapter *adapter;
	uint16_t bus;
	char *sysfs_path = NULL;
	const struct cable_pairing *cp;
	int fd;

	adapter = btd_adapter_get_default();
	if (!adapter)
		return;

	cp = get_pairing_type_for_device(udevice, &bus, &sysfs_path);
	if (!cp || (cp->type != CABLE_PAIRING_SIXAXIS &&
			cp->type != CABLE_PAIRING_DS4 &&
			cp->type != CABLE_PAIRING_PROCON)) {
		g_free(sysfs_path);
		return;
	}

	if (bus != BUS_USB) {
		g_free(sysfs_path);
		return;
	}

	info("sixaxis: compatible device connected: %s (%04X:%04X %s)",
				cp->name, cp->vid, cp->pid, sysfs_path);

	fd = open(udev_device_get_devnode(udevice), O_RDWR);
	if (fd < 0) {
		g_free(sysfs_path);
		return;
	}

	/* Only close the fd if an authentication is not pending */
	if (!setup_device(fd, sysfs_path, cp, adapter))
		close(fd);

	g_free(sysfs_path);
}

static void device_removed(struct udev_device *udevice)
{
	struct authentication_closure *closure;
	const char *sysfs_path;

	sysfs_path = udev_device_get_syspath(udevice);
	if (!sysfs_path)
		return;

	closure = g_hash_table_lookup(pending_auths, sysfs_path);
	if (!closure)
		return;

	g_hash_table_steal(pending_auths, sysfs_path);
	/* Unplug during the handshake: a pre-existing pairing survives. */
	auth_closure_destroy(closure, !closure->existing);
}

static gboolean monitor_watch(GIOChannel *source, GIOCondition condition,
							gpointer data)
{
	struct udev_device *udevice;

	udevice = udev_monitor_receive_device(monitor);
	if (!udevice)
		return TRUE;

	if (!g_strcmp0(udev_device_get_action(udevice), "add"))
		device_added(udevice);
	else if (!g_strcmp0(udev_device_get_action(udevice), "remove"))
		device_removed(udevice);

	udev_device_unref(udevice);

	return TRUE;
}

static int sixaxis_init(void)
{
	GIOChannel *channel;

	DBG("");

	ctx = udev_new();
	if (!ctx)
		return -EIO;

	monitor = udev_monitor_new_from_netlink(ctx, "udev");
	if (!monitor) {
		udev_unref(ctx);
		ctx = NULL;

		return -EIO;
	}

	/* Listen for newly connected hidraw interfaces */
	udev_monitor_filter_add_match_subsystem_devtype(monitor, "hidraw",
									NULL);
	udev_monitor_enable_receiving(monitor);

	channel = g_io_channel_unix_new(udev_monitor_get_fd(monitor));
	watch_id = g_io_add_watch(channel, G_IO_IN, monitor_watch, NULL);
	g_io_channel_unref(channel);

	pending_auths = g_hash_table_new(g_str_hash,
					g_str_equal);

	return 0;
}

static void sixaxis_exit(void)
{
	GHashTableIter iter;
	gpointer value;

	DBG("");

	g_hash_table_iter_init(&iter, pending_auths);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		struct authentication_closure *closure = value;
		auth_closure_destroy(closure, true);
	}
	g_hash_table_destroy(pending_auths);
	pending_auths = NULL;

	g_source_remove(watch_id);
	watch_id = 0;

	udev_monitor_unref(monitor);
	monitor = NULL;

	udev_unref(ctx);
	ctx = NULL;
}

BLUETOOTH_PLUGIN_DEFINE(sixaxis, VERSION, BLUETOOTH_PLUGIN_PRIORITY_LOW,
						sixaxis_init, sixaxis_exit)
