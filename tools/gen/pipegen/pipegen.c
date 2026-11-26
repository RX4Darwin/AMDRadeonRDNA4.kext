/* pipegen: run Linux DC's own DCN 4.01 code for lighting one new HDMI stream and plane against a recorder
 * (rec.c) and print the register sequence. Built and run by run.sh, which explains the rest. Two more
 * scenarios give references for the driver's DisplayPort code: "dp" (a new timing on a trained link) and
 * "dplink" (a stream lit on a link that is trained against a simulated sink, with the AUX transfers). */
#include "resource/dcn401/dcn401_resource.c"   /* its static creators and register tables */
#include "dcn401/dcn401_dccg.h"
#include "dce/dce_clock_source.h"
#include "rec.h"

/* Segment bases are sentinels so that an address decodes as (base_idx + 1) << 24 | dword. */
static uint32_t dcn_bases[8]  = { 1u << 24, 2u << 24, 3u << 24, 4u << 24, 5u << 24, 6u << 24, 7u << 24, 8u << 24 };
static uint32_t nbio_bases[8] = { 0x11u << 24, 0x12u << 24, 0x13u << 24, 0x14u << 24, 0x15u << 24, 0x16u << 24, 0x17u << 24, 0x18u << 24 };

static struct dc g_dc;
static struct clk_mgr g_clk_mgr;
static struct clk_mgr_funcs no_clk_funcs;
static struct dc_context g_ctx;
static struct dc_bios g_bios;
static struct resource_pool g_pool;

static bool g_link_detail;     /* the "dplink" scenario */

/* ---- the BIOS command callbacks: on DCN 4.01 these become DMUB commands; record what was asked ---- */
static enum bp_result bios_encoder_control(struct dc_bios *bp, struct bp_encoder_control *c)
{
	if (rec_log_reads)
		fprintf(stderr, "DMUB  encoder_control action=%d engine=%d signal=%d lanes=%d pixclk=%u depth=%d\n",
			c->action, c->engine_id, c->signal, c->lanes_number, c->pixel_clock, c->color_depth);
	rec_mark("dmub:encoder_control", c->action);
	return BP_RESULT_OK;
}
static enum bp_result bios_transmitter_control(struct dc_bios *bp, struct bp_transmitter_control *c)
{
	if (rec_log_reads)
		fprintf(stderr, "DMUB  transmitter_control action=%d engine=%d transmitter=%d lanes=%d pixclk=%u pll=%d signal=%d hpd=%d depth=%d coherent=%d\n",
		c->action, c->engine_id, c->transmitter, c->lanes_number, c->pixel_clock, c->pll_id, c->signal, c->hpd_sel,
		c->color_depth, c->coherent);
	if (g_link_detail) {    /* a DisplayPort link is brought up in several of these: say which */
		char what[112];
		snprintf(what, sizeof(what), "dmub:transmitter_control:action=%d:lanes=%d:symclk=%u:lane=%d:drive=0x%02x",
			 c->action, c->lanes_number, c->pixel_clock, c->lane_select, c->lane_settings);
		rec_mark(what, c->action);
		return BP_RESULT_OK;
	}
	rec_mark("dmub:transmitter_control", c->action);
	return BP_RESULT_OK;
}
static enum bp_result bios_set_pixel_clock(struct dc_bios *bp, struct bp_pixel_clock_parameters *p)
{
	if (rec_log_reads)
		fprintf(stderr, "DMUB  set_pixel_clock controller=%d pll=%d clk100hz=%u encoder=%u signal=%d depth=%d\n",
		p->controller_id, p->pll_id, p->target_pixel_clock_100hz, p->encoder_object_id.id, p->signal_type, p->color_depth);
	rec_mark("dmub:set_pixel_clock", 0);
	return BP_RESULT_OK;
}
/* The GOP lit a display: amdgpu takes the hardware over from a running VBIOS ("accelerated mode"); in the
 * "init" scenario nothing runs (after a power loss the VBIOS has not run). */
static bool g_cold;
static bool bios_accelerated(struct dc_bios *bp) { return !g_cold; }
static const struct dc_vbios_funcs bios_funcs = {
	.is_accelerated_mode = bios_accelerated,
	.encoder_control = bios_encoder_control,
	.transmitter_control = bios_transmitter_control,
	.set_pixel_clock = bios_set_pixel_clock,
};

struct cfg {
	int pipe;      /* OTG = OPP = HUBP = DPP = MPCC instance */
	int dig;       /* DIG front-end (stream encoder) */
	int link;      /* DIG back-end / PHY / PLL */
	int hpd;       /* 0-based HPD source */
	bool dp;       /* the "dp" scenario: retime a DisplayPort stream that is already lit (sequence_dp) */
	bool train;    /* the "dplink" scenario: light a DisplayPort stream on a dark pipe, training its link */
	bool dual;     /* the "dpdual" scenario: an HDMI stream on a DisplayPort connector, through a passive DP++ adapter */
	bool y420;     /* "y420": the HDMI stream in YCbCr 4:2:0, half the TMDS clock (amdgpu's force_yuv420_output) */
	bool dsc;      /* "dsc" (after dplink, sink kind d): the stream compressed, as dc_dsc_compute_config chooses */
	bool drr;      /* the "drr" scenario: variable refresh on a lit DisplayPort stream (sequence_drr) */
	bool audio;    /* the "audio" scenario: audio on a lit DisplayPort stream (sequence_audio) */
	struct dc_dsc_config dsc_cfg;
	int sink_swing, sink_pre;     /* the drive the simulated sink asks for before it reports lock */
	char sink_kind;               /* its capabilities: sink_reset() */
	struct dc_crtc_timing t;      /* the new stream */
	unsigned surf_w, surf_h;      /* "on W H": the plane shows the top-left of a surface this size (a mode switch
	                               * on a pipe lit for a larger mode); 0 = a surface of the mode's own size */
	struct dc_crtc_timing lit;    /* the stream the firmware lit */
	int vstartup, vupdate_offset, vupdate_width, vready_offset, pstate_keepout;   /* from DML */
};


/* ---- DML 2.1: the timing-dependent HUBP values and global sync for the new pipe ---- */
#include "dml_top.h"
#include "dml2_internal_shared_types.h"
#include "bounding_boxes/dcn4_soc_bb.h"

static struct dml2_display_cfg_programming *g_prog;
static enum dml2_output_link_dp_rate g_dsc_rate = dml2_dp_rate_hbr3;   /* the "dsc" stream's link (dsc_choose) */
static unsigned g_dsc_lanes = 4;

static void dml_stream(struct dml2_display_cfg *d, int i, const struct dc_crtc_timing *t, bool hdmi,
		       unsigned surf_w, unsigned surf_h, const struct dc_dsc_config *dsc)
{
	struct dml2_stream_parameters *s = &d->stream_descriptors[i];
	struct dml2_plane_parameters *p = &d->plane_descriptors[i];
	unsigned w = t->h_addressable, h = t->v_addressable;

	s->timing.h_active = w; s->timing.v_active = h;
	s->timing.h_total = t->h_total; s->timing.v_total = t->v_total;
	s->timing.h_front_porch = t->h_front_porch; s->timing.v_front_porch = t->v_front_porch;
	s->timing.h_sync_width = t->h_sync_width;
	s->timing.h_blank_end = t->h_total - t->h_front_porch - w;
	s->timing.v_blank_end = t->v_total - t->v_front_porch - h;
	s->timing.pixel_clock_khz = t->pix_clk_100hz / 10;
	s->timing.bpc = 8;
	s->timing.dsc.enable = dsc ? dml2_dsc_enable : dml2_dsc_disable;
	if (dsc) {      /* what dml21_translation_helper takes from the stream: its DSC config, link HBR3 x4 */
		s->timing.dsc.dsc_compressed_bpp_x16 = dsc->bits_per_pixel;
		s->timing.dsc.overrides.num_slices = dsc->num_slices_h;
		s->output.output_dp_link_rate = g_dsc_rate;
	}
	s->timing.drr_config.disallowed = true;
	s->timing.vblank_nom = t->v_total - h;
	s->output.output_dp_lane_count = dsc ? g_dsc_lanes : 4;
	s->output.output_encoder = hdmi ? dml2_hdmi : dml2_dp;
	s->output.output_format = t->pixel_encoding == PIXEL_ENCODING_YCBCR420 ? dml2_420 : dml2_444;
	if (!dsc)
		s->output.output_dp_link_rate = dml2_dp_rate_na;
	s->output.output_disabled = true;
	s->overrides.odm_mode = dml2_odm_mode_auto;
	s->overrides.disable_dynamic_odm = true;
	s->overrides.hw.twait_budgeting.fclk_pstate = dml2_twait_budgeting_setting_if_needed;
	s->overrides.hw.twait_budgeting.uclk_pstate = dml2_twait_budgeting_setting_if_needed;
	s->overrides.hw.twait_budgeting.stutter_enter_exit = dml2_twait_budgeting_setting_if_needed;

	p->stream_index = i;
	p->surface.plane0.width = surf_w ? surf_w : w; p->surface.plane0.height = surf_h ? surf_h : h;
	p->surface.plane0.pitch = p->surface.plane0.width;
	p->surface.dcc.informative.dcc_rate_plane0 = 2.0;
	p->surface.dcc.informative.dcc_rate_plane1 = 2.0;
	p->surface.tiling = dml2_sw_linear;
	p->cursor.cursor_bpp = 32; p->cursor.cursor_width = 256; p->cursor.num_cursors = 1;
	p->pixel_format = dml2_444_32;
	p->composition.viewport.plane0.width = w; p->composition.viewport.plane0.height = h;
	p->composition.scaler_info.plane0.h_ratio = 1.0; p->composition.scaler_info.plane0.v_ratio = 1.0;
	p->composition.scaler_info.plane1.h_ratio = 1.0; p->composition.scaler_info.plane1.v_ratio = 1.0;
	p->composition.scaler_info.plane0.h_taps = 1; p->composition.scaler_info.plane0.v_taps = 1;
	p->composition.scaler_info.plane1.h_taps = 1; p->composition.scaler_info.plane1.v_taps = 1;
	p->composition.scaler_info.rect_out_width = w;
	p->composition.rotation_angle = dml2_rotation_0;
	p->composition.rect_out_height_spans_vactive = true;
	p->immediate_flip = true;
	p->overrides.gpuvm_min_page_size_kbytes = dml2_socbb_dcn401.gpuvm_min_page_size_kbytes;
}

/* Stream 0 is the display the firmware lit (its timing only shapes shared clocks); stream 1 is ours.
 * A discrete card scans out of the frame buffer aperture, so GPU VM is off, as amdgpu has it. */
static void run_dml(struct cfg *c)
{
	struct dml2_instance *inst = calloc(1, dml2_get_instance_size_bytes());
	struct dml2_initialize_instance_in_out *init = calloc(1, sizeof(*init));
	struct dml2_display_cfg *d = calloc(1, sizeof(*d));
	struct dml2_build_mode_programming_in_out bmp;
	const union dml2_global_sync_programming *g;

	g_prog = calloc(1, sizeof(*g_prog));
	init->dml2_instance = inst;
	init->options.project_id = dml2_project_dcn4x_stage2_auto_drr_svp;
	init->options.pmo_options.disable_dyn_odm = true;
	init->options.pmo_options.disable_dyn_odm_for_multi_stream = true;
	init->options.pmo_options.disable_dyn_odm_for_stream_with_svp = true;
	init->options.pmo_options.disable_svp = true;
	init->options.pmo_options.disable_drr_clamped = true;
	init->options.pmo_options.disable_drr_var = true;
	init->options.pmo_options.disable_fams2 = true;
	init->soc_bb = dml2_socbb_dcn401;
	init->soc_bb.qos_parameters = dml_dcn4_variant_a_soc_qos_params;
	init->ip_caps = dml2_dcn401_max_ip_caps;
	if (!dml2_initialize_instance(init)) { fprintf(stderr, "DML init failed\n"); exit(1); }

	d->gpuvm_enable = false;
	d->gpuvm_max_page_table_levels = 4;
	d->minimize_det_reallocation = true;
	d->overrides.enable_subvp_implicit_pmo = true;
	d->num_streams = 2; d->num_planes = 2;
	dml_stream(d, 0, &c->lit, false, 0, 0, NULL);
	dml_stream(d, 1, &c->t, !c->dsc, c->surf_w, c->surf_h, c->dsc ? &c->dsc_cfg : NULL);
	bmp.dml2_instance = inst; bmp.display_config = d; bmp.programming = g_prog;
	if (!dml2_build_mode_programming(&bmp)) { fprintf(stderr, "DML: mode not supported\n"); exit(1); }

	g = &g_prog->stream_programming[1].global_sync;
	c->vstartup = g->dcn4x.vstartup_lines; c->vupdate_offset = g->dcn4x.vupdate_offset_pixels;
	c->vupdate_width = g->dcn4x.vupdate_vupdate_width_pixels; c->vready_offset = g->dcn4x.vready_offset_pixels;
	c->pstate_keepout = g->dcn4x.pstate_keepout_start_lines;
	if (g_prog->plane_programming[1].num_dpps_required != 1) {
		fprintf(stderr, "DML: the new pipe needs %u DPPs (ODM): not modelled here\n",
			g_prog->plane_programming[1].num_dpps_required);
		exit(1);
	}
	fprintf(stderr, "DML: dispclk %lu kHz, dpprefclk %lu kHz, dcfclk %lu kHz; new pipe dppclk %lu kHz, det %u segments, "
		"vstartup %d\n", g_prog->min_clocks.dcn4x.dispclk_khz, g_prog->min_clocks.dcn4x.dpprefclk_khz,
		g_prog->min_clocks.dcn4x.active.dcfclk_khz, g_prog->plane_programming[1].min_clocks.dcn4x.dppclk_khz,
		g_prog->plane_programming[1].pipe_regs[0]->det_size, c->vstartup);
}

/* ---- the boundary to things that are not register writes ---- */
#include "link_service.h"
#include "link/link_dpms.h"
#include "link/link_factory.h"
#include "link/protocols/link_ddc.h"
#include "dcn10/dcn10_mpc.h"
#include "dcn20/dcn20_opp.h"
#include "dcn20/dcn20_hubbub.h"
#include "dcn31/dcn31_vpg.h"
#include "dcn401/dcn401_mpc.h"
#include "dcn10/dcn10_stream_encoder.h"

/* ---- "dplink": the monitor at the other end of the AUX channel ----
 *
 * Linux reads and writes the sink's DPCD through dm_helpers_dp_*_dpcd. Here that is a sink of four HBR3 lanes
 * that does what link training asks of it: it reports clock recovery on a lane once a training pattern is on
 * and the lane's voltage swing is what it wants, and equalisation, symbol lock and alignment once the pattern
 * is TPS2 or later and the pre-emphasis is what it wants. Until then it asks for those levels. Three kinds:
 *   a  DPCD 1.4 with TPS3 and TPS4, read at once (TRAINING_AUX_RD_INTERVAL 0)
 *   b  DPCD 1.2 with TPS3, read 4 ms after a change
 *   c  DPCD 1.2 with TPS2 only
 * Every transfer is recorded: "aux:w:<address>:<bytes>" and "aux:r:<address>:<bytes>". */
static uint8_t g_dpcd[0x800];
static int g_sink_swing, g_sink_pre;

static void sink_reset(const struct cfg *c)
{
	memset(g_dpcd, 0, sizeof(g_dpcd));
	g_dpcd[0x000] = c->sink_kind == 'a' ? 0x14 : 0x12;        /* DPCD_REV */
	g_dpcd[0x001] = 0x1e;                                     /* MAX_LINK_RATE HBR3 */
	g_dpcd[0x002] = c->sink_kind == 'c' ? 0x84 : 0xc4;        /* enhanced framing, TPS3, four lanes */
	g_dpcd[0x003] = c->sink_kind == 'a' ? 0x81 : 0x01;        /* TPS4, 0.5 % down-spread */
	g_dpcd[0x00e] = c->sink_kind == 'b' ? 0x01 : 0x00;        /* TRAINING_AUX_RD_INTERVAL */
	if (c->sink_kind == 'd') {     /* kind a with a DSC 1.2 decoder and FEC (dscgen.c's first sink) */
		static const uint8_t dsc[16] = { 0x01, 0x21, 0x00, 0x05, 0x2b, 0x00, 0x01, 0x00,
						 0x00, 0x07, 0x06, 0x11, 0x08, 0x00, 0x00, 0x00 };
		const char *caps = getenv("PIPEGEN_DSC_CAPS");   /* a real sink's DPCD 0x060-0x06F, 32 hex digits */

		g_dpcd[0x000] = 0x14; g_dpcd[0x003] = 0x81;
		memcpy(&g_dpcd[0x060], dsc, sizeof(dsc));
		for (int i = 0; caps && i < 16 && sscanf(caps + 2 * i, "%2hhx", &g_dpcd[0x060 + i]) == 1; i++)
			;
		g_dpcd[0x090] = 0x01;                                 /* FEC_CAPABLE */
	}
	g_sink_swing = c->sink_swing; g_sink_pre = c->sink_pre;
}

static void sink_settle(void)
{
	int pattern = g_dpcd[0x102] & 0x0f, lanes = g_dpcd[0x101] & 0x1f, aligned = lanes > 0;

	if (!pattern)           /* out of training: the lanes stay as they were trained */
		return;
	memset(&g_dpcd[0x202], 0, 6);
	for (int l = 0; l < lanes; l++) {
		int swing = g_dpcd[0x103 + l] & 3, pre = (g_dpcd[0x103 + l] >> 3) & 3;
		int cr = pattern && swing >= g_sink_swing, eq = cr && pattern >= 2 && pre >= g_sink_pre;

		g_dpcd[0x202 + l / 2] |= (cr | eq << 1 | eq << 2) << (4 * (l & 1));
		g_dpcd[0x206 + l / 2] |= (g_sink_swing | (pattern >= 2 ? g_sink_pre : 0) << 2) << (4 * (l & 1));
		aligned = aligned && eq;
	}
	g_dpcd[0x204] = aligned;                  /* INTERLANE_ALIGN_DONE */
}

static void aux_mark(char dir, uint32_t address, const uint8_t *data, uint32_t size)
{
	char what[160];
	int n = snprintf(what, sizeof(what), "aux:%c:%03x:", dir, address);

	for (uint32_t i = 0; i < size && n < (int)sizeof(what) - 3; i++)
		n += snprintf(what + n, sizeof(what) - n, "%02x", data[i]);
	if (rec_log_reads)
		fprintf(stderr, "AUX   %s\n", what);
	rec_mark(what, size);
}

bool dm_helpers_dp_read_dpcd(struct dc_context *ctx, const struct dc_link *link, uint32_t address, uint8_t *data,
			     uint32_t size)
{
	if (address + size > sizeof(g_dpcd)) { memset(data, 0, size); aux_mark('r', address, data, size); return true; }
	sink_settle();
	memcpy(data, &g_dpcd[address], size);
	aux_mark('r', address, data, size);
	return true;
}

bool dm_helpers_dp_write_dpcd(struct dc_context *ctx, const struct dc_link *link, uint32_t address,
			      const uint8_t *data, uint32_t size)
{
	aux_mark('w', address, data, size);
	if (address + size <= sizeof(g_dpcd))
		memcpy(&g_dpcd[address], data, size);
	if (address <= 0x101 && address + size > 0x100)     /* a new rate or lane count: the lock is gone */
		memset(&g_dpcd[0x202], 0, 6);
	return true;
}

bool dm_helpers_dp_write_dsc_enable(struct dc_context *ctx, const struct dc_stream_state *stream, bool enable)
{	/* amdgpu_dm_helpers.c, SST: DP_DSC_ENABLE = 1 (decoding) or 0 on the sink */
	const uint8_t v = enable;
	return dm_helpers_dp_write_dpcd(ctx, stream->link, 0x160, &v, 1);
}

void write_scdc_data(struct ddc_service *ddc, uint32_t pix_clk, bool lte_340_scramble)
{
	rec_mark("scdc:write_tmds_config", pix_clk > 340000 ? 3 : lte_340_scramble ? 1 : 0);
}
void read_scdc_data(struct ddc_service *ddc) { rec_mark("scdc:read_status", 0); }
static bool no_128b_132b(struct pipe_ctx *p) { return false; }
static enum dp_link_encoding enc_8b_10b(const struct dc_link_settings *s) { return DP_8b_10b_ENCODING; }
static void no_trace(struct dc_link *l, uint8_t s) { }
static bool no_fec(const struct dc_link *l) { return false; }   /* (FEC only with DSC; the audio scenario has none) */
uint32_t dp_link_bandwidth_kbps(const struct dc_link *link, const struct dc_link_settings *link_setting);
static const struct link_service g_link_srv = {
	.set_dpms_on = link_set_dpms_on,
	.dp_link_bandwidth_kbps = dp_link_bandwidth_kbps,      /* the "audio" scenario's bandwidth check */
	.dp_is_fec_supported = no_fec,
	.dp_is_128b_132b_signal = no_128b_132b,
	.dp_get_encoding_format = enc_8b_10b,
	.dp_trace_source_sequence = no_trace,
};

/* ---- one pipe going from unused to an HDMI stream with one visible 32-bit linear plane ---- */
#define SURFACE_PLACEHOLDER 0x00a5c3e15a3c1e00ull     /* replaced by the real address when the plan runs */

static struct dc_state *g_old, *g_new;
static struct pipe_ctx *g_pipe;

static void make_state(const struct cfg *c)
{
	struct dc *dc = &g_dc;
	struct dc_state *old = calloc(1, sizeof(*old)), *ctx = calloc(1, sizeof(*ctx));
	struct pipe_ctx *pipe = &ctx->res_ctx.pipe_ctx[c->pipe];
	struct dc_stream_state *stream = calloc(1, sizeof(*stream));
	struct dc_plane_state *plane = calloc(1, sizeof(*plane));
	struct dc_link *link = calloc(1, sizeof(*link));
	struct dc_sink *sink = calloc(1, sizeof(*sink));
	struct encoder_init_data eid = { 0 };
	const struct dml2_per_plane_programming *pp = &g_prog->plane_programming[1];
	unsigned w = c->t.h_addressable, h = c->t.v_addressable;

	eid.channel = CHANNEL_ID_DDC1 + c->link;
	eid.hpd_source = c->hpd;
	eid.transmitter = TRANSMITTER_UNIPHY_A + c->link;
	eid.encoder.type = OBJECT_TYPE_ENCODER;
	eid.encoder.id = ENCODER_ID_INTERNAL_UNIPHY + c->link / 2;   /* UNIPHY, UNIPHY1, ...: two links each */
	eid.encoder.enum_id = ENUM_ID_1 + (c->link & 1);
	eid.connector.type = OBJECT_TYPE_CONNECTOR;
	eid.connector.id = c->dp || c->dual ? CONNECTOR_ID_DISPLAY_PORT : CONNECTOR_ID_HDMI_TYPE_A;
	eid.connector.enum_id = ENUM_ID_1;
	eid.ctx = &g_ctx;

	dc->current_state = old;
	dc->link_srv = &g_link_srv;
	dc->links[0] = link; dc->link_count = 1;
	const enum signal_type signal = c->dp ? SIGNAL_TYPE_DISPLAY_PORT : SIGNAL_TYPE_HDMI_TYPE_A;
	/* Behind a DP++ adapter the connector stays DisplayPort and the stream and sink are HDMI, as detection leaves a
	 * link with a DP-HDMI dongle. */
	link->dc = dc; link->ctx = &g_ctx; link->connector_signal = c->dual ? SIGNAL_TYPE_DISPLAY_PORT : signal;
	if (c->train) {         /* a link to train: the sink of sink_reset(), as detection would have read it */
		struct dc_link_settings want = { LANE_COUNT_FOUR, LINK_RATE_HIGH2, LINK_SPREAD_DISABLED };
		const char *other = getenv("PIPEGEN_DP_LINK");          /* "lanes rate", rate as LINK_BW_SET has it */
		int lanes, rate;

		if (other && sscanf(other, "%d %d", &lanes, &rate) == 2) {
			want.lane_count = lanes; want.link_rate = rate;
		}

		sink_reset(c);
		if (c->dsc && !other)   /* the link the DSC config was chosen against (dsc_choose) */
			want.link_rate = LINK_RATE_HIGH3;
		dc->link_srv = link_create_link_service();
		link->dpcd_caps.dpcd_rev.raw = g_dpcd[0x000];
		link->dpcd_caps.max_ln_count.raw = g_dpcd[0x002];
		link->dpcd_caps.max_down_spread.raw = g_dpcd[0x003];
		link->reported_link_cap = link->verified_link_cap = (struct dc_link_settings){
			LANE_COUNT_FOUR, LINK_RATE_HIGH3, LINK_SPREAD_05_DOWNSPREAD_30KHZ };
		pipe->link_config.dp_link_settings = want;
	} else if (c->dp) {     /* a trained link: four lanes at HBR3, as the firmware brings a 4K sink up */
		link->cur_link_settings.lane_count = LANE_COUNT_FOUR;
		link->cur_link_settings.link_rate = LINK_RATE_HIGH3;
		link->link_status.link_active = true;
	}
	link->link_enc = dcn401_link_encoder_create(&g_ctx, &eid);
	link->link_enc->preferred_engine = ENGINE_ID_DIGA + c->dig;
	link->link_id = eid.connector;
	link->ddc_hw_inst = c->link;
	link->local_sink = sink; link->type = dc_connection_single;
	sink->link = link; sink->ctx = &g_ctx; sink->sink_signal = signal;
	sink->edid_caps.scdc_present = true;

	stream->ctx = &g_ctx; stream->link = link; stream->sink = sink; stream->signal = signal;
	stream->timing = c->t;
	if (c->dsc) {
		stream->timing.flags.DSC = 1;
		stream->timing.dsc_cfg = c->dsc_cfg;
		pipe->stream_res.dsc = g_pool.dscs[c->pipe];
		link->dpcd_caps.fec_cap.raw = g_dpcd[0x090];
		memcpy(link->dpcd_caps.dsc_caps.dsc_basic_caps.raw, &g_dpcd[0x060], 16);
	}
	stream->phy_pix_clk = c->t.pix_clk_100hz / 10 / (c->y420 ? 2 : 1);
	stream->src = (struct rect){ 0, 0, w, h }; stream->dst = stream->src;
	stream->output_color_space = c->y420 ? COLOR_SPACE_YCBCR709 : COLOR_SPACE_SRGB;
	stream->qs_bit = 1;                      /* send "full range" in the AVI infoframe: a desktop */
	stream->content_type = DISPLAY_CONTENT_TYPE_GRAPHICS;
	stream->clamping.c_depth = c->t.display_color_depth; stream->clamping.pixel_encoding = c->t.pixel_encoding;
	stream->clamping.clamping_level = CLAMPING_FULL_RANGE;
	stream->out_transfer_func.type = TF_TYPE_BYPASS;
	stream->test_pattern.type = DP_TEST_PATTERN_VIDEO_MODE;

	plane->ctx = &g_ctx;
	plane->format = SURFACE_PIXEL_FORMAT_GRPH_ARGB8888;
	plane->address.type = PLN_ADDR_TYPE_GRAPHICS;
	plane->address.grph.addr.quad_part = SURFACE_PLACEHOLDER;
	plane->plane_size.surface_size = (struct rect){ 0, 0, c->surf_w ? c->surf_w : w, c->surf_h ? c->surf_h : h };
	plane->plane_size.surface_pitch = plane->plane_size.surface_size.width;
	plane->tiling_info.gfxversion = DcGfxAddr3;
	plane->tiling_info.gfx_addr3.swizzle = DC_ADDR3_SW_LINEAR;
	plane->rotation = ROTATION_ANGLE_0;
	plane->visible = true;
	plane->color_space = COLOR_SPACE_SRGB;
	plane->src_rect = stream->src; plane->dst_rect = stream->src; plane->clip_rect = stream->src;
	plane->scaling_quality.h_taps = 1; plane->scaling_quality.v_taps = 1;
	plane->scaling_quality.h_taps_c = 1; plane->scaling_quality.v_taps_c = 1;
	plane->in_transfer_func.type = TF_TYPE_BYPASS;
	plane->blend_tf.type = TF_TYPE_BYPASS;
	plane->hdr_mult = dc_fixpt_one;
	plane->global_alpha_value = 0xff;
	plane->update_bits.full_update = 1;

	ctx->stream_count = 1; ctx->streams[0] = stream;
	ctx->stream_status[0].plane_count = 1; ctx->stream_status[0].plane_states[0] = plane;
	pipe->stream = stream; pipe->plane_state = plane; pipe->pipe_idx = c->pipe;
	pipe->stream_res.tg = g_pool.timing_generators[c->pipe];
	pipe->stream_res.opp = g_pool.opps[c->pipe];
	pipe->stream_res.stream_enc = dcn401_stream_encoder_create(ENGINE_ID_DIGA + c->dig, &g_ctx);
	pipe->plane_res.hubp = g_pool.hubps[c->pipe];
	pipe->plane_res.dpp = g_pool.dpps[c->pipe];
	pipe->plane_res.mpcc_inst = c->pipe;
	pipe->clock_source = dcn401_clock_source_create(&g_ctx, &g_bios, CLOCK_SOURCE_ID_PLL0 + c->link,
							&clk_src_regs[c->link], false);
	if (c->train)           /* a DisplayPort stream's pixel clock is the DTO, as resource assignment has it */
		pipe->clock_source = g_pool.dp_clock_source =
			dcn401_clock_source_create(&g_ctx, &g_bios, CLOCK_SOURCE_ID_DP_DTO, &clk_src_regs[0], true);
	/* dce110_resource_build_pipe_hw_param: a 4:2:0 stream asks its PLL for half the pixel clock */
	pipe->stream_res.pix_clk_params.requested_pix_clk_100hz = c->t.pix_clk_100hz / (c->y420 ? 2 : 1);
	pipe->stream_res.pix_clk_params.flags.SUPPORT_YCBCR420 = c->y420;
	pipe->stream_res.pix_clk_params.encoder_object_id = eid.encoder;
	pipe->stream_res.pix_clk_params.signal_type = signal;
	pipe->stream_res.pix_clk_params.controller_id = CONTROLLER_ID_D0 + c->pipe;
	pipe->stream_res.pix_clk_params.color_depth = c->t.display_color_depth;
	pipe->stream_res.pix_clk_params.pixel_encoding = c->t.pixel_encoding;
	/* dcn401_build_pipe_pix_clk_params: a 4:2:0 TMDS stream enters the DIG two pixels a cycle */
	/* dcn401_build_pipe_pix_clk_params: a DisplayPort stream enters the DIG 2, 4 or 8 pixels a cycle above 1, 2 or 4
	 * times DPREFCLK (720 MHz, g_clk_mgr) */
	{
		const unsigned khz = c->t.pix_clk_100hz / 10, ref = 720000;

		pipe->stream_res.pix_clk_params.dio_se_pix_per_cycle = c->y420 ? 2 : !c->dp ? 1 :
			khz > 4 * ref ? 8 : khz > 2 * ref ? 4 : khz > ref ? 2 : 1;
	}
	pipe->pll_settings.actual_pix_clk_100hz = pipe->stream_res.pix_clk_params.requested_pix_clk_100hz;
	pipe->pll_settings.adjusted_pix_clk_100hz = pipe->stream_res.pix_clk_params.requested_pix_clk_100hz;
	pipe->pll_settings.calculated_pix_clk_100hz = pipe->stream_res.pix_clk_params.requested_pix_clk_100hz;

	pipe->global_sync = g_prog->stream_programming[1].global_sync;
	pipe->hubp_regs = *pp->pipe_regs[0];
	pipe->unbounded_req = pp->pipe_regs[0]->rq_regs.unbounded_request_enabled;
	pipe->det_buffer_size_kb = pp->pipe_regs[0]->det_size * 64;
	pipe->plane_res.bw.dppclk_khz = pp->min_clocks.dcn4x.dppclk_khz;
	/* Linux divides the DPP reference clock down to what this pipe needs. The frequency the firmware left
	 * that reference at is not known here, so the pipe takes it undivided: never too slow. */
	g_pool.dccg->ref_dppclk = pipe->plane_res.bw.dppclk_khz;

	/* hubbub32_force_wm_propagate_to_pipes rewrites the urgency watermark with the value Linux last programmed,
	 * to push it to the new pipe. Here that value is whatever the register holds: leave it unknown, so the
	 * step comes out as "write back what you read". */
	g_pool.ref_clocks.dchub_ref_clock_inKhz = 50000;
	TO_DCN20_HUBBUB(g_pool.hubbub)->watermarks.a.urgent_ns = rec_peek(0) ? 0x7fffffff : 0;

	if (!resource_build_scaling_params(pipe)) { fprintf(stderr, "scaling params failed\n"); exit(1); }
	g_old = old; g_new = ctx; g_pipe = pipe;
}

static void sequence(const struct cfg *c)
{
	struct dc *dc = &g_dc;
	struct pipe_ctx *pipe = g_pipe;
	struct hubp *hubp = pipe->plane_res.hubp;

	/* Linux skips the TMDS divider write when the register already holds the divider it wants: /4 for RGB,
	 * /2 (field 0) for 4:2:0. Take the write path: the pipe may last have run the other one (a 4:2:0 Dell
	 * ran at 30 Hz on a divider RGB had left, card, 2026-11-04). */
	rec_poke_field(dccg_regs.OTG_PIXEL_RATE_DIV, OTG_PIXEL_RATE_DIV__OTG0_TMDS_PIXEL_RATE_DIV_MASK << (c->pipe * 5),
		       c->y420 ? OTG_PIXEL_RATE_DIV__OTG0_TMDS_PIXEL_RATE_DIV_MASK << (c->pipe * 5) : 0);

	{	/* Status the code reads back. Each is settled to the state the block is in once the step before it
		 * has taken effect; the plan waits for that state where Linux polls for it. */
		struct dcn20_opp *opp = TO_DCN20_OPP(pipe->stream_res.opp);
		struct dcn10_stream_encoder *se = DCN10STRENC_FROM_STRENC(pipe->stream_res.stream_enc);
		struct dcn31_vpg *vpg = DCN31_VPG_FROM_VPG(pipe->stream_res.stream_enc->vpg);
		struct dcn401_mpc *mpc = TO_DCN401_MPC(g_pool.mpc);

		/* dcn20_wait_for_blank_complete: up to 1000 x 100 us for the pattern generator to latch */
		rec_poll("opp2_dpg_is_pending", opp->regs->DPG_STATUS, DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING_MASK, 0, 100000);
		/* enc35_reset_fifo waits for RESET_DONE only while the front-end symbol clock runs: it does, the
		 * step before enabled it */
		rec_poke_field(se->regs->DIG_FE_CLK_CNTL, DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON_MASK,
			       DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON_MASK);
		if (c->train) {
			/* the encoder was never used: it sends no secondary packets, so the info-packet update
			 * leaves the secondary stream off (as in the "dp" scenario) */
			rec_poke(se->regs->DP_SEC_CNTL, 0);
			rec_poke(se->regs->DP_SEC_METADATA_TRANSMISSION, 0);
		}
		/* vpg31_poweron returns early when the packet memory is already awake; take the wake-up path */
		rec_poke_field(vpg->regs->VPG_MEM_PWR, VPG0_VPG_MEM_PWR__VPG_GSP_MEM_PWR_STATE_MASK,
			       VPG0_VPG_MEM_PWR__VPG_GSP_MEM_PWR_STATE_MASK);
		/* dcn401_set_mcm_luts picks the LUT bank not in use: none is, the blender was never lit */
		rec_poke_field(mpc->mpc_regs->MPCC_MCM_1DLUT_CONTROL[c->pipe], 0xffffffff, 0);
		rec_poke_field(mpc->mpc_regs->MPCC_MCM_SHAPER_CONTROL[c->pipe], 0xffffffff, 0);
		rec_poke_field(mpc->mpc_regs->MPCC_MCM_3DLUT_MODE[c->pipe], 0xffffffff, 0);
		/* mpc1_mpc_init_single_inst clears the output mux of whatever OPP the blender was feeding. It feeds
		 * none: the plan refuses to run otherwise (Pipe2::Precondition), since a blender that feeds an OPP
		 * belongs to a lit pipe. */
		rec_poke_field(TO_DCN10_MPC(g_pool.mpc)->mpc_regs->MPCC_OPP_ID[c->pipe], 0xf, 0xf);
		rec_mark("require:mpcc_opp_id_none", TO_DCN10_MPC(g_pool.mpc)->mpc_regs->MPCC_OPP_ID[c->pipe]);
		if (pipe->stream_res.dsc) {
			/* dsc401_enable asserts that the engine is not running for another OPP: on a dark pipe it is
			 * off and forwards nothing, which the plan requires (an engine in use belongs to a lit pipe) */
			struct dcn401_dsc *d = TO_DCN401_DSC(pipe->stream_res.dsc);

			rec_poke_field(d->dsc_regs->DSC_TOP_CONTROL, d->dsc_mask->DSC_CLOCK_EN, 0);
			rec_poke_field(d->dsc_regs->DSCRM_DSC_FORWARD_CONFIG, d->dsc_mask->DSCRM_DSC_FORWARD_EN, 0);
			rec_mark("require:dsc_off", d->dsc_regs->DSCRM_DSC_FORWARD_CONFIG);
		}
	}

	/* What dcn401_init_hw does once for every block, lit or not, repeated for the blocks this pipe uses in case
	 * the firmware's own start-up skipped the ones it did not light. The link encoder's hw_init is left out:
	 * it programs the AUX channel, which a TMDS link does not use and whose instance is board wiring. */
	rec_mark("begin:init", 0);
	if (pipe->stream_res.dsc) {
		/* dcn32_update_dsc_pg, as the commit runs it before the stream: the engine taken out of the power gating
		 * dcn401_init_hw leaves every DSC domain in (DOMAIN16 + engine; DOMAIN_PGFSM_PWR_STATUS 2 = gated), through the
		 * IP request window, open as the GOP leaves it (the plan requires that further down) */
		const uint32_t status[4] = { hwseq_reg.DOMAIN16_PG_STATUS, hwseq_reg.DOMAIN17_PG_STATUS,
					     hwseq_reg.DOMAIN18_PG_STATUS, hwseq_reg.DOMAIN19_PG_STATUS };
		const int inst = pipe->stream_res.dsc->inst;

		rec_poke_field(status[inst], DOMAIN16_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK, 2u << 30);
		rec_poke_field(hwseq_reg.DC_IP_REQUEST_CNTL, DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK,
			       DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK);
		rec_poll("dcn32_dsc_pg_control", status[inst], DOMAIN16_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS_MASK, 0, 1000);
		g_new->res_ctx.is_dsc_acquired[inst] = true;
		dcn32_update_dsc_pg(dc, g_new, false);
	}
	g_pool.mpc->funcs->mpc_init_single_inst(g_pool.mpc, c->pipe);
	pipe->stream_res.tg->funcs->tg_init(pipe->stream_res.tg);

	/* dc_commit_state_no_check: dce110_apply_ctx_to_hw for the one new stream ... */
	rec_mark("begin:stream", 0);
	if (dce110_apply_single_controller_ctx_to_hw(pipe, g_new, dc) != DC_OK) { fprintf(stderr, "stream failed\n"); exit(1); }

	/* ... then the planes of the new context under the update lock. dcn401_program_front_end_for_ctx is not
	 * called whole: it also locks and reprograms every other lit pipe, and this plan leaves those alone. */
	dc->hwss.detect_pipe_changes(g_old, g_new, &g_old->res_ctx.pipe_ctx[c->pipe], pipe);
	/* dcn20_power_on_plane_resources opens the IP request window only when it finds it closed, and closes it
	 * again. The firmware leaves it open (card survey, 2026-10-04), and then Linux does not touch it; the
	 * plan requires it open, since the power-up write is ignored through a closed window. */
	rec_poke_field(hwseq_reg.DC_IP_REQUEST_CNTL, DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK,
		       DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK);
	rec_mark("require:ip_request_open", hwseq_reg.DC_IP_REQUEST_CNTL);
	rec_mark("begin:plane", 0);
	dc->hwss.pipe_control_lock(dc, pipe, true);
	dcn401_program_pipe(dc, pipe, g_new);
	dc->hwss.pipe_control_lock(dc, pipe, false);
	/* dcn401_post_unlock_program_front_end: let the enable latch before anyone flips */
	rec_mark("wait:flip", TO_DCN20_HUBP(hubp)->hubp_regs->DCSURF_FLIP_CONTROL);

	/* Display sleep and wake of the stream just lit: what dc does for DPMS off and on, with the timing
	 * generator left running. By then the committed state is the current one, which is where
	 * dcn401_disable_link_output looks for the pipe whose PHY clock it reprograms. */
	dc->current_state = g_new;
	rec_mark("begin:sleep", 0);
	link_set_dpms_off(pipe);
	rec_mark("begin:wake", 0);
	if (link_set_dpms_on(g_new, pipe) != DC_DPMS_SUCCESS) { fprintf(stderr, "wake failed\n"); exit(1); }
	rec_mark("end", 0);
}

/* ---- the "dp" scenario: a new timing on a DisplayPort stream whose link stays trained ----
 *
 * Linux has no such sequence: for a timing change it takes the link down and trains it again. These are the
 * functions it runs on the stream between those two points, in its order (dce110_blank_stream, the timing
 * generator off, dcn401_enable_stream_timing, the MSA, dcn401_enable_stream, dcn401_unblank_stream). The
 * output is a reference for test/atomdump.cpp to hold src/modeset.cpp's DisplayPort plan against. */
static void sequence_dp(const struct cfg *c)
{
	struct dc *dc = &g_dc;
	struct pipe_ctx *pipe = g_pipe;
	struct dcn10_stream_encoder *se = DCN10STRENC_FROM_STRENC(pipe->stream_res.stream_enc);
	struct dcn20_opp *opp = TO_DCN20_OPP(pipe->stream_res.opp);

	/* the stream is running (dp_blank returns at once otherwise) and the front-end clock is on */
	rec_poke_field(se->regs->DP_VID_STREAM_CNTL, DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
		       DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK);
	rec_poke_field(se->regs->DIG_FE_CLK_CNTL, DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON_MASK,
		       DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON_MASK);
	rec_poll("opp2_dpg_is_pending", opp->regs->DPG_STATUS, DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING_MASK, 0, 100000);
	/* no secondary packets are being sent (the firmware's DP_SEC_CNTL reads 0 on the card): the info-packet
	 * update then leaves the secondary stream off */
	rec_poke(se->regs->DP_SEC_CNTL, 0);
	rec_poke(se->regs->DP_SEC_METADATA_TRANSMISSION, 0);

	rec_mark("begin:blank", 0);
	dc->hwss.blank_stream(pipe);
	pipe->stream_res.tg->funcs->disable_crtc(pipe->stream_res.tg);
	rec_mark("begin:timing", 0);
	if (dcn401_enable_stream_timing(pipe, g_new, dc) != DC_OK) { fprintf(stderr, "stream timing failed\n"); exit(1); }
	rec_mark("begin:stream", 0);
	resource_build_info_frame(pipe);
	dc->hwss.enable_stream(pipe);
	dc->hwss.unblank_stream(pipe, &pipe->stream->link->cur_link_settings);
	rec_mark("end", 0);
}

/* ---- the "drr" scenario: variable refresh (FreeSync, DisplayPort Adaptive-Sync) on a lit stream ----
 *
 * What amdgpu does when a stream's refresh becomes variable (dc_stream_adjust_vmin_vmax -> hwss.set_drr), when it
 * becomes fixed again, and after each flip while it is variable (the manual trigger in dc.c's commit_planes_for_stream,
 * which ends the stretched blank). V totals 0x2001 (minimum) and 0x3001 (maximum) mark the values the driver puts in for
 * the mode's own (src/drr.cpp). */
static void sequence_drr(void)
{
	struct dc *dc = &g_dc;
	struct pipe_ctx *pipe = g_pipe;
	struct timing_generator *tg = pipe->stream_res.tg;
	struct dc_crtc_timing_adjust on = { .v_total_min = 0x2001, .v_total_max = 0x3001 }, off = { 0 };

	rec_mark("begin:on", 0);
	dc->hwss.set_drr(&pipe, 1, on);
	rec_mark("begin:off", 0);
	dc->hwss.set_drr(&pipe, 1, off);
	rec_mark("begin:trigger", 0);
	tg->funcs->program_manual_trigger(tg);
	rec_mark("end", 0);
}

/* ---- the "audio" scenario: audio on a lit DisplayPort stream (docs/audio-plan.md) ----
 *
 * What amdgpu does for a stream with audio, on a pipe whose stream already runs: the Azalia endpoint's one-time
 * setup (dce_aud_hw_init), the endpoint's sink description from the EDID (dce110_apply_single_controller_ctx_to_hw:
 * build_audio_output, the link's setup_audio_output = the stream encoder's dp_audio_setup, az_configure), the audio
 * wall-clock DTO (dce110_setup_audio_dto's DP branch: DCN 4.01's DCCG has no set_audio_dtbclk_dto, so
 * wall_dto_setup) and the enable (dce110_enable_audio_stream: az_enable, the link's enable_audio_packet). The sink's
 * audio is PIPEGEN_AUDIO="<mfr id> <product id> <SAD, 3 bytes hex> <speaker allocation> <name>", as
 * dm_helpers_parse_edid_caps and amdgpu_dm_fill_audio_info read them (ids little endian from EDID bytes 8-11);
 * default: the rig's Samsung (Odyssey G70D: LPCM 2 channels, 32/44.1/48 kHz, 16/20/24 bit, front left/right). */
static void sequence_audio(const struct cfg *c)
{
	struct dc *dc = &g_dc;
	struct pipe_ctx *pipe = g_pipe;
	struct dc_stream_state *stream = pipe->stream;
	struct dcn10_stream_encoder *se = DCN10STRENC_FROM_STRENC(pipe->stream_res.stream_enc);
	const char *spec = getenv("PIPEGEN_AUDIO");
	unsigned mfr = 0x2d4c, product = 0xe06f, sad = 0x090707, speakers = 0x01;
	char name[AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS] = "Odyssey G70D";

	if (spec && sscanf(spec, "%x %x %x %x %19[^\n]", &mfr, &product, &sad, &speakers, name) != 5) {
		fprintf(stderr, "bad PIPEGEN_AUDIO\n"); exit(2);
	}
	struct audio *audio = dcn401_create_audio(&g_ctx, 0);   /* the first free audio: endpoint 0 */
	const struct dce_audio_registers *ar = ((struct dce_audio *)audio)->regs;   /* (DCE_AUD is dce_audio.c's) */
	rec_indirect(ar->AZALIA_F0_CODEC_ENDPOINT_INDEX, ar->AZALIA_F0_CODEC_ENDPOINT_DATA);
	g_pool.audios[0] = audio; g_pool.audio_count = 1;
	pipe->stream_res.audio = audio;
	stream->audio_info.manufacture_id = mfr;
	stream->audio_info.product_id = product;
	memcpy(stream->audio_info.display_name, name, sizeof(name));
	stream->audio_info.mode_count = 1;
	stream->audio_info.modes[0].format_code = (enum audio_format_code)((sad >> 19) & 0xf);
	stream->audio_info.modes[0].channel_count = ((sad >> 16) & 7) + 1;
	stream->audio_info.modes[0].sample_rates.all = (sad >> 8) & 0x7f;
	stream->audio_info.modes[0].sample_size = sad & 0xff;
	stream->audio_info.flags.all = speakers;

	/* the stream is running (as sequence_dp) and no secondary packets are being sent yet */
	rec_poke_field(se->regs->DP_VID_STREAM_CNTL, DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
		       DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK);
	rec_poke(se->regs->DP_SEC_CNTL, 0);

	pipe->link_config.dp_link_settings = stream->link->cur_link_settings;   /* the link as it runs (make_state) */
	const struct link_hwss *link_hwss = get_link_hwss(stream->link, &pipe->link_res);
	struct audio_output out = { 0 };

	rec_mark("begin:init", 0);
	audio->funcs->hw_init(audio);
	rec_mark("begin:configure", 0);
	build_audio_output(g_new, pipe, &out);
	link_hwss->setup_audio_output(pipe, &out, audio->inst);
	audio->funcs->az_configure(audio, stream->signal, &out.crtc_info, &stream->audio_info, &out.dp_link_info);
	rec_mark("begin:dto", 0);
	audio->funcs->wall_dto_setup(audio, stream->signal, &out.crtc_info, &out.pll_info);
	rec_mark("begin:enable", 0);
	dc->hwss.enable_audio_stream(pipe);
	rec_mark("end", 0);
}

/* Two runs, unknown registers reading all-zeros then all-ones: a full write whose value differs between the runs
 * carries bits read back from the register, so it becomes an update that leaves those bits alone. A different
 * sequence of events means a read steered the code; that has to be settled with rec_poke(), not guessed. */
static struct ev run0[8192]; static int n0;

static void emit(void)
{
	static const char *k[] = { "W", "U", "WAIT", "DELAY", "MARK", "COPY" };
	if (n0 != nev) {
		fprintf(stderr, "DIVERGED: %d events vs %d\n", n0, nev);
		for (int i = 0; i < n0 && i < nev; i++)
			if (run0[i].kind != evs[i].kind || run0[i].addr != evs[i].addr) {
				for (int j = i > 2 ? i - 2 : 0; j < i + 4; j++)
					fprintf(stderr, "  %d: run0 %d %08x %08x %s | run1 %d %08x %08x %s\n", j, run0[j].kind, run0[j].addr,
						run0[j].value, run0[j].label, evs[j].kind, evs[j].addr, evs[j].value, evs[j].label);
				break;
			}
		exit(1);
	}
	for (int i = 0; i < nev; i++) {
		struct ev a = run0[i], b = evs[i];
		if (a.kind != b.kind || a.addr != b.addr || a.mask != b.mask || a.arg != b.arg) {
			fprintf(stderr, "DIVERGED at event %d (%s / %s)\n", i, a.label, b.label); exit(1);
		}
		if ((a.kind == EV_WRITE || a.kind == EV_UPDATE) && a.value != b.value) {
			a.kind = EV_UPDATE; a.mask &= ~(a.value ^ b.value); a.value &= a.mask;
		} else if (a.value != b.value) {
			fprintf(stderr, "DIVERGED value at event %d (%s)\n", i, a.label); exit(1);
		}
		if (a.kind == EV_MARK) printf("MARK  %s 0x%x\n", a.label, a.arg);
		else if (a.kind == EV_COPY) printf("COPY  %u 0x%04x %u 0x%04x ; %s\n", (a.addr >> 24) - 1, a.addr & 0xffffff,
						 (a.arg >> 24) - 1, a.arg & 0xffffff, a.label);
		else if (a.kind == EV_DELAY) printf("DELAY %u ; %s\n", a.arg, a.label);
		else printf("%-5s %u 0x%04x %08x %08x %u ; %s\n", k[a.kind], (a.addr >> 24) - 1,
			    a.addr & 0xffffff, a.mask, a.value, a.arg, a.label);
	}
}

/* ---- the "init" scenario: dcn401_init_hw on display hardware with nothing running ----
 *
 * What amdgpu does to the display hardware when it starts it, here as after a power loss (resume from S3): the
 * card's four connectors as links (DisplayPort on links 0 and 1, HPD1-2; HDMI on 2 and 3, HPD3-4), every pipe
 * off, an empty committed state, and the clocks left to the kext's own SMU requests (src/clk.cpp): the clock
 * manager's update_clocks is a mark. The part of lighting the displays from nothing that comes before
 * lighting a pipe. */
static void clk_update(struct clk_mgr *c, struct dc_state *s, bool safe) { rec_mark("clk:update_clocks", 0); }
static int clk_dentist(struct clk_mgr *c) { return 0; }
static struct clk_mgr_funcs init_clk_funcs = { .update_clocks = clk_update, .get_dispclk_from_dentist = clk_dentist };
static struct clk_bw_params g_bw;
/* hubbub401_init_watermarks copies watermark set A into the others: recorded as copies (rec_copy_mode). */
static struct hubbub_funcs g_hubbub_funcs;
static void (*g_init_watermarks)(struct hubbub *);
static void init_watermarks_as_copies(struct hubbub *hubbub)
{
	rec_copy_mode = 1;
	g_init_watermarks(hubbub);
	rec_copy_mode = 0;
}
/* dc.c's: this card has no eDP panel. */
void dc_get_edp_links(const struct dc *dc, struct dc_link **edp_links, unsigned int *edp_num) { *edp_num = 0; }
/* dc_link_exports.c's (that file also has dc_get_edp_links): what build_audio_output asks of the link */
uint32_t dc_link_bandwidth_kbps(const struct dc_link *link, const struct dc_link_settings *link_settings)
{
	return link->dc->link_srv->dp_link_bandwidth_kbps(link, link_settings);
}
/* dc.c's: on this card it only turns idle power features (MALL as a cache, through DMUB) off and on again,
 * nothing a display needs to light: a mark. */
void dc_allow_idle_optimizations_internal(struct dc *dc, bool allow, char const *caller_name)
{
	rec_mark(allow ? "dc:allow_idle_optimizations:on" : "dc:allow_idle_optimizations:off", allow);
}

static void sequence_init(void)
{
	struct dc *dc = &g_dc;

	dc->current_state = calloc(1, sizeof(*dc->current_state));
	dc->link_srv = link_create_link_service();
	for (int l = 0; l < 4; l++) {
		struct dc_link *link = calloc(1, sizeof(*link));
		struct encoder_init_data eid = { 0 };
		const bool dp = l < 2;

		eid.channel = CHANNEL_ID_DDC1 + l;
		eid.hpd_source = HPD_SOURCEID1 + l;
		eid.transmitter = TRANSMITTER_UNIPHY_A + l;
		eid.encoder.type = OBJECT_TYPE_ENCODER;
		eid.encoder.id = ENCODER_ID_INTERNAL_UNIPHY + l / 2;
		eid.encoder.enum_id = ENUM_ID_1 + (l & 1);
		eid.connector.type = OBJECT_TYPE_CONNECTOR;
		eid.connector.id = dp ? CONNECTOR_ID_DISPLAY_PORT : CONNECTOR_ID_HDMI_TYPE_A;
		eid.connector.enum_id = ENUM_ID_1 + (l & 1);
		eid.ctx = &g_ctx;
		link->dc = dc; link->ctx = &g_ctx; link->link_index = l;
		link->connector_signal = dp ? SIGNAL_TYPE_DISPLAY_PORT : SIGNAL_TYPE_HDMI_TYPE_A;
		link->ep_type = DISPLAY_ENDPOINT_PHY;
		link->link_enc = dcn401_link_encoder_create(&g_ctx, &eid);
		link->link_id = eid.connector;
		link->ddc_hw_inst = l;
		dc->links[l] = link;
	}
	dc->link_count = 4;
	for (int i = 0; i < 4; i++) {
		g_pool.dscs[i] = dcn401_dsc_create(&g_ctx, i);
		g_pool.clock_sources[i] = dcn401_clock_source_create(&g_ctx, &g_bios, CLOCK_SOURCE_COMBO_PHY_PLL0 + i,
								     &clk_src_regs[i], false);
	}
	g_pool.clk_src_count = DCN401_CLK_SRC_TOTAL;
	g_pool.dp_clock_source = dcn401_clock_source_create(&g_ctx, &g_bios, CLOCK_SOURCE_ID_DP_DTO, &clk_src_regs[0], true);
	g_clk_mgr.funcs = &init_clk_funcs;
	g_clk_mgr.bw_params = &g_bw;
	g_hubbub_funcs = *g_pool.hubbub->funcs;
	g_init_watermarks = g_hubbub_funcs.init_watermarks;
	g_hubbub_funcs.init_watermarks = init_watermarks_as_copies;
	g_pool.hubbub->funcs = &g_hubbub_funcs;
	g_bios.fw_info.pll_info.crystal_frequency = 100000;   /* the card's reference clock, kHz */

	/* Nothing running, as after a power loss: every timing generator and DIG back end off (enable bits at
	 * their reset value, 0), the IP request window closed, no VBIOS display (g_cold: not "accelerated"). */
	rec_poke_field(hwseq_reg.DC_IP_REQUEST_CNTL, DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK, 0);
	for (int i = 0; i < 4; i++) {
		struct optc *tg = DCN10TG_FROM_TG(g_pool.timing_generators[i]);
		struct dcn10_link_encoder *enc = TO_DCN10_LINK_ENC(dc->links[i]->link_enc);

		rec_poke_field(tg->tg_regs->OTG_CONTROL, tg->tg_mask->OTG_MASTER_EN, 0);
		rec_poke_field(enc->link_regs->DIG_BE_CLK_CNTL, enc->link_mask->DIG_BE_CLK_EN, 0);
		rec_poke_field(enc->link_regs->DIG_BE_EN_CNTL, enc->link_mask->DIG_BE_ENABLE, 0);
		/* no blender feeds an OPP */
		rec_poke_field(TO_DCN10_MPC(g_pool.mpc)->mpc_regs->MPCC_OPP_ID[i], 0xf, 0xf);
		/* hubp2_set_blank_regs waits for the HUBP's requests to drain unless it reads 0 (power gated): it is
		 * not gated, and the plan waits as Linux polls */
		struct dcn20_hubp *hubp = TO_DCN20_HUBP(g_pool.hubps[i]);
		rec_poll("hubp2_set_blank_regs", hubp->hubp_regs->DCHUBP_CNTL, hubp->hubp_mask->HUBP_NO_OUTSTANDING_REQ,
			 hubp->hubp_mask->HUBP_NO_OUTSTANDING_REQ, 100000);
	}
	g_cold = true;

	/* On a warm card the timing generators lit before are still running (a pipe put to sleep keeps its own):
	 * stopped first the way Linux stops one (optc401_disable_crtc), recorded on its own for each OTG. */
	for (int i = 0; i < 4; i++) {
		char mark[16];
		snprintf(mark, sizeof(mark), "begin:otgoff%d", i);
		rec_mark(mark, 0);
		g_pool.timing_generators[i]->funcs->disable_crtc(g_pool.timing_generators[i]);
	}

	rec_mark("begin:init_hw", 0);
	dc->hwss.init_hw(dc);
	rec_mark("end", 0);
}

static bool g_dsc;     /* the "dsc" scenario: setup() creates the DSC engines */

static void setup(void)
{
	struct dc_context *ctx = &g_ctx;
	memset(&g_dc, 0, sizeof(g_dc)); memset(&g_pool, 0, sizeof(g_pool));
	g_ctx.dc = &g_dc; g_ctx.dc_bios = &g_bios; g_ctx.dcn_reg_offsets = dcn_bases; g_ctx.nbio_reg_offsets = nbio_bases;
	g_ctx.dce_version = DCN_VERSION_4_01;
	g_bios.funcs = &bios_funcs; g_bios.ctx = &g_ctx; g_bios.fw_info_valid = true;
	g_dc.ctx = &g_ctx; g_dc.debug = debug_defaults_drv; g_dc.res_pool = &g_pool;
	g_dc.caps.max_v_total = (1 << 15) - 1;
	g_dc.clk_mgr = &g_clk_mgr; g_clk_mgr.ctx = ctx;
	g_clk_mgr.funcs = &no_clk_funcs;        /* the clocks stay the firmware's: nothing is asked of the manager */
	g_clk_mgr.dprefclk_khz = 720000;        /* the DCN 4.01 bounding box's dprefclk_mhz */
#undef REG_STRUCT
#define REG_STRUCT dccg_regs
	dccg_regs_init();
	g_pool.dccg = dccg401_create(ctx, &dccg_regs, &dccg_shift, &dccg_mask);
#undef REG_STRUCT
#define REG_STRUCT clk_src_regs
	clk_src_regs_init(0, A), clk_src_regs_init(1, B), clk_src_regs_init(2, C), clk_src_regs_init(3, D);

	g_pool.res_cap = &res_cap_dcn4_01; g_pool.funcs = &dcn401_res_pool_funcs;
	g_pool.pipe_count = 4; g_pool.mpcc_count = 4; g_pool.timing_generator_count = 4;
	for (int i = 0; i < 4; i++) {
		g_pool.hubps[i] = dcn401_hubp_create(ctx, i);
		g_pool.dpps[i] = dcn401_dpp_create(ctx, i);
		g_pool.opps[i] = dcn401_opp_create(ctx, i);
		g_pool.timing_generators[i] = dcn401_timing_generator_create(ctx, i);
	}
	if (g_dsc) {    /* the "dsc" scenario's engines; their constructor's clock-gating writes are not the pipe's */
		const int keep = nev;
		for (int i = 0; i < 4; i++)
			g_pool.dscs[i] = dcn401_dsc_create(ctx, i);
		nev = keep;
	}
	for (int i = 0; i < 4; i++)        /* dcn401_init_hw */
		g_pool.opps[i]->mpc_tree_params.opp_id = g_pool.opps[i]->inst;
	g_pool.mpc = dcn401_mpc_create(ctx, 4, 4);
	g_pool.hubbub = dcn401_hubbub_create(ctx);
	g_dc.hwseq = dcn401_hwseq_create(ctx);
	dcn401_hw_sequencer_init_functions(&g_dc);
	g_dc.caps.color.dpp.dcn_arch = 1; g_dc.caps.color.dpp.icsc = 1; g_dc.caps.color.dpp.post_csc = 1;
	g_dc.caps.color.dpp.gamma_corr = 1; g_dc.caps.color.mpc.gamut_remap = 1; g_dc.caps.color.mpc.ogam_ram = 1;
	g_dc.caps.color.mpc.ocsc = 1; g_dc.caps.color.mpc.preblend = true; g_dc.caps.color.mpc.num_3dluts = 4;
	g_dc.caps.post_blend_color_processing = true; g_dc.caps.vtotal_limited_by_fp2 = true;
	g_dc.config.use_spl = true; g_dc.config.prefer_easf = true;
}

/* ---- "dsc": the configuration amdgpu chooses for the stream, as apply_dsc_policy_for_stream asks
 * dc_dsc_compute_config for it: the sink's DPCD 0x060 caps (no branch device), the default options, the link (HBR3 x4
 * or PIPEGEN_DP_LINK) with FEC
 * (dp_link_bandwidth_kbps), DCN 4.01's encoder with the bounding box's top DISPCLK (2 GHz) and DSCCLK a third of it
 * (dcn401_get_max_clock_khz), ODM combine up to 4 (dcn401_resource_construct). */
static unsigned dsc_max_clock(struct clk_mgr *m, enum clk_type t) { return t == CLK_TYPE_DSCCLK ? 2000000 / 3 : 2000000; }

static void dsc_choose(struct cfg *c)
{
	static const uint8_t no_branch[3];
	static struct clk_mgr_funcs funcs = { .get_max_clock_khz = dsc_max_clock };
	struct dsc_dec_dpcd_caps caps = { 0 };
	struct dc_dsc_config_options opt;
	const char *other = getenv("PIPEGEN_DP_LINK");          /* as make_state: "lanes rate", else HBR3 x4 */
	unsigned lanes = 4, rate = LINK_RATE_HIGH3;

	if (other && sscanf(other, "%u %u", &lanes, &rate) != 2) { fprintf(stderr, "bad PIPEGEN_DP_LINK\n"); exit(2); }
	g_dsc_lanes = lanes;
	g_dsc_rate = rate == LINK_RATE_HIGH3 ? dml2_dp_rate_hbr3 : rate == LINK_RATE_HIGH2 ? dml2_dp_rate_hbr2 : dml2_dp_rate_hbr;
	const uint32_t kbps = rate * LINK_RATE_REF_FREQ_IN_KHZ * BITS_PER_DP_BYTE * lanes / 10000 *
			      (DATA_EFFICIENCY_8b_10b_x10000 / 100 * DATA_EFFICIENCY_8b_10b_FEC_EFFICIENCY_x100);

	setup();
	sink_reset(c);
	g_clk_mgr.funcs = &funcs;
	g_dc.caps.max_odm_combine_factor = 4;
	dc_dsc_get_default_config_option(&g_dc, &opt);
	opt.force_dsc_when_not_needed = getenv("PIPEGEN_DSC_FORCE") != NULL;   /* a test: DSC on a stream that fits without */
	if (!dc_dsc_parse_dsc_dpcd(&g_dc, &g_dpcd[0x060], no_branch, &caps) ||
	    (!opt.force_dsc_when_not_needed && dc_bandwidth_in_kbps_from_timing(&c->t, DC_LINK_ENCODING_DP_8b_10b) <= kbps) ||
	    !dc_dsc_compute_config(g_pool.dscs[c->pipe], &caps, &opt, kbps, &c->t, DC_LINK_ENCODING_DP_8b_10b, &c->dsc_cfg)) {
		fprintf(stderr, "DSC: not used on this link (the stream fits uncompressed, or no configuration fits)\n");
		exit(1);
	}
	fprintf(stderr, "DSC: %u x %u slices of %u lines, %u.%04u bpp, version 1.%u, line buffer %u\n", c->dsc_cfg.num_slices_h,
		c->dsc_cfg.num_slices_v, c->t.v_addressable / c->dsc_cfg.num_slices_v, c->dsc_cfg.bits_per_pixel / 16,
		c->dsc_cfg.bits_per_pixel % 16 * 625, c->dsc_cfg.version_minor, c->dsc_cfg.linebuf_depth);
}

int main(int argc, char **argv)
{
	struct cfg c = { 0 };
	unsigned v[16];
	bool verbose;

	if (argc > 1 && !strcmp(argv[1], "init")) {
		for (int run = 0; run < 2; run++) {
			rec_reset(run ? 0xffffffff : 0);
			rec_log_reads = argc > 2 && run == 0;
			setup();
			sequence_init();
			if (run == 0) { memcpy(run0, evs, sizeof(struct ev) * nev); n0 = nev; }
		}
		emit();
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "drr")) { c.dp = c.drr = true; argv++; argc--; }
	else if (argc > 1 && !strcmp(argv[1], "audio")) { c.dp = c.audio = true; argv++; argc--; }
	else if (argc > 1 && !strcmp(argv[1], "dp")) { c.dp = true; argv++; argc--; }
	else if (argc > 1 && !strcmp(argv[1], "dpdual")) { c.dual = true; argv++; argc--; }
	if (argc > 1 && !strcmp(argv[1], "y420")) { c.y420 = true; argv++; argc--; }
	if (argc > 4 && !strcmp(argv[1], "dplink")) {
		c.dp = c.train = g_link_detail = true;
		c.sink_kind = argv[2][0]; c.sink_swing = atoi(argv[3]); c.sink_pre = atoi(argv[4]);
		argv += 4; argc -= 4;
		if (argc > 1 && !strcmp(argv[1], "dsc")) { c.dsc = g_dsc = true; argv++; argc--; }
	}
	if (argc > 3 && !strcmp(argv[1], "on")) {
		c.surf_w = (unsigned)strtoul(argv[2], NULL, 0); c.surf_h = (unsigned)strtoul(argv[3], NULL, 0);
		argv += 3; argc -= 3;
	}
	verbose = argc > 17;
	if (argc < 17) {
		fprintf(stderr, "usage: pipegen [dp | drr | audio | dpdual | dplink K S P] [on W H] pipe dig link hpd  hactive hfront hsync hback  vactive vfront vsync "
				"vback  khz hpositive vpositive vic [verbose]\n  hpd counts from 1, as the VBIOS path records do\n"
				"  dp: the reference for retiming a lit DisplayPort stream instead of the second-pipe plan\n"
				"  drr: variable refresh on, off, and the trigger after a flip, on a lit DisplayPort stream\n"
				"  audio: audio on a lit DisplayPort stream (PIPEGEN_AUDIO: the sink's audio, see sequence_audio)\n"
				"  dpdual: an HDMI stream on a DisplayPort connector (a passive DP++ adapter)\n"
				"  y420: (after dpdual, or alone for HDMI) the HDMI stream in YCbCr 4:2:0\n"
				"  dplink K S P [dsc]: light a DisplayPort stream, training its link against a simulated sink of kind\n"
				"                K (a, b, c, d) that wants voltage swing S and pre-emphasis P; dsc (kind d): compressed\n"
				"  on W H: the plane shows the top-left of a W x H surface, as after a mode switch\n");
		return 2;
	}
	for (int i = 0; i < 16; i++) v[i] = (unsigned)strtoul(argv[i + 1], NULL, 0);
	c.pipe = v[0]; c.dig = v[1]; c.link = v[2]; c.hpd = v[3] - 1;
	c.t.h_addressable = v[4]; c.t.h_front_porch = v[5]; c.t.h_sync_width = v[6]; c.t.h_total = v[4] + v[5] + v[6] + v[7];
	c.t.v_addressable = v[8]; c.t.v_front_porch = v[9]; c.t.v_sync_width = v[10]; c.t.v_total = v[8] + v[9] + v[10] + v[11];
	c.t.pix_clk_100hz = v[12] * 10;
	c.t.flags.HSYNC_POSITIVE_POLARITY = v[13] != 0; c.t.flags.VSYNC_POSITIVE_POLARITY = v[14] != 0;
	c.t.vic = v[15];
	c.t.aspect_ratio = v[4] * 9 == v[8] * 16 ? ASPECT_RATIO_16_9 : v[4] * 3 == v[8] * 4 ? ASPECT_RATIO_4_3 : ASPECT_RATIO_NO_DATA;
	c.t.pixel_encoding = c.y420 ? PIXEL_ENCODING_YCBCR420 : PIXEL_ENCODING_RGB;
	/* PIPEGEN_BPC=10: 10 bits a component on the wire (Linux's own choice for the Samsung at 4K144 with DSC) */
	c.t.display_color_depth = getenv("PIPEGEN_BPC") && atoi(getenv("PIPEGEN_BPC")) == 10 ? COLOR_DEPTH_101010 : COLOR_DEPTH_888;
	if (c.pipe < (c.dp ? 0 : 1) || c.pipe > 3 || c.dig > 3 || c.link > 3 || c.hpd > 5 || !v[12]) { fprintf(stderr, "bad configuration\n"); return 2; }

	/* The display the firmware lit: the 3840x2160@60 CVT-RB raster the GOP uses on a 4K sink, the heaviest
	 * companion this card's firmware sets up. It only enters DML's shared-clock and bandwidth terms. */
	c.lit.h_total = 4000; c.lit.h_addressable = 3840; c.lit.h_front_porch = 48; c.lit.h_sync_width = 32;
	c.lit.v_total = 2222; c.lit.v_addressable = 2160; c.lit.v_front_porch = 3; c.lit.v_sync_width = 5;
	c.lit.pix_clk_100hz = 5332500;
	if (c.dsc)
		dsc_choose(&c);
	run_dml(&c);

	for (int run = 0; run < 2; run++) {
		rec_reset(run ? 0xffffffff : 0);
		rec_log_reads = verbose && run == 0;
		setup();
		make_state(&c);
		if (c.drr) sequence_drr(); else if (c.audio) sequence_audio(&c); else if (c.dp && !c.train) sequence_dp(&c); else sequence(&c);
		if (run == 0) { memcpy(run0, evs, sizeof(struct ev) * nev); n0 = nev; }
	}
	printf("CONFIG %d %d %d %d  %u %u %u %u  %u %u %u %u  %u %d %d  %d %u  %d\n", c.pipe, c.dig, c.link, c.hpd + 1,
	       v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13] != 0, v[14] != 0,
	       c.vstartup, g_prog->plane_programming[1].pipe_regs[0]->det_size, c.y420);
	emit();
	return 0;
}
