/* pipegen: run Linux DC's own DCN 4.01 code for lighting one new HDMI stream and plane against a recorder
 * (rec.c) and print the register sequence. Built and run by run.sh, which explains the rest. */
#include "resource/dcn401/dcn401_resource.c"   /* its static creators and register tables */
#include "dcn401/dcn401_dccg.h"
#include "dce/dce_clock_source.h"
#include "rec.h"

/* Segment bases are sentinels so that an address decodes as (base_idx + 1) << 24 | dword. */
static uint32_t dcn_bases[8]  = { 1u << 24, 2u << 24, 3u << 24, 4u << 24, 5u << 24, 6u << 24, 7u << 24, 8u << 24 };
static uint32_t nbio_bases[8] = { 0x11u << 24, 0x12u << 24, 0x13u << 24, 0x14u << 24, 0x15u << 24, 0x16u << 24, 0x17u << 24, 0x18u << 24 };

static struct dc g_dc;
static struct clk_mgr g_clk_mgr;
static struct dc_context g_ctx;
static struct dc_bios g_bios;
static struct resource_pool g_pool;

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
static const struct dc_vbios_funcs bios_funcs = {
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
	struct dc_crtc_timing t;      /* the new stream */
	struct dc_crtc_timing lit;    /* the stream the firmware lit */
	int vstartup, vupdate_offset, vupdate_width, vready_offset, pstate_keepout;   /* from DML */
};


/* ---- DML 2.1: the timing-dependent HUBP values and global sync for the new pipe ---- */
#include "dml_top.h"
#include "dml2_internal_shared_types.h"
#include "bounding_boxes/dcn4_soc_bb.h"

static struct dml2_display_cfg_programming *g_prog;

static void dml_stream(struct dml2_display_cfg *d, int i, const struct dc_crtc_timing *t, bool hdmi)
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
	s->timing.dsc.enable = dml2_dsc_disable;
	s->timing.drr_config.disallowed = true;
	s->timing.vblank_nom = t->v_total - h;
	s->output.output_dp_lane_count = 4;
	s->output.output_encoder = hdmi ? dml2_hdmi : dml2_dp;
	s->output.output_format = dml2_444;
	s->output.output_dp_link_rate = dml2_dp_rate_na;
	s->output.output_disabled = true;
	s->overrides.odm_mode = dml2_odm_mode_auto;
	s->overrides.disable_dynamic_odm = true;
	s->overrides.hw.twait_budgeting.fclk_pstate = dml2_twait_budgeting_setting_if_needed;
	s->overrides.hw.twait_budgeting.uclk_pstate = dml2_twait_budgeting_setting_if_needed;
	s->overrides.hw.twait_budgeting.stutter_enter_exit = dml2_twait_budgeting_setting_if_needed;

	p->stream_index = i;
	p->surface.plane0.width = w; p->surface.plane0.height = h; p->surface.plane0.pitch = w;
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
	dml_stream(d, 0, &c->lit, false);
	dml_stream(d, 1, &c->t, true);
	bmp.dml2_instance = inst; bmp.display_config = d; bmp.programming = g_prog;
	if (!dml2_build_mode_programming(&bmp)) { fprintf(stderr, "DML: mode not supported\n"); exit(1); }

	g = &g_prog->stream_programming[1].global_sync;
	c->vstartup = g->dcn4x.vstartup_lines; c->vupdate_offset = g->dcn4x.vupdate_offset_pixels;
	c->vupdate_width = g->dcn4x.vupdate_vupdate_width_pixels; c->vready_offset = g->dcn4x.vready_offset_pixels;
	c->pstate_keepout = g->dcn4x.pstate_keepout_start_lines;
	fprintf(stderr, "DML: dispclk %lu kHz, dpprefclk %lu kHz, dcfclk %lu kHz; new pipe dppclk %lu kHz, det %u segments, "
		"vstartup %d\n", g_prog->min_clocks.dcn4x.dispclk_khz, g_prog->min_clocks.dcn4x.dpprefclk_khz,
		g_prog->min_clocks.dcn4x.active.dcfclk_khz, g_prog->plane_programming[1].min_clocks.dcn4x.dppclk_khz,
		g_prog->plane_programming[1].pipe_regs[0]->det_size, c->vstartup);
}

/* ---- the boundary to things that are not register writes ---- */
#include "link_service.h"
#include "link/link_dpms.h"
#include "link/protocols/link_ddc.h"
#include "dcn10/dcn10_mpc.h"
#include "dcn20/dcn20_opp.h"
#include "dcn20/dcn20_hubbub.h"
#include "dcn31/dcn31_vpg.h"
#include "dcn401/dcn401_mpc.h"
#include "dcn10/dcn10_stream_encoder.h"

void write_scdc_data(struct ddc_service *ddc, uint32_t pix_clk, bool lte_340_scramble)
{
	rec_mark("scdc:write_tmds_config", pix_clk > 340000 ? 3 : lte_340_scramble ? 1 : 0);
}
void read_scdc_data(struct ddc_service *ddc) { rec_mark("scdc:read_status", 0); }
static bool no_128b_132b(struct pipe_ctx *p) { return false; }
static enum dp_link_encoding enc_8b_10b(const struct dc_link_settings *s) { return DP_8b_10b_ENCODING; }
static void no_trace(struct dc_link *l, uint8_t s) { }
static const struct link_service g_link_srv = {
	.set_dpms_on = link_set_dpms_on,
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
	eid.connector.id = c->dp ? CONNECTOR_ID_DISPLAY_PORT : CONNECTOR_ID_HDMI_TYPE_A;
	eid.connector.enum_id = ENUM_ID_1;
	eid.ctx = &g_ctx;

	dc->current_state = old;
	dc->link_srv = &g_link_srv;
	dc->links[0] = link; dc->link_count = 1;
	const enum signal_type signal = c->dp ? SIGNAL_TYPE_DISPLAY_PORT : SIGNAL_TYPE_HDMI_TYPE_A;
	link->dc = dc; link->ctx = &g_ctx; link->connector_signal = signal;
	if (c->dp) {            /* a trained link: four lanes at HBR3, as the firmware brings a 4K sink up */
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
	stream->phy_pix_clk = c->t.pix_clk_100hz / 10;
	stream->src = (struct rect){ 0, 0, w, h }; stream->dst = stream->src;
	stream->output_color_space = COLOR_SPACE_SRGB;
	stream->qs_bit = 1;                      /* send "full range" in the AVI infoframe: a desktop */
	stream->content_type = DISPLAY_CONTENT_TYPE_GRAPHICS;
	stream->clamping.c_depth = COLOR_DEPTH_888; stream->clamping.pixel_encoding = PIXEL_ENCODING_RGB;
	stream->clamping.clamping_level = CLAMPING_FULL_RANGE;
	stream->out_transfer_func.type = TF_TYPE_BYPASS;
	stream->test_pattern.type = DP_TEST_PATTERN_VIDEO_MODE;

	plane->ctx = &g_ctx;
	plane->format = SURFACE_PIXEL_FORMAT_GRPH_ARGB8888;
	plane->address.type = PLN_ADDR_TYPE_GRAPHICS;
	plane->address.grph.addr.quad_part = SURFACE_PLACEHOLDER;
	plane->plane_size.surface_size = (struct rect){ 0, 0, w, h };
	plane->plane_size.surface_pitch = w;
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
	pipe->stream_res.pix_clk_params.requested_pix_clk_100hz = c->t.pix_clk_100hz;
	pipe->stream_res.pix_clk_params.encoder_object_id = eid.encoder;
	pipe->stream_res.pix_clk_params.signal_type = signal;
	pipe->stream_res.pix_clk_params.controller_id = CONTROLLER_ID_D0 + c->pipe;
	pipe->stream_res.pix_clk_params.color_depth = COLOR_DEPTH_888;
	pipe->stream_res.pix_clk_params.pixel_encoding = PIXEL_ENCODING_RGB;
	pipe->stream_res.pix_clk_params.dio_se_pix_per_cycle = 1;
	pipe->pll_settings.actual_pix_clk_100hz = c->t.pix_clk_100hz;
	pipe->pll_settings.adjusted_pix_clk_100hz = c->t.pix_clk_100hz;
	pipe->pll_settings.calculated_pix_clk_100hz = c->t.pix_clk_100hz;

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

	/* Linux skips the TMDS divider write when the register already holds /4. Take the write path: it is
	 * the same end state either way. */
	rec_poke_field(dccg_regs.OTG_PIXEL_RATE_DIV, OTG_PIXEL_RATE_DIV__OTG0_TMDS_PIXEL_RATE_DIV_MASK << (c->pipe * 5), 0);

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
	}

	/* What dcn401_init_hw does once for every block, lit or not, repeated for the blocks this pipe uses in case
	 * the firmware's own start-up skipped the ones it did not light. The link encoder's hw_init is left out:
	 * it programs the AUX channel, which a TMDS link does not use and whose instance is board wiring. */
	rec_mark("begin:init", 0);
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
 * output is a reference for tools/atomdump.cpp to hold src/modeset.cpp's DisplayPort plan against. */
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

/* Two runs, unknown registers reading all-zeros then all-ones: a full write whose value differs between the runs
 * carries bits read back from the register, so it becomes an update that leaves those bits alone. A different
 * sequence of events means a read steered the code; that has to be settled with rec_poke(), not guessed. */
static struct ev run0[8192]; static int n0;

static void emit(void)
{
	static const char *k[] = { "W", "U", "WAIT", "DELAY", "MARK" };
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
		else if (a.kind == EV_DELAY) printf("DELAY %u ; %s\n", a.arg, a.label);
		else printf("%-5s %u 0x%04x %08x %08x %u ; %s\n", k[a.kind], (a.addr >> 24) - 1,
			    a.addr & 0xffffff, a.mask, a.value, a.arg, a.label);
	}
}

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

int main(int argc, char **argv)
{
	struct cfg c = { 0 };
	unsigned v[16];
	bool verbose;

	if (argc > 1 && !strcmp(argv[1], "dp")) { c.dp = true; argv++; argc--; }
	verbose = argc > 17;
	if (argc < 17) {
		fprintf(stderr, "usage: pipegen [dp] pipe dig link hpd  hactive hfront hsync hback  vactive vfront vsync vback  "
				"khz hpositive vpositive vic [verbose]\n  hpd counts from 1, as the VBIOS path records do\n"
				"  dp: the reference for retiming a lit DisplayPort stream instead of the second-pipe plan\n");
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
	c.t.pixel_encoding = PIXEL_ENCODING_RGB; c.t.display_color_depth = COLOR_DEPTH_888;
	if (c.pipe < (c.dp ? 0 : 1) || c.pipe > 3 || c.dig > 3 || c.link > 3 || c.hpd > 5 || !v[12]) { fprintf(stderr, "bad configuration\n"); return 2; }

	/* The display the firmware lit: the 3840x2160@60 CVT-RB raster the GOP uses on a 4K sink, the heaviest
	 * companion this card's firmware sets up. It only enters DML's shared-clock and bandwidth terms. */
	c.lit.h_total = 4000; c.lit.h_addressable = 3840; c.lit.h_front_porch = 48; c.lit.h_sync_width = 32;
	c.lit.v_total = 2222; c.lit.v_addressable = 2160; c.lit.v_front_porch = 3; c.lit.v_sync_width = 5;
	c.lit.pix_clk_100hz = 5332500;
	run_dml(&c);

	for (int run = 0; run < 2; run++) {
		rec_reset(run ? 0xffffffff : 0);
		rec_log_reads = verbose && run == 0;
		setup();
		make_state(&c);
		if (c.dp) sequence_dp(&c); else sequence(&c);
		if (run == 0) { memcpy(run0, evs, sizeof(struct ev) * nev); n0 = nev; }
	}
	printf("CONFIG %d %d %d %d  %u %u %u %u  %u %u %u %u  %u %d %d  %d %u\n", c.pipe, c.dig, c.link, c.hpd + 1,
	       v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13] != 0, v[14] != 0,
	       c.vstartup, g_prog->plane_programming[1].pipe_regs[0]->det_size);
	emit();
	return 0;
}
