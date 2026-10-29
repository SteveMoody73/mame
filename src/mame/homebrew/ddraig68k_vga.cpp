/*
 * Y Ddraig GfxVGA expansion card.
 *
 * Functional model of the Spartan-6 VDP in D:\Dev\ddraig\FPGA\GfxVGA
 * (BOARD_VER 2.1, BOARD_ID 0x13). The register behaviour follows the RTL
 * (RegSystem, RegPalette, RegTilemap, RegSprites, ...), not the older docs.
 *
 * - 2 MiB SRAM (1M words). The CPU sees one 1 MiB page in the slot's data
 *   window, selected by RAMPAGE bit 0. All display pointers are 20-bit word
 *   addresses.
 * - 1024-entry RGB565 palette. Text uses bank*128 + 4-bit colour, bitmap
 *   bank*256 + index, tiles bank*128 + (tile palette*16 + pixel), sprites
 *   bank*256 + (sprite palette*16 + pixel).
 * - Text mode: 8x16 or 8x8 glyphs from font RAM, cells in SRAM at TEXT_BASE.
 * - Bitmap modes: RGB565, 8/4/2/1 bpp and Atari ST planar 4 bpp.
 * - Tilemap mode: 320x240 logical, pixel-doubled to 640x480. Four layers,
 *   8x8 or 16x16 4 bpp tiles, 32..256-tile maps, line and column scroll.
 * - 256 sprites, 16x16 4 bpp, 320x240 logical, 48 per line by default.
 *   The selector scans descriptors from index 0 upwards and the renderer only
 *   overwrites a line-buffer pixel with a strictly lower priority value, so
 *   for equal priority the LOWER sprite index is drawn on top. Against tiles,
 *   a sprite wins when its priority is <= the best tile layer's priority.
 * - 32x32 2 bpp hardware cursor at full output resolution.
 * - Vblank and line interrupts, ORed onto the slot IRQ.
 * - Drawing engine as synthesised (G_LINE = G_TRIANGLE = false, G_PATTERN =
 *   true): patterned/clipped rectangle fill, linear and 2D blit and 1 bpp
 *   expand, with replace/XOR/transparent/OR/AND raster ops. Line and
 *   triangle commands are no-ops, as on the card.
 * - DMA engine: word copy, word fill and BA2 delta decode, with the RTL's
 *   error checks.
 *
 * Draw and DMA commands complete instantly, so their status registers always
 * read idle. Not emulated: the copper, register/SRAM access timing and exact
 * mid-line raster timing. Display register writes take effect from the
 * current scanline.
 */

#include "emu.h"
#include "ddraig68k_vga.h"

#include "screen.h"

#define LOG_UNHANDLED (1U << 1)
#define VERBOSE (LOG_UNHANDLED)
#include "logmacro.h"

namespace {

class ddraig_gfxvga_device : public device_t, public ddraig_card_interface
{
public:
	ddraig_gfxvga_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	virtual u16 reg_r(offs_t offset, u16 mem_mask) override;
	virtual void reg_w(offs_t offset, u16 data, u16 mem_mask) override;
	virtual u16 data_r(offs_t offset, u16 mem_mask) override;
	virtual void data_w(offs_t offset, u16 data, u16 mem_mask) override;

protected:
	virtual void device_add_mconfig(machine_config &config) override;
	virtual const tiny_rom_entry *device_rom_region() const override;
	virtual void device_start() override;
	virtual void device_reset() override;
	virtual void device_post_load() override;

private:
	static constexpr u32 VRAM_WORDS = 0x100000;
	static constexpr u32 VRAM_MASK = VRAM_WORDS - 1;
	static constexpr unsigned NUM_SPRITES = 256;
	static constexpr int LOGICAL_WIDTH = 320;
	static constexpr int LOGICAL_HEIGHT = 240;

	enum : u16
	{
		MODE_TEXT = 0,
		MODE_BITMAP = 1,
		MODE_BITMAPHIRES = 2,
		MODE_TILEMAP = 3
	};

	struct video_mode
	{
		u32 clock;
		int htotal, hvisible, vtotal, vvisible;
		int text_cols, text_rows16, text_rows8;
	};
	static const video_mode s_modes[4];

	struct tile_layer
	{
		u32 data;       // tile graphics base (word address)
		u32 map;        // tile map base (word address)
		u16 scroll_x;
		u16 scroll_y;
		u16 ctrl;       // tile8, map h/w, transparent index, priority, line scroll, enable
		u16 vctrl;      // layer palette, map mode, column scroll enable
	};

	required_device<screen_device> m_screen;
	required_region_ptr<u8> m_font_rom;
	required_region_ptr<u16> m_pattern_rom;

	std::unique_ptr<u16[]> m_vram;
	std::unique_ptr<rgb_t[]> m_rgb565;
	u16 m_palette[1024];
	u8 m_font[0x2000];

	// System and display groups
	u16 m_control;
	u16 m_interrupt;
	u16 m_int_line_y;
	u16 m_int_line_prev;
	u16 m_bg_color;
	u8 m_ram_page;
	u8 m_overflow;
	bool m_int_vblank;
	bool m_int_line;
	u32 m_fb_base;
	u32 m_fb_active;
	u32 m_linescroll_base;
	u32 m_vscroll_base;

	// Text and font groups
	u32 m_text_base;
	u16 m_cursor_pos;
	u16 m_cursor_size;
	u16 m_font_addr;
	u8 m_blink_count;

	// Palette group
	u16 m_pal_idx;
	u16 m_pal_ctrl0;
	u16 m_pal_ctrl1;

	tile_layer m_tile[4];

	// Sprite group
	u64 m_sprite[NUM_SPRITES];
	u64 m_sprite_latch;
	u8 m_sprite_idx;
	u32 m_sprite_base;
	u8 m_max_line_sprites;

	// Hardware cursor group
	u16 m_hwc_ctrl;
	u16 m_hwc_x;
	u16 m_hwc_y;
	u8 m_hwc_addr;
	u16 m_hwc_data[128];
	u16 m_hwc_col[3];

	// Drawing groups
	u32 m_draw_base;
	u16 m_draw_stride;
	u16 m_draw_col0;
	u16 m_draw_col1;
	u16 m_draw_mode;
	u16 m_draw_pattern;
	u16 m_draw_param[7];
	u16 m_pattern_addr;
	u16 m_pattern_ram[1024];
	u16 m_clip[4];  // x0, y0, x1, y1

	// DMA group
	u32 m_dma_src;
	u32 m_dma_dst;
	u16 m_dma_src_words;
	u16 m_dma_dst_words;
	u16 m_dma_fill;
	u16 m_dma_status;

	// Logical-line compositor cache: bit 15 = valid, bits 9:0 = palette index
	u16 m_comp_line[LOGICAL_WIDTH];
	u64 m_comp_frame;
	int m_comp_y;

	int m_res;
	emu_timer *m_line_timer;

	u16 mode() const { return m_control & 7; }
	bool sprites_enabled() const { return BIT(m_control, 11); }
	int effective_res() const;
	void update_video_mode();
	void update_irq() { irq_w((m_int_vblank || m_int_line) ? ASSERT_LINE : CLEAR_LINE); }
	void sync_display() { m_screen->update_partial(m_screen->vpos()); }

	void tile_layer_w(unsigned layer, unsigned reg, u16 data);
	void sprite_w(unsigned reg, u16 data);
	void draw_command(u16 command);
	void draw_word(u32 addr, u16 colour, unsigned op);
	void dma_start(unsigned operation);

	TIMER_CALLBACK_MEMBER(line_tick);
	void vblank_w(int state);

	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void compose_line(int ly);
	void render_tile_layer(unsigned layer, int ly, u8 *line);
	void render_sprites(int ly, u16 *line);
	u8 text_pixel(int x, int y) const;
	bool bitmap_pixel(int x, int y, u16 &value) const;
	rgb_t palette_rgb(u16 index) const { return m_rgb565[m_palette[index & 0x3ff]]; }

	static s32 sext10(u32 value) { return util::sext(value, 10); }
};

// 640x480 is generated from 25.0 MHz on the card (59.5 Hz), the others are VESA.
const ddraig_gfxvga_device::video_mode ddraig_gfxvga_device::s_modes[4] =
{
	{ 25'000'000,  800,  640, 525, 480,  80, 30, 60 },
	{ 40'000'000, 1056,  800, 628, 600, 100, 37, 75 },
	{ 65'000'000, 1344, 1024, 806, 768, 128, 48, 96 },
	{ 25'000'000,  800,  640, 525, 480,  80, 30, 60 }
};

ROM_START(ddraig_gfxvga)
	ROM_REGION(0x2000, "font", 0)
	// DEFAULT_FONT from FontData_pkg.vhd: 8x16 glyphs at 0x0000, 8x8 glyphs at 0x1000
	ROM_LOAD("gfxvga_font.bin", 0x0000, 0x2000, CRC(a95728bc) SHA1(018a4ffeb560878327422a0c176e6c5fe81af9f2))

	ROM_REGION16_BE(0x800, "patterns", 0)
	// pattern_ram initialiser from PatternRAM.vhd: 64 fill patterns of 16 rows
	ROM_LOAD("gfxvga_patterns.bin", 0x0000, 0x0800, CRC(a0486887) SHA1(84d83ca0c967d9e9cd636da66864bc4d7b6650e9))
ROM_END

ddraig_gfxvga_device::ddraig_gfxvga_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: device_t(mconfig, DDRAIG_GFXVGA, tag, owner, clock)
	, ddraig_card_interface(mconfig, *this)
	, m_screen(*this, "screen")
	, m_font_rom(*this, "font")
	, m_pattern_rom(*this, "patterns")
{
}

const tiny_rom_entry *ddraig_gfxvga_device::device_rom_region() const
{
	return ROM_NAME(ddraig_gfxvga);
}

void ddraig_gfxvga_device::device_add_mconfig(machine_config &config)
{
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_raw(s_modes[0].clock, s_modes[0].htotal, 0, s_modes[0].hvisible, s_modes[0].vtotal, 0, s_modes[0].vvisible);
	m_screen->set_screen_update(FUNC(ddraig_gfxvga_device::screen_update));
	m_screen->screen_vblank().set(FUNC(ddraig_gfxvga_device::vblank_w));
}

void ddraig_gfxvga_device::device_start()
{
	m_vram = std::make_unique<u16[]>(VRAM_WORDS);
	m_rgb565 = std::make_unique<rgb_t[]>(0x10000);
	for (unsigned i = 0; i < 0x10000; i++)
		m_rgb565[i] = rgb_t(pal5bit(i >> 11), pal6bit(i >> 5), pal5bit(i));

	std::fill_n(m_palette, std::size(m_palette), 0);
	std::fill_n(m_sprite, std::size(m_sprite), 0);
	std::fill_n(m_hwc_data, std::size(m_hwc_data), 0);
	std::copy_n(&m_font_rom[0], sizeof(m_font), m_font);
	std::copy_n(&m_pattern_rom[0], std::size(m_pattern_ram), m_pattern_ram);

	m_line_timer = timer_alloc(FUNC(ddraig_gfxvga_device::line_tick), this);
	m_res = 0;
	m_comp_frame = ~u64(0);
	m_comp_y = -1;

	save_pointer(NAME(m_vram), VRAM_WORDS);
	save_item(NAME(m_palette));
	save_item(NAME(m_font));
	save_item(NAME(m_control));
	save_item(NAME(m_interrupt));
	save_item(NAME(m_int_line_y));
	save_item(NAME(m_int_line_prev));
	save_item(NAME(m_bg_color));
	save_item(NAME(m_ram_page));
	save_item(NAME(m_overflow));
	save_item(NAME(m_int_vblank));
	save_item(NAME(m_int_line));
	save_item(NAME(m_fb_base));
	save_item(NAME(m_fb_active));
	save_item(NAME(m_linescroll_base));
	save_item(NAME(m_vscroll_base));
	save_item(NAME(m_text_base));
	save_item(NAME(m_cursor_pos));
	save_item(NAME(m_cursor_size));
	save_item(NAME(m_font_addr));
	save_item(NAME(m_blink_count));
	save_item(NAME(m_pal_idx));
	save_item(NAME(m_pal_ctrl0));
	save_item(NAME(m_pal_ctrl1));
	save_item(STRUCT_MEMBER(m_tile, data));
	save_item(STRUCT_MEMBER(m_tile, map));
	save_item(STRUCT_MEMBER(m_tile, scroll_x));
	save_item(STRUCT_MEMBER(m_tile, scroll_y));
	save_item(STRUCT_MEMBER(m_tile, ctrl));
	save_item(STRUCT_MEMBER(m_tile, vctrl));
	save_item(NAME(m_sprite));
	save_item(NAME(m_sprite_latch));
	save_item(NAME(m_sprite_idx));
	save_item(NAME(m_sprite_base));
	save_item(NAME(m_max_line_sprites));
	save_item(NAME(m_hwc_ctrl));
	save_item(NAME(m_hwc_x));
	save_item(NAME(m_hwc_y));
	save_item(NAME(m_hwc_addr));
	save_item(NAME(m_hwc_data));
	save_item(NAME(m_hwc_col));
	save_item(NAME(m_draw_base));
	save_item(NAME(m_draw_stride));
	save_item(NAME(m_draw_col0));
	save_item(NAME(m_draw_col1));
	save_item(NAME(m_draw_mode));
	save_item(NAME(m_draw_pattern));
	save_item(NAME(m_draw_param));
	save_item(NAME(m_pattern_addr));
	save_item(NAME(m_pattern_ram));
	save_item(NAME(m_clip));
	save_item(NAME(m_dma_src));
	save_item(NAME(m_dma_dst));
	save_item(NAME(m_dma_src_words));
	save_item(NAME(m_dma_dst_words));
	save_item(NAME(m_dma_fill));
	save_item(NAME(m_dma_status));
}

void ddraig_gfxvga_device::device_reset()
{
	// Register reset values from the RTL. SRAM, palette, font and sprite RAM
	// are not cleared by reset on the card.
	m_control = 0;
	m_interrupt = 0;
	m_int_line_y = 0;
	m_int_line_prev = 0x3ff;
	m_bg_color = 0;
	m_ram_page = 0;
	m_overflow = 0;
	m_int_vblank = false;
	m_int_line = false;
	m_fb_base = m_fb_active = 0;
	m_linescroll_base = 0;
	m_vscroll_base = 0;
	m_text_base = 0;
	m_cursor_pos = 0;
	m_cursor_size = 0x0e0d;
	m_font_addr = 0;
	m_blink_count = 0;
	m_pal_idx = 0;
	m_pal_ctrl0 = 0;
	m_pal_ctrl1 = 0;

	// Layer generics: 1-2 are 16x16 tiles on 32x32 maps, 3-4 8x8 tiles on 64x64 maps
	static const u8 priority[4] = { 11, 8, 5, 2 };
	for (unsigned i = 0; i < 4; i++)
	{
		bool const tile8 = i >= 2;
		u16 const log2 = tile8 ? 1 : 0;
		m_tile[i] = tile_layer{ 0, 0, 0, 0, u16((tile8 ? 0x8000 : 0) | (log2 << 13) | (log2 << 11) | (priority[i] << 3)), 0 };
	}

	m_sprite_latch = 0;
	m_sprite_idx = 0;
	m_sprite_base = 0;
	m_max_line_sprites = 48;

	m_hwc_ctrl = 0;
	m_hwc_x = m_hwc_y = 0;
	m_hwc_addr = 0;
	std::fill_n(m_hwc_col, std::size(m_hwc_col), 0);

	m_draw_base = 0;
	m_draw_stride = 320;
	m_draw_col0 = m_draw_col1 = 0;
	m_draw_mode = 0;
	m_draw_pattern = 0;
	std::fill_n(m_draw_param, std::size(m_draw_param), 0);
	m_pattern_addr = 0;
	m_clip[0] = 0;
	m_clip[1] = 0;
	m_clip[2] = 319;
	m_clip[3] = 239;

	m_dma_src = m_dma_dst = 0;
	m_dma_src_words = m_dma_dst_words = 0;
	m_dma_fill = 0;
	m_dma_status = 0;

	m_comp_frame = ~u64(0);
	update_irq();
	update_video_mode();
}

void ddraig_gfxvga_device::device_post_load()
{
	m_comp_frame = ~u64(0);
	update_video_mode();
}

int ddraig_gfxvga_device::effective_res() const
{
	// Tile and sprite paths are 320x240 logical, so they force 640x480 timing.
	if (mode() == MODE_TILEMAP || sprites_enabled())
		return 0;
	return (m_control >> 6) & 3;
}

void ddraig_gfxvga_device::update_video_mode()
{
	int const res = effective_res();
	if (res != m_res)
	{
		m_res = res;
		video_mode const &m = s_modes[res];
		rectangle const visarea(0, m.hvisible - 1, 0, m.vvisible - 1);
		m_screen->configure(m.htotal, m.vtotal, visarea, HZ_TO_ATTOSECONDS(m.clock) * m.htotal * m.vtotal);
	}
	m_line_timer->adjust(m_screen->time_until_pos(m_screen->vpos(), s_modes[m_res].hvisible));
}

TIMER_CALLBACK_MEMBER(ddraig_gfxvga_device::line_tick)
{
	// Start of hblank on physical line v: the line interrupt is raised here
	// when the NEXT line is the target, as in VDPController's Interrupts process.
	video_mode const &m = s_modes[m_res];
	int const v = m_screen->vpos();
	if (v < m.vvisible && !m_int_line && BIT(m_interrupt, 1))
	{
		u16 next = v + 1;
		if (mode() == MODE_BITMAP || mode() == MODE_TILEMAP)
			next >>= 1;
		if (next == m_int_line_y && m_int_line_prev != next)
		{
			m_int_line_prev = next;
			m_int_line = true;
			update_irq();
		}
	}
	if (v == m.vtotal - 1)
		m_int_line_prev = 0x3ff;

	int const nextv = (v + 1) % m.vtotal;
	m_line_timer->adjust(m_screen->time_until_pos(nextv, m.hvisible));
}

void ddraig_gfxvga_device::vblank_w(int state)
{
	if (!state)
		return;
	m_fb_active = m_fb_base;
	m_blink_count++;
	if (BIT(m_interrupt, 0) && !m_int_vblank)
	{
		m_int_vblank = true;
		update_irq();
	}
}

u16 ddraig_gfxvga_device::data_r(offs_t offset, u16 mem_mask)
{
	return m_vram[(u32(m_ram_page) << 19) | (offset & 0x7ffff)];
}

void ddraig_gfxvga_device::data_w(offs_t offset, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_vram[(u32(m_ram_page) << 19) | (offset & 0x7ffff)]);
}

u16 ddraig_gfxvga_device::reg_r(offs_t offset, u16 mem_mask)
{
	unsigned const group = (offset >> 3) & 0xf;
	unsigned const reg = offset & 7;
	switch (group)
	{
	case 0x0:
		if (reg == 0)
			return (m_screen->vblank() ? 0x8000 : 0) | (m_screen->hblank() ? 0x4000 : 0) | (u16(m_overflow) << 8)
					| (m_int_line ? 0x0004 : 0) | (m_int_vblank ? 0x0002 : 0) | 0x0001;
		if (reg == 1)
			return m_control;
		break;
	case 0x4:
		if (reg == 2)
			return m_pal_ctrl0;
		if (reg == 3)
			return m_pal_ctrl1;
		break;
	case 0x5: case 0x6: case 0x7: case 0x8:
		if (reg == 6)
			return m_tile[group - 5].ctrl;
		if (reg == 7)
			return m_tile[group - 5].vctrl;
		break;
	case 0xa:
		if (reg == 0)
			return m_hwc_ctrl;
		break;
	case 0xb:
		if (reg == 3)
			return 0x0007; // draw engine idle, FIFO has space, FIFO empty
		break;
	case 0xe:
		switch (reg)
		{
		case 0: return m_dma_src & 0xffff;
		case 1: return m_dma_src >> 16;
		case 2: return m_dma_dst & 0xffff;
		case 3: return m_dma_dst >> 16;
		case 4: return m_dma_src_words;
		case 5: return m_dma_dst_words;
		case 6: return m_dma_fill;
		case 7: return m_dma_status;
		}
		break;
	case 0xf:
		if (reg == 6)
			return 0x0201; // BOARD_VER 2.1
		if (reg == 7)
			return 0x1313; // BOARD_TYPE_ID in both bytes
		break;
	}
	return 0xf0f0;
}

void ddraig_gfxvga_device::reg_w(offs_t offset, u16 data, u16 mem_mask)
{
	unsigned const group = (offset >> 3) & 0xf;
	unsigned const reg = offset & 7;

	if (group <= 0xa)
		sync_display();

	switch (group)
	{
	case 0x0: // System
		switch (reg)
		{
		case 0:
			if (BIT(data, 2)) m_int_vblank = false;
			if (BIT(data, 3)) m_int_line = false;
			m_overflow &= ~((data >> 8) & 7);
			update_irq();
			break;
		case 1:
			m_control = data;
			update_video_mode();
			break;
		case 2: m_interrupt = data & 7; break;
		case 3: m_int_line_y = data & 0x3ff; break;
		case 4: m_ram_page = data & 1; break;
		case 5: m_bg_color = data; break;
		default: LOGMASKED(LOG_UNHANDLED, "write to unused register %02x = %04x\n", offset << 1, data); break;
		}
		break;

	case 0x1: // Display pointers
		switch (reg)
		{
		case 0: m_fb_base = (m_fb_base & 0xf0000) | data; break;
		case 1: m_fb_base = (m_fb_base & 0x0ffff) | (u32(data & 0xf) << 16); break;
		case 2: m_linescroll_base = (m_linescroll_base & 0xf0000) | data; break;
		case 3: m_linescroll_base = (m_linescroll_base & 0x0ffff) | (u32(data & 0xf) << 16); break;
		case 4: m_vscroll_base = (m_vscroll_base & 0xf0000) | data; break;
		case 5: m_vscroll_base = (m_vscroll_base & 0x0ffff) | (u32(data & 0xf) << 16); break;
		default: break;
		}
		break;

	case 0x2: // Text (0x20/0x22 are legacy BRAM text registers with no effect)
		switch (reg)
		{
		case 2: m_text_base = (m_text_base & 0xf0000) | data; break;
		case 3: m_text_base = (m_text_base & 0x0ffff) | (u32(data & 0xf) << 16); break;
		case 4: m_cursor_pos = data; break;
		case 5: m_cursor_size = data; break;
		default: break;
		}
		break;

	case 0x3: // Font RAM
		if (reg == 0)
			m_font_addr = data & 0x1fff;
		else if (reg == 1)
		{
			m_font[m_font_addr] = data & 0xff;
			m_font_addr = (m_font_addr + 1) & 0x1fff;
		}
		break;

	case 0x4: // Palette
		switch (reg)
		{
		case 0: m_pal_idx = data & 0x3ff; break;
		case 1:
			m_palette[m_pal_idx] = data;
			m_pal_idx = (m_pal_idx + 1) & 0x3ff;
			break;
		case 2: m_pal_ctrl0 = data & 0x001f; break;
		case 3: m_pal_ctrl1 = data & 0x3fff; break;
		default: break;
		}
		break;

	case 0x5: case 0x6: case 0x7: case 0x8:
		tile_layer_w(group - 5, reg, data);
		break;

	case 0x9:
		sprite_w(reg, data);
		break;

	case 0xa: // Hardware cursor
		switch (reg)
		{
		case 0: m_hwc_ctrl = data & 1; break;
		case 1: m_hwc_x = data & 0x3ff; break;
		case 2: m_hwc_y = data & 0x3ff; break;
		case 3: m_hwc_addr = data & 0x7f; break;
		case 4:
			m_hwc_data[m_hwc_addr] = data;
			m_hwc_addr = (m_hwc_addr + 1) & 0x7f;
			break;
		default: m_hwc_col[reg - 5] = data; break;
		}
		break;

	case 0xb: // Drawing
		switch (reg)
		{
		case 0: m_draw_base = (m_draw_base & 0xf0000) | data; break;
		case 1: m_draw_base = (m_draw_base & 0x0ffff) | (u32(data & 0xf) << 16); break;
		case 2: m_draw_stride = data; break;
		case 4: m_draw_col0 = data; break;
		case 5: m_draw_col1 = data; break;
		case 6: m_draw_mode = data & 7; break;
		case 7: m_draw_pattern = data; break;
		default: break;
		}
		break;

	case 0xc: // Draw parameters and commit
		if (reg < 7)
			m_draw_param[reg] = data;
		else
			draw_command(data);
		break;

	case 0xd: // Pattern RAM and clip window
		switch (reg)
		{
		case 0: m_pattern_addr = data & 0x3ff; break;
		case 1:
			m_pattern_ram[m_pattern_addr] = data;
			m_pattern_addr = (m_pattern_addr + 1) & 0x3ff;
			break;
		case 2: m_clip[0] = data & 0x3ff; break;
		case 3: m_clip[1] = data & 0x1ff; break;
		case 4: m_clip[2] = data & 0x3ff; break;
		case 5: m_clip[3] = data & 0x1ff; break;
		default: break;
		}
		break;

	case 0xe: // DMA
		switch (reg)
		{
		case 0: m_dma_src = (m_dma_src & 0xf0000) | data; break;
		case 1: m_dma_src = (m_dma_src & 0x0ffff) | (u32(data & 0xf) << 16); break;
		case 2: m_dma_dst = (m_dma_dst & 0xf0000) | data; break;
		case 3: m_dma_dst = (m_dma_dst & 0x0ffff) | (u32(data & 0xf) << 16); break;
		case 4: m_dma_src_words = data; break;
		case 5: m_dma_dst_words = data; break;
		case 6: m_dma_fill = data; break;
		case 7:
			if (BIT(data, 14))
				m_dma_status = 0;
			else if (BIT(data, 15))
				dma_start(data & 3);
			break;
		}
		break;

	case 0xf:
		if (reg < 6)
			LOGMASKED(LOG_UNHANDLED, "%s: copper register %02x = %04x not emulated\n", machine().describe_context(), offset << 1, data);
		break;

	default:
		break;
	}
}

void ddraig_gfxvga_device::tile_layer_w(unsigned layer, unsigned reg, u16 data)
{
	tile_layer &t = m_tile[layer];
	switch (reg)
	{
	case 0: t.data = (t.data & 0xf0000) | data; break;
	case 1: t.data = (t.data & 0x0ffff) | (u32(data & 0xf) << 16); break;
	case 2: t.map = (t.map & 0xf0000) | data; break;
	case 3: t.map = (t.map & 0x0ffff) | (u32(data & 0xf) << 16); break;
	case 4: t.scroll_x = data; break;
	case 5: t.scroll_y = data; break;
	case 6: t.ctrl = data; break;
	case 7: t.vctrl = data & 0x007f; break;
	}
}

void ddraig_gfxvga_device::sprite_w(unsigned reg, u16 data)
{
	// Each field write modifies the descriptor latched by the last index
	// write and stores the whole 64-bit descriptor back (RegSprites).
	u64 &d = m_sprite_latch;
	switch (reg)
	{
	case 0: m_sprite_base = (m_sprite_base & 0xf0000) | data; return;
	case 1: m_sprite_base = (m_sprite_base & 0x0ffff) | (u32(data & 0xf) << 16); return;
	case 2:
		m_sprite_idx = data & 0xff;
		m_sprite_latch = m_sprite[m_sprite_idx];
		return;
	case 3: d = (d & ~(u64(0x3ff) << 30)) | (u64(data & 0x3ff) << 30); break;
	case 4: d = (d & ~(u64(0x3ff) << 20)) | (u64(data & 0x3ff) << 20); break;
	case 5:
		d = (d & ~(u64(0x7ff) << 40) & ~(u64(1) << 57)) | (u64(data & 0x7ff) << 40) | (u64(BIT(data, 11)) << 57);
		break;
	case 6:
		d &= ~(u64(0x7f) << 51 | u64(0x1f) << 58);
		d |= u64(BIT(data, 15)) << 63;          // enable
		d |= u64((data >> 6) & 0xf) << 59;      // priority
		d |= u64(BIT(data, 4)) << 56;           // flip X
		d |= u64(BIT(data, 5)) << 55;           // flip Y
		d |= u64(data & 0xf) << 51;             // palette
		break;
	case 7:
		m_max_line_sprites = data & 0x3f;
		return;
	}
	m_sprite[m_sprite_idx] = d;
}

enum : unsigned { DRAW_REPLACE, DRAW_XOR, DRAW_TRANSPARENT, DRAW_OR, DRAW_AND };

void ddraig_gfxvga_device::draw_word(u32 addr, u16 colour, unsigned op)
{
	u16 &word = m_vram[addr & VRAM_MASK];
	switch (op)
	{
	case DRAW_XOR: word ^= colour; break;
	case DRAW_OR:  word |= colour; break;
	case DRAW_AND: word &= colour; break;
	default:       word = colour; break;
	}
}

void ddraig_gfxvga_device::draw_command(u16 command)
{
	// Drawing.vhd: canvas pixels are 16-bit words at draw_base + y * stride + x.
	// Coordinates are signed 16-bit, saturated to 12 bits.
	auto const sat12 = [] (u16 v) { return std::clamp<int>(s16(v), -2048, 2047); };
	static const u8 ops[8] = { DRAW_REPLACE, DRAW_XOR, DRAW_TRANSPARENT, DRAW_REPLACE, DRAW_OR, DRAW_AND, DRAW_REPLACE, DRAW_REPLACE };
	unsigned const op = ops[m_draw_mode & 7];
	int const cx0 = m_clip[0], cy0 = m_clip[1], cx1 = m_clip[2], cy1 = m_clip[3];
	u16 const *const p = m_draw_param;

	switch (command & 0x7f)
	{
	case 0x01: // filled rectangle
	case 0x08: // legacy linear fill, routed to the rectangle path
	{
		int const x0 = sat12(p[0]), y0 = sat12(p[1]), x1 = sat12(p[2]), y1 = sat12(p[3]);
		int const lo_x = std::min(x0, x1), hi_x = std::max(x0, x1);
		int const lo_y = std::min(y0, y1), hi_y = std::max(y0, y1);
		if (hi_x < cx0 || lo_x > cx1 || hi_y < cy0 || lo_y > cy1)
			break;
		unsigned const pattern = m_draw_pattern & 0x3f;
		bool const solid = !pattern;
		// Relative patterns are anchored to the rectangle's unclipped corner.
		unsigned const ox = BIT(m_draw_pattern, 15) ? (lo_x & 15) : 0;
		unsigned const oy = BIT(m_draw_pattern, 15) ? (lo_y & 15) : 0;
		for (int y = std::clamp(lo_y, cy0, cy1); y <= std::clamp(hi_y, cy0, cy1); y++)
		{
			u32 const row = m_draw_base + u32(y) * m_draw_stride;
			u16 const bits = m_pattern_ram[(pattern << 4) | ((y - oy) & 15)];
			for (int x = std::clamp(lo_x, cx0, cx1); x <= std::clamp(hi_x, cx0, cx1); x++)
			{
				bool const on = solid || BIT(bits, 15 - ((x - ox) & 15));
				if (op == DRAW_TRANSPARENT && !on)
					continue;
				draw_word(row + (x & 0x3ff), on ? m_draw_col0 : m_draw_col1, op);
			}
		}
		break;
	}

	case 0x06: // linear copy
	case 0x07: // 2D copy
	{
		// x0/y0 = source, x1/y1 = destination (high nibble / low word),
		// x2 = width in words, y2 = height (0 = linear, stride = width)
		u32 src = (u32(p[0] & 0xf) << 16) | p[1];
		u32 dst = (u32(p[2] & 0xf) << 16) | p[3];
		u32 const width = p[4];
		u32 lines = p[5] ? p[5] : 1;
		u32 const stride = p[5] ? m_draw_stride : width;
		u32 words = width;
		while (true)
		{
			m_vram[dst & VRAM_MASK] = m_vram[src & VRAM_MASK];
			if (words <= 1 && lines <= 1)
				break;
			u32 advance = 1;
			if (words > 1)
				words--;
			else
			{
				lines--;
				words = width;
				if (stride > width)
					advance = stride - width + 1;
			}
			src += advance;
			dst += advance;
		}
		break;
	}

	case 0x09: // expand the 16-bit pattern register to one row of pixels
	{
		int const x = sat12(p[0]), y = sat12(p[1]);
		if (y < cy0 || y > cy1)
			break;
		u32 const row = m_draw_base + u32(y) * m_draw_stride;
		for (int bit = 0; bit < 16; bit++)
		{
			int const px = x + bit;
			bool const on = BIT(m_draw_pattern, 15 - bit);
			if (px < cx0 || px > cx1 || (op == DRAW_TRANSPARENT && !on))
				continue;
			draw_word(row + (px & 0x3ff), on ? m_draw_col0 : m_draw_col1, op);
		}
		break;
	}

	case 0x02: case 0x03: case 0x04: case 0x05:
		// The card's bitstream is built with G_LINE and G_TRIANGLE false.
		LOGMASKED(LOG_UNHANDLED, "%s: draw command %02x is compiled out of the card\n", machine().describe_context(), command);
		break;

	default:
		break;
	}
}

void ddraig_gfxvga_device::dma_start(unsigned operation)
{
	// MemStreamDMA.vhd. Status: b0 busy, b1 done, b2 error, b15:12 error code.
	enum { ERR_NONE, ERR_BAD_OPERATION, ERR_ZERO_LENGTH, ERR_ADDRESS_RANGE, ERR_SOURCE_UNDERRUN, ERR_DEST_OVERRUN, ERR_TRAILING_SOURCE };
	auto const finish = [this] (unsigned error) { m_dma_status = error ? (0x0004 | (error << 12)) : 0x0002; };

	u32 src = m_dma_src, dst = m_dma_dst;
	u32 src_left = m_dma_src_words, dst_left = m_dma_dst_words;

	if (operation == 0)
		return finish(ERR_BAD_OPERATION);
	if (!dst_left)
		return finish(ERR_ZERO_LENGTH);
	if (dst + dst_left > VRAM_WORDS)
		return finish(ERR_ADDRESS_RANGE);
	if (operation != 2 && !src_left)
		return finish(ERR_ZERO_LENGTH);
	if (operation != 2 && src + src_left > VRAM_WORDS)
		return finish(ERR_ADDRESS_RANGE);

	if (operation == 1) // copy
	{
		if (src_left < dst_left)
			return finish(ERR_SOURCE_UNDERRUN);
		while (dst_left--)
			m_vram[dst++] = m_vram[src++];
		return finish(ERR_NONE);
	}
	if (operation == 2) // fill
	{
		while (dst_left--)
			m_vram[dst++] = m_dma_fill;
		return finish(ERR_NONE);
	}

	// BA2 delta packet: command word bits 15:14 = SKIP, LITERAL, FILL_0000,
	// FILL_WORD; bits 13:0 = run length - 1.
	while (true)
	{
		if (!src_left)
			return finish(ERR_SOURCE_UNDERRUN);
		u16 const command = m_vram[src++];
		src_left--;
		u32 const count = (command & 0x3fff) + 1;
		if (count > dst_left)
			return finish(ERR_DEST_OVERRUN);
		switch (command >> 14)
		{
		case 0:
			dst += count;
			break;
		case 1:
			if (count > src_left)
				return finish(ERR_SOURCE_UNDERRUN);
			for (u32 i = 0; i < count; i++)
				m_vram[dst++] = m_vram[src++];
			src_left -= count;
			break;
		case 2:
			for (u32 i = 0; i < count; i++)
				m_vram[dst++] = 0;
			break;
		case 3:
		{
			if (!src_left)
				return finish(ERR_SOURCE_UNDERRUN);
			u16 const value = m_vram[src++];
			src_left--;
			for (u32 i = 0; i < count; i++)
				m_vram[dst++] = value;
			break;
		}
		}
		dst_left -= count;
		if (!dst_left)
			return finish(src_left ? ERR_TRAILING_SOURCE : ERR_NONE);
		if (!src_left)
			return finish(ERR_SOURCE_UNDERRUN);
	}
}

void ddraig_gfxvga_device::render_tile_layer(unsigned layer, int ly, u8 *line)
{
	// TileLineEngine: 9-bit scroll and line arithmetic, maps wrap at their
	// programmed size, tiles are 4 bpp with the leftmost pixel in bits 15:12.
	tile_layer const &t = m_tile[layer];
	bool const tile8 = BIT(t.ctrl, 15);
	unsigned const lw = (t.ctrl >> 11) & 3;
	unsigned const lh = (t.ctrl >> 13) & 3;
	unsigned const lscr = (t.ctrl >> 1) & 3;
	bool const vscroll = BIT(t.vctrl, 0);
	unsigned const map_mode = (t.vctrl >> 1) & 3;
	bool const map32 = map_mode == 1;
	int const size = tile8 ? 8 : 16;
	int const shift = tile8 ? 3 : 4;
	u32 const wmask = (32U << lw) - 1;
	u32 const hmask = (32U << lh) - 1;

	u32 const scroll_y = t.scroll_y & 0x1ff;
	u32 scroll_x = t.scroll_x & 0x1ff;
	u32 ls_y = 0;
	if (lscr)
	{
		u32 const y = (ly + scroll_y) & 0x1ff;
		if (lscr == 3)
		{
			u32 const addr = m_linescroll_base + (y << 1);
			scroll_x = (scroll_x + util::sext(m_vram[addr & VRAM_MASK], 9)) & 0x1ff;
			ls_y = m_vram[(addr + 1) & VRAM_MASK] & 0x1ff;
		}
		else
		{
			u32 const addr = m_linescroll_base + ((lscr == 2) ? (layer << 9) : 0) + y;
			scroll_x = (scroll_x + util::sext(m_vram[addr & VRAM_MASK], 9)) & 0x1ff;
		}
	}

	int const pix_off = scroll_x & (size - 1);
	u32 map_x = (scroll_x >> shift) & wmask;
	int const tiles = LOGICAL_WIDTH / size + (pix_off ? 1 : 0);

	for (int step = 0; step < tiles; step++)
	{
		u32 const vs_delta = vscroll ? (m_vram[(m_vscroll_base + (layer << 6) + step) & VRAM_MASK] & 0x1ff) : 0;
		u32 const y = (ly + scroll_y + ls_y + vs_delta) & 0x1ff;
		u32 const row_in_tile = y & (size - 1);
		u32 const tile_row = (y >> shift) & 0xff & hmask;
		u32 row_off = tile_row << (5 + lw);
		if (map32)
			row_off <<= 1;
		u32 const entry_addr = t.map + row_off + (map32 ? (map_x << 1) : map_x);

		u16 const entry = m_vram[entry_addr & VRAM_MASK];
		u32 index;
		u8 palette;
		bool flipx, flipy;
		if (map32)
		{
			palette = entry >> 12;
			flipx = BIT(entry, 11);
			flipy = BIT(entry, 10);
			index = m_vram[(entry_addr + 1) & VRAM_MASK] & 0x3fff;
		}
		else if (map_mode == 2)
		{
			palette = (t.vctrl >> 3) & 0xf;
			flipx = BIT(entry, 14);
			flipy = BIT(entry, 15);
			index = entry & 0x3fff;
		}
		else
		{
			palette = entry >> 13;
			flipx = BIT(entry, 11);
			flipy = BIT(entry, 12);
			index = entry & 0x7ff;
		}

		u32 const row = flipy ? (size - 1 - row_in_tile) : row_in_tile;
		u32 const row_addr = t.data + (tile8 ? ((index << 4) + (row << 1)) : ((index << 6) + (row << 2)));
		for (int b = 0; b < size; b++)
		{
			int const x = step * size + b - pix_off;
			if (x < 0 || x >= LOGICAL_WIDTH)
				continue;
			int const pix = flipx ? (b ^ (size - 1)) : b;
			u16 const word = m_vram[(row_addr + (pix >> 2)) & VRAM_MASK];
			line[x] = (palette << 4) | ((word >> (12 - 4 * (pix & 3))) & 0xf);
		}
		map_x = (map_x + 1) & wmask;
	}
}

void ddraig_gfxvga_device::render_sprites(int ly, u16 *line)
{
	// line[x]: bit 15 valid, 14:11 priority, 10:7 palette, 3:0 pixel (SpriteLineRenderer)
	std::fill_n(line, LOGICAL_WIDTH, 0);
	unsigned hits = 0;
	for (unsigned i = 0; i < NUM_SPRITES; i++)
	{
		u64 const d = m_sprite[i];
		if (!BIT(d, 63))
			continue;
		s32 const y = sext10(d >> 20);
		s32 const y_end = sext10((d >> 20) + 16);
		if (ly < y || ly >= y_end)
			continue;
		s32 const x = sext10(d >> 30);
		if (x >= LOGICAL_WIDTH || sext10((d >> 30) + 15) < 0)
			continue;
		if (hits >= m_max_line_sprites)
		{
			m_overflow |= 1;
			break;
		}
		hits++;

		u32 row = (ly - y) & 15;
		if (BIT(d, 55))
			row = 15 - row;
		u32 const tile = ((d >> 40) & 0x7ff) | (BIT(d, 57) << 11);
		u32 const addr = m_sprite_base + (tile << 6) + (row << 2);
		u16 words[4];
		for (int w = 0; w < 4; w++)
			words[w] = m_vram[(addr + w) & VRAM_MASK];
		bool const flipx = BIT(d, 56);
		u16 const pri = (d >> 59) & 0xf;
		u16 const pal = (d >> 51) & 0xf;
		for (int w = 0; w < 16; w++)
		{
			int const px = x + w;
			if (px < 0 || px >= LOGICAL_WIDTH)
				continue;
			int const src = flipx ? 15 - w : w;
			u16 const pix = (words[src >> 2] >> (12 - 4 * (src & 3))) & 0xf;
			if (!pix)
				continue;
			u16 const old = line[px];
			if (!BIT(old, 15) || pri < ((old >> 11) & 0xf))
				line[px] = 0x8000 | (pri << 11) | (pal << 7) | pix;
		}
	}
}

void ddraig_gfxvga_device::compose_line(int ly)
{
	// PixelCompositor: lowest priority value wins, transparent tile pixels
	// match the layer's transparent index, equal tile priority keeps the
	// earlier layer, and sprites win ties with tiles.
	u8 tiles[4][LOGICAL_WIDTH];
	u16 sprites[LOGICAL_WIDTH];
	bool layer_on[4];
	for (unsigned i = 0; i < 4; i++)
	{
		layer_on[i] = mode() == MODE_TILEMAP && BIT(m_tile[i].ctrl, 0);
		if (layer_on[i])
			render_tile_layer(i, ly, tiles[i]);
	}
	bool const spr_on = sprites_enabled();
	if (spr_on)
		render_sprites(ly, sprites);

	for (int x = 0; x < LOGICAL_WIDTH; x++)
	{
		unsigned best_pri = 15;
		u16 best_pal = 0;
		bool hit = false;
		for (unsigned i = 0; i < 4; i++)
		{
			if (!layer_on[i])
				continue;
			tile_layer const &t = m_tile[i];
			u8 const pix = tiles[i][x];
			unsigned const pri = (t.ctrl >> 3) & 0xf;
			if ((pix & 0xf) != ((t.ctrl >> 7) & 0xf) && pri < best_pri)
			{
				best_pri = pri;
				best_pal = ((((m_pal_ctrl1 >> (3 * i)) & 7) << 7) + pix) & 0x3ff;
				hit = true;
			}
		}
		if (spr_on && BIT(sprites[x], 15) && ((sprites[x] >> 11) & 0xf) <= best_pri)
		{
			best_pal = (((m_pal_ctrl1 >> 12) & 3) << 8) | ((sprites[x] >> 3) & 0xf0) | (sprites[x] & 0xf);
			hit = true;
		}
		m_comp_line[x] = (hit ? 0x8000 : 0) | best_pal;
	}
}

u8 ddraig_gfxvga_device::text_pixel(int x, int y) const
{
	// TextGenerator: cells are bg[15:12] fg[11:8] char[7:0]; 8x8 glyphs live at font 0x1000.
	video_mode const &m = s_modes[m_res];
	bool const font8 = BIT(m_control, 10);
	int const fh = font8 ? 8 : 16;
	int const row = y / fh;
	if (row >= (font8 ? m.text_rows8 : m.text_rows16))
		return 0;
	int const col = x >> 3;
	unsigned const grow = y & (fh - 1);
	u16 const cell = m_vram[(m_text_base + row * m.text_cols + col) & VRAM_MASK];
	u8 const glyph = m_font[font8 ? (0x1000 + ((cell & 0xff) << 3) + grow) : (((cell & 0xff) << 4) + grow)];
	bool const glyph_bit = BIT(glyph, 7 - (x & 7));
	u8 const fg = (cell >> 8) & 0xf;
	u8 const bg = cell >> 12;
	bool const blink = BIT(m_control, 9);
	bool const blink_phase = BIT(m_blink_count, 5);

	bool on = glyph_bit;
	if (blink && BIT(bg, 3) && blink_phase)
		on = false;
	if (BIT(m_control, 8) && blink_phase && col == (m_cursor_pos & 0xff) && row == (m_cursor_pos >> 8)
			&& grow >= (m_cursor_size & 0xf) && grow <= ((m_cursor_size >> 8) & 0xf))
		on = !glyph_bit;
	return on ? fg : (blink ? (bg & 7) : bg);
}

bool ddraig_gfxvga_device::bitmap_pixel(int x, int y, u16 &value) const
{
	// Returns true for direct RGB565, false for a palette index.
	video_mode const &m = s_modes[m_res];
	bool const lowres = mode() == MODE_BITMAP;
	int const lx = lowres ? (x >> 1) : x;
	int const ly = lowres ? (y >> 1) : y;
	u32 const width = lowres ? (m.hvisible >> 1) : m.hvisible;
	unsigned depth = (m_control >> 3) & 7;
	if (m_res == 2 && depth == 0 && mode() == MODE_BITMAPHIRES)
		depth = 1; // 1024x768 full resolution is clamped to 8 bpp
	u32 wpl;
	switch (depth)
	{
	case 1: wpl = width >> 1; break;
	case 2: case 5: wpl = width >> 2; break;
	case 3: wpl = width >> 3; break;
	case 4: wpl = width >> 4; break;
	default: wpl = width; break;
	}
	u32 const base = m_fb_active + ly * wpl;
	u8 index;
	switch (depth)
	{
	case 1: { u16 const w = m_vram[(base + (lx >> 1)) & VRAM_MASK]; index = BIT(lx, 0) ? (w & 0xff) : (w >> 8); break; }
	case 2: index = (m_vram[(base + (lx >> 2)) & VRAM_MASK] >> (12 - 4 * (lx & 3))) & 0xf; break;
	case 3: index = (m_vram[(base + (lx >> 3)) & VRAM_MASK] >> (14 - 2 * (lx & 7))) & 3; break;
	case 4: index = BIT(m_vram[(base + (lx >> 4)) & VRAM_MASK], 15 - (lx & 15)); break;
	case 5:
	{
		u32 const group = base + ((lx >> 4) << 2);
		unsigned const bit = 15 - (lx & 15);
		index = 0;
		for (int p = 0; p < 4; p++)
			index |= BIT(m_vram[(group + p) & VRAM_MASK], bit) << p;
		break;
	}
	default:
		value = m_vram[(base + lx) & VRAM_MASK];
		return true;
	}
	value = ((m_pal_ctrl0 & 3) << 8) | index;
	return false;
}

u32 ddraig_gfxvga_device::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	video_mode const &m = s_modes[m_res];
	u16 const md = mode();
	bool const overlay = BIT(m_control, 14);
	bool const spr_on = sprites_enabled();
	bool const use_comp = md == MODE_TILEMAP || spr_on;
	unsigned const scanline_mode = (md == MODE_TILEMAP || md == MODE_BITMAP) ? ((m_control >> 12) & 3) : 0;
	u16 const text_bank = ((m_pal_ctrl0 >> 2) & 7) << 7;
	rgb_t const bg = m_rgb565[m_bg_color];

	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		u32 *const dst = &bitmap.pix(y);
		if (BIT(m_control, 15) || y >= m.vvisible)
		{
			std::fill(dst + cliprect.min_x, dst + cliprect.max_x + 1, rgb_t::black());
			continue;
		}

		if (use_comp && (y >> 1) < LOGICAL_HEIGHT)
		{
			u64 const frame = screen.frame_number();
			if (m_comp_frame != frame || m_comp_y != (y >> 1))
			{
				compose_line(y >> 1);
				m_comp_frame = frame;
				m_comp_y = y >> 1;
			}
		}

		int const cur_row = y - m_hwc_y;
		bool const cur_line = BIT(m_hwc_ctrl, 0) && cur_row >= 0 && cur_row < 32;

		for (int x = cliprect.min_x; x <= cliprect.max_x && x < m.hvisible; x++)
		{
			rgb_t c;
			int const cur_col = x - m_hwc_x;
			unsigned const cur_pix = (cur_line && cur_col >= 0 && cur_col < 32)
					? ((m_hwc_data[(cur_row << 2) | (cur_col >> 3)] >> (14 - 2 * (cur_col & 7))) & 3) : 0;
			if (cur_pix)
				c = m_rgb565[m_hwc_col[cur_pix - 1]];
			else if (md == MODE_TEXT)
				c = palette_rgb(text_bank | text_pixel(x, y));
			else
			{
				u8 const text = overlay ? text_pixel(x, y) : 0;
				u16 const comp = use_comp ? m_comp_line[x >> 1] : 0;
				if (text)
					c = palette_rgb(text_bank | text);
				else if (md == MODE_TILEMAP)
					c = BIT(comp, 15) ? palette_rgb(comp) : bg;
				else if (md == MODE_BITMAP || md == MODE_BITMAPHIRES)
				{
					u16 value;
					if (spr_on && (comp & 0xf))
						c = palette_rgb(comp);
					else if (bitmap_pixel(x, y, value))
						c = m_rgb565[value];
					else
						c = palette_rgb(value);
				}
				else
					c = bg;
			}

			if (scanline_mode && BIT(y, 0))
			{
				if (scanline_mode == 1)
					c = rgb_t::black();
				else
				{
					// The card halves or quarters the RGB565 components.
					unsigned const s = scanline_mode - 1;
					u16 const r5 = (c.r() >> 3) >> s, g6 = (c.g() >> 2) >> s, b5 = (c.b() >> 3) >> s;
					c = m_rgb565[(r5 << 11) | (g6 << 5) | b5];
				}
			}
			dst[x] = c;
		}
	}
	return 0;
}

} // anonymous namespace

DEFINE_DEVICE_TYPE_PRIVATE(DDRAIG_GFXVGA, ddraig_card_interface, ddraig_gfxvga_device, "ddraig_gfxvga", "Y Ddraig GfxVGA card")
