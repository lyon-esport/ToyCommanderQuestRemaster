#include "ta_ctx.h"
#include "spg.h"
#include "cfg/option.h"
#include "Renderer_if.h"
#include "serialize.h"
#include "stdclass.h"
#include "network/ggpo.h"

#include <mutex>
#include <vector>

extern u32 fskip;
static int RenderCount;

TA_context* ta_ctx;
tad_context ta_tad;

static void tactx_Recycle(TA_context* ctx);
static TA_context *tactx_Find(u32 addr, bool allocnew = false);

// tcvr: the head-driven camera's turn, per picture, carried with the polygons themselves.
// The camera hook latches the turn of a game frame when the game ends its scene (rnd_FrameEnd,
// kmRender). Toy Commander draws a frame into a vertex buffer in RAM (Kamui 2, two banks used
// in turn) and that buffer reaches the TA later: each display list starts (TA_LIST_INIT) and at
// once gets a bank by channel 2 DMA, then the game draws its new frame into that same bank, and
// the PVR renders a list at the next render start. So the vertices of frame N are sent at frame
// N+2 and shown from frame N+3: a picture given the latest latched turn was 3 pictures early
// (headset test 2026-11-18: a 30 degree step of the camera alone showed the world 30 degrees off
// for 3 pictures; during a head turn, the world swam by the turn of the last 100 ms).
// So the turn follows the data: at the scene's end, the bank last sent (the one the frame was
// just drawn into) gets the frame's turn; a list that receives a bank by DMA takes that bank's
// turn; the render start gives the picture its list's turn. A list that got no known bank
// (polygons sent another way) falls back to the latest latched turn.
struct TcvrTurn
{
	float yaw = 0.f, pitch = 0.f;
	u32 sample = 0;
};
static TcvrTurn latchedTurn;
static u32 listBank;		// the first DMA source of the current display list (0: none yet)
static bool listHasBank;
struct TcvrBankTurn
{
	u32 bank = 0;
	TcvrTurn turn;
};
static TcvrBankTurn bankTurns[4];
static unsigned bankNext;
static u32 tcvrTaggedPictures, tcvrLatchedPictures;

void tcvrSetFrameCamTurn(float yaw, float pitch, u32 sample)
{
	latchedTurn = { yaw, pitch, sample };
	if (listBank == 0)
		return;
	for (TcvrBankTurn& b : bankTurns)
		if (b.bank == listBank)
		{
			b.turn = latchedTurn;
			return;
		}
	bankTurns[bankNext++ % std::size(bankTurns)] = { listBank, latchedTurn };
}

void tcvrNoteListInit()
{
	listHasBank = false;
	if (ta_ctx != nullptr)
		ta_ctx->tcvrTagged = false;
}

void tcvrNoteTaDma(u32 src)
{
	if (ta_ctx == nullptr || listHasBank)
		return;
	listHasBank = true;
	listBank = src;
	for (const TcvrBankTurn& b : bankTurns)
		if (b.bank == src)
		{
			ta_ctx->tcvrTagged = true;
			ta_ctx->tcvrCamYaw = b.turn.yaw;
			ta_ctx->tcvrCamPitch = b.turn.pitch;
			ta_ctx->tcvrCamSample = b.turn.sample;
			return;
		}
}

void tcvrTakeFrameCamTurn(TA_context& ctx)
{
	rend_context& rend = ctx.rend;
	TcvrTurn t = latchedTurn;
	if (ctx.tcvrTagged)
		t = { ctx.tcvrCamYaw, ctx.tcvrCamPitch, ctx.tcvrCamSample };
	if (!rend.isRTT && (latchedTurn.yaw != 0.f || t.yaw != 0.f))
	{
		(ctx.tcvrTagged ? tcvrTaggedPictures : tcvrLatchedPictures)++;
		if (tcvrTaggedPictures + tcvrLatchedPictures >= 600)
		{
			NOTICE_LOG(PVR, "VR head camera: of the last 600 turned pictures, %u took their vertex buffer's turn, %u the latest one",
					tcvrTaggedPictures, tcvrLatchedPictures);
			tcvrTaggedPictures = tcvrLatchedPictures = 0;
		}
	}
	rend.tcvrCamYaw = rend.isRTT ? 0.f : t.yaw;
	rend.tcvrCamPitch = rend.isRTT ? 0.f : t.pitch;
	rend.tcvrCamSample = rend.isRTT ? 0 : t.sample;
}

void SetCurrentTARC(u32 addr)
{
	if (addr != TACTX_NONE)
	{
		if (ta_ctx)
			SetCurrentTARC(TACTX_NONE);

		verify(ta_ctx == 0);
		//set new context
		ta_ctx = tactx_Find(addr,true);

		//copy cached params
		ta_tad = ta_ctx->tad;
	}
	else
	{
		//Flush cache to context
		verify(ta_ctx != 0);
		ta_ctx->tad=ta_tad;
		
		//clear context
		ta_ctx=0;
		ta_tad.Reset(0);
	}
}

static TA_context* rqueue;
static cResetEvent frame_finished;

bool QueueRender(TA_context* ctx)
{
	verify(ctx != 0);
	
	bool skipFrame = !rend_is_enabled();
	if (!skipFrame)
	{
		RenderCount++;
		if (RenderCount % (config::SkipFrame + 1) != 0)
			skipFrame = true;
		else if (config::ThreadedRendering && rqueue != nullptr && !ggpo::active()
				&& (config::AutoSkipFrame == 0 || (config::AutoSkipFrame == 1 && SH4FastEnough)))
			// (online, hotd2-vr: skipped instead, see Renderer_if.cpp OnlineRenderWaitMs)
			// The previous render hasn't completed yet so we wait.
			// If autoskipframe is enabled (normal level), we only do so if the CPU is running
			// fast enough over the last frames
			frame_finished.Wait();
	}

	if (skipFrame || rqueue)
	{
		tactx_Recycle(ctx);
		if (rend_is_enabled())
			fskip++;
		return false;
	}
	// disable net rollbacks until the render thread has processed the frame
	rend_disable_rollback();
	frame_finished.Reset();
	verify(rqueue == nullptr);
	rqueue = ctx;


	return true;
}

TA_context* DequeueRender()
{
	if (rqueue != nullptr)
		FrameCount++;

	return rqueue;
}

void FinishRender(TA_context* ctx)
{
	if (ctx != nullptr)
	{
		verify(rqueue == ctx);
		rqueue = nullptr;
		tactx_Recycle(ctx);
	}
	frame_finished.Set();
}

static std::mutex mtx_pool;
using Lock = std::lock_guard<std::mutex>;

static std::vector<TA_context*> ctx_pool;
static std::vector<TA_context*> ctx_list;

TA_context *tactx_Alloc()
{
	TA_context *ctx = nullptr;
	{
		Lock _(mtx_pool);
		if (!ctx_pool.empty()) {
			ctx = ctx_pool.back();
			ctx_pool.pop_back();
		}
	}

	if (ctx == nullptr) {
		ctx = new TA_context();
		ctx->Alloc();
	}
	return ctx;
}

static void tactx_Recycle(TA_context* ctx)
{
	if (ctx->nextContext != nullptr)
		tactx_Recycle(ctx->nextContext);
	Lock _(mtx_pool);
	if (ctx_pool.size() > 3) {
		delete ctx;
	}
	else {
		ctx->Reset();
		ctx_pool.push_back(ctx);
	}
}

static TA_context *tactx_Find(u32 addr, bool allocnew)
{
	TA_context *oldCtx = nullptr;
	for (TA_context *ctx : ctx_list)
	{
		if (ctx->Address == addr) {
			ctx->lastFrameUsed = FrameCount;
			return ctx;
		}
		if (FrameCount - ctx->lastFrameUsed > 60)
			oldCtx = ctx;
	}

	if (allocnew)
	{
		TA_context *ctx;
		if (oldCtx != nullptr)
		{
			ctx = oldCtx;
			ctx->Reset();
		}
		else
		{
			ctx = tactx_Alloc();
			ctx_list.push_back(ctx);
		}
		ctx->Address = addr;
		ctx->lastFrameUsed = FrameCount;

		return ctx;
	}
	return nullptr;
}

TA_context *tactx_Pop(u32 addr)
{
	for (size_t i = 0; i < ctx_list.size(); i++)
	{
		if (ctx_list[i]->Address == addr)
		{
			TA_context *ctx = ctx_list[i];
			
			if (::ta_ctx == ctx)
				SetCurrentTARC(TACTX_NONE);

			ctx_list.erase(ctx_list.begin() + i);

			return ctx;
		}
	}
	return nullptr;
}

void tactx_Term()
{
	if (ta_ctx != nullptr)
		SetCurrentTARC(TACTX_NONE);

	for (TA_context *ctx : ctx_list)
		delete ctx;
	ctx_list.clear();

	Lock _(mtx_pool);
	for (TA_context *ctx : ctx_pool)
		delete ctx;
	ctx_pool.clear();
}

const u32 NULL_CONTEXT = ~0u;

static void serializeContext(Serializer& ser, const TA_context *ctx)
{
	if (ser.dryrun())
	{
		// Maximum size: address, size, data
		ser.skip(4 + 4 + TA_DATA_SIZE);
		return;
	}
	if (ctx == nullptr)
	{
		ser << NULL_CONTEXT;
		return;
	}
	ser << ctx->Address;
	const tad_context& tad = ctx == ::ta_ctx ? ta_tad : ctx->tad;
	const u32 taSize = tad.thd_data - tad.thd_root;
	ser << taSize;
	ser.serialize(tad.thd_root, taSize);
}

static void deserializeContext(Deserializer& deser, TA_context **pctx)
{
	u32 address;
	deser >> address;
	if (address == NULL_CONTEXT)
	{
		*pctx = nullptr;
		return;
	}
	u32 size;
	deser >> size;
	if (size > TA_DATA_SIZE)
		throw Deserializer::Exception("Invalid TA context size");
	*pctx = tactx_Find(address, true);
	tad_context& tad = (*pctx)->tad;
	deser.deserialize(tad.thd_root, size);
	tad.thd_data = tad.thd_root + size;
	if (deser.version() < Deserializer::V26)
	{
		u32 render_pass_count;
		deser >> render_pass_count;
		deser.skip(sizeof(u32) * render_pass_count);
	}
}

void SerializeTAContext(Serializer& ser)
{
	ser << (u32)ctx_list.size();
	int curCtx = -1;
	for (const auto& ctx : ctx_list)
	{
		if (ctx == ::ta_ctx)
			curCtx = (int)(&ctx - &ctx_list[0]);
		serializeContext(ser, ctx);
	}
	ser << curCtx;
}

void DeserializeTAContext(Deserializer& deser)
{
	if (::ta_ctx != nullptr)
		SetCurrentTARC(TACTX_NONE);
	if (deser.version() >= Deserializer::V25)
	{
		u32 listSize;
		deser >> listSize;
		for (const auto& ctx : ctx_list)
			tactx_Recycle(ctx);
		ctx_list.clear();
		for (u32 i = 0; i < listSize; i++)
		{
			TA_context *ctx;
			deserializeContext(deser, &ctx);
		}
		int curCtx;
		deser >> curCtx;
		if (curCtx >= 0 && curCtx < (int)ctx_list.size())
			SetCurrentTARC(ctx_list[curCtx]->Address);
	}
	else
	{
		TA_context *ta_cur_ctx;
		deserializeContext(deser, &ta_cur_ctx);
		if (ta_cur_ctx != nullptr)
			SetCurrentTARC(ta_cur_ctx->Address);
		if (deser.version() >= Deserializer::V20)
			deserializeContext(deser, &ta_cur_ctx);
	}
}
