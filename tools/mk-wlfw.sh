#!/bin/bash
# Give the kernel's build ath11k's descriptions of the messages a WCN6750's firmware speaks, for
# drivers/wifi/hakios.wlfw.c (a stand-in for ath11k's own qmi.c: see there). They are taken out of
# the driver's sources as they are -- the structures of its qmi.h and the tables of its qmi.c --
# and written as one header into the kernel's build directory, where the compiler finds it and
# git does not: ath11k's text is BSD-3-Clause-Clear and is not kept in the repository.
# Without that header hakios.wlfw.c compiles to a line that says so.
#
# usage: tools/mk-wlfw.sh          then build the kernel (./xnu-build.sh); again after a clean build
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
ATH=$ROOT/xnu-deps/build/ath11k/src/ath11k
[ -f "$ATH/qmi.c" ] || "$ROOT/tools/mk-ath11k.sh"
OUT=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
[ -d "$OUT" ] || { echo "mk-wlfw.sh: no build directory $OUT: build the kernel once first" >&2; exit 1; }
python3 - "$ATH" "$(cat "$ATH/../.release" | cut -d' ' -f1)" > "$OUT/hakios.wlfw_ath11k.h.new" <<'PY'
import re
import sys

src, release = sys.argv[1], sys.argv[2]
c = open(src + '/qmi.c').read()
h = open(src + '/qmi.h').read()
print('/* ath11k\'s qmi.h and the message tables of its qmi.c (Linux %s, BSD-3-Clause-Clear), put together by' % release)
print(' * tools/mk-wlfw.sh: not to be edited, not to be kept. */')
# the names qmi.h uses of a Linux kernel around it, outside its message definitions: nothing here does anything
print('''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
#define __iomem
struct list_head { struct list_head *next, *prev; };
struct work_struct { int unused; };
struct workqueue_struct;
typedef struct { int unused; } spinlock_t;
typedef struct { int unused; } wait_queue_head_t;
''')
print(re.sub(r'^#include <linux/[^>]*>\n', '', h, flags=re.M))
# every table, in the file's order (a table is used by the ones after it)
print('\n'.join(re.findall(r'static const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S)))

# What the configuration message is made of for this chip: the two tables its entry of the
# driver's parameter list (core.c) names, from hw.c, with the definitions they are written in
# (ce.h, htc.h), and from that entry how many of each table's rows are sent.
core = open(src + '/core.c').read()
hw = open(src + '/hw.c').read()
ce = open(src + '/ce.h').read()
htc = open(src + '/htc.h').read()
entry = core[core.index('"wcn6750 hw1.0"'):]
entry = entry[:entry.index('\n\t},')]
def of(field):
    return re.search(r'\.%s = (\w+),' % field, entry).group(1)
print('/* as Linux has it on a little-endian machine, whatever this kernel\'s headers define */')
print('#undef __BIG_ENDIAN\n#define __cpu_to_le32(x) (x)')
print(re.search(r'#define CE_ATTR_BYTE_SWAP_DATA.*?#endif\n', ce, re.S).group(0))
print(''.join(re.findall(r'^#define PIPEDIR_\w+.*\n', ce, re.M)))
for name in ('service_to_pipe', 'ce_pipe_config'):
    print(re.search(r'struct %s \{.*?\n\};\n' % name, ce, re.S).group(0))
print(re.search(r'enum ath11k_htc_svc_gid \{.*?\n\};\n', htc, re.S).group(0))
print(re.search(r'#define SVC\(group, idx\).*?\n.*?\n', htc).group(0))
print(re.search(r'enum ath11k_htc_svc_id \{.*?\n\};\n', htc, re.S).group(0))
print('#undef SVC')
for kind, field in (('ce_pipe_config', 'target_ce_config'), ('service_to_pipe', 'svc_to_ce_map')):
    print('static ' + re.search(r'const struct %s %s\[\] = \{.*?\n\};\n' % (kind, of(field)), hw, re.S).group(0))
print('#define WLFW_PIPES\t\t%s\n#define WLFW_PIPE_COUNT\t\t%s' % (of('target_ce_config'), of('target_ce_count')))
print('#define WLFW_SERVICES\t\t%s\n#define WLFW_SERVICE_COUNT\t%s' % (of('svc_to_ce_map'), of('svc_to_ce_map_len')))
print('#define WLFW_SENDS_INI\t\t%d\t/* fw_wmi_diag_event */' % (of('fw_wmi_diag_event') == 'true'))
print('#define WLFW_SHADOW_REGISTERS\t%d\t/* supports_shadow_regs */' % (of('supports_shadow_regs') == 'true'))
print('''
/* WCN3990 / Adrastea (ath10k) MSA & Cal Report QMI messages and SNOC CE tables */
#define QMI_WLFW_MSA_INFO_REQ_V01 0x002D
#define WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN 18
#define QMI_WLFW_MSA_READY_REQ_V01 0x002E
#define WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN 0
#define QMI_WLFW_CAL_REPORT_REQ_V01 0x0026
#define WLFW_CAL_REPORT_REQ_MSG_V01_MAX_MSG_LEN 28
#define QMI_WLFW_MAX_MEM_REG_V01 2
#define QMI_WLFW_MAX_NUM_CAL_V01 5

struct wlfw_memory_region_info_s_v01 {
	u64 region_addr;
	u32 size;
	u8 secure_flag;
};

struct wlfw_msa_info_req_msg_v01 {
	u64 msa_addr;
	u32 size;
};

struct wlfw_msa_info_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u32 mem_region_info_len;
	struct wlfw_memory_region_info_s_v01 mem_region_info[QMI_WLFW_MAX_MEM_REG_V01];
};

struct wlfw_msa_ready_req_msg_v01 {
	char placeholder;
};

struct wlfw_msa_ready_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct wlfw_cal_report_req_msg_v01 {
	u32 meta_data_len;
	u32 meta_data[QMI_WLFW_MAX_NUM_CAL_V01];
	u8 xo_cal_data_valid;
	u8 xo_cal_data;
};

struct wlfw_cal_report_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

static const struct qmi_elem_info wlfw_memory_region_info_s_v01_ei[] = {
	{
		.data_type      = QMI_UNSIGNED_8_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u64),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0,
		.offset         = offsetof(struct wlfw_memory_region_info_s_v01, region_addr),
	},
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0,
		.offset         = offsetof(struct wlfw_memory_region_info_s_v01, size),
	},
	{
		.data_type      = QMI_UNSIGNED_1_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0,
		.offset         = offsetof(struct wlfw_memory_region_info_s_v01, secure_flag),
	},
	{}
};

static const struct qmi_elem_info wlfw_msa_info_req_msg_v01_ei[] = {
	{
		.data_type      = QMI_UNSIGNED_8_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u64),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct wlfw_msa_info_req_msg_v01, msa_addr),
	},
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct wlfw_msa_info_req_msg_v01, size),
	},
	{}
};

static const struct qmi_elem_info wlfw_msa_info_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct wlfw_msa_info_resp_msg_v01, resp),
		.ei_array       = qmi_response_type_v01_ei,
	},
	{
		.data_type      = QMI_DATA_LEN,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x03,
		.offset         = offsetof(struct wlfw_msa_info_resp_msg_v01, mem_region_info_len),
	},
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = QMI_WLFW_MAX_MEM_REG_V01,
		.elem_size      = sizeof(struct wlfw_memory_region_info_s_v01),
		.array_type     = VAR_LEN_ARRAY,
		.tlv_type       = 0x03,
		.offset         = offsetof(struct wlfw_msa_info_resp_msg_v01, mem_region_info),
		.ei_array       = wlfw_memory_region_info_s_v01_ei,
	},
	{}
};

static const struct qmi_elem_info wlfw_msa_ready_req_msg_v01_ei[] = {
	{}
};

static const struct qmi_elem_info wlfw_msa_ready_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct wlfw_msa_ready_resp_msg_v01, resp),
		.ei_array       = qmi_response_type_v01_ei,
	},
	{}
};

static const struct qmi_elem_info wlfw_cal_report_req_msg_v01_ei[] = {
	{
		.data_type      = QMI_DATA_LEN,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct wlfw_cal_report_req_msg_v01, meta_data_len),
	},
	{
		.data_type      = QMI_SIGNED_4_BYTE_ENUM,
		.elem_len       = QMI_WLFW_MAX_NUM_CAL_V01,
		.elem_size      = sizeof(u32),
		.array_type     = VAR_LEN_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct wlfw_cal_report_req_msg_v01, meta_data),
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct wlfw_cal_report_req_msg_v01, xo_cal_data_valid),
	},
	{
		.data_type      = QMI_UNSIGNED_1_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct wlfw_cal_report_req_msg_v01, xo_cal_data),
	},
	{}
};

static const struct qmi_elem_info wlfw_cal_report_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type     = NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct wlfw_cal_report_resp_msg_v01, resp),
		.ei_array       = qmi_response_type_v01_ei,
	},
	{}
};

static const struct ce_pipe_config wcn3990_pipes[] = {
	{ .pipenum = 0,  .pipedir = PIPEDIR_OUT,   .nentries = 32,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
	{ .pipenum = 1,  .pipedir = PIPEDIR_IN,    .nentries = 32,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
	{ .pipenum = 2,  .pipedir = PIPEDIR_IN,    .nentries = 64,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
	{ .pipenum = 3,  .pipedir = PIPEDIR_OUT,   .nentries = 32,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
	{ .pipenum = 4,  .pipedir = PIPEDIR_OUT,   .nentries = 256,  .nbytes_max = 256,   .flags = CE_ATTR_FLAGS | CE_ATTR_DIS_INTR },
	{ .pipenum = 5,  .pipedir = PIPEDIR_OUT,   .nentries = 1024, .nbytes_max = 64,    .flags = CE_ATTR_FLAGS | CE_ATTR_DIS_INTR },
	{ .pipenum = 6,  .pipedir = PIPEDIR_INOUT, .nentries = 32,   .nbytes_max = 16384, .flags = CE_ATTR_FLAGS },
	{ .pipenum = 7,  .pipedir = 4,             .nentries = 0,    .nbytes_max = 0,     .flags = CE_ATTR_FLAGS | CE_ATTR_DIS_INTR },
	{ .pipenum = 8,  .pipedir = PIPEDIR_IN,    .nentries = 32,   .nbytes_max = 2048,  .flags = 0 },
	{ .pipenum = 9,  .pipedir = PIPEDIR_IN,    .nentries = 32,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
	{ .pipenum = 10, .pipedir = PIPEDIR_IN,    .nentries = 32,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
	{ .pipenum = 11, .pipedir = PIPEDIR_IN,    .nentries = 32,   .nbytes_max = 2048,  .flags = CE_ATTR_FLAGS },
};

static const struct service_to_pipe wcn3990_services[] = {
	{ 0x0104, PIPEDIR_OUT, 3 }, { 0x0104, PIPEDIR_IN, 2 },
	{ 0x0102, PIPEDIR_OUT, 3 }, { 0x0102, PIPEDIR_IN, 2 },
	{ 0x0101, PIPEDIR_OUT, 3 }, { 0x0101, PIPEDIR_IN, 2 },
	{ 0x0103, PIPEDIR_OUT, 3 }, { 0x0103, PIPEDIR_IN, 2 },
	{ 0x0100, PIPEDIR_OUT, 3 }, { 0x0100, PIPEDIR_IN, 2 },
	{ 0x0001, PIPEDIR_OUT, 0 }, { 0x0001, PIPEDIR_IN, 2 },
	{ 0xfe00, PIPEDIR_OUT, 0 }, { 0xfe00, PIPEDIR_IN, 2 },
	{ 0x0300, PIPEDIR_OUT, 4 }, { 0x0300, PIPEDIR_IN, 1 },
	{ 0xfe00, PIPEDIR_OUT, 5 },
	{ 0x0301, PIPEDIR_IN,  9 },
	{ 0x0302, PIPEDIR_IN, 10 },
	{ 0x0600, PIPEDIR_IN, 11 },
};

static const struct qmi_wlanfw_shadow_reg_cfg_s_v01 wcn3990_shadow_regs[] = {
	{ 0, 0x3c }, { 3, 0x3c }, { 4, 0x3c }, { 5, 0x3c }, { 7, 0x3c },
	{ 1, 0x40 }, { 2, 0x40 }, { 7, 0x40 }, { 8, 0x40 }, { 9, 0x40 }, { 10, 0x40 }, { 11, 0x40 },
};
''')
PY
cmp -s "$OUT/hakios.wlfw_ath11k.h.new" "$OUT/hakios.wlfw_ath11k.h" 2>/dev/null && rm "$OUT/hakios.wlfw_ath11k.h.new" ||
	mv "$OUT/hakios.wlfw_ath11k.h.new" "$OUT/hakios.wlfw_ath11k.h"
echo "mk-wlfw.sh: $OUT/hakios.wlfw_ath11k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/hakios.wlfw_ath11k.h") tables)"
