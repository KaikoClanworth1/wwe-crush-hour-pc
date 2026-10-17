import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
p = runtime('src', 'lib', 'Kernel', 'Stubs', 'MPEG.cpp')
s = open(p, encoding='utf-8').read()

helper = '''
        // libmpeg calls the game's sceMpegCbNodata / sceMpegCbBackground callbacks from inside
        // sceMpegGetPicture whenever the decoder starves; many players (e.g. RenderWare VidTex)
        // only feed sceMpegDemuxPss from those callbacks. Queue them and wake GetPicture afterwards.
        constexpr uint32_t kMpegCbNodata = 1u;
        constexpr uint32_t kMpegCbBackground = 4u;

        bool queueStarvationCallbacks(uint8_t *rdram, R5900Context *callerCtx, PS2Runtime *runtime, uint32_t mpegAddr)
        {
            std::vector<MpegRegisteredCallback> toCall;
            {
                auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
                if (it == g_mpeg_stub_state.callbacksByMpeg.end())
                    return false;
                for (const MpegRegisteredCallback &cb : it->second)
                    if (!cb.stream && cb.type == kMpegCbNodata)
                        toCall.push_back(cb);
                if (toCall.empty())
                    for (const MpegRegisteredCallback &cb : it->second)
                        if (!cb.stream && cb.type == kMpegCbBackground)
                            toCall.push_back(cb);
            }
            bool queued = false;
            for (const MpegRegisteredCallback &cb : toCall)
            {
                if (!rdram || !callerCtx || !runtime || cb.func == 0u || !runtime->hasFunction(cb.func))
                    continue;
                const uint32_t cbDataAddr = runtime->guestMalloc(kMpegCallbackDataSize, 16u);
                if (cbDataAddr == 0u)
                    continue;
                if (uint8_t *p = getMemPtr(rdram, cbDataAddr))
                {
                    std::memset(p, 0, kMpegCallbackDataSize);
                    std::memcpy(p, &cb.type, sizeof(uint32_t));
                }
                R5900Context callbackCtx = *callerCtx;
                SET_GPR_U32(&callbackCtx, 4, mpegAddr);
                SET_GPR_U32(&callbackCtx, 5, cbDataAddr);
                SET_GPR_U32(&callbackCtx, 6, cb.data);
                SET_GPR_U32(&callbackCtx, 7, 0u);
                SET_GPR_U32(&callbackCtx, 29, 0u);
                SET_GPR_U32(&callbackCtx, 31, 0u);
                callbackCtx.pc = cb.func;
                GuestInvocation invocation{};
                invocation.kind = GuestInvocationKind::RpcCallback;
                invocation.context = callbackCtx;
                invocation.onComplete = [runtime, cbDataAddr, mpegAddr](const R5900Context &, R5900Context &)
                {
                    runtime->guestFree(cbDataAddr);
                    runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
                };
                runtime->eeScheduler().queueInvocation(std::move(invocation));
                queued = true;
            }
            return queued;
        }
'''
anchor = '        void dispatchStreamCallbacks(uint8_t *rdram,'
assert anchor in s
s = s.replace(anchor, helper + '\n' + anchor, 1)

old = '''                lock.unlock();
                runtime->eeScheduler().waitExternal(
                    EeWaitReason::Mpeg,
                    kMpegPictureWaitType,
                    mpegAddr,'''
new = '''                queueStarvationCallbacks(rdram, ctx, runtime, mpegAddr);
                lock.unlock();
                runtime->eeScheduler().waitExternal(
                    EeWaitReason::Mpeg,
                    kMpegPictureWaitType,
                    mpegAddr,'''
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
