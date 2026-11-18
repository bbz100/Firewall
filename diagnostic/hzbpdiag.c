/*
 * HZ Silicom PCI diagnostic for OPNsense 26.7.
 * Read-only PCI configuration identification. Does not access BARs, I/O
 * resources, GPIO, EEPROM, watchdogs, bypass registers or network interfaces.
 * This module does not attach to NIC drivers or change device configuration.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <dev/pci/pcivar.h>

#define HZ_VENDOR 0x8086
#define HZ_DEVICE 0x1521
#define HZ_SUBVENDOR 0x1374
#define HZ_SUBDEVICE 0x03d0

static int
hzbpdiag_event(module_t mod __unused, int event, void *arg __unused)
{
    device_t dev;
    int fn, matched;
    uint16_t vendor, device, subvendor, subdevice;

    switch (event) {
    case MOD_LOAD:
        matched = 0;
        printf("hzbpdiag: read-only Silicom PCI scan begin\n");
        for (fn = 0; fn < 4; ++fn) {
            dev = pci_find_dbsf(0, 2, 0, fn);
            if (dev == NULL) {
                printf("hzbpdiag: pci0:2:0:%d not present\n", fn);
                continue;
            }
            vendor = pci_get_vendor(dev);
            device = pci_get_device(dev);
            subvendor = pci_get_subvendor(dev);
            subdevice = pci_get_subdevice(dev);
            printf("hzbpdiag: pci0:2:0:%d vendor=%04x device=%04x subvendor=%04x subdevice=%04x\n",
                fn, vendor, device, subvendor, subdevice);
            if (vendor == HZ_VENDOR && device == HZ_DEVICE &&
                subvendor == HZ_SUBVENDOR && subdevice == HZ_SUBDEVICE)
                ++matched;
        }
        printf("hzbpdiag: %d/4 expected Silicom I350 ports identified; no hardware control attempted\n",
            matched);
        return (matched == 4 ? 0 : ENXIO);

    case MOD_UNLOAD:
        printf("hzbpdiag: unloaded (no hardware settings changed)\n");
        return (0);

    default:
        return (EOPNOTSUPP);
    }
}

static moduledata_t hzbpdiag_mod = {
    "hzbpdiag",
    hzbpdiag_event,
    NULL
};

DECLARE_MODULE(hzbpdiag, hzbpdiag_mod, SI_SUB_PROTO_IF, SI_ORDER_ANY);
MODULE_VERSION(hzbpdiag, 1);
