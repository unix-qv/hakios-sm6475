#ifndef _LX_QCOM_SCM_H
#define _LX_QCOM_SCM_H
#include <linux/types.h>
#include <linux/errno.h>

#define QCOM_SCM_VMID_HLOS	0x3
#define QCOM_SCM_VMID_MSS_MSA	0xf
#define QCOM_SCM_VMID_WLAN	0x18
#define QCOM_SCM_VMID_WLAN_CE	0x19
#define QCOM_SCM_PERM_RW	0x6

struct qcom_scm_vmperm {
	int vmid;
	int perm;
};

static inline int
qcom_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *srcvm,
    const struct qcom_scm_vmperm *newvm, unsigned int dest_cnt)
{
	return (0);
}
#endif
