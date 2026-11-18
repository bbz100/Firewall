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
    ("#include <sys/param.h>", "#include <sys/callout.h>\n#include <sys/param.h>"),
    ("ifp->if_index", "if_getindex(ifp)"),
    ("->ndev->if_flags", "->ndev->if_flags"), # replacement below handles modern opaque ifnet
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
# Suppress only unused-but-set warnings in this first API survey, not other errors.
c.write_text(data)
print("Applied experimental API migration")
