import struct
import subprocess

# 1. Compile bitbang_transfer (leaf function, strictly scratch registers x0-x15 only)
bb_asm = """
.global _start
.text

_start:
    // x0 = tx, x1 = rx, w2 = length
    cbz w2, 9f

    adrp x8, tlmm_var
    ldr x8, [x8, :lo12:tlmm_var]

    add x10, x8, #0xd, lsl #12   // GPIO 13 (CS)
    add x11, x8, #0xc, lsl #12   // GPIO 12 (CLK)
    add x12, x8, #0xb, lsl #12   // GPIO 11 (MOSI)
    add x13, x8, #0xa, lsl #12   // GPIO 10 (MISO)

    // Ensure pin direction:
    mov w9, #0x280               // OUT=1, 6mA, func 0
    str w9, [x10]                // CS CFG
    str w9, [x11]                // CLK CFG
    str w9, [x12]                // MOSI CFG
    mov w9, #3                   // IN, pull-up, func 0
    str w9, [x13]                // MISO CFG

    // CS = 0
    str wzr, [x10, #4]

    // CS setup delay
    mov w14, #40
1:  subs w14, w14, #1
    b.ne 1b

    mov w3, #0                   // byte index i = 0

byte_loop:
    mov w5, #0                   // tx_byte = 0
    cbz x0, 2f
    ldrb w5, [x0, x3]
2:
    mov w4, #8                   // bit counter = 8
    mov w6, #0                   // rx_byte = 0

bit_loop:
    lsr w9, w5, #7
    and w9, w9, #1
    lsl w9, w9, #1               // bit 1 is OUT
    str w9, [x12, #4]            // MOSI bit
    lsl w5, w5, #1

    // Setup delay
    mov w14, #25
3:  subs w14, w14, #1
    b.ne 3b

    // CLK = 1
    mov w9, #2
    str w9, [x11, #4]

    // Sample delay
    mov w14, #25
4:  subs w14, w14, #1
    b.ne 4b

    // Read MISO (bit 0 is IN)
    ldr w9, [x13, #4]
    and w9, w9, #1
    lsl w6, w6, #1
    orr w6, w6, w9

    // CLK = 0
    str wzr, [x11, #4]

    // Hold delay
    mov w14, #25
5:  subs w14, w14, #1
    b.ne 5b

    subs w4, w4, #1
    b.ne bit_loop

    cbz x1, 6f
    strb w6, [x1, x3]
6:
    add w3, w3, #1
    cmp w3, w2
    b.lo byte_loop

    // CS hold delay
    mov w14, #40
7:  subs w14, w14, #1
    b.ne 7b

    // CS = 1
    mov w9, #2
    str w9, [x10, #4]

    // Inter-transfer delay
    mov w14, #100
8:  subs w14, w14, #1
    b.ne 8b

9:
    mov w0, #1
    ret
"""

with open("/tmp/bb.s", "w") as f: f.write(bb_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/bb.s", "-o", "/tmp/bb.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a7308", "--defsym=tlmm_var=0xfffffff007ed6ad0", "/tmp/bb.o", "-o", "/tmp/bb.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/bb.elf", "/tmp/bb.bin"])
with open("/tmp/bb.bin", "rb") as f: bb_bin = f.read()
print("bitbang_transfer size:", len(bb_bin))

# 2. Compile parrot_init (placed at 0xfffffff0074a4fe0)
init_asm = """
.global _start
.text

_start:
    // Frame setup
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]

    adrp x25, globals_base
    ldr x8, [x25, #0xad0]       // x8 = tlmm

    // 1. IOVDD (GPIO 38): Ensure OUT=1, 8mA
    mov w9, #2
    mov w10, #0x6004
    movk w10, #2, lsl #16       // 0x26004 = GPIO_IO(38)
    str w9, [x8, x10]

    mov w9, #0x2c0              // CFG: func 0, 8mA, pull none, out enable
    mov w10, #0x6000
    movk w10, #2, lsl #16       // 0x26000 = GPIO_CFG(38)
    str w9, [x8, x10]

    // Delay 15 ms:
    mov w0, #15
    mov w1, #0x4240
    movk w1, #0xf, lsl #16
    bl delay_for_interval

    // 2. RESET (GPIO 131) low:
    ldr x8, [x25, #0xad0]
    mov w10, #0x3004
    movk w10, #8, lsl #16       // 0x83004 = GPIO_IO(131)
    str wzr, [x8, x10]

    mov w9, #0x2c3              // func 0, 8mA, pull-up, out enable
    mov w10, #0x3000
    movk w10, #8, lsl #16       // 0x83000 = GPIO_CFG(131)
    str w9, [x8, x10]

    // Delay 10 ms:
    mov w0, #10
    mov w1, #0x4240
    movk w1, #0xf, lsl #16
    bl delay_for_interval

    // 3. RESET (GPIO 131) high:
    ldr x8, [x25, #0xad0]
    mov w9, #2
    mov w10, #0x3004
    movk w10, #8, lsl #16
    str w9, [x8, x10]

    // Delay 200 ms:
    mov w0, #200
    mov w1, #0x4240
    movk w1, #0xf, lsl #16
    bl delay_for_interval

    // 4. Configure INT (GPIO 65): in, pull-up
    ldr x8, [x25, #0xad0]
    mov w9, #3                  // in, pull-up
    mov w10, #0x1000
    movk w10, #4, lsl #16       // 0x41000 = GPIO_CFG(65)
    str w9, [x8, x10]

    // 5. Configure SPI pins:
    add x10, x8, #0xd, lsl #12   // GPIO 13 (CS)
    add x11, x8, #0xc, lsl #12   // GPIO 12 (CLK)
    add x12, x8, #0xb, lsl #12   // GPIO 11 (MOSI)
    add x13, x8, #0xa, lsl #12   // GPIO 10 (MISO)

    mov w9, #2
    str w9, [x10, #4]           // CS = 1
    mov w9, #0x280
    str w9, [x10]               // CS CFG
    str wzr, [x11, #4]          // CLK = 0
    str w9, [x11]               // CLK CFG
    str wzr, [x12, #4]          // MOSI = 0
    str w9, [x12]               // MOSI CFG
    mov w9, #3                  // in, pull-up
    str w9, [x13]               // MISO CFG

    // 6. Set up driver state:
    adrp x8, spi_transfer_var
    adrp x9, bitbang_func
    add x9, x9, :lo12:bitbang_func
    str x9, [x8, :lo12:spi_transfer_var]

    mov w9, #1
    strb w9, [x25, #0xb28]      // spi_by_hand = 1
    str w9, [x25, #0xaf8]       // touch_chip = TOUCH_FOCALTECH

    mov w22, #4000
    str w22, [x25, #0xafc]      // touch_poll_us = 4000

    // Set poll_us in parent goodix_thread stack frame:
    ldr x8, [x29]               // parent x29
    stur w22, [x8, #-0x84]
    stur w22, [x8, #-0x90]

    // 7. Call chip_probe(1) with up to 5 attempts:
    mov w19, #5
probe_try:
    mov w0, #1
    bl chip_probe
    cbnz w0, probe_ok

    mov w0, #100
    mov w1, #0x4240
    movk w1, #0xf, lsl #16
    bl delay_for_interval
    subs w19, w19, #1
    b.ne probe_try

probe_ok:
    // Restore saved frame
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48

    // Set up registers expected by success path:
    adrp x23, globals_base
    adrp x19, globals_base
    adrp x21, globals_base
    // Jump straight to success path at 0xfffffff0074a53f4
    b succ_path
"""

with open("/tmp/parrot_init.s", "w") as f: f.write(init_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/parrot_init.s", "-o", "/tmp/parrot_init.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a4fe0",
  "--defsym=delay_for_interval=0xfffffff0072cbb18",
  "--defsym=spi_transfer_var=0xfffffff007ed6b20",
  "--defsym=chip_probe=0xfffffff0074a69e8",
  "--defsym=succ_path=0xfffffff0074a53f4",
  "--defsym=globals_base=0xfffffff007ed6000",
  "--defsym=bitbang_func=0xfffffff0074a7308",
  "/tmp/parrot_init.o", "-o", "/tmp/parrot_init.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/parrot_init.elf", "/tmp/parrot_init.bin"])
with open("/tmp/parrot_init.bin", "rb") as f: init_bin = f.read()
print("parrot_init size:", len(init_bin))

# 3. Compile poll_patch (replaces 0x4a1ea8:0x4a1ee8 in _chip_poll for PV2)
poll_asm = """
.global _start
.text
_start:
    adrp x19, page_ed7
    add  x24, x19, #0x7e4       // PV2 slot 0 starts at buf + 4
    ldurb w25, [x24, #-3]       // read buf[1] (touch_count in bits 3:0)
    and   w25, w25, #0x0f
    cbz   w25, lift_path        // if 0 active touches -> jump to lift handler
    add   x27, x19, #0x824      // contact slots array base
    mov   w26, #0xc             // slot size 12
    adrp  x28, page_ed6
    strb  wzr, [x8, #0x24]
    strb  wzr, [x8, #0x30]
    strb  wzr, [x8, #0x3c]
    strb  wzr, [x8, #0x48]
    strb  wzr, [x8, #0x54]
    strb  wzr, [x8, #0x60]
    strb  wzr, [x8, #0x6c]
    b     loop_start
"""
with open("/tmp/poll_patch.s", "w") as f: f.write(poll_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/poll_patch.s", "-o", "/tmp/poll_patch.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a5ea8",
  "--defsym=page_ed7=0xfffffff007ed7000",
  "--defsym=page_ed6=0xfffffff007ed6000",
  "--defsym=lift_path=0xfffffff0074a6344",
  "--defsym=loop_start=0xfffffff0074a5ef4",
  "/tmp/poll_patch.o", "-o", "/tmp/poll_patch.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/poll_patch.elf", "/tmp/poll_patch.bin"])
with open("/tmp/poll_patch.bin", "rb") as f: poll_bin = f.read()
assert len(poll_bin) == 64
print("poll_patch size:", len(poll_bin))

# 3b. Compile stride_patch (replaces 0x4a1ee8:0x4a1ef4 for PV2 stride 8)
stride_asm = """
.global _start
.text
_start:
    add   x24, x24, #8          // Stride is 8 bytes in PV2
    subs  x25, x25, #1
    b.eq  lift_path
"""
with open("/tmp/stride.s", "w") as f: f.write(stride_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/stride.s", "-o", "/tmp/stride.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a5ee8",
  "--defsym=lift_path=0xfffffff0074a6344",
  "/tmp/stride.o", "-o", "/tmp/stride.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/stride.elf", "/tmp/stride.bin"])
with open("/tmp/stride.bin", "rb") as f: stride_bin = f.read()
assert len(stride_bin) == 12
print("stride_patch size:", len(stride_bin))

# 3c. Compile loop_head_patch (replaces 0x4a1ef4:0x4a1f14 for PV2 touch_id & event_flag)
loop_head_asm = """
.global _start
.text
_start:
    ldrb  w8, [x24, #2]         // byte 2: touch_id (7:4), y_high (3:0)
    cmp   w8, #0x9f
    b.hi  lift_path
    lsr   x20, x8, #4           // touch_id in x20!
    ldrb  w23, [x24]            // byte 0: event_flag (7:6), x_high (3:0)
    and   w9, w23, #0xc0
    cmp   w9, #0x40             // is LIFT (0x40)?
    b.ne  coord_unpack
"""
with open("/tmp/loop_head.s", "w") as f: f.write(loop_head_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/loop_head.s", "-o", "/tmp/loop_head.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a5ef4",
  "--defsym=lift_path=0xfffffff0074a6344",
  "--defsym=coord_unpack=0xfffffff0074a5f40",
  "/tmp/loop_head.o", "-o", "/tmp/loop_head.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/loop_head.elf", "/tmp/loop_head.bin"])
with open("/tmp/loop_head.bin", "rb") as f: loop_head_bin = f.read()
assert len(loop_head_bin) == 32
print("loop_head size:", len(loop_head_bin))

# 3d. Compile coord_unpack_patch (replaces 0x4a1f40:0x4a1f64 for PV2 raw X & Y)
coord_unpack_asm = """
.global _start
.text
_start:
    ldrb  w9, [x24, #1]         // x_mid
    lsl   w9, w9, #4
    bfi   w9, w23, #12, #4      // insert x_high
    ldrb  w10, [x24, #4]        // byte 4: x_low (7:4), y_low (3:0)
    orr   w9, w9, w10, lsr #4   // raw_x in w9!
    ldrb  w11, [x24, #3]        // y_mid
    ubfiz w8, w8, #12, #4       // y_high at bit 12
    orr   w8, w8, w11, lsl #4   // insert y_mid
    bfxil w8, w10, #0, #4       // insert y_low -> raw_y in w8!
"""
with open("/tmp/coord_unpack.s", "w") as f: f.write(coord_unpack_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/coord_unpack.s", "-o", "/tmp/coord_unpack.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a5f40", "/tmp/coord_unpack.o", "-o", "/tmp/coord_unpack.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/coord_unpack.elf", "/tmp/coord_unpack.bin"])
with open("/tmp/coord_unpack.bin", "rb") as f: coord_unpack_bin = f.read()
assert len(coord_unpack_bin) == 36
print("coord_unpack size:", len(coord_unpack_bin))

# 4. Compile clamp_patch (replaces 0x4a1f64:0x4a1f88 in _chip_poll)
clamp_asm = """
.global _start
.text
_start:
    mov   w11, #17279
    subs  w13, w11, w9
    csel  w11, wzr, w13, lo
    csel  w9, w11, w9, lo
    mov   w12, #38399
    subs  w14, w12, w8
    csel  w12, wzr, w14, lo
    csel  w8, w12, w8, lo
    ldr   w10, [x28, #0xac8]
"""
with open("/tmp/clamp.s", "w") as f: f.write(clamp_asm)
subprocess.check_call(["llvm-mc", "-arch=aarch64", "-filetype=obj", "/tmp/clamp.s", "-o", "/tmp/clamp.o"])
subprocess.check_call(["ld.lld", "-Ttext=0xfffffff0074a5f64", "/tmp/clamp.o", "-o", "/tmp/clamp.elf"])
subprocess.check_call(["llvm-objcopy", "-O", "binary", "/tmp/clamp.elf", "/tmp/clamp.bin"])
with open("/tmp/clamp.bin", "rb") as f: clamp_bin = f.read()
assert len(clamp_bin) == 36
print("clamp_patch size:", len(clamp_bin))

with open("/home/uralredux/Документы/project/hakios/hakios-kernel.orig", "rb") as f:
    buf = bytearray(f.read())

# 0. TLMM Physical Base: 0x0f000000 -> 0x0f100000
assert struct.unpack("<I", buf[0x4a0cb0:0x4a0cb4])[0] == 0x52a1e000
buf[0x4a0cb0:0x4a0cb4] = struct.pack("<I", 0x52a1e200) # mov w0, #0x0f100000

# 1. Regulator string: ldoc2 -> ldoe7
assert buf[0xa3d2d:0xa3d2d+6] == b"ldoc2\x00"
buf[0xa3d2d:0xa3d2d+6] = b"ldoe7\x00"

# 2. Voltage constant: 1800 mV (0x708) -> 3300 mV (0xce4)
assert struct.unpack("<I", buf[0x4a1a3c:0x4a1a40])[0] == 0x5280e10d
buf[0x4a1a3c:0x4a1a40] = struct.pack("<I", 0x52819c8d)

# 3. Skip Redwood show_state and touch_chip_detect (branch directly to cmd_db & RPMh power_on at 0x4a4e90)
assert struct.unpack("<I", buf[0x4a0d18:0x4a0d1c])[0] == 0xf9456f48
buf[0x4a0d18:0x4a0d1c] = struct.pack("<I", 0x1400005e) # b 0xfffffff0074a4e90

# 4. Insert parrot_init at 0x4a0fe0 (right after power_on enables ldoe7 and GPIO 38)
assert struct.unpack("<I", buf[0x4a0fe0:0x4a0fe4])[0] == 0xf9456f48
buf[0x4a0fe0:0x4a0fe0+len(init_bin)] = init_bin

# 5. Insert bitbang_transfer at 0x4a3308
buf[0x4a3308:0x4a3308+len(bb_bin)] = bb_bin

# 6. Make probe failure non-fatal in chip_probe (do NOT enter boot ROM)
assert struct.unpack("<I", buf[0x4a2a38:0x4a2a3c])[0] == 0x360008a0
buf[0x4a2a38:0x4a2a3c] = struct.pack("<I", 0x36004100) # tbz w0, #0, 0xfffffff0074a7258

# 7. FocalTech Chip ID: change 0x62 (FT3680) to 0x72 (FT3683G)
# In _focaltech_wait_valid (5 occurrences of cmp w8, #0x62):
wait_valid_offsets = [0x4a369c, 0x4a36f4, 0x4a374c, 0x4a37a4, 0x4a37fc]
for off in wait_valid_offsets:
    assert struct.unpack("<I", buf[off:off+4])[0] == 0x7101891f, f"bad opcode at {hex(off)}"
    buf[off:off+4] = struct.pack("<I", 0x7101c91f) # cmp w8, #0x72

# In _chip_probe (3 occurrences of cmp w22, #0x62):
probe_offsets = [0x4a2be0, 0x4a2e20, 0x4a2ed4]
for off in probe_offsets:
    assert struct.unpack("<I", buf[off:off+4])[0] == 0x71018adf, f"bad opcode at {hex(off)}"
    buf[off:off+4] = struct.pack("<I", 0x7101cadf) # cmp w22, #0x72

# 8. Touch point count and lift handler in _chip_poll (replaces hardcoded 10 with buf[1] & 0xf)
assert len(poll_bin) == 64
buf[0x4a1ea8:0x4a1ee8] = poll_bin

# 8b. Stride 8 for PV2
assert len(stride_bin) == 12
assert struct.unpack("<I", buf[0x4a1ee8:0x4a1eec])[0] == 0x91001b18 # add x24, x24, #6
buf[0x4a1ee8:0x4a1ef4] = stride_bin

# 8c. Touch ID & event flag unpack for PV2
assert len(loop_head_bin) == 32
assert struct.unpack("<I", buf[0x4a1ef4:0x4a1ef8])[0] == 0x385fe308 # ldurb w8, [x24, #-2]
buf[0x4a1ef4:0x4a1f14] = loop_head_bin

# 9. Coordinate decoding in _chip_poll for Parrot FT3683G (PV2 16x subpixel, 17280x38400)
assert len(coord_unpack_bin) == 36
assert struct.unpack("<I", buf[0x4a1f40:0x4a1f44])[0] == 0x385fd309
buf[0x4a1f40:0x4a1f64] = coord_unpack_bin

# Clamping and flip selection in _chip_poll (replaces 0x4a1f64:0x4a1f88)
assert len(clamp_bin) == 36
buf[0x4a1f64:0x4a1f88] = clamp_bin

# Division reciprocal constants:
# X divisor 17279 constant: 0x0003caf5bb94cb37
assert struct.unpack("<I", buf[0x4a1fb0:0x4a1fb4])[0] == 0xd2893caa
buf[0x4a1fb0:0x4a1fb4] = struct.pack("<I", 0xd29966ea) # mov x10, #0xcb37
assert struct.unpack("<I", buf[0x4a1fb4:0x4a1fb8])[0] == 0xf2bf73aa
buf[0x4a1fb4:0x4a1fb8] = struct.pack("<I", 0xf2b7728a) # movk x10, #0xbb94, lsl #16
assert struct.unpack("<I", buf[0x4a1fb8:0x4a1fbc])[0] == 0xf2c232ca
buf[0x4a1fb8:0x4a1fbc] = struct.pack("<I", 0xf2d95eaa) # movk x10, #0xcaf5, lsl #32
assert struct.unpack("<I", buf[0x4a1fbc:0x4a1fc0])[0] == 0xf2e000ca
buf[0x4a1fbc:0x4a1fc0] = struct.pack("<I", 0xf2e0006a) # movk x10, #0x3, lsl #48

# Y divisor 38399 constant: 0x0001b4eb04fae9c7
assert struct.unpack("<I", buf[0x4a1fcc:0x4a1fd0])[0] == 0xd299d669
buf[0x4a1fcc:0x4a1fd0] = struct.pack("<I", 0xd29d38e9) # mov x9, #0xe9c7
assert struct.unpack("<I", buf[0x4a1fd0:0x4a1fd4])[0] == 0xf2adae29
buf[0x4a1fd0:0x4a1fd4] = struct.pack("<I", 0xf2a09f49) # movk x9, #0x4fa, lsl #16
assert struct.unpack("<I", buf[0x4a1fd4:0x4a1fd8])[0] == 0xf2d76289
buf[0x4a1fd4:0x4a1fd8] = struct.pack("<I", 0xf2d69d69) # movk x9, #0xb4eb, lsl #32
assert struct.unpack("<I", buf[0x4a1fd8:0x4a1fdc])[0] == 0xf2e00049
buf[0x4a1fd8:0x4a1fdc] = struct.pack("<I", 0xf2e00029) # movk x9, #0x1, lsl #48

# 9. SPMI v7 Buttons patches
buf[0x4a3ed0:0x4a3ed4] = struct.pack("<I", 0x52a00021) # mov w1, #0x10000
buf[0x4a3ee8:0x4a3eec] = struct.pack("<I", 0x52880001) # mov w1, #0x4000
buf[0x4a3f3c:0x4a3f40] = struct.pack("<I", 0x14000007) # b 0xfffffff0074a7f58
buf[0x4a3f60:0x4a3f70] = struct.pack("<IIII", 0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f)
buf[0x4a47cc:0x4a47d0] = struct.pack("<I", 0x52840018) # mov w24, #0x2000
buf[0x4a482c:0x4a4830] = struct.pack("<I", 0xd1400908) # sub x8, x8, #2, lsl #12
buf[0x4a4a08:0x4a4a0c] = struct.pack("<I", 0x8b091518) # add x24, x8, x9, lsl #5

# 10. Flashlight (Torch) for Parrot PM6150L
# flash.id in struct peripheral flash: 0x2ee -> 0x5d3 (SPMI slave 5, peripheral 0xd3)
assert struct.unpack("<H", buf[0xb54190:0xb54192])[0] == 0x02ee
buf[0xb54190:0xb54192] = struct.pack("<H", 0x05d3)

# alarm.id in struct peripheral alarm: 0x20a -> 0x10a (SPMI slave 1, peripheral 0x0a)
assert struct.unpack("<H", buf[0xb54178:0xb5417a])[0] == 0x020a
buf[0xb54178:0xb5417a] = struct.pack("<H", 0x010a)

# _put safety check: cmp w11, #0x2ee -> cmp w11, #0x5d3
assert struct.unpack("<I", buf[0x4c7fc4:0x4c7fc8])[0] == 0x710bb97f
buf[0x4c7fc4:0x4c7fc8] = struct.pack("<I", 0x71174d7f)

# _put arbiter channel check: sub w10, w10, #0x2e000 -> sub w10, w10, #0x5d, lsl #12
assert struct.unpack("<I", buf[0x4c7fe8:0x4c7fec])[0] == 0x5140b94a
buf[0x4c7fe8:0x4c7fec] = struct.pack("<I", 0x5141754a)

# _put arbiter channel check: cmp w10, #0xe00 -> cmp w10, #0x300
assert struct.unpack("<I", buf[0x4c7fec:0x4c7ff0])[0] == 0x7138015f
buf[0x4c7fec:0x4c7ff0] = struct.pack("<I", 0x710c015f)

# SPMI v7 Arbiter physical mappings & offsets for Parrot:
# 1. CORE_PHYS: 0x0c440000 -> 0x0c400000 (0xfffffff0074ca62c)
assert struct.unpack("<I", buf[0x4c662c:0x4c6630])[0] == 0x52a18880
buf[0x4c662c:0x4c6630] = struct.pack("<I", 0x52a18800)
# CORE_SIZE: 0x1100 -> 0x10000 (0xfffffff0074ca630)
assert struct.unpack("<I", buf[0x4c6630:0x4c6634])[0] == 0x52822001
buf[0x4c6630:0x4c6634] = struct.pack("<I", 0x52a00021)

# 2. CNFG_PHYS: 0x0c40a000 -> 0x0c42d000 (0xfffffff0074ca640)
assert struct.unpack("<I", buf[0x4c6640:0x4c6644])[0] == 0x52940000
assert struct.unpack("<I", buf[0x4c6644:0x4c6648])[0] == 0x72a18800
buf[0x4c6640:0x4c6644] = struct.pack("<I", 0x529a0000) # mov w0, #0xd000
buf[0x4c6644:0x4c6648] = struct.pack("<I", 0x72a18840) # movk w0, #0xc42, lsl #16
# CNFG_SIZE: 0x1000 -> 0x4000 (0xfffffff0074ca648)
assert struct.unpack("<I", buf[0x4c6648:0x4c664c])[0] == 0x52820001
buf[0x4c6648:0x4c664c] = struct.pack("<I", 0x52880001)

# 3. OBSERVER_PHYS: 0x0e600000 -> 0x0c440000 (0xfffffff0074ca658)
assert struct.unpack("<I", buf[0x4c6658:0x4c665c])[0] == 0x52a1cc00
buf[0x4c6658:0x4c665c] = struct.pack("<I", 0x52a18880)

# 4. Version check: b.hi -> b (0xfffffff0074ca688, branch directly over "not version 5" error)
assert struct.unpack("<I", buf[0x4c6688:0x4c668c])[0] == 0x540000a8
buf[0x4c6688:0x4c668c] = struct.pack("<I", 0x14000005)

# 5. Flash subtype check bypass: b.ne -> nop (0xfffffff0074ca824, accept PM6150L subtype)
assert struct.unpack("<I", buf[0x4c6824:0x4c6828])[0] == 0x54fff7e1
buf[0x4c6824:0x4c6828] = struct.pack("<I", 0xd503201f)

# 6. CHNLS write channel base: 0x0c600000 -> 0x0c500000 (0xfffffff0074ca84c)
assert struct.unpack("<I", buf[0x4c684c:0x4c6850])[0] == 0x52a18c09
buf[0x4c684c:0x4c6850] = struct.pack("<I", 0x52a18a09)

# 7. CHNLS write channel stride: lsl #16 -> lsl #12 (4KB per channel in SPMI v7, 0xfffffff0074ca850)
assert struct.unpack("<I", buf[0x4c6850:0x4c6854])[0] == 0x8b084120
buf[0x4c6850:0x4c6854] = struct.pack("<I", 0x8b083120)

# 8. spmi_read observer stride: lsl #7 -> lsl #5 (32 bytes per channel in SPMI v7, 0xfffffff0074caf04)
assert struct.unpack("<I", buf[0x4c6f04:0x4c6f08])[0] == 0x8b171e73
buf[0x4c6f04:0x4c6f08] = struct.pack("<I", 0x8b171673)

# 9. _find_channel channels table base: 0x900 -> 0x2000 (0xfffffff0074cb108)
assert struct.unpack("<I", buf[0x4c7108:0x4c710c])[0] == 0x52812018
buf[0x4c7108:0x4c710c] = struct.pack("<I", 0x52840018)

# 10. _find_channel owner offset: sub x8, x8, #0x200 -> sub x8, x8, #2, lsl #12 (0xfffffff0074cb174)
assert struct.unpack("<I", buf[0x4c7174:0x4c7178])[0] == 0xd1080108
buf[0x4c7174:0x4c7178] = struct.pack("<I", 0xd1400908)

# 11. _put arbiter channel check: ldr w10, [x10, #0x900] -> ldr w10, [x10, #0x2000] (0xfffffff0074cbfe0)
assert struct.unpack("<I", buf[0x4c7fe0:0x4c7fe4])[0] == 0xb949014a
buf[0x4c7fe0:0x4c7fe4] = struct.pack("<I", 0xb960014a)

# Write directly to hakios-kernel (and parrot copy)
with open("/home/uralredux/Документы/project/hakios/hakios-kernel", "wb") as f:
    f.write(buf)

with open("/home/uralredux/Документы/project/hakios/hakios-kernel.parrot", "wb") as f:
    f.write(buf)

print("Kernel build successful! Patched hakios-kernel with RPMh ldoe7 power-on.")
