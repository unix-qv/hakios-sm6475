#ifndef _LX_QCOM_SMEM_H
#define _LX_QCOM_SMEM_H
#include <linux/types.h>
#include <linux/err.h>
#include <linux/errno.h>

#define QCOM_SMEM_HOST_ANY	-1

static inline void *
qcom_smem_get(unsigned host, unsigned item, size_t *size)
{
	return (ERR_PTR(-ENOENT));
}
#endif
