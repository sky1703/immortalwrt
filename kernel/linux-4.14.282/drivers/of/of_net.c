/*
 * OF helpers for network devices.
 *
 * This file is released under the GPLv2
 *
 * Initially copied out of arch/powerpc/kernel/prom_parse.c
 */
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/export.h>
#include <linux/mtd/mtd.h>

/**
 * of_get_phy_mode - Get phy mode for given device_node
 * @np:	Pointer to the given device_node
 *
 * The function gets phy interface string from property 'phy-mode' or
 * 'phy-connection-type', and return its index in phy_modes table, or errno in
 * error case.
 */
int of_get_phy_mode(struct device_node *np)
{
	const char *pm;
	int err, i;

	err = of_property_read_string(np, "phy-mode", &pm);
	if (err < 0)
		err = of_property_read_string(np, "phy-connection-type", &pm);
	if (err < 0)
		return err;

	for (i = 0; i < PHY_INTERFACE_MODE_MAX; i++)
		if (!strcasecmp(pm, phy_modes(i)))
			return i;

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(of_get_phy_mode);

static void *of_get_mac_addr(struct device_node *np, const char *name)
{
	struct property *pp = of_find_property(np, name, NULL);

	if (pp && pp->length == ETH_ALEN && is_valid_ether_addr(pp->value))
		return pp->value;
	return NULL;
}

typedef int(*mtd_mac_address_read)(struct mtd_info *mtd, loff_t from, u_char *buf);

static int read_mtd_mac_address(struct mtd_info *mtd, loff_t from, u_char *mac)
{
	size_t retlen;

	return mtd_read(mtd, from, 6, &retlen, mac);
}

static int read_mtd_mac_address_ascii(struct mtd_info *mtd, loff_t from, u_char *mac)
{
	size_t retlen;
	char buf[17];

	if (mtd_read(mtd, from, 12, &retlen, buf)) {
		return -1;
	}
	if (sscanf(buf, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
			&mac[0], &mac[1], &mac[2], &mac[3],
			&mac[4], &mac[5]) == 6) {
		if (mac[0] == '\0' && mac[1] == '\0') { /* First 2 bytes are zero, probably a bug. Trying to re-read */
			buf[4] = '\0'; /* Make it null-terminated */
			if (sscanf(buf, "%4hx", mac) == 1)
				*(uint16_t*)mac = htons(*(uint16_t*)mac);
		}
		return 0;
	}
	if (mtd_read(mtd, from+12, 5, &retlen, buf+12)) {
		return -1;
	}
	if (sscanf(buf, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
			&mac[0], &mac[1], &mac[2], &mac[3],
			&mac[4], &mac[5]) == 6) {
		return 0;
	}
	return -1;
}

static struct mtd_mac_address_property {
	char *name;
	mtd_mac_address_read read;
} mtd_mac_address_properties[] = {
	{
		.name = "mtd-mac-address",
		.read = read_mtd_mac_address,
	}, {
		.name = "mtd-mac-address-ascii",
		.read = read_mtd_mac_address_ascii,
	},
};

static const void *of_get_mac_address_mtd(struct device_node *np)
{
#ifdef CONFIG_MTD
	struct device_node *mtd_np = NULL;
	struct property *prop;
	int size, ret = -1;
	struct mtd_info *mtd;
	const char *part;
	const __be32 *list;
	phandle phandle;
	u32 mac_inc = 0;
	u8 mac[ETH_ALEN];
	void *addr;
	u32 inc_idx;
	int i;

	for (i = 0; i < ARRAY_SIZE(mtd_mac_address_properties); i++) {
		list = of_get_property(np, mtd_mac_address_properties[i].name, &size);
		if (!list || (size != (2 * sizeof(*list))))
			continue;

		phandle = be32_to_cpup(list++);
		if (phandle)
			mtd_np = of_find_node_by_phandle(phandle);

		if (!mtd_np)
			continue;

		part = of_get_property(mtd_np, "label", NULL);
		if (!part)
			part = mtd_np->name;

		mtd = get_mtd_device_nm(part);
		if (IS_ERR(mtd))
			continue;

		ret = mtd_mac_address_properties[i].read(mtd, be32_to_cpup(list), mac);
		put_mtd_device(mtd);
		if (!ret) {
			break;
		}
	}
	if (ret) {
		return NULL;
	}

	if (of_property_read_u32(np, "mtd-mac-address-increment-byte", &inc_idx))
		inc_idx = 5;
	if (inc_idx > 5)
		return NULL;

	if (!of_property_read_u32(np, "mtd-mac-address-increment", &mac_inc))
		mac[inc_idx] += mac_inc;

	if (!is_valid_ether_addr(mac))
		return NULL;

	addr = of_get_mac_addr(np, "mac-address");
	if (addr) {
		memcpy(addr, mac, ETH_ALEN);
		return addr;
	}

	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		return NULL;

	prop->name = "mac-address";
	prop->length = ETH_ALEN;
	prop->value = kmemdup(mac, ETH_ALEN, GFP_KERNEL);
	if (!prop->value || of_add_property(np, prop))
		goto free;

	return prop->value;
free:
	kfree(prop->value);
	kfree(prop);
#endif
	return NULL;
}

/**
 * Search the device tree for the best MAC address to use.  'mac-address' is
 * checked first, because that is supposed to contain to "most recent" MAC
 * address. If that isn't set, then 'local-mac-address' is checked next,
 * because that is the default address.  If that isn't set, then the obsolete
 * 'address' is checked, just in case we're using an old device tree.
 *
 * Note that the 'address' property is supposed to contain a virtual address of
 * the register set, but some DTS files have redefined that property to be the
 * MAC address.
 *
 * All-zero MAC addresses are rejected, because those could be properties that
 * exist in the device tree, but were not set by U-Boot.  For example, the
 * DTS could define 'mac-address' and 'local-mac-address', with zero MAC
 * addresses.  Some older U-Boots only initialized 'local-mac-address'.  In
 * this case, the real MAC is in 'local-mac-address', and 'mac-address' exists
 * but is all zeros.
 *
 * If a mtd-mac-address property exists, try to fetch the MAC address from the
 * specified mtd device, and store it as a 'mac-address' property
*/
const void *of_get_mac_address(struct device_node *np)
{
	const void *addr;

	addr = of_get_mac_address_mtd(np);
	if (addr)
		return addr;

	addr = of_get_mac_addr(np, "mac-address");
	if (addr)
		return addr;

	addr = of_get_mac_addr(np, "local-mac-address");
	if (addr)
		return addr;

	return of_get_mac_addr(np, "address");
}
EXPORT_SYMBOL(of_get_mac_address);
