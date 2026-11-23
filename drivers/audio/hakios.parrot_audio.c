#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern bool hakios_audio_recording_fed(void);
extern void hakios_audio_record_fed(const int16_t *frames, uint32_t count);

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000ull;
}

static int say(const char *format, ...) __printflike(1, 2);
static bool pa_leaving;

static int
say(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!pa_leaving) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
wr(vm_offset_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
sleep_ms(uint32_t ms)
{
	if (pa_leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- RPMh VRM regulators (SM6475) ------------------------------------------------------------------ */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc_base + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008u
#define MSGID_RESPONSE          0x100u
#define VRM_VOLTAGE             0u
#define VRM_ENABLE              4u
#define VRM_MODE                8u
#define VRM_MODE_LDO_HPM        7u

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static vm_offset_t cmd_db_base, rsc_base;

static uint32_t
cmd_db_find(const char *name)
{
	const struct cmd_db_header *db;
	char id[8] = { 0 };

	if (cmd_db_base == 0) {
		cmd_db_base = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	db = (const struct cmd_db_header *)cmd_db_base;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_write(const uint32_t *addr, const uint32_t *data, unsigned count)
{
	bool done = false;

	if (rsc_base == 0) {
		rsc_base = ml_io_map(RSC_PHYS, RSC_SIZE);
	}
	for (int i = 0; i < 2000; i++) {
		if (rd(TCS(TCS_STATUS)) != 0) {
			break;
		}
		spin_us(100);
	}
	if (rd(TCS(TCS_STATUS)) == 0) {
		say("paudio: RPMh TCS0 busy (ctrl=%#x)\n", rd(TCS(TCS_CONTROL)));
		return false;
	}
	wr(TCS(TCS_IRQ_CLEAR), 1u);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), addr[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1u);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1u);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) | TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) | TCS_CONTROL_TRIGGER);
	for (int i = 0; i < 2000; i++) {
		if (rd(TCS(TCS_IRQ_STATUS)) & 1u) {
			done = true;
			break;
		}
		spin_us(50);
	}
	wr(TCS(TCS_IRQ_CLEAR), 1u);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~(TCS_CONTROL_TRIGGER | TCS_CONTROL_ENABLE));
	wr(TCS(TCS_CMD_ENABLE), 0u);
	return done;
}

static void
vrm_enable_reg(const char *name, uint32_t mv, uint32_t mode, bool set_mode)
{
	uint32_t base = cmd_db_find(name);
	uint32_t addrs[3], vals[3];
	unsigned n = 0;

	if (base == 0) {
		say("paudio: VRM %s not in cmd-db\n", name);
		return;
	}
	addrs[n] = base + VRM_VOLTAGE;
	vals[n++] = mv;
	if (set_mode) {
		addrs[n] = base + VRM_MODE;
		vals[n++] = mode;
	}
	addrs[n] = base + VRM_ENABLE;
	vals[n++] = 1;
	bool ok = rpmh_write(addrs, vals, n);
	say("paudio: VRM %s (%#x) %u mV: %s\n", name, base, mv, ok ? "ok" : "FAIL");
}

/* ---- AudioReach PRM over GPR ----------------------------------------------------------------------- */

#define GPR_PORT_PRM                    0x00000002u
#define GPR_PORT_PAUDIO                 0x00002010u
#define GPR_IBASIC_RSP_RESULT           0x02001005u

#define PRM_CMD_REQUEST_HW_RSC          0x0100100Fu
#define PRM_CMD_RSP_REQUEST_HW_RSC      0x02001002u
#define PRM_CMD_RELEASE_HW_RSC          0x01001010u
#define PRM_CMD_RSP_RELEASE_HW_RSC      0x02001003u

#define PARAM_ID_RSC_AUDIO_HW_CLK       0x0800102Cu
#define PARAM_ID_RSC_HW_CORE            0x08001032u

#define Q6PRM_HW_CORE_ID_LPASS          1u
#define Q6PRM_HW_CORE_ID_DCODEC         2u

#define CLK_VA_CORE                     0x307u
#define CLK_VA_CORE_2X                  0x308u
#define CLK_TX_CORE                     0x30cu
#define CLK_TX_NPL                      0x30du
#define CLK_RX_CORE                     0x30eu
#define CLK_RX_NPL                      0x30fu
#define CLK_RX_TX_MCLK                  0x312u
#define CLK_RX_TX_2X                    0x313u
#define CLK_RX_MCLK2_2X                 0x318u

struct apm_cmd_header {
	uint32_t payload_address_lsw;
	uint32_t payload_address_msw;
	uint32_t mem_map_handle;
	uint32_t payload_size;
} __attribute__((packed));

struct apm_module_param_data {
	uint32_t module_instance_id;
	uint32_t param_id;
	uint32_t param_size;
	uint32_t error_code;
} __attribute__((packed));

struct prm_hw_core_pkt {
	struct apm_cmd_header hdr;
	struct apm_module_param_data param;
	uint32_t hw_core_id;
} __attribute__((packed));

struct prm_clk_req_pkt {
	struct apm_cmd_header hdr;
	struct apm_module_param_data param;
	uint32_t num_clk_id;
	uint32_t clock_id;
	uint32_t clock_freq;
	uint32_t clock_attri;
	uint32_t clock_root;
} __attribute__((packed));

struct prm_clk_rel_pkt {
	struct apm_cmd_header hdr;
	struct apm_module_param_data param;
	uint32_t num_clk_id;
	uint32_t clock_id;
} __attribute__((packed));

static bool prm_core_voted[4];
static uint32_t prm_clks_on[12];
static unsigned prm_clks_count;

static bool
prm_vote_core(uint32_t core_id, bool enable)
{
	struct prm_hw_core_pkt p;
	struct hakios_gpr_answer ans;
	uint32_t op = enable ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC;
	uint32_t exp = enable ? PRM_CMD_RSP_REQUEST_HW_RSC : PRM_CMD_RSP_RELEASE_HW_RSC;

	bzero(&p, sizeof(p));
	p.hdr.payload_size = (uint32_t)(sizeof(p) - sizeof(p.hdr));
	p.param.module_instance_id = GPR_PORT_PRM;
	p.param.param_id = PARAM_ID_RSC_HW_CORE;
	p.param.param_size = sizeof(p.hw_core_id);
	p.hw_core_id = core_id;

	if (!hakios_adsp_gpr(GPR_PORT_PAUDIO, GPR_PORT_PRM, 0, op, &p, sizeof(p), 1000, &ans)) {
		say("paudio: PRM core %u %s: NO ANSWER\n", core_id, enable ? "vote" : "unvote");
		return false;
	}
	bool ok = (ans.opcode == exp && ans.words[1] == 0) ||
	    (ans.opcode == GPR_IBASIC_RSP_RESULT && ans.words[1] == 0);
	say("paudio: PRM core %u %s: %s (op %#x len %u w0=%#x w1=%#x)\n",
	    core_id, enable ? "vote" : "unvote", ok ? "ok" : "ERR",
	    ans.opcode, ans.length, ans.words[0], ans.words[1]);
	if (ok && core_id < 4) {
		prm_core_voted[core_id] = enable;
	}
	return ok;
}

static bool
prm_req_clk(uint32_t clk_id, uint32_t freq, uint32_t attr)
{
	struct prm_clk_req_pkt p;
	struct hakios_gpr_answer ans;

	bzero(&p, sizeof(p));
	p.hdr.payload_size = (uint32_t)(sizeof(p) - sizeof(p.hdr));
	p.param.module_instance_id = GPR_PORT_PRM;
	p.param.param_id = PARAM_ID_RSC_AUDIO_HW_CLK;
	p.param.param_size = (uint32_t)(sizeof(p) - sizeof(p.hdr) - sizeof(p.param));
	p.num_clk_id = 1;
	p.clock_id = clk_id;
	p.clock_freq = freq;
	p.clock_attri = attr;
	p.clock_root = 0;

	if (!hakios_adsp_gpr(GPR_PORT_PAUDIO, GPR_PORT_PRM, 0, PRM_CMD_REQUEST_HW_RSC, &p, sizeof(p), 1000, &ans)) {
		say("paudio: PRM clk %#x (%u Hz, attr %u): NO ANSWER\n", clk_id, freq, attr);
		return false;
	}
	bool ok = (ans.opcode == PRM_CMD_RSP_REQUEST_HW_RSC && ans.words[1] == 0) ||
	    (ans.opcode == GPR_IBASIC_RSP_RESULT && ans.words[1] == 0);
	say("paudio: PRM clk %#x (%u Hz, attr %u): %s (op %#x w0=%#x w1=%#x)\n",
	    clk_id, freq, attr, ok ? "ok" : "ERR", ans.opcode, ans.words[0], ans.words[1]);
	if (ok && prm_clks_count < 12) {
		prm_clks_on[prm_clks_count++] = clk_id;
	}
	return ok;
}

static void
prm_rel_clk(uint32_t clk_id)
{
	struct prm_clk_rel_pkt p;
	struct hakios_gpr_answer ans;

	bzero(&p, sizeof(p));
	p.hdr.payload_size = (uint32_t)(sizeof(p) - sizeof(p.hdr));
	p.param.module_instance_id = GPR_PORT_PRM;
	p.param.param_id = PARAM_ID_RSC_AUDIO_HW_CLK;
	p.param.param_size = (uint32_t)(sizeof(p) - sizeof(p.hdr) - sizeof(p.param));
	p.num_clk_id = 1;
	p.clock_id = clk_id;

	(void)hakios_adsp_gpr(GPR_PORT_PAUDIO, GPR_PORT_PRM, 0, PRM_CMD_RELEASE_HW_RSC, &p, sizeof(p), 500, &ans);
}

/* ---- TLMM + I2C bitbang (qupv3_se2_i2c: GPIO 24 SDA, GPIO 25 SCL) ---------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm_base + 0x1000UL * (n))
#define GPIO_IO(n)              (tlmm_base + 0x1000UL * (n) + 4)
#define CFG_PULL_UP             3u
#define CFG_OUTPUT              (1u << 9)

#define PIN_I2C_SDA             24u
#define PIN_I2C_SCL             25u
#define PIN_PA_RST_L            36u
#define PIN_PA_RST_R            103u
#define PIN_WCD_RST             102u

static vm_offset_t tlmm_base;

static inline void
i2c_half(void)
{
	spin_us(5);
}

static void
i2c_line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0u : CFG_OUTPUT));
}

static bool
i2c_level(unsigned pin)
{
	return (rd(GPIO_IO(pin)) & 1u) != 0;
}

static void
i2c_start(void)
{
	i2c_line(PIN_I2C_SDA, true);
	i2c_line(PIN_I2C_SCL, true);
	i2c_half();
	i2c_line(PIN_I2C_SDA, false);
	i2c_half();
	i2c_line(PIN_I2C_SCL, false);
	i2c_half();
}

static void
i2c_stop(void)
{
	i2c_line(PIN_I2C_SDA, false);
	i2c_half();
	i2c_line(PIN_I2C_SCL, true);
	i2c_half();
	i2c_line(PIN_I2C_SDA, true);
	i2c_half();
}

static bool
i2c_bit(bool out)
{
	bool in;

	i2c_line(PIN_I2C_SDA, out);
	i2c_half();
	i2c_line(PIN_I2C_SCL, true);
	for (unsigned s = 0; !i2c_level(PIN_I2C_SCL) && s < 100; s++) {
		i2c_half();
	}
	i2c_half();
	in = i2c_level(PIN_I2C_SDA);
	i2c_line(PIN_I2C_SCL, false);
	return in;
}

static bool
i2c_tx(uint8_t b)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((b >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_rx(bool ack)
{
	uint8_t b = 0;

	for (int bit = 7; bit >= 0; bit--) {
		b = (uint8_t)((b << 1) | (i2c_bit(true) ? 1 : 0));
	}
	i2c_bit(!ack);
	return b;
}

static bool
i2c_probe_addr(uint8_t addr)
{
	i2c_start();
	bool ack = i2c_tx((uint8_t)(addr << 1));
	i2c_stop();
	return ack;
}

static bool
i2c_read_reg16(uint8_t addr, uint8_t reg, uint16_t *val)
{
	i2c_start();
	if (!i2c_tx((uint8_t)(addr << 1)) || !i2c_tx(reg)) {
		i2c_stop();
		return false;
	}
	i2c_start();
	if (!i2c_tx((uint8_t)((addr << 1) | 1))) {
		i2c_stop();
		return false;
	}
	uint8_t hi = i2c_rx(true);
	uint8_t lo = i2c_rx(false);
	i2c_stop();
	*val = (uint16_t)((hi << 8) | lo);
	return true;
}

static bool
i2c_write_reg16(uint8_t addr, uint8_t reg, uint16_t val)
{
	i2c_start();
	bool ok = i2c_tx((uint8_t)(addr << 1)) && i2c_tx(reg) &&
	    i2c_tx((uint8_t)(val >> 8)) && i2c_tx((uint8_t)(val & 0xff));
	i2c_stop();
	return ok;
}

/* ---- FourSemi FS1815 Dual Speaker Amplifiers (0x34 top, 0x35 bottom) ------------------------------- */

struct fs_reg_op {
	uint8_t reg;
	uint16_t mask; /* 0xffff = plain write, otherwise update_bits */
	uint16_t val;
};

/* Factory INIT table from stock vendor/firmware/frsm-spk1.bin & frsm-spk2.bin */
static const struct fs_reg_op fs1815_init_tbl[] = {
	{ 0x3e, 0xffff, 0x8500 }, { 0x3f, 0xffff, 0x8110 }, { 0x4c, 0xffff, 0x7008 }, { 0x2a, 0xffff, 0x0030 },
	{ 0x58, 0xffff, 0x827a }, { 0x59, 0xffff, 0xd821 }, { 0x5a, 0xffff, 0xd841 }, { 0x5b, 0xffff, 0xd07b },
	{ 0x5c, 0xffff, 0xd800 }, { 0x88, 0xffff, 0xa000 }, { 0x89, 0xffff, 0x0032 }, { 0x1f, 0xffff, 0xca91 },
	{ 0xcc, 0x0010, 0x0000 }, { 0x1f, 0xffff, 0x0000 }, { 0xbc, 0x01f0, 0x0020 }, { 0x4c, 0x0008, 0x0000 },
};

/* Factory MUSIC tables from frsm-spk1.bin (0x34) & frsm-spk2.bin (0x35) */
static const struct fs_reg_op fs1815_music_34[] = {
	{ 0x20, 0xffff, 0x1752 }, { 0x22, 0xffff, 0x0108 }, { 0x23, 0xffff, 0x9407 }, { 0x21, 0xffff, 0xb560 },
	{ 0x18, 0xffff, 0x000a }, { 0x4c, 0x0008, 0x0008 }, { 0x3f, 0x8000, 0x0000 }, { 0xbc, 0x8000, 0x0000 },
	{ 0x1f, 0xffff, 0xca91 }, { 0x58, 0xffff, 0x821a }, { 0x59, 0xffff, 0x0c2c }, { 0x19, 0x001f, 0x0009 },
	{ 0x5a, 0xffff, 0xfcba }, { 0x5b, 0x0c00, 0x0c00 }, { 0x5d, 0xffff, 0x2005 }, { 0x1f, 0xffff, 0x0000 },
};

static const struct fs_reg_op fs1815_music_35[] = {
	{ 0x20, 0xffff, 0x1752 }, { 0x22, 0xffff, 0x0108 }, { 0x23, 0xffff, 0x9407 }, { 0x21, 0xffff, 0xb560 },
	{ 0x18, 0xffff, 0x000a }, { 0x4c, 0x0008, 0x0008 }, { 0x3f, 0x8000, 0x0000 }, { 0xbc, 0x8000, 0x0000 },
	{ 0x1f, 0xffff, 0xca91 }, { 0x58, 0xffff, 0x821a }, { 0x59, 0xffff, 0x0c2c }, { 0x19, 0x001f, 0x0010 },
	{ 0x5a, 0xffff, 0x0cba }, { 0x5b, 0x0c00, 0x0c00 }, { 0x5d, 0xffff, 0x2005 }, { 0x1f, 0xffff, 0x0000 },
};

static bool fs1815_active;

static bool
fs1815_bus_take(uint32_t *cfg_sda, uint32_t *cfg_scl, uint32_t *io_sda, uint32_t *io_scl)
{
	*cfg_sda = rd(GPIO_CFG(PIN_I2C_SDA));
	*cfg_scl = rd(GPIO_CFG(PIN_I2C_SCL));
	*io_sda  = rd(GPIO_IO(PIN_I2C_SDA));
	*io_scl  = rd(GPIO_IO(PIN_I2C_SCL));
	i2c_line(PIN_I2C_SDA, true);
	i2c_line(PIN_I2C_SCL, true);
	i2c_half();
	if (!i2c_level(PIN_I2C_SDA) || !i2c_level(PIN_I2C_SCL)) {
		wr(GPIO_IO(PIN_I2C_SDA), *io_sda & 2);
		wr(GPIO_IO(PIN_I2C_SCL), *io_scl & 2);
		wr(GPIO_CFG(PIN_I2C_SDA), *cfg_sda);
		wr(GPIO_CFG(PIN_I2C_SCL), *cfg_scl);
		return false;
	}
	return true;
}

static void
fs1815_bus_release(uint32_t cfg_sda, uint32_t cfg_scl, uint32_t io_sda, uint32_t io_scl)
{
	wr(GPIO_IO(PIN_I2C_SDA), io_sda & 2);
	wr(GPIO_IO(PIN_I2C_SCL), io_scl & 2);
	wr(GPIO_CFG(PIN_I2C_SDA), cfg_sda);
	wr(GPIO_CFG(PIN_I2C_SCL), cfg_scl);
}

static void
fs1815_apply_tbl(uint8_t addr, const struct fs_reg_op *tbl, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		if (tbl[i].mask == 0xffffu) {
			i2c_write_reg16(addr, tbl[i].reg, tbl[i].val);
		} else {
			uint16_t cur = 0;
			if (i2c_read_reg16(addr, tbl[i].reg, &cur)) {
				i2c_write_reg16(addr, tbl[i].reg, (uint16_t)((cur & ~tbl[i].mask) | (tbl[i].val & tbl[i].mask)));
			}
		}
	}
}

static bool
fs1815_enable_mask(uint8_t mask)
{
	uint32_t cs, cc, is, ic;

	if (!fs1815_bus_take(&cs, &cc, &is, &ic)) {
		say("paudio: FS1815 bus busy\n");
		return false;
	}
	static const uint8_t addrs[2] = { 0x34, 0x35 };
	for (int i = 0; i < 2; i++) {
		if ((mask & (1u << i)) == 0) {
			continue;
		}
		uint8_t a = addrs[i];
		/* Soft reset */
		i2c_write_reg16(a, 0x10, 0x0003);
	}
	sleep_ms(15);
	for (int i = 0; i < 2; i++) {
		if ((mask & (1u << i)) == 0) {
			continue;
		}
		uint8_t a = addrs[i];
		uint16_t st = 0;
		for (int t = 0; t < 20; t++) {
			if (i2c_read_reg16(a, 0x0e, &st) && (st & 3u) == 3u) {
				break;
			}
			sleep_ms(2);
		}
		fs1815_apply_tbl(a, fs1815_init_tbl, sizeof(fs1815_init_tbl) / sizeof(fs1815_init_tbl[0]));
		i2c_write_reg16(a, 0x1f, 0x5a5a);
		if (a == 0x34) {
			fs1815_apply_tbl(a, fs1815_music_34, sizeof(fs1815_music_34) / sizeof(fs1815_music_34[0]));
		} else {
			fs1815_apply_tbl(a, fs1815_music_35, sizeof(fs1815_music_35) / sizeof(fs1815_music_35[0]));
		}
		/* Start up & unmute (exact fs18v2_start_up + fs18v2_set_mute(false)) */
		i2c_write_reg16(a, 0x11, 0x008f);
		sleep_ms(5);
		i2c_write_reg16(a, 0x10, 0x0000);
		uint16_t r10 = 0, r11 = 0, r00 = 0;
		i2c_read_reg16(a, 0x10, &r10);
		i2c_read_reg16(a, 0x11, &r11);
		i2c_read_reg16(a, 0x00, &r00);
		say("paudio: FS1815 %#04x ON (r00=%#06x r10=%#06x r11=%#06x)\n", a, r00, r10, r11);
	}
	fs1815_active = true;
	fs1815_bus_release(cs, cc, is, ic);
	return true;
}

static bool
fs1815_enable_both(void)
{
	return fs1815_enable_mask(0x3u);
}

static void
fs1815_disable_both(void)
{
	uint32_t cs, cc, is, ic;

	if (!fs1815_active || tlmm_base == 0) {
		return;
	}
	fs1815_active = false;
	if (!fs1815_bus_take(&cs, &cc, &is, &ic)) {
		return;
	}
	static const uint8_t addrs[2] = { 0x34, 0x35 };
	for (int i = 0; i < 2; i++) {
		uint8_t a = addrs[i];
		uint16_t r3f = 0;
		if (i2c_read_reg16(a, 0x3f, &r3f) && (r3f & 0x8000u)) {
			i2c_write_reg16(a, 0x3f, (uint16_t)(r3f & 0x7fffu));
			sleep_ms(10);
		}
		i2c_write_reg16(a, 0x10, 0x0001); /* mute */
	}
	sleep_ms(20);
	for (int i = 0; i < 2; i++) {
		i2c_write_reg16(addrs[i], 0x11, 0x0000); /* shut down */
	}
	fs1815_bus_release(cs, cc, is, ic);
	say("paudio: FS1815 both muted & off\n");
}

static void
i2c_scan_bus(const char *tag)
{
	uint32_t cfg_sda = rd(GPIO_CFG(PIN_I2C_SDA)), cfg_scl = rd(GPIO_CFG(PIN_I2C_SCL));
	uint32_t io_sda = rd(GPIO_IO(PIN_I2C_SDA)), io_scl = rd(GPIO_IO(PIN_I2C_SCL));
	unsigned found = 0;

	i2c_line(PIN_I2C_SDA, true);
	i2c_line(PIN_I2C_SCL, true);
	i2c_half();
	if (!i2c_level(PIN_I2C_SDA) || !i2c_level(PIN_I2C_SCL)) {
		say("paudio: I2C(%s) bus not idle: SDA=%u SCL=%u (orig cfg %#x %#x)\n",
		    tag, i2c_level(PIN_I2C_SDA), i2c_level(PIN_I2C_SCL), cfg_sda, cfg_scl);
		wr(GPIO_IO(PIN_I2C_SDA), io_sda & 2);
		wr(GPIO_IO(PIN_I2C_SCL), io_scl & 2);
		wr(GPIO_CFG(PIN_I2C_SDA), cfg_sda);
		wr(GPIO_CFG(PIN_I2C_SCL), cfg_scl);
		return;
	}
	say("paudio: I2C(%s) scan GPIO24/25:\n", tag);
	for (uint8_t a = 0x34; a <= 0x35; a++) {
		if (i2c_probe_addr(a)) {
			uint16_t v[16] = { 0 };
			for (uint8_t r = 0; r < 16; r++) {
				i2c_read_reg16(a, r, &v[r]);
			}
			say("paudio:   FS1815 %#04x r0..7: %04x %04x %04x %04x %04x %04x %04x %04x\n",
			    a, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
			say("paudio:   FS1815 %#04x r8..f: %04x %04x %04x %04x %04x %04x %04x %04x\n",
			    a, v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
			found++;
		}
	}
	if (found == 0) {
		say("paudio:   (no FS1815 at 0x34/0x35)\n");
	}
	wr(GPIO_IO(PIN_I2C_SDA), io_sda & 2);
	wr(GPIO_IO(PIN_I2C_SCL), io_scl & 2);
	wr(GPIO_CFG(PIN_I2C_SDA), cfg_sda);
	wr(GPIO_CFG(PIN_I2C_SCL), cfg_scl);
}

static void
probe_pa_i2c(void)
{
	i2c_scan_bus("FS1815");
}

/* ---- LPASS Macros + LPI TLMM + SoundWire Masters + WCD937x ----------------------------------------- */

#define RX_MACRO_PHYS           0x03200000UL
#define RX_SWR_PHYS             0x03210000UL
#define TX_MACRO_PHYS           0x03220000UL
#define VA_SWR_PHYS             0x033b0000UL
#define VA_MACRO_PHYS           0x033f0000UL
#define RX_MUXSEL_PAGE          0x033a4000UL
#define RX_MUXSEL_OFF           0x0d8u
#define VA_MUXSEL_PHYS          0x03420000UL
#define LPI_PHYS                0x03440000UL
#define LPI_SIZE                0x20000UL
#define LPI_SLEW_PHYS           0x034da000UL

#define SWRM_COMP_HW_VERSION            0x0000u
#define SWRM_COMP_CFG                   0x0004u
#define SWRM_COMP_SW_RESET              0x0008u
#define SWRM_COMP_STATUS                0x0014u
#define SWRM_LINK_MANAGER_EE            0x0018u
#define SWRM_COMP_MASTER_ID             0x0104u
#define SWRM_INTERRUPT_STATUS           0x0200u
#define SWRM_INTERRUPT_EN               0x0204u
#define SWRM_INTERRUPT_CLEAR            0x0208u
#define SWRM_CPU1_INTERRUPT_EN          0x0210u
#define SWRM_CMD_FIFO_WR_CMD            0x0300u
#define SWRM_CMD_FIFO_RD_CMD            0x0304u
#define SWRM_CMD_FIFO_STATUS            0x030cu
#define SWRM_CMD_FIFO_CFG               0x0314u
#define SWRM_CMD_FIFO_RD_FIFO           0x0318u
#define SWRM_ENUMERATOR_CFG             0x0500u
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_1(m) (0x0530u + 0x8u * (m))
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_2(m) (0x0534u + 0x8u * (m))
#define SWRM_MCP_FRAME_CTRL_BANK(m)     (0x101cu + 0x40u * (m))
#define SWRM_MCP_BUS_CTRL               0x1044u
#define SWRM_MCP_CFG                    0x1048u
#define SWRM_MCP_STATUS                 0x104cu
#define SWRM_MCP_SLV_STATUS             0x1090u
#define SWRM_DP_PORT_CTRL_BANK(n, m)    (0x1024u + 0x100u * (n) + 0x40u * (m))
#define SWRM_DP_PORT_CTRL_2_BANK(n, m)  (0x1028u + 0x100u * (n) + 0x40u * (m))
#define SWRM_DP_BLOCK_CTRL1(n)          (0x102cu + 0x100u * (n))
#define SWRM_DP_BLOCK_CTRL2_BANK(n, m)  (0x1030u + 0x100u * (n) + 0x40u * (m))
#define SWRM_DP_PORT_HCTRL_BANK(n, m)   (0x1034u + 0x100u * (n) + 0x40u * (m))
#define SWRM_DP_BLOCK_CTRL3_BANK(n, m)  (0x1038u + 0x100u * (n) + 0x40u * (m))
#define SWRM_DP_SAMPLECTRL2_BANK(n, m)  (0x103cu + 0x100u * (n) + 0x40u * (m))
#define SWRM_INTERRUPT_STATUS_MASK      0x1fdfdu

#define SWRS_SCP_CONTROL                0x0044u
#define SWRS_SCP_FRAME_CTRL_BANK(m)     (0x0060u + 0x10u * (m))
#define SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(m) (0x00e0u + 0x10u * (m))
#define SWRS_DP_BLOCK_CTRL1(n)          (0x0103u + 0x100u * (n))
#define SWRS_DP_CHANNEL_ENABLE_BANK(n, m) (0x0120u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_SAMPLE_CONTROL_1_BANK(n, m) (0x0122u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_SAMPLE_CONTROL_2_BANK(n, m) (0x0123u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_OFFSET_CONTROL_1_BANK(n, m) (0x0124u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_OFFSET_CONTROL_2_BANK(n, m) (0x0125u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_HCONTROL_BANK(n, m)     (0x0126u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_BLOCK_CTRL3_BANK(n, m)  (0x0127u + 0x100u * (n) + 0x10u * (m))
#define SWRS_DP_LANE_CONTROL_BANK(n, m) (0x0128u + 0x100u * (n) + 0x10u * (m))

#define WCD937X_TX_ID                   0x000a01170223ull
#define WCD937X_RX_ID                   0x000a01170224ull

static vm_offset_t rx_macro_base, rx_swr_base, tx_macro_base, va_macro_base, va_swr_base;
static vm_offset_t lpi_base, lpi_slew_base, rx_mux_base, va_mux_base;
static uint8_t wcd_tx_dev, wcd_rx_dev;

static void
reg_bits(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t val)
{
	uint32_t v = rd(base + off);
	wr(base + off, (v & ~mask) | (val & mask));
}

static bool
swr_master_start(const char *name, vm_offset_t base)
{
	uint32_t ver = rd(base + SWRM_COMP_HW_VERSION);
	uint32_t mid = rd(base + SWRM_COMP_MASTER_ID);
	uint32_t ssp = 9600000u * 2u / (50u * 2u * 4000u); /* 48 */
	uint32_t cfg;
	bool linked = false;

	say("paudio: SWR %s: HW_VER=%#x MASTER_ID=%#x initial STATUS=%#x\n",
	    name, ver, mid, rd(base + SWRM_COMP_STATUS));

	wr(base + SWRM_COMP_SW_RESET, 1u);
	spin_us(100);
	wr(base + SWRM_COMP_SW_RESET, 1u);
	spin_us(100);

	/* SWRM v1.7.0 (0x01070000): select CPU1 execution environment (ee=1), clk_start = BIT(1) << 1 = 0x4 */
	cfg = (rd(base + SWRM_MCP_CFG) & ~0x3e0000u) | (0x1fu << 17);
	wr(base + SWRM_MCP_FRAME_CTRL_BANK(0), (1u << 3) | ((ssp - 1u) << 16));
	wr(base + SWRM_ENUMERATOR_CFG, 1u);
	wr(base + SWRM_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK);
	wr(base + SWRM_MCP_CFG, cfg);
	if (ver == 0x01070000u) {
		wr(base + SWRM_LINK_MANAGER_EE, 1u);
		wr(base + SWRM_MCP_BUS_CTRL, 1u << 2); /* SWRM_MCP_BUS_CLK_START << 1 */
	} else {
		wr(base + SWRM_MCP_BUS_CTRL, 2u);
	}
	wr(base + SWRM_CMD_FIFO_CFG, 7u | 0x80000000u);
	wr(base + SWRM_COMP_CFG, 3u);
	wr(base + SWRM_INTERRUPT_CLEAR, 0xffffffffu);
	wr(base + SWRM_CPU1_INTERRUPT_EN, SWRM_INTERRUPT_STATUS_MASK);

	for (int i = 0; i < 100 && !linked; i++) {
		linked = (rd(base + SWRM_COMP_STATUS) & 1u) != 0;
		if (!linked) {
			spin_us(510);
		}
	}
	say("paudio: SWR %s: link %s (COMP_STATUS=%#x MCP_STATUS=%#x IRQ=%#x EE=%#x BUS=%#x)\n",
	    name, linked ? "UP" : "DOWN",
	    rd(base + SWRM_COMP_STATUS), rd(base + SWRM_MCP_STATUS), rd(base + SWRM_INTERRUPT_STATUS),
	    rd(base + SWRM_LINK_MANAGER_EE), rd(base + SWRM_MCP_BUS_CTRL));
	return linked;
}

static void
swr_master_stop(vm_offset_t base)
{
	if (base == 0) {
		return;
	}
	wr(base + SWRM_COMP_CFG, 0u);
	wr(base + SWRM_INTERRUPT_EN, 0u);
	wr(base + SWRM_CPU1_INTERRUPT_EN, 0u);
	wr(base + SWRM_COMP_SW_RESET, 1u);
}

static bool
swr_read_reg(vm_offset_t base, uint8_t dev, uint16_t reg, uint8_t *val)
{
	static uint8_t rid = 0;
	uint32_t ans = 0;

	rid = rid < 14 ? (uint8_t)(rid + 1) : 0;
	wr(base + SWRM_CMD_FIFO_RD_CMD,
	    (uint32_t)reg | ((uint32_t)rid << 16) | ((uint32_t)dev << 20) | (1u << 24));
	spin_us(255);
	for (int t = 0; t < 4; t++) {
		for (int i = 0; i < 20 && ((rd(base + SWRM_CMD_FIFO_STATUS) >> 16) & 0x1fu) == 0; i++) {
			spin_us(500);
		}
		ans = rd(base + SWRM_CMD_FIFO_RD_FIFO);
		if (((ans >> 8) & 0xfu) == rid) {
			*val = (uint8_t)ans;
			return true;
		}
		spin_us(505);
	}
	return false;
}

static uint8_t
swr_wait_slave(const char *name, vm_offset_t base, uint64_t expected_id)
{
	for (int ms = 0; ms < 300; ms++) {
		uint32_t st = rd(base + SWRM_MCP_SLV_STATUS);
		for (uint8_t d = 1; d <= 11; d++) {
			uint64_t id = ((uint64_t)rd(base + SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32) |
			    rd(base + SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));
			uint32_t dst = (st >> (2 * d)) & 3u;
			if ((id & 0xffffffffffffull) == expected_id && (dst == 1 || dst == 2)) {
				say("paudio: SWR %s: FOUND dev %u ID %#llx (st %u) in %d ms\n",
				    name, d, id & 0xffffffffffffull, dst, ms);
				return d;
			}
		}
		sleep_ms(1);
	}
	say("paudio: SWR %s: expected ID %#llx NOT found (SLV_STATUS=%#x)\n",
	    name, expected_id, rd(base + SWRM_MCP_SLV_STATUS));
	for (uint8_t d = 1; d <= 11; d++) {
		uint64_t id = ((uint64_t)rd(base + SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32) |
		    rd(base + SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));
		if (id != 0) {
			say("paudio:   dev %u: ID %#llx\n", d, id);
		}
	}
	return 0;
}

static void
swr_write_reg(vm_offset_t base, uint8_t dev, uint16_t reg, uint8_t val, uint8_t cmd_id)
{
	uint32_t cmd = (uint32_t)reg | ((uint32_t)cmd_id << 16) | ((uint32_t)dev << 20) | ((uint32_t)val << 24);
	for (int i = 0; i < 20 && ((rd(base + SWRM_CMD_FIFO_STATUS) >> 8) & 0x1fu) >= 16; i++) {
		spin_us(500);
	}
	wr(base + SWRM_CMD_FIFO_WR_CMD, cmd);
	spin_us(155);
}

static uint8_t swr_wid;

static void
swr_wr(vm_offset_t base, uint8_t dev, uint16_t reg, uint8_t val)
{
	swr_wid = swr_wid < 14 ? (uint8_t)(swr_wid + 1) : 0;
	swr_write_reg(base, dev, reg, val, swr_wid);
}

static void
swr_broadcast(vm_offset_t base, uint16_t reg, uint8_t val)
{
	swr_write_reg(base, 0xfu, reg, val, 0xfu);
	sleep_ms(10);
}

static bool
wcd_bits(uint16_t reg, uint8_t mask, uint8_t val)
{
	uint8_t cur = 0;
	if (wcd_tx_dev == 0 || !swr_read_reg(va_swr_base, wcd_tx_dev, reg, &cur)) {
		return false;
	}
	uint8_t next = (uint8_t)((cur & ~mask) | (val & mask));
	if (next != cur) {
		swr_wr(va_swr_base, wcd_tx_dev, reg, next);
	}
	return true;
}

static uint32_t pa_rx2_base = 0x580u;

static void
pa_rx_set_vol(int db0, int db2)
{
	if (rx_macro_base == 0) {
		return;
	}
	if (db0 < -84) {
		db0 = -84;
	}
	if (db0 > 12) {
		db0 = 12;
	}
	if (db2 < -84) {
		db2 = -84;
	}
	if (db2 > 12) {
		db2 = 12;
	}
	wr(rx_macro_base + 0x414u, (uint32_t)(uint8_t)(int8_t)db0);
	wr(rx_macro_base + pa_rx2_base + 0x14u, (uint32_t)(uint8_t)(int8_t)db2);
}

static void
tx_dec2_unmute(void)
{
	if (tx_macro_base == 0) {
		return;
	}
	reg_bits(tx_macro_base, 0x500u, 0x10u, 0x00u);
}

static void
tx_dec2_settle(void)
{
	if (tx_macro_base == 0) {
		return;
	}
	reg_bits(tx_macro_base, 0x504u, 0x60u, 0x00u);
	reg_bits(tx_macro_base, 0x518u, 0x03u, 0x02u);
	spin_us(25);
	reg_bits(tx_macro_base, 0x518u, 0x03u, 0x01u);
}

static void
va_swr_cfg_bank(uint8_t bank, bool set_div2)
{
	if (set_div2) {
		swr_wr(va_swr_base, 0xfu, SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(bank), 0x01);
	}
	/* Exact parrot DTB qcom,swr-tx-port-params for WCD937x ADC1 (4.8 MHz): si=1, off1=0, off2=0, lane=1 */
	swr_wr(va_swr_base, wcd_tx_dev, SWRS_DP_CHANNEL_ENABLE_BANK(0, bank), 0x01);
	swr_wr(va_swr_base, wcd_tx_dev, SWRS_DP_SAMPLE_CONTROL_1_BANK(0, bank), 1);
	swr_wr(va_swr_base, wcd_tx_dev, SWRS_DP_SAMPLE_CONTROL_2_BANK(0, bank), 0);
	swr_wr(va_swr_base, wcd_tx_dev, SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), 0);
	swr_wr(va_swr_base, wcd_tx_dev, SWRS_DP_OFFSET_CONTROL_2_BANK(0, bank), 0);
	swr_wr(va_swr_base, wcd_tx_dev, SWRS_DP_LANE_CONTROL_BANK(0, bank), 1);

	wr(va_swr_base + SWRM_DP_PORT_CTRL_BANK(1, bank), (0x01u << 24) | 1u);
	wr(va_swr_base + SWRM_DP_SAMPLECTRL2_BANK(1, bank), 0u);
	wr(va_swr_base + SWRM_DP_PORT_CTRL_2_BANK(1, bank), 1u);
	wr(va_swr_base + SWRM_DP_PORT_HCTRL_BANK(1, bank), 0xf0u);
}

static void
rx_swr_cfg_bank(uint8_t bank, bool set_div2)
{
	if (set_div2) {
		swr_wr(rx_swr_base, 0xfu, SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(bank), 0x01);
	}
	/* Port 1: HPH_L/EAR ch0 */
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_BLOCK_CTRL1(0), 0x01);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_CHANNEL_ENABLE_BANK(0, bank), 0x01);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_SAMPLE_CONTROL_1_BANK(0, bank), 0x03);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_SAMPLE_CONTROL_2_BANK(0, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_OFFSET_CONTROL_1_BANK(0, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_OFFSET_CONTROL_2_BANK(0, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_LANE_CONTROL_BANK(0, bank), 0x01);
	wr(rx_swr_base + SWRM_DP_BLOCK_CTRL1(1), 0x01u);
	wr(rx_swr_base + SWRM_DP_PORT_CTRL_BANK(1, bank), (0x01u << 24) | 0x03u);
	wr(rx_swr_base + SWRM_DP_SAMPLECTRL2_BANK(1, bank), 0x00u);
	wr(rx_swr_base + SWRM_DP_PORT_CTRL_2_BANK(1, bank), 0x01u);
	wr(rx_swr_base + SWRM_DP_PORT_HCTRL_BANK(1, bank), 0xf0u);

	/* Port 2: CLSH ch0 */
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_BLOCK_CTRL1(1), 0x07);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_CHANNEL_ENABLE_BANK(1, bank), 0x01);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_SAMPLE_CONTROL_1_BANK(1, bank), 0x3f);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_SAMPLE_CONTROL_2_BANK(1, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_OFFSET_CONTROL_1_BANK(1, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_OFFSET_CONTROL_2_BANK(1, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_HCONTROL_BANK(1, bank), (3u << 4) | 6u);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_BLOCK_CTRL3_BANK(1, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_LANE_CONTROL_BANK(1, bank), 0x00);
	wr(rx_swr_base + SWRM_DP_BLOCK_CTRL1(2), 0x07u);
	wr(rx_swr_base + SWRM_DP_PORT_CTRL_BANK(2, bank), (0x01u << 24) | 0x3fu);
	wr(rx_swr_base + SWRM_DP_SAMPLECTRL2_BANK(2, bank), 0x00u);
	wr(rx_swr_base + SWRM_DP_PORT_CTRL_2_BANK(2, bank), 0x00u);
	wr(rx_swr_base + SWRM_DP_PORT_HCTRL_BANK(2, bank), (6u << 4) | 3u);
	wr(rx_swr_base + SWRM_DP_BLOCK_CTRL3_BANK(2, bank), 0x00u);

	/* Port 4: LO/AUX ch0 */
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_CHANNEL_ENABLE_BANK(3, bank), 0x01);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_SAMPLE_CONTROL_1_BANK(3, bank), 0x07);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_SAMPLE_CONTROL_2_BANK(3, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_OFFSET_CONTROL_1_BANK(3, bank), 0x01);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_OFFSET_CONTROL_2_BANK(3, bank), 0x00);
	swr_wr(rx_swr_base, wcd_rx_dev, SWRS_DP_LANE_CONTROL_BANK(3, bank), 0x00);
	wr(rx_swr_base + SWRM_DP_PORT_CTRL_BANK(4, bank), (0x01u << 24) | (1u << 8) | 0x07u);
	wr(rx_swr_base + SWRM_DP_SAMPLECTRL2_BANK(4, bank), 0x00u);
	wr(rx_swr_base + SWRM_DP_PORT_CTRL_2_BANK(4, bank), 0x00u);
	wr(rx_swr_base + SWRM_DP_PORT_HCTRL_BANK(4, bank), 0xf0u);
}

static void
wcd937x_setup_hw(bool enable_spkr, bool enable_mic, int init_vol_db)
{
	uint32_t id0 = rd(va_macro_base + 0x0c0u) & 0xffu;
	uint32_t id1 = rd(va_macro_base + 0x0c4u) & 0xffu;
	uint32_t id2 = rd(va_macro_base + 0x0c8u) & 0xffu;
	uint32_t rev_min = (id2 >> 4) & 0xfu;
	say("paudio: VA CORE_ID=%02x %02x %02x (v%u.%u)\n", id0, id1, id2, id0, rev_min);

	/* 1. WCD937x io_init (wcd937x_io_init from wcd937x.c) */
	uint8_t ef29 = 0, ef16 = 0, ef17 = 0;
	swr_read_reg(va_swr_base, wcd_tx_dev, 0x34cd, &ef29);
	swr_read_reg(va_swr_base, wcd_tx_dev, 0x34c0, &ef16);
	swr_read_reg(va_swr_base, wcd_tx_dev, 0x34c1, &ef17);
	wcd_bits(0x3103, 0x0e, (ef16 == 0x02 || ef17 > 0x09) ? (ef29 & 0x0e) : 0x0e);
	wcd_bits(0x3103, 0x80, 0x80);
	sleep_ms(1);
	wcd_bits(0x3103, 0x40, 0x40);
	sleep_ms(1);
	wcd_bits(0x312a, 0x10, 0x00);
	wcd_bits(0x3029, 0xf0, 0x80);
	wcd_bits(0x3001, 0x80, 0x80);
	wcd_bits(0x3001, 0x40, 0x40);
	sleep_ms(10);
	wcd_bits(0x3001, 0x40, 0x00);
	wcd_bits(0x30e2, 0xff, 0xd9);
	wcd_bits(0x306b, 0xff, 0xfa);
	wcd_bits(0x306e, 0xff, 0xfa);
	wcd_bits(0x3071, 0xff, 0xfa);
	wcd_bits(0x306c, 0x38, 0x00);
	wcd_bits(0x306f, 0x38, 0x00);
	wcd_bits(0x3072, 0x38, 0x00);
	if (ef16 == 0x01) {
		wcd_bits(0x3029, 0xf0, 0xb0);
	} else if (ef16 == 0x02) {
		wcd_bits(0x3133, 0x1f, 0x04);
		wcd_bits(0x3136, 0x1f, 0x04);
		wcd_bits(0x3029, 0xf0, 0xb0);
		wcd_bits(0x3132, 0xf0, 0x50);
	}
	wcd_bits(0x3022, 0x3f, 0x22); /* MICBIAS1 = 2.7V ((2700-1000)/50 = 34 = 0x22) */

	/* 2. Microphone path: WCD937x TX ADC1 (AMIC1) -> VA SWR DP1 -> TX MACRO DEC2 */
	if (enable_mic && wcd_tx_dev != 0) {
		reg_bits(va_macro_base, 0x0d0u, 0xeeu, 0xccu);
		reg_bits(va_macro_base, 0x0d4u, 0xeeu, 0xccu);
		reg_bits(va_macro_base, 0x0d8u, 0xeeu, 0xccu);
		wr(tx_macro_base + 0x0d0u, 0u); /* SWR_AMIC0_CTL default DIV2 */
		wr(tx_macro_base + 0x0d4u, 0u); /* SWR_AMIC1_CTL default DIV2 */

		uint8_t bank = (rd(va_swr_base + SWRM_MCP_STATUS) & 1u) ? 0u : 1u;
		va_swr_cfg_bank(bank, true);
		uint32_t frame = (rd(va_swr_base + SWRM_MCP_FRAME_CTRL_BANK(bank)) & ~0xff07ffu) |
		    (1u << 3) | 7u | ((6u - 1u) << 16);
		wr(va_swr_base + SWRM_MCP_FRAME_CTRL_BANK(bank), frame);
		swr_broadcast(va_swr_base, SWRS_SCP_FRAME_CTRL_BANK(bank), (1u << 3) | 7u);
		say("paudio: VA SWR bank -> %u (want %u)\n", rd(va_swr_base + SWRM_MCP_STATUS) & 1u, bank);
		va_swr_cfg_bank(bank ^ 1u, false);

		/* WCD937x MICBIAS1 + ADC1 + REQ (exact wcd937x.c sequence) */
		wcd_bits(0x3409, 0xf0, 0xf0); /* DIGITAL_CDC_DIG_CLK_CTL bits 7:4 */
		wcd_bits(0x3408, 0x18, 0x18); /* DIGITAL_CDC_ANA_CLK_CTL bits 4:3 */
		wcd_bits(0x306c, 0x01, 0x01); /* MICB1_TEST_CTL_2 */
		wcd_bits(0x306f, 0x01, 0x01); /* MICB2_TEST_CTL_2 */
		wcd_bits(0x3072, 0x01, 0x01); /* MICB3_TEST_CTL_2 */
		wcd_bits(0x3022, 0xc0, 0x40); /* ANA_MICB1 enable */
		sleep_ms(2);
		wcd_bits(0x300e, 0x1f, 8);    /* ANA_TX_CH1 gain = 8 (+12 dB) */
		wcd_bits(0x3457, 0x03, 0x02); /* DIGITAL_CDC_REQ_CTL: BIT(1)=1, BIT(0)=0 */
		wcd_bits(0x300f, 0x40, 0x40); /* ANA_TX_CH2 HPF hold */
		wcd_bits(0x3011, 0x40, 0x40); /* ANA_TX_CH3_HPF */
		wcd_bits(0x300e, 0x80, 0x80); /* ANA_TX_CH1 BIT(7): ENABLE ADC1! */
		wcd_bits(0x300f, 0x40, 0x00); /* release HPF hold */
		wcd_bits(0x300f, 0x80, 0x80); /* ANA_TX_CH2 BIT(7) */
		wcd_bits(0x3010, 0x80, 0x80); /* ANA_TX_CH3 BIT(7) */

		/* TX_MACRO DEC2: SWR_MIC0 -> DEC2 -> TX_AIF1_CAP (48 kHz) */
		reg_bits(tx_macro_base, 0x504u, 0x80u, 0x00u); /* analog mic */
		reg_bits(tx_macro_base, 0x110u, 0x0fu, 0x01u); /* ADC_MUX2_CFG0 = SWR_MIC0 */
		reg_bits(tx_macro_base, 0x114u, 0x03u, 0x01u); /* ADC_MUX2_CFG1 = SWR_MIC */
		reg_bits(tx_macro_base, 0x500u, 0x0fu, 0x04u); /* 48 kHz */
		reg_bits(tx_macro_base, 0x504u, 0x06u, 0x00u); /* ADC mode 0 */
		reg_bits(tx_macro_base, 0x500u, 0x10u, 0x10u); /* mute while HPF settles */
		reg_bits(tx_macro_base, 0x500u, 0x20u, 0x20u); /* CLK_EN */
		reg_bits(tx_macro_base, 0x504u, 0x60u, 0x40u); /* HPF 150 Hz during startup */
		reg_bits(tx_macro_base, 0x518u, 0x03u, 0x02u); /* HPF_F_CHANGE */
		sleep_ms(6);
		reg_bits(tx_macro_base, 0x518u, 0x02u, 0x00u);
		wr(tx_macro_base + 0x50cu, 0x00u);             /* TX_VOL_CTL = 0 dB */
		uint8_t ch1_rb = 0;
		swr_read_reg(va_swr_base, wcd_tx_dev, 0x300e, &ch1_rb);
		say("paudio: TX DEC2 + ADC1 ON (tx2_ctl=%#x mux=%#x/%#x ch1=%#x)\n",
		    rd(tx_macro_base + 0x500u), rd(tx_macro_base + 0x110u), rd(tx_macro_base + 0x114u), ch1_rb);
	}

	/* 3. Speaker path: RX_MACRO INT0(RX0)->EAR + INT2(RX1)->AUX -> RX SWR -> WCD937x */
	if (enable_spkr && wcd_rx_dev != 0) {
		bool is_v25 = (id0 == 0x02 && rev_min >= 5u);
		uint32_t stride = is_v25 ? 0xc0u : 0x80u;
		uint32_t rx2_base = 0x400u + 2u * stride;
		uint32_t rx2_sec7 = is_v25 ? (rx2_base + 0x40u) : (rx2_base + 0x34u);
		uint32_t rx2_dsm  = rx2_base + 0x4cu; /* 0x54c (<2.5) or 0x5cc (>=2.5) */
		pa_rx2_base = rx2_base;

		uint8_t bank = (rd(rx_swr_base + SWRM_MCP_STATUS) & 1u) ? 0u : 1u;
		rx_swr_cfg_bank(bank, true);
		uint32_t frame = (rd(rx_swr_base + SWRM_MCP_FRAME_CTRL_BANK(bank)) & ~0xff07ffu) |
		    (1u << 3) | 7u | ((6u - 1u) << 16);
		wr(rx_swr_base + SWRM_MCP_FRAME_CTRL_BANK(bank), frame);
		swr_broadcast(rx_swr_base, SWRS_SCP_FRAME_CTRL_BANK(bank), (1u << 3) | 7u);
		say("paudio: RX SWR bank -> %u (want %u, stride=%#x rx2=%#x)\n",
		    rd(rx_swr_base + SWRM_MCP_STATUS) & 1u, bank, stride, rx2_base);
		rx_swr_cfg_bank(bank ^ 1u, false);

		/* Probe-time RX_MACRO DSM delay & DC coeff (lpass-rx-macro.c probe) */
		reg_bits(rx_macro_base, 0x434u, 0x07u, 0x02u);    /* RX0 SEC7: DSM_OUT_DELAY_TWO_SAMPLE */
		reg_bits(rx_macro_base, rx2_sec7, 0x07u, 0x02u);  /* RX2 SEC7: DSM_OUT_DELAY_TWO_SAMPLE */
		reg_bits(rx_macro_base, 0x410u, 0x03u, 0x02u);    /* RX0 CFG3: DC_COEFF_SEL_TWO */
		reg_bits(rx_macro_base, rx2_base + 0x10u, 0x03u, 0x02u); /* RX2 CFG3: DC_COEFF_SEL_TWO */

		/* RX_MACRO routing: RX0 -> INT0 (EAR), RX1 -> INT2 (AUX) */
		reg_bits(rx_macro_base, 0x180u, 0x0fu, 0x05u);
		reg_bits(rx_macro_base, 0x190u, 0x0fu, 0x06u);
		/* Class-H block */
		reg_bits(rx_macro_base, 0x200u, 0x01u, 0x01u);
		wr(rx_macro_base + 0x220u, 0xc0u);             /* K1_LSB */
		reg_bits(rx_macro_base, 0x21cu, 0x0fu, 0x00u); /* K1_MSB */
		reg_bits(rx_macro_base, 0x20cu, 0x3fu, 0x39u); /* HPH_V_PA (EAR mode) */
		reg_bits(rx_macro_base, 0x208u, 0x07u, 0x00u); /* DECAY_CTRL */

		/* RX0 (INT0 -> EAR -> FS1815 0x34) */
		wr(rx_macro_base + 0x400u, 0x14u);             /* mute, 48 kHz */
		reg_bits(rx_macro_base, 0x440u, 0x01u, 0x01u); /* DSM_CLK_EN */
		reg_bits(rx_macro_base, 0x40cu, 0x03u, 0x03u); /* HPF cut freq */
		reg_bits(rx_macro_base, 0x410u, 0x03u, 0x03u); /* DC coeff for 48 kHz */
		reg_bits(rx_macro_base, 0x404u, 0x40u, 0x40u); /* RX0 CLSH_EN (bit 6) */
		reg_bits(rx_macro_base, 0x408u, 0x03u, 0x03u); /* HPH_L_EAR_SEL (bit 1) | CLSH_DSM_OUT (bit 0) */
		wr(rx_macro_base + 0x400u, 0x24u);             /* CLK_EN, unmuted, 48 kHz */

		/* RX2 (INT2 -> AUX -> FS1815 0x35) */
		wr(rx_macro_base + rx2_base + 0x00u, 0x14u);   /* mute, 48 kHz */
		reg_bits(rx_macro_base, rx2_dsm, 0x01u, 0x01u);/* DSM_CLK_EN */
		reg_bits(rx_macro_base, rx2_base + 0x0cu, 0x03u, 0x03u);
		reg_bits(rx_macro_base, rx2_base + 0x04u, 0x18u, 0x18u); /* RX2 CLSH_EN (bit 4) | DLY_Z (bit 3) */
		reg_bits(rx_macro_base, rx2_base + 0x08u, 0x04u, 0x00u); /* aux_hpf off */
		wr(rx_macro_base + rx2_base + 0x00u, 0x24u);   /* CLK_EN, unmuted, 48 kHz */

		/* Apply digital volume AFTER CLK_EN (POST_PMU) */
		pa_rx_set_vol(init_vol_db, init_vol_db - 3);

		/* WCD937x rx_clk_enable (exact wcd937x.c order) */
		wcd_bits(0x3409, 0x08u, 0x08u); /* DIG_CLK_CTL: RX_CLK(3) */
		wcd_bits(0x3408, 0x01u, 0x01u); /* ANA_CLK_CTL: bit 0 */
		wcd_bits(0x3008, 0x01u, 0x01u); /* ANA_RX_SUPPLIES: RX_BIAS */
		wcd_bits(0x340d, 0x40u, 0x00u); /* CDC_RX0_CTL */
		wcd_bits(0x340e, 0x40u, 0x00u); /* CDC_RX1_CTL */
		wcd_bits(0x340f, 0x40u, 0x00u); /* CDC_RX2_CTL */
		wcd_bits(0x3408, 0x02u, 0x02u); /* ANA_CLK_CTL: bit 1 */

		/* EAR DAC + AUX DAC setup (wcd937x.c) */
		wcd_bits(0x3450, 0x01u, 0x00u); /* EAR_PATH_CTL: RDAC3_MUX = RX1 */
		wcd_bits(0x344e, 0x04u, 0x04u); /* CDC_HPH_GAIN_CTL */
		wcd_bits(0x3409, 0x01u, 0x01u); /* DIG_CLK_CTL: RX0(0) */
		wcd_bits(0x30d9, 0x80u, 0x00u); /* HPH_RDAC_CLK_CTL1 */
		wcd_bits(0x3408, 0x04u, 0x04u); /* ANA_CLK_CTL: AUX(2) */
		wcd_bits(0x3128, 0x10u, 0x10u); /* AUX_AUXPA: AUX DAC(4) */
		wcd_bits(0x3409, 0x04u, 0x04u); /* DIG_CLK_CTL: RX2(2) */
		wcd_bits(0x344f, 0x01u, 0x01u); /* CDC_AUX_GAIN_CTL */
		sleep_ms(5);

		/* Class-H v3 enable (exact wcd-clsh-v2.c v3_state_ear) */
		wcd_bits(0x30a4, 0x04u, 0x00u); /* FLYBACK_EN */
		wcd_bits(0x3008, 0x02u, 0x00u); /* ANA_RX_SUPPLIES: CLS_H */
		wcd_bits(0x3008, 0x04u, 0x00u); /* VNEG default */
		wcd_bits(0x30a8, 0xf0u, 0x70u); /* FLYBACK_VNEG_CTRL_4 */
		wcd_bits(0x30af, 0xe0u, 0xa0u); /* force_iq_ctl */
		spin_us(110);
		wcd_bits(0x3099, 0x02u, 0x02u); /* CLASSH_MODE_3 */
		wcd_bits(0x3098, 0xffu, 0x1cu); /* CLASSH_MODE_2 */
		wcd_bits(0x30a5, 0xe0u, 0xe0u); /* FLYBACK_VNEG_CTRL_1 */
		wcd_bits(0x3008, 0x40u, 0x40u); /* ANA_RX_SUPPLIES: VNEG */
		spin_us(110);
		wcd_bits(0x30af, 0xe0u, 0xe0u); /* FLYBACK_VNEGDAC_CTRL_2 */
		spin_us(510);
		wcd_bits(0x30c7, 0xffu, 0xaau); /* RX_BIAS_FLYB_BUFF */
		spin_us(110);
		wcd_bits(0x3008, 0x80u, 0x80u); /* ANA_RX_SUPPLIES: VPOS/BUCK */
		sleep_ms(2);
		wcd_bits(0x3099, 0x02u, 0x00u); /* CLASSH_MODE_3 */
		wcd_bits(0x3098, 0xffu, 0x3au); /* CLASSH_MODE_2 */
		sleep_ms(2);

		/* EAR PGA (0 dB: bits[6:2]=4 -> 0x90, BIT(7)=1) + AUX PA */
		wcd_bits(0x300b, 0xfcu, 0x90u); /* ANA_EAR_COMPANDER_CTL: 0 dB gain, BIT(7)=1 */
		wcd_bits(0x300a, 0x80u, 0x80u); /* ANA_EAR: EAR PGA ON */
		wcd_bits(0x3128, 0x80u, 0x80u); /* AUX_AUXPA: AUX PA(7) ON */
		sleep_ms(6);
		say("paudio: RX_MACRO + WCD937x EAR/AUX ON (rx0=%#x rx2=%#x sec7=%#x/%#x vol=%d/%d dB)\n",
		    rd(rx_macro_base + 0x400u), rd(rx_macro_base + rx2_base),
		    rd(rx_macro_base + 0x434u), rd(rx_macro_base + rx2_sec7),
		    init_vol_db, init_vol_db - 3);
	}
}

static void test_audioreach_graphs(bool play_tone_and_spkr);
static void pa_darwin_run(void);

static void
probe_wcd937x(uint32_t mode)
{
	/* LPI pins: VA/TX SWR = 0(clk),1(d0),2(d1),14(d2) [4mA, func1]; RX SWR = 3(clk),4(d0),5(d1) [2mA, func1] */
	static const struct { unsigned pin; uint32_t pull, drv; } lpi_pins[] = {
		{ 0, 0, 1 }, { 1, 2, 1 }, { 2, 2, 1 }, { 14, 2, 1 },
		{ 3, 0, 0 }, { 4, 2, 0 }, { 5, 2, 0 },
	};
	uint32_t saved_lpi_cfg[7], saved_lpi_io[7], saved_slew;
	uint32_t saved_rst_cfg, saved_rst_io;

	rx_macro_base = ml_io_map(RX_MACRO_PHYS, 0x1000);
	rx_swr_base   = ml_io_map(RX_SWR_PHYS, 0x2000);
	tx_macro_base = ml_io_map(TX_MACRO_PHYS, 0x1000);
	va_macro_base = ml_io_map(VA_MACRO_PHYS, 0x1000);
	va_swr_base   = ml_io_map(VA_SWR_PHYS, 0x2000);
	lpi_base      = ml_io_map(LPI_PHYS, LPI_SIZE);
	lpi_slew_base = ml_io_map(LPI_SLEW_PHYS, 0x1000);
	rx_mux_base   = ml_io_map(RX_MUXSEL_PAGE, 0x1000);
	va_mux_base   = ml_io_map(VA_MUXSEL_PHYS, 0x1000);

	say("paudio: muxsel initial: va=%#x rx=%#x\n",
	    rd(va_mux_base), rd(rx_mux_base + RX_MUXSEL_OFF));
	say("paudio: lpi_slew initial: %#x\n", rd(lpi_slew_base));

	/* Release WCD937x reset (TLMM GPIO 102: 16mA output, low 50us -> high 5ms) */
	saved_rst_cfg = rd(GPIO_CFG(PIN_WCD_RST));
	saved_rst_io  = rd(GPIO_IO(PIN_WCD_RST));
	say("paudio: WCD reset gpio102 before: cfg=%#x io=%#x\n", saved_rst_cfg, saved_rst_io);
	wr(GPIO_IO(PIN_WCD_RST), 0u);
	wr(GPIO_CFG(PIN_WCD_RST), 0x3c0u); /* gpio, no pull, 16mA, output */
	spin_us(50);
	wr(GPIO_IO(PIN_WCD_RST), 2u);
	sleep_ms(5);

	/* Configure LPI TLMM pins */
	saved_slew = rd(lpi_slew_base);
	wr(lpi_slew_base, (saved_slew & ~0x3fffu) | 0x1555u);
	for (unsigned i = 0; i < 7; i++) {
		unsigned pin = lpi_pins[i].pin;
		saved_lpi_cfg[i] = rd(lpi_base + 0x1000UL * pin);
		saved_lpi_io[i]  = rd(lpi_base + 0x1000UL * pin + 4);
		uint32_t cfg = (saved_lpi_cfg[i] & ~0x3ffu) |
		    lpi_pins[i].pull | (1u << 2) | (lpi_pins[i].drv << 6);
		wr(lpi_base + 0x1000UL * pin, cfg);
		wr(lpi_base + 0x1000UL * pin + 4, 0u);
	}

	/* Enable VA macro fs-gen + VA SWR clock (0x033f0000) */
	reg_bits(va_macro_base, 0x000u, 0x01u, 0x01u);
	reg_bits(va_macro_base, 0x004u, 0x03u, 0x03u);
	reg_bits(va_macro_base, 0x004u, 0x03u, 0x01u);
	reg_bits(va_macro_base, 0x080u, 0x02u, 0x02u);
	reg_bits(va_macro_base, 0x008u, 0x02u, 0x02u);
	reg_bits(va_macro_base, 0x008u, 0x01u, 0x01u);
	reg_bits(va_macro_base, 0x008u, 0x02u, 0x00u);

	/* Enable TX macro MCLK/FS */
	wr(tx_macro_base + 0x090u, 1u);
	reg_bits(tx_macro_base, 0x000u, 0x01u, 0x01u);
	reg_bits(tx_macro_base, 0x004u, 0x01u, 0x01u);

	/* Enable RX macro MCLK/MCLK2/FS + RX SWR clock (0x03200000) */
	reg_bits(rx_macro_base, 0x100u, 0x03u, 0x03u);
	reg_bits(rx_macro_base, 0x104u, 0x02u, 0x00u);
	reg_bits(rx_macro_base, 0x104u, 0x01u, 0x01u);
	reg_bits(rx_macro_base, 0x108u, 0x02u, 0x02u);
	reg_bits(rx_macro_base, 0x108u, 0x01u, 0x01u);
	reg_bits(rx_macro_base, 0x108u, 0x02u, 0x00u);
	say("paudio: rx_macro regs: top0=%#x top8=%#x mclk=%#x fs=%#x swr=%#x\n",
	    rd(rx_macro_base + 0x000u), rd(rx_macro_base + 0x008u),
	    rd(rx_macro_base + 0x100u), rd(rx_macro_base + 0x104u), rd(rx_macro_base + 0x108u));

	sleep_ms(2);

	/* Start VA SoundWire master (wcd937x-tx-slave) */
	wcd_tx_dev = 0;
	if (swr_master_start("VA(TX)", va_swr_base)) {
		wcd_tx_dev = swr_wait_slave("VA(TX)", va_swr_base, WCD937X_TX_ID);
		if (wcd_tx_dev != 0) {
			uint8_t id0 = 0, id1 = 0, id2 = 0, id3 = 0, s0 = 0, s1 = 0;
			swr_read_reg(va_swr_base, wcd_tx_dev, 0x3401, &id0);
			swr_read_reg(va_swr_base, wcd_tx_dev, 0x3402, &id1);
			swr_read_reg(va_swr_base, wcd_tx_dev, 0x3403, &id2);
			swr_read_reg(va_swr_base, wcd_tx_dev, 0x3404, &id3);
			swr_read_reg(va_swr_base, wcd_tx_dev, 0x0050, &s0);
			swr_read_reg(va_swr_base, wcd_tx_dev, 0x0051, &s1);
			say("paudio: WCD937x TX CHIP_ID=%02x %02x %02x %02x SCP_ID=%02x %02x\n",
			    id0, id1, id2, id3, s0, s1);
		}
	}

	/* Start RX SoundWire master (wcd937x-rx-slave) */
	wcd_rx_dev = 0;
	if (!swr_master_start("RX", rx_swr_base)) {
		wr(rx_mux_base + RX_MUXSEL_OFF, 1u);
		reg_bits(rx_macro_base, 0x108u, 0x02u, 0x02u);
		reg_bits(rx_macro_base, 0x108u, 0x01u, 0x01u);
		reg_bits(rx_macro_base, 0x108u, 0x02u, 0x00u);
		sleep_ms(2);
		say("paudio: retry RX SWR with rx_muxsel=1 (reads %#x)\n", rd(rx_mux_base + RX_MUXSEL_OFF));
		swr_master_start("RX-mux1", rx_swr_base);
	}
	if (rd(rx_swr_base + SWRM_COMP_STATUS) & 1u) {
		wcd_rx_dev = swr_wait_slave("RX", rx_swr_base, WCD937X_RX_ID);
		if (wcd_rx_dev != 0) {
			uint8_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
			swr_read_reg(rx_swr_base, wcd_rx_dev, 0x0050, &s0);
			swr_read_reg(rx_swr_base, wcd_rx_dev, 0x0051, &s1);
			swr_read_reg(rx_swr_base, wcd_rx_dev, 0x0052, &s2);
			swr_read_reg(rx_swr_base, wcd_rx_dev, 0x0053, &s3);
			say("paudio: WCD937x RX SCP_ID=%02x %02x %02x %02x\n", s0, s1, s2, s3);
		}
	}

	/* Run AudioReach graph + WCD937x + FS1815 (mode 2/3: test; mode 4: full Darwin audio) */
	if (mode == 4) {
		uint32_t below_db = 6;
		PE_parse_boot_argn("parrot_audio_below", &below_db, sizeof(below_db));
		if (below_db > 48) {
			below_db = 48;
		}
		wcd937x_setup_hw(true, true, -(int)below_db);
		pa_darwin_run();
	} else if (mode >= 2) {
		uint32_t below_db = 12;
		PE_parse_boot_argn("parrot_audio_below", &below_db, sizeof(below_db));
		wcd937x_setup_hw(true, true, -(int)below_db);
		test_audioreach_graphs(mode >= 3);
	}

	/* Clean teardown */
	swr_master_stop(rx_swr_base);
	swr_master_stop(va_swr_base);
	reg_bits(rx_macro_base, 0x108u, 0x01u, 0x00u);
	reg_bits(rx_macro_base, 0x104u, 0x01u, 0x00u);
	reg_bits(rx_macro_base, 0x100u, 0x03u, 0x00u);
	reg_bits(va_macro_base, 0x008u, 0x01u, 0x00u);
	reg_bits(va_macro_base, 0x080u, 0x02u, 0x00u);
	reg_bits(va_macro_base, 0x004u, 0x03u, 0x00u);
	reg_bits(va_macro_base, 0x000u, 0x01u, 0x00u);

	for (unsigned i = 0; i < 7; i++) {
		unsigned pin = lpi_pins[i].pin;
		wr(lpi_base + 0x1000UL * pin, saved_lpi_cfg[i]);
		wr(lpi_base + 0x1000UL * pin + 4, saved_lpi_io[i]);
	}
	wr(lpi_slew_base, saved_slew);
	wr(GPIO_IO(PIN_WCD_RST), 0u);
	wr(GPIO_CFG(PIN_WCD_RST), saved_rst_cfg);
	wr(GPIO_IO(PIN_WCD_RST), saved_rst_io & 2u);
}

/* ---- SMMU (0x15000000, SID 0x1801) + AudioReach GPR Graphs ----------------------------------------- */

#define APPS_SMMU_PHYS          0x15000000UL
#define SMMU_SCR0               0x000u
#define SMMU_IDR0               0x020u
#define SMMU_IDR1               0x024u
#define SMMU_SGFSR              0x048u
#define SMMU_SGFSYNR1           0x054u
#define SMMU_SMR(n)             (0x800u + 4u * (n))
#define SMMU_S2CR(n)            (0xc00u + 4u * (n))
#define SMMU_CBAR(n)            (4u * (n))
#define SMMU_CBAR_STAGE_1       ((1u << 16) | (3u << 8) | (0xfu << 12))
#define SMMU_CBA2R(n)           (0x800u + 4u * (n))
#define SMMU_CB_SCTLR           0x00u
#define SMMU_CB_TCR2            0x10u
#define SMMU_CB_TTBR0           0x20u
#define SMMU_CB_TTBR1           0x28u
#define SMMU_CB_TCR             0x30u
#define SMMU_CB_MAIR0           0x38u
#define SMMU_CB_MAIR1           0x3cu
#define SMMU_CB_FSR             0x58u
#define SMMU_CB_FAR             0x60u
#define SMMU_CB_FSYNR0          0x68u
#define SMMU_CB_TLBIASID        0x610u
#define SMMU_CB_TLBSYNC         0x7f0u
#define SMMU_CB_TLBSTATUS       0x7f4u
#define SMMU_SCTLR_ON           ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 8) | (1u << 12))
#define SMMU_TCR_39_BITS        (25u | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23))
#define SMMU_TCR2_VAL           (1u | (1u << 4) | (7u << 15))
#define SMMU_MAIR0_VAL          0xf404ff44u
#define SMMU_FSR_ALL            0xc00001feu
#define AUDIO_PTE               (3ull | (1ull << 6) | (2ull << 8) | (1ull << 10) | (1ull << 11))

#define AUDIO_STREAM            0x1801u
#define AUDIO_IOVA_BASE         0x10000000ull
#define AUDIO_DSP_SID_BITS      (1ull << 32)

#define PB_PERIODS              8u
#define PB_PERIOD_FRAMES        480u
#define PB_PERIOD_BYTES         (PB_PERIOD_FRAMES * 4u)
#define PB_BUF_BYTES            (PB_PERIODS * 4096u)
#define PB_RING_PRIME           1920u   /* 40 ms jitter buffer before draining */
#define CAP_PERIODS             8u
#define CAP_PERIOD_FRAMES       960u
#define CAP_PERIOD_BYTES        (CAP_PERIOD_FRAMES * 2u)
#define CAP_BUF_BYTES           (CAP_PERIODS * 4096u)
#define CMD_OOB_OFFSET          (PB_BUF_BYTES + CAP_BUF_BYTES)
#define CMD_OOB_BYTES           4096u
#define TOTAL_DMA_BYTES         (PB_BUF_BYTES + CAP_BUF_BYTES + CMD_OOB_BYTES)

static struct {
	bool on, passed;
	vm_offset_t base, second, ctx;
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables, dma_buf;
	uint64_t dsp_addr;
} pa_smmu;

static bool
pa_smmu_setup(void)
{
	bool used[256] = { false };

	if (pa_smmu.base == 0) {
		pa_smmu.base = ml_io_map(APPS_SMMU_PHYS, 0x2000);
	}
	uint32_t scr0 = rd(pa_smmu.base + SMMU_SCR0);
	uint32_t idr0 = rd(pa_smmu.base + SMMU_IDR0);
	uint32_t idr1 = rd(pa_smmu.base + SMMU_IDR1);
	uint32_t matches = idr0 & 0xffu, contexts = idr1 & 0xffu;

	if (scr0 & (1u << 3)) {
		say("paudio: SMMU 16-bit streams unsupported\n");
		return false;
	}
	pa_smmu.page = (idr1 >> 31) ? 0x10000u : 0x1000u;
	pa_smmu.pages = 2u << ((idr1 >> 28) & 7u);
	pa_smmu.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00u; n++) {
		uint32_t smr = rd(pa_smmu.base + SMMU_SMR(n));
		uint32_t s2cr = rd(pa_smmu.base + SMMU_S2CR(n));
		if (smr >> 31) {
			used[s2cr & 0xffu] = true;
			uint32_t stream = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;
			if (((stream ^ AUDIO_STREAM) & ~mask) == 0) {
				if (((s2cr >> 16) & 3u) == 1u) {
					pa_smmu.passed = true;
					break;
				}
				say("paudio: SMMU stream 0x1801 already owned (smr=%#x s2cr=%#x)\n", smr, s2cr);
				return false;
			}
		} else if (pa_smmu.match == ~0u) {
			pa_smmu.match = n;
			pa_smmu.old_smr = smr;
			pa_smmu.old_s2cr = s2cr;
		}
	}

	if (pa_smmu.dma_buf == 0) {
		if (kmem_alloc(kernel_map, &pa_smmu.dma_buf, TOTAL_DMA_BYTES,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			say("paudio: no DMA memory\n");
			return false;
		}
	}

	if (pa_smmu.passed) {
		pa_smmu.dsp_addr = kvtophys(pa_smmu.dma_buf);
		say("paudio: SMMU bypass active, phys=%#llx\n", pa_smmu.dsp_addr);
		return true;
	}

	for (pa_smmu.context = 0; pa_smmu.context < contexts && used[pa_smmu.context]; pa_smmu.context++) {
	}
	if (pa_smmu.match == ~0u || pa_smmu.context >= contexts) {
		say("paudio: no free SMMU match/context\n");
		return false;
	}
	pa_smmu.second = ml_io_map(APPS_SMMU_PHYS + pa_smmu.page, 0x1000);
	pa_smmu.ctx = ml_io_map(APPS_SMMU_PHYS + (pa_smmu.pages + pa_smmu.context) * pa_smmu.page, 0x1000);
	pa_smmu.old_sctlr = rd(pa_smmu.ctx + SMMU_CB_SCTLR);
	if (pa_smmu.old_sctlr & 1u) {
		say("paudio: SMMU ctx %u already enabled\n", pa_smmu.context);
		return false;
	}

	if (pa_smmu.tables == 0) {
		if (kmem_alloc(kernel_map, &pa_smmu.tables, 3 * 4096,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
	}
	bzero((void *)pa_smmu.tables, 3 * 4096);
	uint64_t *l1 = (uint64_t *)pa_smmu.tables;
	uint64_t *l2 = (uint64_t *)(pa_smmu.tables + 4096);
	uint64_t *l3 = (uint64_t *)(pa_smmu.tables + 8192);
	l1[(AUDIO_IOVA_BASE >> 30) & 0x1ffu] = kvtophys((vm_offset_t)l2) | 3ull;
	l2[(AUDIO_IOVA_BASE >> 21) & 0x1ffu] = kvtophys((vm_offset_t)l3) | 3ull;
	for (unsigned pg = 0; pg < TOTAL_DMA_BYTES / 4096u; pg++) {
		l3[((AUDIO_IOVA_BASE >> 12) & 0x1ffu) + pg] = kvtophys(pa_smmu.dma_buf + 4096u * pg) | AUDIO_PTE;
	}
	for (vm_offset_t a = pa_smmu.tables; a < pa_smmu.tables + 3 * 4096; a += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (a));
	}
	__asm__ volatile ("dsb sy" ::: "memory");

	uint64_t ttbr0 = kvtophys(pa_smmu.tables) | ((uint64_t)pa_smmu.context << 48);
	wr(pa_smmu.ctx + SMMU_CB_SCTLR, 0u);
	wr(pa_smmu.second + SMMU_CBA2R(pa_smmu.context), 1u);
	wr(pa_smmu.second + SMMU_CBAR(pa_smmu.context), SMMU_CBAR_STAGE_1);
	wr(pa_smmu.ctx + SMMU_CB_TCR2, SMMU_TCR2_VAL);
	wr(pa_smmu.ctx + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(pa_smmu.ctx + SMMU_CB_TTBR0, (uint32_t)ttbr0);
	wr(pa_smmu.ctx + SMMU_CB_TTBR0 + 4, (uint32_t)(ttbr0 >> 32));
	wr(pa_smmu.ctx + SMMU_CB_TTBR1, 0u);
	wr(pa_smmu.ctx + SMMU_CB_TTBR1 + 4, 0u);
	wr(pa_smmu.ctx + SMMU_CB_MAIR0, SMMU_MAIR0_VAL);
	wr(pa_smmu.ctx + SMMU_CB_MAIR1, 0u);
	wr(pa_smmu.ctx + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(pa_smmu.ctx + SMMU_CB_TLBIASID, pa_smmu.context);
	wr(pa_smmu.ctx + SMMU_CB_TLBSYNC, 1u);
	for (unsigned ms = 0; ms < 100 && (rd(pa_smmu.ctx + SMMU_CB_TLBSTATUS) & 1u); ms++) {
		sleep_ms(1);
	}
	wr(pa_smmu.ctx + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	wr(pa_smmu.base + SMMU_S2CR(pa_smmu.match), pa_smmu.context);
	wr(pa_smmu.base + SMMU_SMR(pa_smmu.match), (1u << 31) | AUDIO_STREAM);
	pa_smmu.on = true;
	pa_smmu.dsp_addr = AUDIO_DSP_SID_BITS | AUDIO_IOVA_BASE;
	say("paudio: SMMU match=%u ctx=%u mapped %u KB at dsp=%#llx\n",
	    pa_smmu.match, pa_smmu.context, TOTAL_DMA_BYTES / 1024u, pa_smmu.dsp_addr);
	return true;
}

static void
pa_smmu_teardown(void)
{
	if (!pa_smmu.on) {
		return;
	}
	uint32_t fsr = rd(pa_smmu.ctx + SMMU_CB_FSR);
	uint32_t sgfsr = rd(pa_smmu.base + SMMU_SGFSR);
	if ((fsr & SMMU_FSR_ALL) || sgfsr) {
		uint64_t far = ((uint64_t)rd(pa_smmu.ctx + SMMU_CB_FAR + 4) << 32) | rd(pa_smmu.ctx + SMMU_CB_FAR);
		say("paudio: SMMU FAULT fsr=%#x far=%#llx fsynr0=%#x sgfsr=%#x sid=%#x\n",
		    fsr, far, rd(pa_smmu.ctx + SMMU_CB_FSYNR0), sgfsr, rd(pa_smmu.base + SMMU_SGFSYNR1));
	} else {
		say("paudio: SMMU clean (0 faults)\n");
	}
	wr(pa_smmu.base + SMMU_SMR(pa_smmu.match), pa_smmu.old_smr);
	wr(pa_smmu.base + SMMU_S2CR(pa_smmu.match), pa_smmu.old_s2cr);
	wr(pa_smmu.ctx + SMMU_CB_SCTLR, pa_smmu.old_sctlr);
	pa_smmu.on = false;
}

/* ---- AudioReach APM GPR Commands ------------------------------------------------------------------- */

#define GPR_PORT_APM                    0x00000001u
#define GPR_PORT_PB_DATA                0x00002020u
#define GPR_PORT_CAP_DATA               0x00002021u

#define APM_CMD_GRAPH_OPEN              0x01001000u
#define APM_CMD_GRAPH_PREPARE           0x01001001u
#define APM_CMD_GRAPH_START             0x01001002u
#define APM_CMD_GRAPH_STOP              0x01001003u
#define APM_CMD_GRAPH_CLOSE             0x01001004u
#define APM_CMD_SET_CFG                 0x01001006u
#define APM_CMD_SHARED_MEM_MAP_REGIONS  0x0100100Cu
#define APM_CMD_SHARED_MEM_UNMAP_REGIONS 0x0100100Du
#define APM_CMD_RSP_SHARED_MEM_MAP_REGIONS 0x02001001u

#define DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2          0x0400100Au
#define DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2          0x0400100Bu
#define DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE    0x05001000u
#define DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 0x05001004u
#define DATA_CMD_RSP_RD_SH_MEM_EP_DATA_BUFFER_V2      0x05001005u

#define MODULE_ID_WR_SHARED_MEM_EP      0x07001000u
#define MODULE_ID_RD_SHARED_MEM_EP      0x07001001u
#define MODULE_ID_PCM_CNV               0x07001003u
#define MODULE_ID_PCM_ENC               0x07001004u
#define MODULE_ID_PCM_DEC               0x07001005u
#define MODULE_ID_CODEC_DMA_SINK        0x07001023u
#define MODULE_ID_CODEC_DMA_SOURCE      0x07001024u

#define APM_PARAM_ID_CONTAINER_CONFIG   0x08001000u
#define APM_PARAM_ID_SUB_GRAPH_CONFIG   0x08001001u
#define APM_PARAM_ID_MODULE_LIST        0x08001002u
#define APM_PARAM_ID_MODULE_PROP        0x08001003u
#define APM_PARAM_ID_MODULE_CONN        0x08001004u
#define APM_PARAM_ID_SUB_GRAPH_LIST     0x08001005u
#define PARAM_ID_RD_SH_MEM_CFG          0x08001007u
#define PARAM_ID_PCM_OUTPUT_FORMAT_CFG  0x08001008u
#define PARAM_ID_MEDIA_FORMAT           0x0800100Cu
#define APM_SUB_GRAPH_PROP_PERF_MODE    0x0800100Eu
#define APM_SUB_GRAPH_PROP_DIRECTION    0x0800100Fu
#define APM_SUB_GRAPH_PROP_SCENARIO_ID  0x08001010u
#define APM_CONT_PROP_CAP_LIST          0x08001011u
#define APM_CONT_PROP_GRAPH_POS         0x08001012u
#define APM_CONT_PROP_STACK_SIZE        0x08001013u
#define APM_CONT_PROP_PROC_DOMAIN       0x08001014u
#define APM_MOD_PROP_PORT_INFO          0x08001015u
#define PARAM_ID_HW_EP_MF_CFG           0x08001017u
#define PARAM_ID_HW_EP_FRAME_SIZE_FACTOR 0x08001018u
#define PARAM_ID_CODEC_DMA_INTF_CFG     0x08001063u
#define PARAM_ID_HW_EP_POWER_MODE_CFG   0x08001176u

#define AR_ALIGN8(x)                    (((x) + 7u) & ~7u)

/* Fixed IDs matching Qualcomm AudioReach topology ranges (SG/CONT: 0x4001+, MOD: 0x6001+) */
#define PB_SG_STR       0x00004001u
#define PB_SG_DEV       0x00004007u
#define PB_CONT_STR     0x00004001u
#define PB_CONT_DEV     0x00004007u
#define PB_IID_SHM      0x00006001u
#define PB_IID_DEC      0x00006002u
#define PB_IID_CNV      0x00006003u
#define PB_IID_DMA      0x00006072u

#define CAP_SG_STR      0x00004003u
#define CAP_SG_DEV      0x00004009u
#define CAP_CONT_STR    0x00004003u
#define CAP_CONT_DEV    0x00004009u
#define CAP_IID_DMA     0x00006090u
#define CAP_IID_CNV     0x00006022u
#define CAP_IID_ENC     0x00006023u
#define CAP_IID_SHM     0x00006024u

struct ar_sg_entry {
	uint32_t sg_id, num_prop;
	uint32_t p1_id, p1_sz, perf_mode;
	uint32_t p2_id, p2_sz, direction;
	uint32_t p3_id, p3_sz, scenario_id;
} __attribute__((packed));

struct ar_cont_entry {
	uint32_t cont_id, num_prop;
	uint32_t c_id, c_sz, ncap, cap_id;
	uint32_t p_id, p_sz, graph_pos;
	uint32_t s_id, s_sz, stack_size;
	uint32_t d_id, d_sz, proc_domain;
} __attribute__((packed));

struct ar_mod_prop_entry {
	uint32_t iid, num_prop;
	uint32_t p_id, p_sz, max_in, max_out;
} __attribute__((packed));

struct ar_conn_entry {
	uint32_t src_iid, src_port, dst_iid, dst_port;
} __attribute__((packed));

static uint32_t ar_mem_handle;
static volatile bool ar_streaming, ar_darwin_mode;
static volatile uint32_t pb_done_cnt, cap_done_cnt, pb_err_cnt, cap_err_cnt;
static volatile uint32_t cap_peak, cap_nonzero;
static volatile uint32_t pb_returned_mask;
static volatile uint32_t pa_pb_underruns;
static volatile int32_t cap_sec_min = 32767, cap_sec_max = -32768;
static volatile uint64_t cap_sec_sq;
static volatile uint32_t cap_sec_cnt;
static volatile int16_t cap_sec_s[4];
static volatile bool cap_sec_ready;
static int32_t cap_rep_min, cap_rep_max;
static uint64_t cap_rep_sq;
static int16_t cap_rep_s[4];

#define PA_RING_FRAMES          16384u  /* 341 ms @ 48 kHz; power of two */
#define MIC_RING_KEEP           1920u   /* 40 ms behind DSP */
#define MIC_RING_MOST           4800u   /* 100 ms max lag before skip */

static int16_t *pa_mic_ring;
static volatile uint64_t pa_mic_written;
static uint64_t pa_mic_read;
static bool pa_mic_primed;
static volatile uint32_t pa_mic_open;

static void
pa_mic_ring_put(const int16_t *samples, uint32_t count)
{
	if (pa_mic_ring == NULL || count == 0) {
		return;
	}
	uint64_t at = pa_mic_written;
	for (uint32_t i = 0; i < count; i++) {
		pa_mic_ring[(at + i) & (PA_RING_FRAMES - 1u)] = samples[i];
	}
	__atomic_store_n(&pa_mic_written, at + count, __ATOMIC_RELEASE);
}

static void ar_queue_pb_buf(unsigned idx);
static void ar_queue_cap_buf(unsigned idx);

static bool
ar_gpr_event_cb(uint32_t src_port, uint32_t dest_port, uint32_t opcode, uint32_t token,
    const uint32_t *payload, uint32_t length)
{
	(void)src_port;
	(void)length;
	if ((opcode == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 ||
	    opcode == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE) &&
	    dest_port == GPR_PORT_PB_DATA) {
		uint32_t status = payload[3];
		if (status == 0) {
			pb_done_cnt++;
		} else {
			pb_err_cnt++;
		}
		if (ar_streaming && token >= 1u && token <= PB_PERIODS) {
			if (ar_darwin_mode) {
				OSBitOrAtomic(1u << (token - 1u), &pb_returned_mask);
			} else if (status == 0) {
				ar_queue_pb_buf(token - 1u);
			}
		}
		if (pb_done_cnt <= 3 || status != 0) {
			say("paudio: PB_DONE #%u op=%#x tok=%#x st=%#x\n", pb_done_cnt, opcode, token, status);
		}
		return true;
	}
	if (opcode == DATA_CMD_RSP_RD_SH_MEM_EP_DATA_BUFFER_V2 && dest_port == GPR_PORT_CAP_DATA) {
		uint32_t status = payload[0], data_sz = payload[4];
		if (status == 0) {
			cap_done_cnt++;
			if (data_sz > 0 && data_sz <= 4096u && token >= 1u && token <= CAP_PERIODS) {
				vm_offset_t va = pa_smmu.dma_buf + PB_BUF_BYTES + (vm_offset_t)(token - 1u) * 4096u;
				for (vm_offset_t a = va; a < va + ((data_sz + 63u) & ~63u); a += 64) {
					__asm__ volatile ("dc ivac, %0" : : "r" (a));
				}
				__asm__ volatile ("dsb sy" ::: "memory");
				const int16_t *s = (const int16_t *)va;
				uint32_t nsamp = data_sz / 2u;
				if (ar_darwin_mode) {
					if (pa_mic_open != 0) {
						pa_mic_ring_put(s, nsamp);
						if (cap_sec_cnt == 0 && nsamp >= 4) {
							cap_sec_s[0] = s[0];
							cap_sec_s[1] = s[1];
							cap_sec_s[2] = s[2];
							cap_sec_s[3] = s[3];
						}
						for (uint32_t i = 0; i < nsamp; i++) {
							int32_t v = s[i];
							if (v < cap_sec_min) {
								cap_sec_min = v;
							}
							if (v > cap_sec_max) {
								cap_sec_max = v;
							}
							cap_sec_sq += (uint64_t)((int64_t)v * (int64_t)v);
						}
						cap_sec_cnt += nsamp;
						if (cap_sec_cnt >= 48000u) {
							cap_rep_min = cap_sec_min;
							cap_rep_max = cap_sec_max;
							cap_rep_sq = cap_sec_sq / cap_sec_cnt;
							for (int k = 0; k < 4; k++) {
								cap_rep_s[k] = cap_sec_s[k];
							}
							cap_sec_min = 32767;
							cap_sec_max = -32768;
							cap_sec_sq = 0;
							cap_sec_cnt = 0;
							cap_sec_ready = true;
						}
					}
				} else if (pa_mic_open != 0) {
					pa_mic_ring_put(s, nsamp);
				}
				for (uint32_t i = 0; i < nsamp; i++) {
					uint32_t mag = (uint32_t)(s[i] < 0 ? -s[i] : s[i]);
					if (mag > cap_peak) {
						cap_peak = mag;
					}
					if (mag > 4u) {
						cap_nonzero++;
					}
				}
			}
			if (ar_streaming && token >= 1u && token <= CAP_PERIODS) {
				ar_queue_cap_buf(token - 1u);
			}
		} else {
			cap_err_cnt++;
		}
		if (cap_done_cnt <= 3 || status != 0) {
			say("paudio: CAP_DONE #%u tok=%#x st=%#x bytes=%u\n", cap_done_cnt, token, status, data_sz);
		}
		return true;
	}
	return false;
}

static bool
ar_apm_cmd(const char *what, uint32_t src_port, uint32_t dest_port, uint32_t opcode,
    const void *body, uint32_t body_len, uint32_t expect_rsp, struct hakios_gpr_answer *out)
{
	struct hakios_gpr_answer ans;
	bool oob = (body_len > 80u && ar_mem_handle != 0 && pa_smmu.dma_buf != 0 && body_len <= CMD_OOB_BYTES);
	uint32_t inline_len = oob ? 0u : body_len;
	uint8_t *pkt = kalloc_data(sizeof(struct apm_cmd_header) + inline_len, Z_WAITOK);
	if (pkt == NULL) {
		return false;
	}
	struct apm_cmd_header *hdr = (struct apm_cmd_header *)pkt;
	bzero(hdr, sizeof(*hdr));
	hdr->payload_size = body_len;
	if (oob) {
		vm_offset_t dst = pa_smmu.dma_buf + CMD_OOB_OFFSET;
		uint64_t dsp_va = pa_smmu.dsp_addr + CMD_OOB_OFFSET;
		memcpy((void *)dst, body, body_len);
		for (vm_offset_t a = dst; a < dst + ((body_len + 63u) & ~63u); a += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (a));
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		hdr->payload_address_lsw = (uint32_t)dsp_va;
		hdr->payload_address_msw = (uint32_t)(dsp_va >> 32);
		hdr->mem_map_handle = ar_mem_handle;
	} else if (body_len > 0 && body != NULL) {
		memcpy(pkt + sizeof(*hdr), body, body_len);
	}
	bool got = hakios_adsp_gpr(src_port, dest_port, 0, opcode, pkt,
	    (uint32_t)(sizeof(*hdr) + inline_len), 1500, &ans);
	kfree_data(pkt, sizeof(struct apm_cmd_header) + inline_len);
	if (!got) {
		say("paudio: %s: NO ANSWER (len=%u oob=%d)\n", what, body_len, oob);
		return false;
	}
	if (out != NULL) {
		*out = ans;
	}
	bool ok = (expect_rsp != 0 && ans.opcode == expect_rsp) ||
	    (ans.opcode == GPR_IBASIC_RSP_RESULT && ans.words[1] == 0);
	say("paudio: %s: %s (op=%#x w0=%#x w1=%#x)\n", what, ok ? "ok" : "ERR",
	    ans.opcode, ans.words[0], ans.words[1]);
	return ok;
}

static bool
ar_map_dma(void)
{
	struct {
		uint16_t mem_pool_id;
		uint16_t num_regions;
		uint32_t property_flag;
		uint32_t addr_lsw;
		uint32_t addr_msw;
		uint32_t size;
	} __attribute__((packed)) req = {
		3u, 1u, 0u,
		(uint32_t)pa_smmu.dsp_addr, (uint32_t)(pa_smmu.dsp_addr >> 32),
		TOTAL_DMA_BYTES
	};
	struct hakios_gpr_answer ans;

	if (!hakios_adsp_gpr(GPR_PORT_PAUDIO, GPR_PORT_APM, 0, APM_CMD_SHARED_MEM_MAP_REGIONS,
	    &req, sizeof(req), 1500, &ans)) {
		say("paudio: SHM_MAP: NO ANSWER\n");
		return false;
	}
	if (ans.opcode == APM_CMD_RSP_SHARED_MEM_MAP_REGIONS && ans.words[0] != 0) {
		ar_mem_handle = ans.words[0];
		say("paudio: SHM_MAP: ok handle=%#x\n", ar_mem_handle);
		return true;
	}
	say("paudio: SHM_MAP: ERR op=%#x w0=%#x w1=%#x\n", ans.opcode, ans.words[0], ans.words[1]);
	return false;
}

static void
ar_unmap_dma(void)
{
	if (ar_mem_handle == 0) {
		return;
	}
	struct hakios_gpr_answer ans;
	uint32_t h = ar_mem_handle;
	(void)hakios_adsp_gpr(GPR_PORT_PAUDIO, GPR_PORT_APM, 0, APM_CMD_SHARED_MEM_UNMAP_REGIONS,
	    &h, sizeof(h), 1000, &ans);
	ar_mem_handle = 0;
}

static bool
ar_graph_mgmt(const char *what, uint32_t opcode, uint32_t sg1, uint32_t sg2)
{
	struct {
		struct apm_module_param_data pd;
		uint32_t num_sg;
		uint32_t sgs[2];
		uint32_t pad;
	} __attribute__((packed)) b;

	bzero(&b, sizeof(b));
	b.pd.module_instance_id = GPR_PORT_APM;
	b.pd.param_id = APM_PARAM_ID_SUB_GRAPH_LIST;
	b.pd.param_size = 16u; /* 4 + 8 + 4 pad -> 8-byte aligned */
	b.num_sg = 2u;
	b.sgs[0] = sg1;
	b.sgs[1] = sg2;
	return ar_apm_cmd(what, GPR_PORT_PAUDIO, GPR_PORT_APM, opcode, &b, sizeof(b), 0, NULL);
}

static bool
ar_open_graph(const char *what, bool is_capture, bool use_gc)
{
	uint8_t buf[1024];
	uint32_t pos = 0;
	bzero(buf, sizeof(buf));

	uint32_t sg_str = is_capture ? CAP_SG_STR : PB_SG_STR;
	uint32_t sg_dev = is_capture ? CAP_SG_DEV : PB_SG_DEV;
	uint32_t c_str  = is_capture ? CAP_CONT_STR : PB_CONT_STR;
	uint32_t c_dev  = is_capture ? CAP_CONT_DEV : PB_CONT_DEV;
	uint32_t sid    = is_capture ? 2u : 1u; /* 1=PLAYBACK, 2=RECORD */
	uint32_t cap_str = use_gc ? 0x0B001001u : 2u; /* GC or CD */
	uint32_t cap_dev = use_gc ? 0x0B001001u : 3u; /* GC or EP */
	uint32_t gpos    = is_capture ? 4u : 1u;      /* GLOBAL_DEV for cap, STREAM for pb */

	/* 1. SUB_GRAPH_CONFIG (2 subgraphs) */
	uint32_t sg_sz = AR_ALIGN8(16u + 4u + 2u * (uint32_t)sizeof(struct ar_sg_entry));
	struct apm_module_param_data *pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = GPR_PORT_APM;
	pd->param_id = APM_PARAM_ID_SUB_GRAPH_CONFIG;
	pd->param_size = sg_sz - 16u;
	*(uint32_t *)(buf + pos + 16) = 2u;
	struct ar_sg_entry *sgs = (struct ar_sg_entry *)(buf + pos + 20);
	/* sg0 = stream, sg1 = device (matching audioreach-topology directions!) */
	sgs[0].sg_id = sg_str; sgs[0].num_prop = 3u;
	sgs[0].p1_id = APM_SUB_GRAPH_PROP_PERF_MODE; sgs[0].p1_sz = 4u; sgs[0].perf_mode = 2u;
	sgs[0].p2_id = APM_SUB_GRAPH_PROP_DIRECTION; sgs[0].p2_sz = 4u; sgs[0].direction = is_capture ? 1u : 2u;
	sgs[0].p3_id = APM_SUB_GRAPH_PROP_SCENARIO_ID; sgs[0].p3_sz = 4u; sgs[0].scenario_id = sid;

	sgs[1].sg_id = sg_dev; sgs[1].num_prop = 3u;
	sgs[1].p1_id = APM_SUB_GRAPH_PROP_PERF_MODE; sgs[1].p1_sz = 4u; sgs[1].perf_mode = 2u;
	sgs[1].p2_id = APM_SUB_GRAPH_PROP_DIRECTION; sgs[1].p2_sz = 4u; sgs[1].direction = 1u; /* TX in audioreach-tplg */
	sgs[1].p3_id = APM_SUB_GRAPH_PROP_SCENARIO_ID; sgs[1].p3_sz = 4u; sgs[1].scenario_id = sid;
	pos += sg_sz;

	/* 2. CONTAINER_CONFIG (2 containers) */
	uint32_t ct_sz = AR_ALIGN8(16u + 4u + 2u * (uint32_t)sizeof(struct ar_cont_entry));
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = GPR_PORT_APM;
	pd->param_id = APM_PARAM_ID_CONTAINER_CONFIG;
	pd->param_size = ct_sz - 16u;
	*(uint32_t *)(buf + pos + 16) = 2u;
	struct ar_cont_entry *cts = (struct ar_cont_entry *)(buf + pos + 20);
	cts[0].cont_id = c_str; cts[0].num_prop = 4u;
	cts[0].c_id = APM_CONT_PROP_CAP_LIST; cts[0].c_sz = 8u; cts[0].ncap = 1u; cts[0].cap_id = cap_str;
	cts[0].p_id = APM_CONT_PROP_GRAPH_POS; cts[0].p_sz = 4u; cts[0].graph_pos = gpos;
	cts[0].s_id = APM_CONT_PROP_STACK_SIZE; cts[0].s_sz = 4u; cts[0].stack_size = 8192u;
	cts[0].d_id = APM_CONT_PROP_PROC_DOMAIN; cts[0].d_sz = 4u; cts[0].proc_domain = 2u;

	cts[1].cont_id = c_dev; cts[1].num_prop = 4u;
	cts[1].c_id = APM_CONT_PROP_CAP_LIST; cts[1].c_sz = 8u; cts[1].ncap = 1u; cts[1].cap_id = cap_dev;
	cts[1].p_id = APM_CONT_PROP_GRAPH_POS; cts[1].p_sz = 4u; cts[1].graph_pos = gpos;
	cts[1].s_id = APM_CONT_PROP_STACK_SIZE; cts[1].s_sz = 4u; cts[1].stack_size = 8192u;
	cts[1].d_id = APM_CONT_PROP_PROC_DOMAIN; cts[1].d_sz = 4u; cts[1].proc_domain = 2u;
	pos += ct_sz;

	/* 3. MODULE_LIST (list0: 3 modules in stream container = 12+24=36B; list1: 1 module in dev container = 12+8=20B) */
	uint32_t ml_sz = AR_ALIGN8(16u + 4u + 36u + 20u);
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = GPR_PORT_APM;
	pd->param_id = APM_PARAM_ID_MODULE_LIST;
	pd->param_size = ml_sz - 16u;
	uint32_t *ml = (uint32_t *)(buf + pos + 16);
	ml[0] = 2u;
	ml[1] = sg_str; ml[2] = c_str; ml[3] = 3u;
	if (!is_capture) {
		ml[4] = MODULE_ID_WR_SHARED_MEM_EP; ml[5] = PB_IID_SHM;
		ml[6] = MODULE_ID_PCM_DEC;          ml[7] = PB_IID_DEC;
		ml[8] = MODULE_ID_PCM_CNV;          ml[9] = PB_IID_CNV;
	} else {
		ml[4] = MODULE_ID_PCM_CNV;          ml[5] = CAP_IID_CNV;
		ml[6] = MODULE_ID_PCM_ENC;          ml[7] = CAP_IID_ENC;
		ml[8] = MODULE_ID_RD_SHARED_MEM_EP; ml[9] = CAP_IID_SHM;
	}
	ml[10] = sg_dev; ml[11] = c_dev; ml[12] = 1u;
	if (!is_capture) {
		ml[13] = MODULE_ID_CODEC_DMA_SINK;   ml[14] = PB_IID_DMA;
	} else {
		ml[13] = MODULE_ID_CODEC_DMA_SOURCE; ml[14] = CAP_IID_DMA;
	}
	pos += ml_sz;

	/* 4. MODULE_PROP (4 modules) */
	uint32_t mp_sz = AR_ALIGN8(16u + 4u + 4u * (uint32_t)sizeof(struct ar_mod_prop_entry));
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = GPR_PORT_APM;
	pd->param_id = APM_PARAM_ID_MODULE_PROP;
	pd->param_size = mp_sz - 16u;
	*(uint32_t *)(buf + pos + 16) = 4u;
	struct ar_mod_prop_entry *mp = (struct ar_mod_prop_entry *)(buf + pos + 20);
	if (!is_capture) {
		mp[0] = (struct ar_mod_prop_entry){ PB_IID_SHM, 1u, APM_MOD_PROP_PORT_INFO, 8u, 0u, 1u };
		mp[1] = (struct ar_mod_prop_entry){ PB_IID_DEC, 1u, APM_MOD_PROP_PORT_INFO, 8u, 1u, 1u };
		mp[2] = (struct ar_mod_prop_entry){ PB_IID_CNV, 1u, APM_MOD_PROP_PORT_INFO, 8u, 1u, 1u };
		mp[3] = (struct ar_mod_prop_entry){ PB_IID_DMA, 1u, APM_MOD_PROP_PORT_INFO, 8u, 1u, 0u };
	} else {
		mp[0] = (struct ar_mod_prop_entry){ CAP_IID_DMA, 1u, APM_MOD_PROP_PORT_INFO, 8u, 0u, 1u };
		mp[1] = (struct ar_mod_prop_entry){ CAP_IID_CNV, 1u, APM_MOD_PROP_PORT_INFO, 8u, 1u, 1u };
		mp[2] = (struct ar_mod_prop_entry){ CAP_IID_ENC, 1u, APM_MOD_PROP_PORT_INFO, 8u, 1u, 1u };
		mp[3] = (struct ar_mod_prop_entry){ CAP_IID_SHM, 1u, APM_MOD_PROP_PORT_INFO, 8u, 1u, 0u };
	}
	pos += mp_sz;

	/* 5. MODULE_CONN (3 connections: port 1 = out, port 2 = in) */
	uint32_t mc_sz = AR_ALIGN8(16u + 4u + 3u * (uint32_t)sizeof(struct ar_conn_entry));
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = GPR_PORT_APM;
	pd->param_id = APM_PARAM_ID_MODULE_CONN;
	pd->param_size = mc_sz - 16u;
	*(uint32_t *)(buf + pos + 16) = 3u;
	struct ar_conn_entry *mc = (struct ar_conn_entry *)(buf + pos + 20);
	if (!is_capture) {
		mc[0] = (struct ar_conn_entry){ PB_IID_SHM, 1u, PB_IID_DEC, 2u };
		mc[1] = (struct ar_conn_entry){ PB_IID_DEC, 1u, PB_IID_CNV, 2u };
		mc[2] = (struct ar_conn_entry){ PB_IID_CNV, 1u, PB_IID_DMA, 2u };
	} else {
		mc[0] = (struct ar_conn_entry){ CAP_IID_DMA, 1u, CAP_IID_CNV, 2u };
		mc[1] = (struct ar_conn_entry){ CAP_IID_CNV, 1u, CAP_IID_ENC, 2u };
		mc[2] = (struct ar_conn_entry){ CAP_IID_ENC, 1u, CAP_IID_SHM, 2u };
	}
	pos += mc_sz;

	return ar_apm_cmd(what, GPR_PORT_PAUDIO, GPR_PORT_APM, APM_CMD_GRAPH_OPEN, buf, pos, 0, NULL);
}

static bool
ar_cfg_codec_dma(const char *what, uint32_t iid, uint32_t lpaif_type, uint32_t intf_idx,
    uint32_t ch_mask, uint16_t nch)
{
	uint8_t buf[160];
	uint32_t pos = 0;
	bzero(buf, sizeof(buf));

	/* 1. HW_EP_MF_CFG (16 + 12 = 28 -> 32) */
	struct apm_module_param_data *pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = iid;
	pd->param_id = PARAM_ID_HW_EP_MF_CFG;
	pd->param_size = 16u;
	*(uint32_t *)(buf + pos + 16) = 48000u;
	*(uint16_t *)(buf + pos + 20) = 16u;
	*(uint16_t *)(buf + pos + 22) = nch;
	*(uint32_t *)(buf + pos + 24) = 1u; /* DATA_FORMAT_FIXED_POINT */
	pos += 32u;

	/* 2. HW_EP_FRAME_SIZE_FACTOR (16 + 4 = 20 -> 24) */
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = iid;
	pd->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	pd->param_size = 8u;
	*(uint32_t *)(buf + pos + 16) = 1u;
	pos += 24u;

	/* 3. CODEC_DMA_INTF_CFG (16 + 12 = 28 -> 32) */
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = iid;
	pd->param_id = PARAM_ID_CODEC_DMA_INTF_CFG;
	pd->param_size = 16u;
	*(uint32_t *)(buf + pos + 16) = lpaif_type;
	*(uint32_t *)(buf + pos + 20) = intf_idx;
	*(uint32_t *)(buf + pos + 24) = ch_mask;
	pos += 32u;

	/* 4. HW_EP_POWER_MODE_CFG (16 + 4 = 20 -> 24) */
	pd = (struct apm_module_param_data *)(buf + pos);
	pd->module_instance_id = iid;
	pd->param_id = PARAM_ID_HW_EP_POWER_MODE_CFG;
	pd->param_size = 8u;
	*(uint32_t *)(buf + pos + 16) = 0u;
	pos += 24u;

	return ar_apm_cmd(what, GPR_PORT_PAUDIO, GPR_PORT_APM, APM_CMD_SET_CFG, buf, pos, 0, NULL);
}

static bool
ar_cfg_pcm_cnv(const char *what, uint32_t iid, uint16_t nch, uint16_t interleaved)
{
	uint8_t buf[64];
	bzero(buf, sizeof(buf));
	/* 16 (pd) + 12 (header) + 16 (payload_pcm_output_format_cfg) + nch -> align8 */
	uint32_t total = AR_ALIGN8(16u + 12u + 16u + nch);
	struct apm_module_param_data *pd = (struct apm_module_param_data *)buf;
	pd->module_instance_id = iid;
	pd->param_id = PARAM_ID_PCM_OUTPUT_FORMAT_CFG;
	pd->param_size = total - 16u;
	/* header */
	*(uint32_t *)(buf + 16) = 1u;           /* DATA_FORMAT_FIXED_POINT */
	*(uint32_t *)(buf + 20) = 0x09001000u;  /* MEDIA_FMT_ID_PCM */
	*(uint32_t *)(buf + 24) = (16u + nch + 3u) & ~3u;
	/* media_cfg */
	*(uint16_t *)(buf + 28) = 16u;          /* bit_width */
	*(uint16_t *)(buf + 30) = 1u;           /* PCM_LSB_ALIGNED */
	*(uint16_t *)(buf + 32) = 16u;          /* bits_per_sample */
	*(uint16_t *)(buf + 34) = 15u;          /* q_factor */
	*(uint16_t *)(buf + 36) = 1u;           /* PCM_LITTLE_ENDIAN */
	*(uint16_t *)(buf + 38) = interleaved;  /* 1=interleaved, 3=deinterleaved_unpacked */
	*(uint16_t *)(buf + 40) = 0u;           /* reserved */
	*(uint16_t *)(buf + 42) = nch;
	buf[44] = 1u;                           /* PCM_CHANNEL_FL */
	if (nch >= 2) {
		buf[45] = 2u;                   /* PCM_CHANNEL_FR */
	}
	return ar_apm_cmd(what, GPR_PORT_PAUDIO, GPR_PORT_APM, APM_CMD_SET_CFG, buf, total, 0, NULL);
}

static bool
ar_cfg_wr_shm_mf(uint32_t iid, uint16_t nch)
{
	uint8_t buf[64];
	bzero(buf, sizeof(buf));
	/* 16 (pd) + 12 (media_format) + 16 (payload_media_fmt_pcm) + nch -> align8 = 48 */
	uint32_t total = AR_ALIGN8(16u + 12u + 16u + nch);
	struct apm_module_param_data *pd = (struct apm_module_param_data *)buf;
	pd->module_instance_id = iid;
	pd->param_id = PARAM_ID_MEDIA_FORMAT;
	pd->param_size = total - 16u;
	*(uint32_t *)(buf + 16) = 1u;           /* DATA_FORMAT_FIXED_POINT */
	*(uint32_t *)(buf + 20) = 0x09001000u;  /* MEDIA_FMT_ID_PCM */
	*(uint32_t *)(buf + 24) = (total - 16u) - 12u;
	*(uint32_t *)(buf + 28) = 48000u;
	*(uint16_t *)(buf + 32) = 16u;          /* bit_width */
	*(uint16_t *)(buf + 34) = 1u;           /* PCM_LSB_ALIGNED */
	*(uint16_t *)(buf + 36) = 16u;          /* bits_per_sample */
	*(uint16_t *)(buf + 38) = 15u;          /* q_factor */
	*(uint16_t *)(buf + 40) = 1u;           /* PCM_LITTLE_ENDIAN */
	*(uint16_t *)(buf + 42) = nch;
	buf[44] = 1u;
	if (nch >= 2) {
		buf[45] = 2u;
	}
	return ar_apm_cmd("WR_SHM_MF", GPR_PORT_PB_DATA, iid, APM_CMD_SET_CFG, buf, total, 0, NULL);
}

static bool
ar_cfg_rd_shm(uint32_t iid)
{
	struct {
		struct apm_module_param_data pd;
		uint32_t num_frames_per_buffer;
		uint32_t metadata_control_flags;
	} __attribute__((packed)) b;

	bzero(&b, sizeof(b));
	b.pd.module_instance_id = iid;
	b.pd.param_id = PARAM_ID_RD_SH_MEM_CFG;
	b.pd.param_size = 8u;
	b.num_frames_per_buffer = 10u; /* 10 ms = 480 samples = 960 bytes per read */
	b.metadata_control_flags = 0u;
	return ar_apm_cmd("RD_SHM_CFG", GPR_PORT_PAUDIO, GPR_PORT_APM, APM_CMD_SET_CFG, &b, sizeof(b), 0, NULL);
}

static void
ar_queue_pb_buf(unsigned idx)
{
	uint64_t addr = pa_smmu.dsp_addr + (uint64_t)idx * 4096ull;
	struct {
		uint32_t lsw, msw, handle, size;
		uint32_t ts_lsw, ts_msw, flags;
		uint32_t md_lsw, md_msw, md_handle, md_size;
	} __attribute__((packed)) w = {
		(uint32_t)addr, (uint32_t)(addr >> 32), ar_mem_handle, PB_PERIOD_BYTES,
		0u, 0u, 0u, 0u, 0u, 0u, 0u
	};
	(void)hakios_adsp_gpr(GPR_PORT_PB_DATA, PB_IID_SHM, idx + 1u,
	    DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2, &w, sizeof(w), 0, NULL);
}

static void
ar_queue_cap_buf(unsigned idx)
{
	uint64_t addr = pa_smmu.dsp_addr + PB_BUF_BYTES + (uint64_t)idx * 4096ull;
	struct {
		uint32_t lsw, msw, handle, size;
		uint32_t md_lsw, md_msw, md_handle, md_size;
	} __attribute__((packed)) r = {
		(uint32_t)addr, (uint32_t)(addr >> 32), ar_mem_handle, CAP_PERIOD_BYTES,
		0u, 0u, 0u, 0u
	};
	(void)hakios_adsp_gpr(GPR_PORT_CAP_DATA, CAP_IID_SHM, idx + 1u,
	    DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2, &r, sizeof(r), 0, NULL);
}

/* 500 Hz pure sine table (96 samples/cycle @ 48 kHz = exact 5 cycles per 480-frame period) at -12 dBFS (amp 8192) */
static void
fill_pb_tone(void)
{
	/* 24-entry quarter-wave sine, scaled to 8192 */
	static const int16_t qsin[25] = {
		0, 536, 1069, 1598, 2120, 2633, 3135, 3623, 4096, 4551,
		4987, 5402, 5793, 6159, 6499, 6811, 7094, 7347, 7568, 7757,
		7912, 8034, 8122, 8174, 8192
	};
	for (unsigned p = 0; p < PB_PERIODS; p++) {
		vm_offset_t va = pa_smmu.dma_buf + (vm_offset_t)p * 4096u;
		int16_t *dst = (int16_t *)va;
		for (unsigned f = 0; f < PB_PERIOD_FRAMES; f++) {
			unsigned ph = f % 96u;
			int16_t v;
			if (ph <= 24u) {
				v = qsin[ph];
			} else if (ph <= 48u) {
				v = qsin[48u - ph];
			} else if (ph <= 72u) {
				v = (int16_t)(-qsin[ph - 48u]);
			} else {
				v = (int16_t)(-qsin[96u - ph]);
			}
			dst[2 * f]     = v;
			dst[2 * f + 1] = v;
		}
		for (vm_offset_t a = va; a < va + 4096u; a += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (a));
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
diag_drain_mic(uint32_t ms)
{
	int16_t stereo[512];
	uint64_t end = now_us() + (uint64_t)ms * 1000ull;

	while (now_us() < end) {
		uint64_t w = __atomic_load_n(&pa_mic_written, __ATOMIC_ACQUIRE);
		while (pa_mic_read < w) {
			uint32_t n = (uint32_t)(w - pa_mic_read);
			if (n > 256u) {
				n = 256u;
			}
			for (uint32_t i = 0; i < n; i++) {
				int16_t s = pa_mic_ring[(pa_mic_read + i) & (PA_RING_FRAMES - 1u)];
				stereo[2u * i] = s;
				stereo[2u * i + 1u] = s;
			}
			pa_mic_read += n;
			if (hakios_audio_recording_fed()) {
				hakios_audio_record_fed(stereo, n);
			}
		}
		sleep_ms(5);
	}
}

static void
test_audioreach_graphs(bool play_tone_and_spkr)
{
	if (!pa_smmu_setup()) {
		return;
	}
	if (!ar_map_dma()) {
		pa_smmu_teardown();
		return;
	}
	if (play_tone_and_spkr) {
		fill_pb_tone();
	}
	hakios_adsp_gpr_events(ar_gpr_event_cb);
	pb_done_cnt = cap_done_cnt = pb_err_cnt = cap_err_cnt = 0;
	cap_peak = cap_nonzero = 0;
	ar_darwin_mode = false;
	ar_streaming = true;

	/* Playback Graph (LPAIF_RXTX_RX_0: lpaif_type=1, intf_idx=1, ch_mask=3, 2ch 48kHz) */
	bool use_gc = false;
	bool pb_open = ar_open_graph("PB_GRAPH_OPEN(CD/EP)", false, false);
	if (!pb_open) {
		use_gc = true;
		pb_open = ar_open_graph("PB_GRAPH_OPEN(GC)", false, true);
	}
	if (pb_open) {
		ar_cfg_codec_dma("PB_CDMA_CFG", PB_IID_DMA, 1u, 1u, 0x3u, 2u);
		ar_cfg_pcm_cnv("PB_PCM_DEC", PB_IID_DEC, 2u, 1u);
		ar_cfg_pcm_cnv("PB_PCM_CNV", PB_IID_CNV, 2u, 3u);
		ar_cfg_wr_shm_mf(PB_IID_SHM, 2u);
		ar_graph_mgmt("PB_PREPARE", APM_CMD_GRAPH_PREPARE, PB_SG_STR, PB_SG_DEV);
		ar_graph_mgmt("PB_START", APM_CMD_GRAPH_START, PB_SG_STR, PB_SG_DEV);
		for (unsigned i = 0; i < PB_PERIODS; i++) {
			ar_queue_pb_buf(i);
		}
	}

	/* Capture Graph (LPAIF_RXTX_TX_3: lpaif_type=1, intf_idx=4, ch_mask=4, 1ch 48kHz) */
	bool cap_open = ar_open_graph("CAP_GRAPH_OPEN", true, use_gc);
	if (cap_open) {
		ar_cfg_codec_dma("CAP_CDMA_CFG", CAP_IID_DMA, 1u, 4u, 0x4u, 1u);
		ar_cfg_pcm_cnv("CAP_PCM_CNV", CAP_IID_CNV, 1u, 1u);
		ar_cfg_pcm_cnv("CAP_PCM_ENC", CAP_IID_ENC, 1u, 1u);
		ar_cfg_rd_shm(CAP_IID_SHM);
		ar_graph_mgmt("CAP_PREPARE", APM_CMD_GRAPH_PREPARE, CAP_SG_DEV, CAP_SG_STR);
		ar_graph_mgmt("CAP_START", APM_CMD_GRAPH_START, CAP_SG_DEV, CAP_SG_STR);
		for (unsigned i = 0; i < CAP_PERIODS; i++) {
			ar_queue_cap_buf(i);
		}
	}

	if (play_tone_and_spkr && pb_open) {
		if (pa_mic_ring == NULL) {
			pa_mic_ring = kalloc_data(PA_RING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
		}
		sleep_ms(200);
		tx_dec2_unmute();
		sleep_ms(100);
		tx_dec2_settle();
		pa_mic_read = __atomic_load_n(&pa_mic_written, __ATOMIC_ACQUIRE);
		pa_mic_open = 1;
		say("paudio: diag seg0 silence (300 ms)\n");
		diag_drain_mic(300);
		say("paudio: diag seg1 EAR 0x34 ONLY (1000 ms)\n");
		fs1815_enable_mask(0x1u);
		diag_drain_mic(1000);
		fs1815_disable_both();
		diag_drain_mic(300);
		say("paudio: diag seg2 AUX 0x35 ONLY (1000 ms)\n");
		fs1815_enable_mask(0x2u);
		diag_drain_mic(1000);
		fs1815_disable_both();
		diag_drain_mic(300);
		pa_mic_open = 0;
	} else {
		sleep_ms(200);
		tx_dec2_unmute();
		sleep_ms(100);
		tx_dec2_settle();
		sleep_ms(400);
	}
	ar_streaming = false;
	say("paudio: stream result: PB done=%u err=%u, CAP done=%u err=%u, mic peak=%u nonzero=%u\n",
	    pb_done_cnt, pb_err_cnt, cap_done_cnt, cap_err_cnt, cap_peak, cap_nonzero);

	if (cap_open) {
		ar_graph_mgmt("CAP_STOP", APM_CMD_GRAPH_STOP, CAP_SG_DEV, CAP_SG_STR);
		ar_graph_mgmt("CAP_CLOSE", APM_CMD_GRAPH_CLOSE, CAP_SG_DEV, CAP_SG_STR);
	}
	if (pb_open) {
		ar_graph_mgmt("PB_STOP", APM_CMD_GRAPH_STOP, PB_SG_STR, PB_SG_DEV);
		ar_graph_mgmt("PB_CLOSE", APM_CMD_GRAPH_CLOSE, PB_SG_STR, PB_SG_DEV);
	}
	hakios_adsp_gpr_events(NULL);
	ar_unmap_dma();
	pa_smmu_teardown();
}

/* ---- Production Darwin Audio (mode=4: /dev/hakios-audio speakers + /dev/hakios-mic) ----------------- */

static volatile bool pa_stopping, pa_thread_running;
static int16_t *pa_pb_ring;
static volatile uint32_t pa_pb_head, pa_pb_tail;
static bool pa_pb_primed;
static volatile uint64_t pa_pb_last_sound_us;
static volatile uint32_t pa_pb_peak;
static uint64_t pa_pb_darwin_frames, pa_pb_fed_frames;
static bool pa_mic_dev_made;

static uint32_t
pa_audio_sink(const int16_t *samples, uint32_t frames)
{
	if (pa_pb_ring == NULL || pa_stopping || frames == 0) {
		return 0;
	}
	uint32_t head = pa_pb_head, tail = pa_pb_tail;
	uint32_t room = PA_RING_FRAMES - (head - tail);
	uint32_t take = frames < room ? frames : room;
	bool sound = false;

	for (uint32_t i = 0; i < take; i++) {
		int16_t l = samples[2u * i];
		int16_t r = samples[2u * i + 1u];
		uint32_t pos = (head + i) & (PA_RING_FRAMES - 1u);
		pa_pb_ring[2u * pos]      = l;
		pa_pb_ring[2u * pos + 1u] = r;
		uint32_t al = (uint32_t)(l < 0 ? -l : l);
		uint32_t ar = (uint32_t)(r < 0 ? -r : r);
		if (al > 16u || ar > 16u) {
			sound = true;
		}
		if (al > pa_pb_peak) {
			pa_pb_peak = al;
		}
		if (ar > pa_pb_peak) {
			pa_pb_peak = ar;
		}
	}
	OSMemoryBarrier();
	pa_pb_head = head + take;
	pa_pb_darwin_frames += take;
	if (sound) {
		pa_pb_last_sound_us = now_us();
	}
	return take;
}

static void
pa_fill_pb_period(unsigned idx)
{
	vm_offset_t va = pa_smmu.dma_buf + (vm_offset_t)idx * 4096u;
	int16_t *dst = (int16_t *)va;
	uint32_t tail = pa_pb_tail, head = pa_pb_head;
	uint32_t avail = head - tail;
	uint32_t n = 0;

	if (!pa_pb_primed && avail >= PB_RING_PRIME) {
		pa_pb_primed = true;
	}
	if (pa_pb_primed) {
		if (avail >= PB_PERIOD_FRAMES) {
			n = PB_PERIOD_FRAMES;
		} else if (avail > 0) {
			n = avail;
			pa_pb_primed = false;
			pa_pb_underruns++;
		} else {
			pa_pb_primed = false;
			pa_pb_underruns++;
		}
	}

	OSMemoryBarrier();
	for (uint32_t i = 0; i < n; i++) {
		uint32_t pos = (tail + i) & (PA_RING_FRAMES - 1u);
		int32_t l = pa_pb_ring[2u * pos];
		int32_t r = pa_pb_ring[2u * pos + 1u];
		/* Gentle peak clamp for speaker amp protection */
		if (l > 28000) {
			l = 28000;
		} else if (l < -28000) {
			l = -28000;
		}
		if (r > 28000) {
			r = 28000;
		} else if (r < -28000) {
			r = -28000;
		}
		dst[2u * i]      = (int16_t)l;
		dst[2u * i + 1u] = (int16_t)r;
	}
	if (n < PB_PERIOD_FRAMES) {
		bzero(dst + 2u * n, (PB_PERIOD_FRAMES - n) * 4u);
	}
	OSMemoryBarrier();
	pa_pb_tail = tail + n;
	pa_pb_fed_frames += n;
	for (vm_offset_t a = va; a < va + PB_PERIOD_BYTES; a += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (a));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static int
pa_mic_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	pa_mic_read = __atomic_load_n(&pa_mic_written, __ATOMIC_ACQUIRE);
	pa_mic_primed = false;
	cap_peak = 0;
	cap_nonzero = 0;
	cap_sec_min = 32767;
	cap_sec_max = -32768;
	cap_sec_sq = 0;
	cap_sec_cnt = 0;
	cap_sec_ready = false;
	pa_mic_open = 1;
	say("paudio: /dev/hakios-mic opened by Darwin\n");
	return 0;
}

static int
pa_mic_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	pa_mic_open = 0;
	say("paudio: /dev/hakios-mic closed (cap_done=%u peak=%u nonzero=%u)\n",
	    cap_done_cnt, cap_peak, cap_nonzero);
	return 0;
}

static int
pa_mic_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&pa_mic_written, __ATOMIC_ACQUIRE);
	uint64_t avail, want;
	int error = 0;

	if (pa_mic_ring == NULL || uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2u;
	avail = written - pa_mic_read;
	if (avail > MIC_RING_MOST) {
		pa_mic_read = written - MIC_RING_KEEP;
		avail = MIC_RING_KEEP;
	}
	if (!pa_mic_primed) {
		if (avail < MIC_RING_KEEP) {
			return 0;
		}
		pa_mic_primed = true;
	}
	if (avail < want) {
		pa_mic_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = pa_mic_read & (PA_RING_FRAMES - 1u);
		uint64_t n = want < avail ? want : avail;
		if (n > PA_RING_FRAMES - at) {
			n = PA_RING_FRAMES - at;
		}
		error = uiomove((caddr_t)&pa_mic_ring[at], (int)(n * 2u), uio);
		pa_mic_read += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static const struct cdevsw pa_mic_cdevsw = {
	.d_open = pa_mic_dev_open, .d_close = pa_mic_dev_close, .d_read = pa_mic_dev_read,
	.d_write = eno_rdwrt, .d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset,
	.d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
pa_darwin_run(void)
{
	uint32_t base_below_db = 6;
	PE_parse_boot_argn("parrot_audio_below", &base_below_db, sizeof(base_below_db));
	if (base_below_db > 48) {
		base_below_db = 48;
	}

	if (pa_pb_ring == NULL) {
		pa_pb_ring = kalloc_data(PA_RING_FRAMES * 2u * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	}
	if (pa_mic_ring == NULL) {
		pa_mic_ring = kalloc_data(PA_RING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	}
	if (pa_pb_ring == NULL || pa_mic_ring == NULL) {
		say("paudio: failed to allocate PCM rings\n");
		return;
	}

	if (!pa_mic_dev_made) {
		int major = cdevsw_add(-1, &pa_mic_cdevsw);
		if (major >= 0 &&
		    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-mic") != NULL) {
			pa_mic_dev_made = true;
			say("paudio: /dev/hakios-mic ready (48 kHz mono 16-bit)\n");
		} else {
			say("paudio: failed to create /dev/hakios-mic\n");
		}
	}

	if (!pa_smmu_setup()) {
		return;
	}
	if (!ar_map_dma()) {
		pa_smmu_teardown();
		return;
	}

	bzero((void *)pa_smmu.dma_buf, PB_BUF_BYTES + CAP_BUF_BYTES);
	for (vm_offset_t a = pa_smmu.dma_buf; a < pa_smmu.dma_buf + PB_BUF_BYTES + CAP_BUF_BYTES; a += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (a));
	}
	__asm__ volatile ("dsb sy" ::: "memory");

	pb_done_cnt = cap_done_cnt = pb_err_cnt = cap_err_cnt = 0;
	cap_peak = cap_nonzero = 0;
	pb_returned_mask = 0;
	pa_pb_head = pa_pb_tail = 0;
	pa_pb_primed = false;
	pa_pb_last_sound_us = 0;
	pa_pb_peak = 0;
	pa_pb_darwin_frames = pa_pb_fed_frames = 0;
	ar_darwin_mode = true;
	ar_streaming = true;
	hakios_adsp_gpr_events(ar_gpr_event_cb);

	bool use_gc = false;
	bool pb_open = ar_open_graph("PB_GRAPH_OPEN(CD/EP)", false, false);
	if (!pb_open) {
		use_gc = true;
		pb_open = ar_open_graph("PB_GRAPH_OPEN(GC)", false, true);
	}
	if (pb_open) {
		ar_cfg_codec_dma("PB_CDMA_CFG", PB_IID_DMA, 1u, 1u, 0x3u, 2u);
		ar_cfg_pcm_cnv("PB_PCM_DEC", PB_IID_DEC, 2u, 1u);
		ar_cfg_pcm_cnv("PB_PCM_CNV", PB_IID_CNV, 2u, 3u);
		ar_cfg_wr_shm_mf(PB_IID_SHM, 2u);
		ar_graph_mgmt("PB_PREPARE", APM_CMD_GRAPH_PREPARE, PB_SG_STR, PB_SG_DEV);
		ar_graph_mgmt("PB_START", APM_CMD_GRAPH_START, PB_SG_STR, PB_SG_DEV);
		for (unsigned i = 0; i < PB_PERIODS; i++) {
			pa_fill_pb_period(i);
			ar_queue_pb_buf(i);
		}
	}

	bool cap_open = ar_open_graph("CAP_GRAPH_OPEN", true, use_gc);
	if (cap_open) {
		ar_cfg_codec_dma("CAP_CDMA_CFG", CAP_IID_DMA, 1u, 4u, 0x4u, 1u);
		ar_cfg_pcm_cnv("CAP_PCM_CNV", CAP_IID_CNV, 1u, 1u);
		ar_cfg_pcm_cnv("CAP_PCM_ENC", CAP_IID_ENC, 1u, 1u);
		ar_cfg_rd_shm(CAP_IID_SHM);
		ar_graph_mgmt("CAP_PREPARE", APM_CMD_GRAPH_PREPARE, CAP_SG_DEV, CAP_SG_STR);
		ar_graph_mgmt("CAP_START", APM_CMD_GRAPH_START, CAP_SG_DEV, CAP_SG_STR);
		for (unsigned i = 0; i < CAP_PERIODS; i++) {
			ar_queue_cap_buf(i);
		}
	}

	if (pb_open) {
		hakios_audio_set_sink("parrot SM6475 speakers (WCD937x + FS1815)", pa_audio_sink);
		say("paudio: DARWIN AUDIO ACTIVE (/dev/hakios-audio + /dev/hakios-mic, ceiling -%u dB)\n",
		    base_below_db);
	}

	uint64_t start_us = now_us();
	bool tx_unmuted = false, tx_settled = false, mic_checked = false, spkr_on = false;
	uint32_t vol_gen = 0;
	bool vol_muted = false;
	unsigned pb_next = 0;

	while (!pa_stopping) {
		uint64_t now = now_us();
		uint64_t elapsed_ms = (now - start_us) / 1000ull;

		if (!tx_unmuted && elapsed_ms >= 200) {
			tx_dec2_unmute();
			tx_unmuted = true;
		}
		if (!tx_settled && elapsed_ms >= 300) {
			tx_dec2_settle();
			cap_peak = 0;
			cap_nonzero = 0;
			tx_settled = true;
		}
		if (!mic_checked && elapsed_ms >= 800) {
			say("paudio: mic settled check (500 ms): peak=%u nonzero=%u\n", cap_peak, cap_nonzero);
			mic_checked = true;
		}
		if (cap_sec_ready) {
			cap_sec_ready = false;
			say("paudio: mic 1s: min=%d max=%d sq=%llu s=%d,%d,%d,%d\n",
			    cap_rep_min, cap_rep_max, (unsigned long long)cap_rep_sq,
			    cap_rep_s[0], cap_rep_s[1], cap_rep_s[2], cap_rep_s[3]);
		}

		/* Feed returned playback buffers in strict FIFO order */
		while (pb_returned_mask & (1u << pb_next)) {
			OSBitAndAtomic(~(1u << pb_next), &pb_returned_mask);
			pa_fill_pb_period(pb_next);
			ar_queue_pb_buf(pb_next);
			pb_next = (pb_next + 1u) % PB_PERIODS;
		}

		/* Track Darwin speaker volume */
		uint32_t extra = 0, gen_now = 0;
		bool muted_now = false;
		if (hakios_audio_volume(&extra, &muted_now, &gen_now) && gen_now != vol_gen) {
			vol_gen = gen_now;
			vol_muted = muted_now;
			int db = -(int)base_below_db - (int)(extra / 8u);
			pa_rx_set_vol(db, db - 3);
			say("paudio: volume -> %s%d dB\n", vol_muted ? "muted, " : "", db);
			if (vol_muted && spkr_on) {
				fs1815_disable_both();
				spkr_on = false;
			}
		}

		/* Auto-enable FS1815 amps on sound, auto-mute after 2.5s of silence */
		uint64_t last_snd = pa_pb_last_sound_us;
		if (!spkr_on && !vol_muted && last_snd != 0 && now >= last_snd && (now - last_snd) < 200000ull) {
			fs1815_enable_both();
			spkr_on = true;
		} else if (spkr_on && (vol_muted || last_snd == 0 || (now >= last_snd && (now - last_snd) > 2500000ull))) {
			fs1815_disable_both();
			say("paudio: pb summary: darwin=%llu fed=%llu peak=%u underruns=%u\n",
			    (unsigned long long)pa_pb_darwin_frames, (unsigned long long)pa_pb_fed_frames,
			    pa_pb_peak, pa_pb_underruns);
			pa_pb_peak = 0;
			spkr_on = false;
		}

		sleep_ms(2);
	}

	if (pb_open) {
		hakios_audio_set_sink("nothing", NULL);
	}
	if (spkr_on) {
		fs1815_disable_both();
	}
	ar_streaming = false;
	say("paudio: Darwin audio stopping (pb_done=%u cap_done=%u darwin_frames=%llu fed=%llu mic_peak=%u)\n",
	    pb_done_cnt, cap_done_cnt, pa_pb_darwin_frames, pa_pb_fed_frames, cap_peak);

	if (cap_open) {
		ar_graph_mgmt("CAP_STOP", APM_CMD_GRAPH_STOP, CAP_SG_DEV, CAP_SG_STR);
		ar_graph_mgmt("CAP_CLOSE", APM_CMD_GRAPH_CLOSE, CAP_SG_DEV, CAP_SG_STR);
	}
	if (pb_open) {
		ar_graph_mgmt("PB_STOP", APM_CMD_GRAPH_STOP, PB_SG_STR, PB_SG_DEV);
		ar_graph_mgmt("PB_CLOSE", APM_CMD_GRAPH_CLOSE, PB_SG_STR, PB_SG_DEV);
	}
	hakios_adsp_gpr_events(NULL);
	ar_unmap_dma();
	pa_smmu_teardown();
}

/* ---- Entry points ---------------------------------------------------------------------------------- */

static bool pa_started;
static uint32_t pa_mode;

static void
pa_bringup_all(uint32_t mode)
{
	say("paudio: === SM6475 audio bring-up (mode=%u) ===\n", mode);

	tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);

	/* 1. Power supplies for WCD937x via RPMh VRM */
	vrm_enable_reg("ldob20", 1800, VRM_MODE_LDO_HPM, true);
	vrm_enable_reg("ldob21", 1800, VRM_MODE_LDO_HPM, true);
	vrm_enable_reg("bobe1", 3300, 0, false);
	sleep_ms(5);

	/* 2. PRM core votes & macro clocks via GPR */
	prm_vote_core(Q6PRM_HW_CORE_ID_LPASS, true);
	prm_vote_core(Q6PRM_HW_CORE_ID_DCODEC, true);

	if (!prm_req_clk(CLK_VA_CORE, 19200000u, 1u)) {
		prm_req_clk(CLK_VA_CORE, 19200000u, 0u);
	}
	prm_req_clk(CLK_VA_CORE_2X, 19200000u, 1u);
	prm_req_clk(CLK_TX_CORE, 19200000u, 1u);
	prm_req_clk(CLK_TX_NPL, 19200000u, 1u);
	prm_req_clk(CLK_RX_CORE, 19200000u, 1u);
	prm_req_clk(CLK_RX_NPL, 19200000u, 1u);
	prm_req_clk(CLK_RX_TX_MCLK, 19200000u, 1u);
	prm_req_clk(CLK_RX_TX_2X, 19200000u, 1u);
	prm_req_clk(CLK_RX_MCLK2_2X, 19200000u, 1u);

	/* 3. I2C scan for speaker PAs (FS1815) */
	probe_pa_i2c();

	/* 4. WCD937x SoundWire + AudioReach GPR playback & capture */
	probe_wcd937x(mode);

	say("paudio: === SM6475 audio finished ===\n");
}

static void
pa_darwin_thread(__unused void *parameter, __unused wait_result_t result)
{
	pa_bringup_all(pa_mode);
	pa_thread_running = false;
	thread_terminate(current_thread());
}

void
hakios_parrot_audio_dsp_up(void)
{
	uint32_t mode = 0, diag = 0;

	if (!hakios_adsp_is_gpr()) {
		return;
	}
	PE_parse_boot_argn("parrot_audio", &mode, sizeof(mode));
	PE_parse_boot_argn("parrot_audio_diag", &diag, sizeof(diag));
	if (diag != 0) {
		mode = 3;
	}
	if (mode == 0) {
		return;
	}
	pa_started = true;
	pa_stopping = false;
	pa_mode = mode;

	if (mode == 4) {
		thread_t th;
		pa_thread_running = true;
		if (kernel_thread_start(pa_darwin_thread, NULL, &th) != KERN_SUCCESS) {
			pa_thread_running = false;
			say("paudio: failed to start Darwin audio thread\n");
			return;
		}
		thread_deallocate(th);
		return;
	}

	pa_bringup_all(mode);
}

void
hakios_parrot_audio_dsp_down(void)
{
	if (!pa_started) {
		return;
	}
	pa_stopping = true;
	for (unsigned ms = 0; ms < 3000 && pa_thread_running; ms++) {
		sleep_ms(1);
	}
	fs1815_disable_both();
	while (prm_clks_count > 0) {
		prm_rel_clk(prm_clks_on[--prm_clks_count]);
	}
	if (prm_core_voted[Q6PRM_HW_CORE_ID_DCODEC]) {
		prm_vote_core(Q6PRM_HW_CORE_ID_DCODEC, false);
	}
	if (prm_core_voted[Q6PRM_HW_CORE_ID_LPASS]) {
		prm_vote_core(Q6PRM_HW_CORE_ID_LPASS, false);
	}
	pa_started = false;
}

void
hakios_parrot_audio_quiesce(void)
{
	pa_leaving = true;
	pa_stopping = true;
	fs1815_disable_both();
}
