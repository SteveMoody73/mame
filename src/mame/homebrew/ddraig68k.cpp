/*
 * Y Ddraig 68000 motherboard.
 * Derived from the rosco_m68k driver, Copyright (c) 2024 The rosco_m68k
 * Open Source Project (MIT License), with portions (c) Chris Hanson
 * (BSD 3-clause License). Original notices are retained here.
 *
 * Current board: 16 MHz MC68000, 1 MiB SRAM, 8 MiB DRAM, 512 KiB ROM,
 * XR68C681 DUART, MC68230 PI/T, VT82C42-compatible keyboard/mouse and
 * Epson RTC72421. Optional original YM2151, GfxVGA (ddraig68k_vga.cpp) and
 * Mega Drive joypad cards on the expansion bus.
 */

#include "emu.h"

#include "ddraig68k_exp.h"
#include "ddraig68k_vga.h"

#include "bus/ata/ataintf.h"
#include "bus/rs232/rs232.h"
#include "cpu/m68000/m68000.h"
#include "machine/68230pit.h"
#include "machine/8042kbdc.h"
#include "machine/mc68681.h"
#include "machine/msm6242.h"
#include "machine/pckeybrd.h"
#include "sound/ymopm.h"
#include "speaker.h"

ddraig_slot_device::ddraig_slot_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: device_t(mconfig, DDRAIG_SLOT, tag, owner, clock)
	, device_single_card_slot_interface<ddraig_card_interface>(mconfig, *this)
	, m_irq(*this) { }

DEFINE_DEVICE_TYPE(DDRAIG_SLOT, ddraig_slot_device, "ddraig_slot", "Y Ddraig expansion slot")

class ddraig_ym2151_device;
DECLARE_DEVICE_TYPE(DDRAIG_YM2151, ddraig_ym2151_device)

class ddraig_ym2151_device : public device_t, public ddraig_card_interface
{
public:
	ddraig_ym2151_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
		: device_t(mconfig, DDRAIG_YM2151, tag, owner, clock)
		, ddraig_card_interface(mconfig, *this)
		, m_ym(*this, "ym") { }

	virtual u16 reg_r(offs_t offset, u16 mem_mask) override
	{
		if (!(mem_mask & 0xff00)) return 0xffff;
		if (offset < 2) return (u16(m_ym->read(offset)) << 8) | 0x00ff;
		if (offset == 0x7f) return 0x2121; // CPLD drives both lanes when UDS is active.
		return 0xffff;
	}
	virtual void reg_w(offs_t offset, u16 data, u16 mem_mask) override
	{
		if (offset < 2 && (mem_mask & 0xff00)) m_ym->write(offset, data >> 8);
		// The clock latch needs BOTH UDS and LDS: byte writes do nothing.
		if (offset == 0x40 && mem_mask == 0xffff)
		{
			m_clock_word = data;
			update_clock();
		}
	}

protected:
	virtual void device_add_mconfig(machine_config &config) override
	{
		SPEAKER(config, "lspeaker").front_left();
		SPEAKER(config, "rspeaker").front_right();
		YM2151(config, m_ym, clock_hz(0xbcfc));
		m_ym->irq_handler().set(FUNC(ddraig_ym2151_device::irq_w));
		m_ym->add_route(0, "lspeaker", 0.5);
		m_ym->add_route(1, "rspeaker", 0.5);
	}
	virtual void device_start() override { save_item(NAME(m_clock_word)); }
	virtual void device_reset() override { m_clock_word = 0xbcfc; update_clock(); }

private:
	required_device<ym2151_device> m_ym;
	u16 m_clock_word = 0xbcfc;
	static u32 clock_hz(u16 word)
	{
		// LTC6903: f = 2^OCT * 2078 / (2 - DAC/1024).
		// The original card CPLD forces the two output-enable bits to 00.
		return u32((double(2078U << (word >> 12)) * 1024.0 / (2048 - ((word >> 2) & 0x3ff))) + 0.5);
	}
	void update_clock() { m_ym->set_unscaled_clock(clock_hz(m_clock_word)); }
};

DEFINE_DEVICE_TYPE(DDRAIG_YM2151, ddraig_ym2151_device, "ddraig_ym2151", "Y Ddraig original YM2151 card")

// Two 3-button Mega Drive pads behind the JoypadController CPLD. The CPLD
// drives SELECT (TH) per port from D8 of an upper-byte write, and the pad
// pins come back on the upper byte of a read: D8..D13 = pins 1,2,3,4,6,9.
class ddraig_joypad_device;
DECLARE_DEVICE_TYPE(DDRAIG_JOYPAD, ddraig_joypad_device)

class ddraig_joypad_device : public device_t, public ddraig_card_interface
{
public:
	ddraig_joypad_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
		: device_t(mconfig, DDRAIG_JOYPAD, tag, owner, clock)
		, ddraig_card_interface(mconfig, *this)
		, m_pads(*this, "PAD%u", 1U) { }

	virtual u16 reg_r(offs_t offset, u16 mem_mask) override
	{
		if (offset >= 0x7e) return 0x4242;
		if (offset < 2) return (u16(pad_pins(offset)) << 8) | 0x00ff;
		return 0xffff;
	}
	virtual void reg_w(offs_t offset, u16 data, u16 mem_mask) override
	{
		if (offset < 2 && (mem_mask & 0xff00)) m_select[offset] = BIT(data, 8);
	}

protected:
	virtual ioport_constructor device_input_ports() const override;
	virtual void device_start() override { save_item(NAME(m_select)); }
	virtual void device_reset() override { m_select[0] = m_select[1] = 0; }

private:
	required_ioport_array<2> m_pads;
	u8 m_select[2] = { 0, 0 };

	u8 pad_pins(unsigned port)
	{
		// Port bits (active high): 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 C, 7 start
		u8 const in = m_pads[port]->read();
		u8 pins;
		if (m_select[port])
			pins = (in & 0x0f) | (BIT(in, 5) << 4) | (BIT(in, 6) << 5);
		else
			pins = (in & 0x03) | 0x0c | (BIT(in, 4) << 4) | (BIT(in, 7) << 5);
		return (~pins & 0x3f) | (m_select[port] << 6) | 0x80;
	}
};

static INPUT_PORTS_START(ddraig_joypad)
	PORT_START("PAD1")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_JOYSTICK_UP) PORT_8WAY PORT_PLAYER(1)
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_JOYSTICK_DOWN) PORT_8WAY PORT_PLAYER(1)
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_JOYSTICK_LEFT) PORT_8WAY PORT_PLAYER(1)
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_JOYSTICK_RIGHT) PORT_8WAY PORT_PLAYER(1)
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_BUTTON1) PORT_NAME("P1 A") PORT_PLAYER(1)
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_BUTTON2) PORT_NAME("P1 B") PORT_PLAYER(1)
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_BUTTON3) PORT_NAME("P1 C") PORT_PLAYER(1)
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_START1)

	PORT_START("PAD2")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_JOYSTICK_UP) PORT_8WAY PORT_PLAYER(2)
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_JOYSTICK_DOWN) PORT_8WAY PORT_PLAYER(2)
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_JOYSTICK_LEFT) PORT_8WAY PORT_PLAYER(2)
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_JOYSTICK_RIGHT) PORT_8WAY PORT_PLAYER(2)
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_BUTTON1) PORT_NAME("P2 A") PORT_PLAYER(2)
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_BUTTON2) PORT_NAME("P2 B") PORT_PLAYER(2)
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_BUTTON3) PORT_NAME("P2 C") PORT_PLAYER(2)
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_START2)
INPUT_PORTS_END

ioport_constructor ddraig_joypad_device::device_input_ports() const
{
	return INPUT_PORTS_NAME(ddraig_joypad);
}

DEFINE_DEVICE_TYPE(DDRAIG_JOYPAD, ddraig_joypad_device, "ddraig_joypad", "Y Ddraig Mega Drive joypad card")

namespace {

class ddraig68k_state : public driver_device
{
public:
	ddraig68k_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_maincpu(*this, "maincpu")
		, m_duart(*this, "duart")
		, m_pit(*this, "pit")
		, m_kbdc(*this, "kbdc")
		, m_rtc(*this, "rtc")
		, m_ata(*this, "ata")
		, m_terminal(*this, "terminal")
		, m_host(*this, "host")
		, m_slots(*this, "slot%u", 1U)
		, m_rom(*this, "monitor")
		, m_bootrom(*this, "bootrom")
	{ }

	void ddraig68k(machine_config &config);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;

private:
	required_device<m68000_device> m_maincpu;
	required_device<xr68c681_device> m_duart;
	required_device<pit68230_device> m_pit;
	required_device<kbdc8042_device> m_kbdc;
	required_device<rtc72421_device> m_rtc;
	required_device<ata_interface_device> m_ata;
	required_device<rs232_port_device> m_terminal;
	required_device<rs232_port_device> m_host;
	required_device_array<ddraig_slot_device, 4> m_slots;
	required_region_ptr<u16> m_rom;
	memory_view m_bootrom;
	u8 m_boot_reads = 0;
	bool m_keyboard_irq = false;
	bool m_mouse_irq = false;

	void mem_map(address_map &map);
	void cpu_space_map(address_map &map);
	u16 bootrom_r(offs_t offset);
	u16 ata_cs0_r(offs_t offset, u16 mem_mask = ~0);
	void ata_cs0_w(offs_t offset, u16 data, u16 mem_mask = ~0);
	u16 ata_cs1_r(offs_t offset, u16 mem_mask = ~0);
	void ata_cs1_w(offs_t offset, u16 data, u16 mem_mask = ~0);
	void keyboard_irq_w(int state);
	void mouse_irq_w(int state);
	void update_keyboard_irq();
	template<unsigned Slot> u16 exp_data_r(offs_t offset, u16 mem_mask = ~0)
	{
		return m_slots[Slot]->present() ? m_slots[Slot]->data_r(offset, mem_mask) : m_maincpu->berr_r();
	}
	template<unsigned Slot> void exp_data_w(offs_t offset, u16 data, u16 mem_mask = ~0)
	{
		if (m_slots[Slot]->present()) m_slots[Slot]->data_w(offset, data, mem_mask);
		else m_maincpu->berr_w(data);
	}
};

static INPUT_PORTS_START(ddraig68k)
INPUT_PORTS_END

static DEVICE_INPUT_DEFAULTS_START(terminal)
	DEVICE_INPUT_DEFAULTS("RS232_RXBAUD", 0xff, RS232_BAUD_38400)
	DEVICE_INPUT_DEFAULTS("RS232_TXBAUD", 0xff, RS232_BAUD_38400)
	DEVICE_INPUT_DEFAULTS("RS232_DATABITS", 0xff, RS232_DATABITS_8)
	DEVICE_INPUT_DEFAULTS("RS232_PARITY", 0xff, RS232_PARITY_NONE)
	DEVICE_INPUT_DEFAULTS("RS232_STOPBITS", 0xff, RS232_STOPBITS_1)
DEVICE_INPUT_DEFAULTS_END

void ddraig68k_state::mem_map(address_map &map)
{
	map.unmap_value_high();
	// The board timeout generates BERR for undecoded addresses/data windows.
	map(0x000000, 0xffffff).rw(m_maincpu, FUNC(m68000_device::berr_r), FUNC(m68000_device::berr_w));
	map(0x000000, 0x0fffff).ram();
	map(0x100000, 0x8fffff).ram();
	map(0xf80000, 0xffffff).rom().region("monitor", 0);

	map(0xf7f000, 0xf7f01f).mirror(0x0000e0).rw(m_duart, FUNC(xr68c681_device::read), FUNC(xr68c681_device::write)).umask16(0xff00);
	map(0xf7f100, 0xf7f13f).mirror(0x0000c0).rw(m_pit, FUNC(pit68230_device::read), FUNC(pit68230_device::write)).umask16(0xff00);
	map(0xf7f200, 0xf7f201).mirror(0x0000fc).rw(m_kbdc, FUNC(kbdc8042_device::port60_r), FUNC(kbdc8042_device::port60_w)).umask16(0xff00);
	map(0xf7f202, 0xf7f203).mirror(0x0000fc).rw(m_kbdc, FUNC(kbdc8042_device::port64_r), FUNC(kbdc8042_device::port64_w)).umask16(0xff00);
	map(0xf7f300, 0xf7f30f).mirror(0x000070).rw(FUNC(ddraig68k_state::ata_cs0_r), FUNC(ddraig68k_state::ata_cs0_w));
	map(0xf7f380, 0xf7f38f).mirror(0x000070).rw(FUNC(ddraig68k_state::ata_cs1_r), FUNC(ddraig68k_state::ata_cs1_w));
	map(0xf7f400, 0xf7f41f).mirror(0x0000e0).rw(m_rtc, FUNC(rtc72421_device::read), FUNC(rtc72421_device::write)).umask16(0xff00);
	// Register apertures get a fallback DTACK even with no card fitted.
	for (unsigned slot = 0; slot < 4; ++slot)
		map(0xf7f500 + slot * 0x100, 0xf7f5ff + slot * 0x100).rw(m_slots[slot], FUNC(ddraig_slot_device::reg_r), FUNC(ddraig_slot_device::reg_w));
	map(0xa00000, 0xafffff).rw(FUNC(ddraig68k_state::exp_data_r<0>), FUNC(ddraig68k_state::exp_data_w<0>));
	map(0xb00000, 0xbfffff).rw(FUNC(ddraig68k_state::exp_data_r<1>), FUNC(ddraig68k_state::exp_data_w<1>));
	map(0xc00000, 0xcfffff).rw(FUNC(ddraig68k_state::exp_data_r<2>), FUNC(ddraig68k_state::exp_data_w<2>));
	map(0xd00000, 0xdfffff).rw(FUNC(ddraig68k_state::exp_data_r<3>), FUNC(ddraig68k_state::exp_data_w<3>));

	// U401 is a 74HC164: /AS clocks four ones through Q3 after reset.
	// ROM is selected globally for the four initial 16-bit vector fetches.
	map(0x000000, 0xffffff).view(m_bootrom);
	m_bootrom[0](0x000000, 0xffffff).r(FUNC(ddraig68k_state::bootrom_r));
}

void ddraig68k_state::cpu_space_map(address_map &map)
{
	// The CPLD asserts VPA on every IACK; DUART/PIT IVRs are not used.
	map(0xfffff0, 0xffffff).m(m_maincpu, FUNC(m68000_device::autovectors_map));
}

u16 ddraig68k_state::bootrom_r(offs_t offset)
{
	u16 const result = m_rom[offset & 0x3ffff];
	if (!machine().side_effects_disabled() && ++m_boot_reads == 4)
		m_bootrom.disable();
	return result;
}

u16 ddraig68k_state::ata_cs0_r(offs_t offset, u16 mem_mask)
{
	// ATA D0..7 connect to CPU D8..15 and ATA D8..15 to CPU D0..7.
	u16 const ata_mask = swapendian_int16(mem_mask);
	return swapendian_int16(m_ata->cs0_r(offset, ata_mask));
}

void ddraig68k_state::ata_cs0_w(offs_t offset, u16 data, u16 mem_mask)
{
	m_ata->cs0_w(offset, swapendian_int16(data), swapendian_int16(mem_mask));
}

u16 ddraig68k_state::ata_cs1_r(offs_t offset, u16 mem_mask)
{
	return swapendian_int16(m_ata->cs1_r(offset, swapendian_int16(mem_mask)));
}

void ddraig68k_state::ata_cs1_w(offs_t offset, u16 data, u16 mem_mask)
{
	m_ata->cs1_w(offset, swapendian_int16(data), swapendian_int16(mem_mask));
}

void ddraig68k_state::keyboard_irq_w(int state)
{
	m_keyboard_irq = bool(state);
	update_keyboard_irq();
}

void ddraig68k_state::mouse_irq_w(int state)
{
	m_mouse_irq = bool(state);
	update_keyboard_irq();
}

void ddraig68k_state::update_keyboard_irq()
{
	m_maincpu->set_input_line(M68K_IRQ_5, (m_keyboard_irq || m_mouse_irq) ? ASSERT_LINE : CLEAR_LINE);
}

void ddraig68k_state::machine_start()
{
	save_item(NAME(m_boot_reads));
	save_item(NAME(m_keyboard_irq));
	save_item(NAME(m_mouse_irq));
}

void ddraig68k_state::machine_reset()
{
	m_boot_reads = 0;
	m_bootrom.select(0);
	m_keyboard_irq = false;
	m_mouse_irq = false;
	update_keyboard_irq();
	// IP2..IP5 are active-low expansion-present inputs.
	m_duart->ip2_w(!m_slots[0]->present());
	m_duart->ip3_w(!m_slots[1]->present());
	m_duart->ip4_w(!m_slots[2]->present());
	m_duart->ip5_w(!m_slots[3]->present());
}

void ddraig68k_state::ddraig68k(machine_config &config)
{
	M68000(config, m_maincpu, 16_MHz_XTAL);
	m_maincpu->set_addrmap(AS_PROGRAM, &ddraig68k_state::mem_map);
	m_maincpu->set_addrmap(m68000_base_device::AS_CPU_SPACE, &ddraig68k_state::cpu_space_map);

	XR68C681(config, m_duart, 3.6864_MHz_XTAL);
	m_duart->irq_cb().set_inputline(m_maincpu, M68K_IRQ_4);
	m_duart->a_tx_cb().set(m_terminal, FUNC(rs232_port_device::write_txd));
	m_duart->b_tx_cb().set(m_host, FUNC(rs232_port_device::write_txd));
	m_duart->outport_cb().set(m_terminal, FUNC(rs232_port_device::write_rts)).bit(0);
	m_duart->outport_cb().append(m_host, FUNC(rs232_port_device::write_rts)).bit(1);

	RS232_PORT(config, m_terminal, default_rs232_devices, "terminal");
	m_terminal->rxd_handler().set(m_duart, FUNC(xr68c681_device::rx_a_w));
	m_terminal->cts_handler().set(m_duart, FUNC(xr68c681_device::ip0_w));
	m_terminal->set_option_device_input_defaults("terminal", DEVICE_INPUT_DEFAULTS_NAME(terminal));
	RS232_PORT(config, m_host, default_rs232_devices, nullptr);
	m_host->rxd_handler().set(m_duart, FUNC(xr68c681_device::rx_b_w));
	m_host->cts_handler().set(m_duart, FUNC(xr68c681_device::ip1_w));

	PIT68230(config, m_pit, 8_MHz_XTAL);
	m_pit->timer_irq_callback().set_inputline(m_maincpu, M68K_IRQ_3);

	KBDC8042(config, m_kbdc);
	// Start with no mouse attached. The generic PS/2 model overwrites an
	// unread mouse reset reply during this OS's probe, blocking keyboard RX.
	// Its keyboard-only mode avoids advertising an unimplemented mouse path.
	m_kbdc->set_keyboard_type(kbdc8042_device::KBDC8042_STANDARD);
	m_kbdc->set_interrupt_type(kbdc8042_device::KBDC8042_DOUBLE);
	m_kbdc->set_keyboard_tag("keyboard");
	m_kbdc->input_buffer_full_callback().set(FUNC(ddraig68k_state::keyboard_irq_w));
	m_kbdc->input_buffer_full_mouse_callback().set(FUNC(ddraig68k_state::mouse_irq_w));
	at_keyboard_device &keyboard(AT_KEYB(config, "keyboard", pc_keyboard_device::KEYBOARD_TYPE::MF2, 2));
	keyboard.keypress().set(m_kbdc, FUNC(kbdc8042_device::keyboard_w));

	RTC72421(config, m_rtc, 32.768_kHz_XTAL);
	ATA_INTERFACE(config, m_ata, 0).options(ata_devices, "hdd", nullptr, false);
	m_ata->irq_handler().set_inputline(m_maincpu, M68K_IRQ_2);

	for (unsigned slot = 0; slot < 4; ++slot)
	{
		DDRAIG_SLOT(config, m_slots[slot], 0);
		m_slots[slot]->option_add("ym2151", DDRAIG_YM2151);
		m_slots[slot]->option_add("gfxvga", DDRAIG_GFXVGA);
		m_slots[slot]->option_add("joypad", DDRAIG_JOYPAD);
		m_slots[slot]->set_default_option(nullptr);
		m_slots[slot]->set_fixed(false);
	}
	m_slots[0]->irq_handler().set_inputline(m_maincpu, M68K_IRQ_1);
	m_slots[1]->irq_handler().set_inputline(m_maincpu, M68K_IRQ_6);
	m_slots[2]->irq_handler().set_inputline(m_maincpu, M68K_IRQ_7);
	// Slot 4 has no dedicated interrupt input on this motherboard.
}

ROM_START(ddraig68k)
	ROM_REGION16_BE(0x080000, "monitor", ROMREGION_ERASEFF)
	// A full, interleaved flash image, generated from an unchanged OS binary.
	ROM_LOAD("ddraig68k.rom", 0x000000, 0x080000, CRC(2fcd393e) SHA1(8f8229d1e97f039e05e36d7cb72da7b6b57d8b18))
ROM_END

} // anonymous namespace

COMP(2025, ddraig68k, 0, 0, ddraig68k, ddraig68k, ddraig68k_state, empty_init, "Y Ddraig", "Y Ddraig 68000", 0)
