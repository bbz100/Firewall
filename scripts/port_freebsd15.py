#!/usr/bin/env python3
"""Experimental FreeBSD 15.1 API migration for Silicom bp_ctl 4.0.2.
Compilation experiment only: timer teardown/lifetime and hardware behavior NOT validated.
"""
from pathlib import Path

base = Path("bp_ctl-4.0.2/driver")
c = base / "bp_mod.c"
h = base / "bypass.h"

def edit(path, replacements):
    data = path.read_text()
    for old, new in replacements:
        if old not in data:
            raise SystemExit(f"Expected source fragment not found in {path}: {old!r}")
        data = data.replace(old, new)
    path.write_text(data)

edit(h, [
    ("struct callout_handle bp_timer;", "struct callout bp_timer;"),
    ("struct callout_handle bp_tpl_timer;", "struct callout bp_tpl_timer;"),
])

edit(c, [
    ("#include <sys/param.h>", "#include <sys/param.h>\n#include <sys/callout.h>"),
    ("ifp->if_index", "if_getindex(ifp)"),
    ("bpctl_dev_arr[dev_idx].ndev->if_flags", "if_getflags(bpctl_dev_arr[dev_idx].ndev)"),
    ("bpctl_dev_arr[pair_idx].ndev->if_flags", "if_getflags(bpctl_dev_arr[pair_idx].ndev)"),
    ("bpctl_dev_arr[idx_dev].ndev->if_flags", "if_getflags(bpctl_dev_arr[idx_dev].ndev)"),
    ("pbpctl_dev->bp_timer= timeout(wd_reset_timer, pbpctl_dev, ", "callout_reset(&pbpctl_dev->bp_timer, "),
    ("pbpctl_dev->bp_tpl_timer = timeout(bp_tpl_timer_fn, pbpctl_dev, ", "callout_reset(&pbpctl_dev->bp_tpl_timer, "),
    ("pbpctl_dev->bp_tpl_timer= timeout(bp_tpl_timer_fn, pbpctl_dev, ", "callout_reset(&pbpctl_dev->bp_tpl_timer, "),
    ("untimeout(wd_reset_timer, pbpctl_dev, pbpctl_dev->bp_timer)", "callout_stop(&pbpctl_dev->bp_timer)"),
    ("untimeout(bp_tpl_timer_fn, pbpctl_dev, pbpctl_dev->bp_tpl_timer)", "callout_stop(&pbpctl_dev->bp_tpl_timer)"),
    ("bpctl_dev_arr=malloc ((device_num)  * sizeof (bpctl_dev_t), M_DEVBUF, M_NOWAIT|M_ZERO);", "bpctl_dev_arr=malloc ((device_num)  * sizeof (bpctl_dev_t), M_DEVBUF, M_NOWAIT|M_ZERO);"),
    ("return(bpmod_alloc_devices());", "for (int i = 0; i < device_num; i++) {\n        callout_init(&bpctl_dev_arr[i].bp_timer, 1);\n        callout_init(&bpctl_dev_arr[i].bp_tpl_timer, 1);\n    }\n\n    return(bpmod_alloc_devices());"),
])
# Correct callout_reset signature (ticks, callback, context)
data = c.read_text()
import re
data = re.sub(r"callout_reset\(&pbpctl_dev->bp_timer, ([^;]*?)\);", r"callout_reset(&pbpctl_dev->bp_timer, \1, wd_reset_timer, pbpctl_dev);", data)
data = re.sub(r"callout_reset\(&pbpctl_dev->bp_tpl_timer, ([^;]*?)\);", r"callout_reset(&pbpctl_dev->bp_tpl_timer, \1, bp_tpl_timer_fn, pbpctl_dev);", data)
# Keep the device node unavailable if the controller did not initialise.
# This is needed for the no-NIC VM smoke test and for safe error handling.
original_load = "err = bpmod_init();\n\t\tbpmod_dev = make_dev"
if original_load not in data:
    raise SystemExit("Cannot find MOD_LOAD initialisation block")
data = data.replace(original_load, "err = bpmod_init();\n\t\tif (err != 0) {\n\t\t\treturn err;\n\t\t}\n\t\tbpmod_dev = make_dev")
# The old driver initialises its mutex before checking for supported devices.
original_no_device = 'printf("No such device\\n"); \n\t\treturn ENODEV;'
if original_no_device not in data:
    raise SystemExit("Cannot find no-device failure path")
data = data.replace(original_no_device, 'printf("No such device\\n"); \n\t\tmtx_destroy(&mtx);\n\t\treturn ENODEV;')
# Fix out-of-bounds reads: C evaluates && from left to right.
data = data.replace(
    "((bpctl_dev_arr[idx_dev].pdev!=NULL)&&(idx_dev<device_num))",
    "((idx_dev<device_num)&&(bpctl_dev_arr[idx_dev].pdev!=NULL))",
)
# Repair the no-hardware PCI scan: initialise pointers, check enumeration
# results, and free each returned list after its last use.
start = data.index("int\nbpmod_find_devices(void)")
end = data.index("int\nbpmod_alloc_devices(void)", start)
scan = data[start:end]
old_tail = "\t\t} \n\t}\n\treturn idx_dev;\n}"
new_tail = ("\t\t}\n\t\tif (pci_children != NULL)\n"
            "\t\t\tfree(pci_children, M_TEMP);\n\t}\n"
            "\tif (pci_devices != NULL)\n\t\tfree(pci_devices, M_TEMP);\n"
            "\treturn idx_dev;\n}")
if old_tail not in scan:
    raise SystemExit("Cannot locate PCI scan tail")
scan = scan.replace("device_t        *pci_devices;", "device_t        *pci_devices = NULL;")
scan = scan.replace("device_t        *pci_children;", "device_t        *pci_children = NULL;")
scan = scan.replace(
    "\tdevclass_get_devices(pci_devclass, &pci_devices, &pci_count);",
    "\tif (devclass_get_devices(pci_devclass, &pci_devices, &pci_count) != 0)\n\t\treturn 0;",
)
scan = scan.replace(
    "\t\tpci_childcount = 0;",
    "\t\tpci_childcount = 0;\n\t\tpci_children = NULL;",
)
scan = scan.replace(
    "\t\tdevice_get_children(*busp, &pci_children, &pci_childcount);",
    "\t\tif (device_get_children(*busp, &pci_children, &pci_childcount) != 0)\n\t\t\tcontinue;",
)
scan = scan.replace(old_tail, new_tail)
data = data[:start] + scan + data[end:]
# Suppress only unused-but-set warnings in this first API survey, not other errors.
c.write_text(data)
print("Applied experimental API migration")
