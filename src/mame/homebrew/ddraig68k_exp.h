/*
 * Y Ddraig 68000 expansion slot.
 *
 * The motherboard provides separate register/data selects to each slot.
 * Keep card decoding here so all cards use the same presence and IRQ path.
 */
#ifndef MAME_HOMEBREW_DDRAIG68K_EXP_H
#define MAME_HOMEBREW_DDRAIG68K_EXP_H

#pragma once

class ddraig_card_interface : public device_interface
{
public:
	// Register window: 256 bytes, offset in words. Data window: 1 MiB, offset in words.
	virtual u16 reg_r(offs_t offset, u16 mem_mask) { return 0xffff; }
	virtual void reg_w(offs_t offset, u16 data, u16 mem_mask) { }
	virtual u16 data_r(offs_t offset, u16 mem_mask) { return 0xffff; }
	virtual void data_w(offs_t offset, u16 data, u16 mem_mask) { }

protected:
	ddraig_card_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device, "ddraig_expansion") { }
	void irq_w(int state);
};

class ddraig_slot_device : public device_t, public device_single_card_slot_interface<ddraig_card_interface>
{
public:
	ddraig_slot_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);
	auto irq_handler() { return m_irq.bind(); }
	void irq_w(int state) { m_irq(state); }
	bool present() const { return m_card != nullptr; }
	u16 reg_r(offs_t offset, u16 mem_mask = ~0) { return m_card ? m_card->reg_r(offset, mem_mask) : 0xffff; }
	void reg_w(offs_t offset, u16 data, u16 mem_mask = ~0) { if (m_card) m_card->reg_w(offset, data, mem_mask); }
	u16 data_r(offs_t offset, u16 mem_mask) { return m_card->data_r(offset, mem_mask); }
	void data_w(offs_t offset, u16 data, u16 mem_mask) { m_card->data_w(offset, data, mem_mask); }

protected:
	virtual void device_start() override { m_card = get_card_device(); }

private:
	devcb_write_line m_irq;
	ddraig_card_interface *m_card = nullptr;
};

DECLARE_DEVICE_TYPE(DDRAIG_SLOT, ddraig_slot_device)

inline void ddraig_card_interface::irq_w(int state)
{
	downcast<ddraig_slot_device &>(*device().owner()).irq_w(state);
}

#endif // MAME_HOMEBREW_DDRAIG68K_EXP_H
