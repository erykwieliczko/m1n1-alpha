/* SPDX-License-Identifier: MIT */

#ifndef PCIE_H
#define PCIE_H

int pcie_init(void);
int pcie_shutdown(void);
int pcie_prepare_fdt(void *dt);
int pcie_handoff_fdt(void *dt);

#endif
