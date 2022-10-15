/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/zephyr.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/drivers/gpio.h>
/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static struct gpio_dt_spec led_one = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios,
						     {0});
static struct gpio_dt_spec led_two = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios,
						     {0});

void configure_led(struct gpio_dt_spec l) {
	int ret;
	if (l.port && !device_is_ready(l.port)) {
		printk("Error %d: LED device %s is not ready; ignoring it\n",
		       ret, l.port->name);
		l.port = NULL;
	}
	if (l.port) {
		ret = gpio_pin_configure_dt(&l, GPIO_OUTPUT);
		if (ret != 0) {
			printk("Error %d: failed to configure LED device %s pin %d\n",
			       ret, l.port->name, l.pin);
			l.port = NULL;
		} else {
			printk("Set up LED at %s pin %d\n", l.port->name, l.pin);
		}
	}
}


static void start_scan(void);

static struct bt_conn *default_conn;


static struct bt_uuid_16 uuid = BT_UUID_INIT_16(0);
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	if (!data) {
		// printk("[UNSUBSCRIBED]\n");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	printk("[NOTIFICATION] data %p length %u\n", data, length);

	return BT_GATT_ITER_CONTINUE;
}
static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	printk("Discover called?\n");
	int err;

	if (!attr) {
		printk("Discover complete\n");
		(void)memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	printk("[ATTRIBUTE] handle %u\n", attr->handle);

	if (attr->uuid->type == 0) {
		printk("[ATTRIBUTE] uuid type: %02X, %04x\n", attr->uuid->type, BT_UUID_16(attr->uuid)->val);
	} else if (attr->uuid->type == 1) {
		printk("[ATTRIBUTE] uuid type: %02X, %08x\n", attr->uuid->type, BT_UUID_32(attr->uuid)->val);
	} else if (attr->uuid->type == 2) {
		uint8_t *val_128 = BT_UUID_128(attr->uuid)->val;
		printk("[ATTRIBUTE] uuid type: %02X, ", attr->uuid->type);
		for (int i = 0; i < BT_UUID_SIZE_128; i++)
		{
			if (i > 0) printf(":");
			printf("%02X", val_128[i]);
		}
		printk("\n");
	}

	// return BT_GATT_ITER_CONTINUE;
	if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_HRS)) {
		memcpy(&uuid, BT_UUID_HRS_MEASUREMENT, sizeof(uuid));
		discover_params.uuid = &uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		printk("[GONA DO HEARTBEAT]\n");
		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("Discover failed (err %d)\n", err);
		}
	} else if (!bt_uuid_cmp(discover_params.uuid,
				BT_UUID_HRS_MEASUREMENT)) {
		memcpy(&uuid, BT_UUID_GATT_CCC, sizeof(uuid));
		discover_params.uuid = &uuid.uuid;
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
		subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("Discover failed (err %d)\n", err);
		}
	} else {
		subscribe_params.notify = notify_func;
		subscribe_params.value = BT_GATT_CCC_NOTIFY;
		subscribe_params.ccc_handle = attr->handle;

		err = bt_gatt_subscribe(conn, &subscribe_params);
		if (err && err != -EALREADY) {
			printk("Subscribe failed (err %d)\n", err);
		} else {
			printk("[SUBSCRIBED]\n");
		}

		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_STOP;
}


#define BT_UUID_CUSTOM_SERVICE_VAL \
	BT_UUID_128_ENCODE(0xDEADBEEF, 0xFEED, 0xBEEF, 0xF1D0, 0xABCD12345678)

static uint8_t* TARGET_UUID = ((uint8_t []) { BT_UUID_CUSTOM_SERVICE_VAL });

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	int err;

	if (default_conn) {
		return;
	}

	/* We're only interested in connectable events */
	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("\n\nDevice found: %s (RSSI %d)\n", addr_str, rssi);

	uint8_t *ptr = ad->data;
	uint8_t size = *ptr++;
	uint8_t ad_type = *ptr++;
	uint8_t flags[--size]; // -1 to remove type byte from size
	memcpy(&flags, ptr, size);
	printk("flag split. size:%i, type: %02X, data:", size, ad_type, flags);
	for (int i = 0; i < size; i++)
	{
		if (i > 0) printf(":");
		printf("%02X", flags[i]);
	}
	printk("\n");

	ptr += size;
	size = *ptr++;
	ad_type = *ptr++;
	uint8_t services[--size]; // -1 to remove type byte from size
	memcpy(&services, ptr, size);
	printk("services split. size:%i, type: %02X, data:", size, ad_type, services);
	for (int i = 0; i < size; i++)
	{
		if (i > 0) printf(":");
		printf("%02X", services[i]);
	}
	printk("\n");

	ptr += size;
	size = *ptr++;
	ad_type = *ptr++;
	uint8_t ad_uuid[--size]; // -1 to remove type byte from size
	memcpy(&ad_uuid, ptr, size);
	printk("ad_uuid split. size:%i, type: %02X, data:", size, ad_type, ad_uuid);
	for (int i = 0; i < size; i++)
	{
		if (i > 0) printf(":");
		printf("%02X", ad_uuid[i]);
		if (ad_uuid[i] != TARGET_UUID[i]) {
			printk("  -- ... doesn't match\n");
			return;
		}
	}
	printk("\n");

	printk("found a match, connecting\n");
	/* connect only to devices in close proximity */
	if (rssi < -70) {
		return;
	}

	if (bt_le_scan_stop()) {
		return;
	}

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &default_conn);
	if (err) {
		printk("Create conn to %s failed (%u)\n", addr_str, err);
		start_scan();
	}
}

static void start_scan(void)
{
	int ret = gpio_pin_toggle_dt(&led_one);

	int err;

	/* This demo doesn't require active scan */
	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}

	printk("Scanning successfully started\n");
}

struct bt_uuid *duuid = BT_UUID_HRS;
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("Failed to connect to %s (%u)\n", addr, err);

		bt_conn_unref(default_conn);
		default_conn = NULL;

		start_scan();
		return;
	}

	if (conn != default_conn) {
		return;
	}

	int ret = gpio_pin_toggle_dt(&led_two);
	printk("Connected: %s\n", addr);
	memcpy(duuid, BT_UUID_HRS, sizeof(duuid));
	discover_params.uuid = duuid;// &uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(default_conn, &discover_params);
	if (err) {
		printk("Discover failed(err %d)\n", err);
		return;
	}

	//bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (conn != default_conn) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s (reason 0x%02x)\n", addr, reason);

	bt_conn_unref(default_conn);
	default_conn = NULL;

	start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

void main(void)
{
	int err;
	configure_led(led_one);
	configure_led(led_two);
	int ret = gpio_pin_toggle_dt(&led_one);
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	start_scan();
}
