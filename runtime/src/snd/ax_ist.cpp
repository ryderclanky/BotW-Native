// snd_core (AX) frame processing ("ist"): the AX frame thread, voice shadow-copy sync, frame/final-mix
// callbacks and the 32->48 kHz upsampling stage.
// Vendored/ported from Cemu (src/Cafe/OS/libs/snd_core/ax_ist.cpp), MPL-2.0.
// Host changes: Cemu's AX thread is a guest OS thread fed by AXOut_update() through an OSMessageQueue.
// Here it is a host thread adopted as a guest thread that paces itself (3 ms per frame, Cemu's
// AXOut_update timing) and calls guest callbacks with guest_call. Device remix matrices
// (AXSetDeviceRemixMatrix) are not imported by BotW and are left out.
#include <pthread.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>

#include "ax.h"
#include "ax_internal.h"
#include "diag.h"
#include "title_diag.h"

#include "botw_target.h"
#if TARGET_OS_IOS  // iOS port: stack size vs. the VA limit without extended VA (ios/src/vm_ios.cpp)
size_t botw_ios_stack_size(size_t want);
#endif

namespace snd_core
{
	sint32 __AXDeviceUpsampleStage[AX_DEV_COUNT]; // AX_UPSAMPLE_STAGE_*

	sint32 AXSetDeviceUpsampleStage(sint32 device, int upsampleStage)
	{
		if (device != AX_DEV_TV && device != AX_DEV_DRC)
			return -1;
		__AXDeviceUpsampleStage[device] = upsampleStage;
		return 0;
	}

	sint32 AXGetDeviceUpsampleStage(sint32 device, uint32be* upsampleStage)
	{
		if (device != AX_DEV_TV && device != AX_DEV_DRC)
			return -1;
		*upsampleStage = __AXDeviceUpsampleStage[device];
		return 0;
	}

	sint32 AXGetInputSamplesPerFrame()
	{
		if (sndGeneric.initParam.rendererFreq == AX_RENDERER_FREQ_48KHZ)
			return AX_SAMPLES_PER_3MS_48KHZ;
		return AX_SAMPLES_PER_3MS_32KHZ;
	}

	sint32 AXGetInputSamplesPerSec()
	{
		sint32 samplesPerFrame = AXGetInputSamplesPerFrame();
		sint32 samplesPerSecond = (samplesPerFrame * 1000) / 3;
		return samplesPerSecond;
	}

	struct AXUpsampler
	{
		sint32 samples[AX_SAMPLES_MAX];
	};

	struct
	{
		AXUpsampler upsamplerArray[6];
		bool useLinearUpsampler; // false -> FIR, true -> linear
	}__AXTVUpsampler;

	struct
	{
		AXUpsampler upsamplerArray[4];
		bool useLinearUpsampler; // false -> FIR, true -> linear
	}__AXDRCUpsampler[2];

	void AXUpsampler_Init(AXUpsampler* upsampler)
	{
		memset(upsampler, 0, sizeof(AXUpsampler));
	}

	struct AXFINALMIXCBPARAM
	{
		/* +0x00 */ MEMPTR<MEMPTR<sint32>> data;
		/* +0x04 */ uint16be numChannelInput;
		/* +0x06 */ uint16be numSamples;
		/* +0x08 */ uint16be numDevices;
		/* +0x0A */ uint16be numChannelOutput;
	};

	static_assert(offsetof(AXFINALMIXCBPARAM, data) == 0x00);
	static_assert(offsetof(AXFINALMIXCBPARAM, numChannelInput) == 0x04);
	static_assert(offsetof(AXFINALMIXCBPARAM, numSamples) == 0x06);
	static_assert(offsetof(AXFINALMIXCBPARAM, numDevices) == 0x08);
	static_assert(offsetof(AXFINALMIXCBPARAM, numChannelOutput) == 0x0A);

	SysAllocator<AXFINALMIXCBPARAM> __AXFinalMixCBStructTV;
	SysAllocator<AXFINALMIXCBPARAM> __AXFinalMixCBStructDRC;
	SysAllocator<AXFINALMIXCBPARAM> __AXFinalMixCBStructRMT;

	SysAllocator<MEMPTR<sint32>, 6> __AXFinalMixCBStructTV_dataPtrArray;
	SysAllocator<MEMPTR<sint32>, 4 * 2> __AXFinalMixCBStructDRC_dataPtrArray;

	sint32 __AXFinalMixOutputChannelCount[AX_DEV_COUNT] = { 0 }; // number of output channels returned by final mix callback

	// callbacks
	MPTR __AXFrameCallback = MPTR_NULL; // set via AXRegisterFrameCallback()
	MPTR __AXAppFrameCallback[AX_APP_FRAME_CALLBACK_MAX];
	MPTR __AXDeviceFinalMixCallback[AX_DEV_COUNT];
	std::mutex __AXAppFrameCallbackMutex; // Cemu: guest OSMutex

	void AXResetCallbacks()
	{
		std::lock_guard<std::mutex> lock(__AXAppFrameCallbackMutex);
		__AXFrameCallback = MPTR_NULL;
		for (sint32 i = 0; i < AX_APP_FRAME_CALLBACK_MAX; i++)
		{
			__AXAppFrameCallback[i] = MPTR_NULL;
		}
		for (sint32 i = 0; i < AX_DEV_COUNT; i++)
			__AXDeviceFinalMixCallback[i] = MPTR_NULL;
	}

	sint32 AXRegisterAppFrameCallback(MPTR funcAddr)
	{
		if (funcAddr == MPTR_NULL)
			return -17;
		std::lock_guard<std::mutex> lock(__AXAppFrameCallbackMutex);
		for (sint32 i = 0; i < AX_APP_FRAME_CALLBACK_MAX; i++)
		{
			if (__AXAppFrameCallback[i] == MPTR_NULL)
			{
				__AXAppFrameCallback[i] = funcAddr;
				return 0;
			}
		}
		return -15;
	}

	sint32 AXDeregisterAppFrameCallback(MPTR funcAddr)
	{
		if (funcAddr == MPTR_NULL)
			return -17;
		std::lock_guard<std::mutex> lock(__AXAppFrameCallbackMutex);
		for (sint32 i = 0; i < AX_APP_FRAME_CALLBACK_MAX; i++)
		{
			if (__AXAppFrameCallback[i] == funcAddr)
			{
				__AXAppFrameCallback[i] = MPTR_NULL;
				return 0;
			}
		}
		return -16; // not found
	}

	MPTR AXRegisterFrameCallback(MPTR funcAddr)
	{
		std::lock_guard<std::mutex> lock(__AXAppFrameCallbackMutex);
		MPTR prevCallbackFunc = __AXFrameCallback;
		__AXFrameCallback = funcAddr;
		return prevCallbackFunc;
	}

	sint32 __AXTVUpsamplerSampleHistory[AX_TV_CHANNEL_COUNT] = { 0 };
	sint32 __AXDRC0UpsamplerSampleHistory[AX_DRC_CHANNEL_COUNT] = { 0 };
	sint32 __AXDRC1UpsamplerSampleHistory[AX_DRC_CHANNEL_COUNT] = { 0 };

	void AXIst_Init()
	{
		// todo - double check defaults
		__AXDeviceUpsampleStage[AX_DEV_TV] = AX_UPSAMPLE_STAGE_BEFORE_FINALMIX;
		__AXDeviceUpsampleStage[AX_DEV_DRC] = AX_UPSAMPLE_STAGE_BEFORE_FINALMIX;
		__AXDeviceUpsampleStage[AX_DEV_RMT] = AX_UPSAMPLE_STAGE_BEFORE_FINALMIX;
		for (sint32 i = 0; i < AX_TV_CHANNEL_COUNT; i++)
			AXUpsampler_Init(__AXTVUpsampler.upsamplerArray + i);
		for (sint32 i = 0; i < AX_DRC_CHANNEL_COUNT; i++)
		{
			AXUpsampler_Init(__AXDRCUpsampler[0].upsamplerArray + i);
			AXUpsampler_Init(__AXDRCUpsampler[1].upsamplerArray + i);
		}
		__AXTVUpsampler.useLinearUpsampler = false;
		__AXDRCUpsampler[0].useLinearUpsampler = false;
		__AXDRCUpsampler[1].useLinearUpsampler = false;
		memset(__AXTVUpsamplerSampleHistory, 0, sizeof(__AXTVUpsamplerSampleHistory));
		memset(__AXDRC0UpsamplerSampleHistory, 0, sizeof(__AXDRC0UpsamplerSampleHistory));
		memset(__AXDRC1UpsamplerSampleHistory, 0, sizeof(__AXDRC1UpsamplerSampleHistory));
		AXResetCallbacks();
	}

	void AXOut_ResetFinalMixCBData()
	{
		sint32 inputSamplesPerFrame = AXGetInputSamplesPerFrame();
		// TV
		__AXFinalMixCBStructTV->data = nullptr;
		__AXFinalMixCBStructTV->numChannelInput = 6;
		__AXFinalMixCBStructTV->numChannelOutput = 6;
		__AXFinalMixCBStructTV->numSamples = inputSamplesPerFrame;
		__AXFinalMixCBStructTV->numDevices = 1;
		// DRC
		__AXFinalMixCBStructDRC->data = nullptr;
		__AXFinalMixCBStructDRC->numChannelInput = 4;
		__AXFinalMixCBStructDRC->numChannelOutput = 4;
		__AXFinalMixCBStructDRC->numSamples = inputSamplesPerFrame;
		__AXFinalMixCBStructDRC->numDevices = 2;
		// RMT
		__AXFinalMixCBStructRMT->data = nullptr;
		__AXFinalMixCBStructRMT->numChannelInput = 1;
		__AXFinalMixCBStructRMT->numChannelOutput = 1;
		__AXFinalMixCBStructRMT->numSamples = 18; // verify
		__AXFinalMixCBStructRMT->numDevices = 4;
	}

	sint32 AXGetDeviceFinalMixCallback(sint32 device, uint32be* funcAddrPtr)
	{
		if (device != AX_DEV_TV && device != AX_DEV_DRC)
			return -1;
		*funcAddrPtr = __AXDeviceFinalMixCallback[device];
		return 0;
	}

	sint32 AXRegisterDeviceFinalMixCallback(sint32 device, MPTR funcAddr)
	{
		if (device != AX_DEV_TV && device != AX_DEV_DRC)
			return -1;
		__AXDeviceFinalMixCallback[device] = funcAddr;
		return 0;
	}

	SysAllocator<sint32, AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT> __AXTVOutputBuffer;
	SysAllocator<sint32, AX_SAMPLES_MAX * AX_DRC_CHANNEL_COUNT * 2> __AXDRCOutputBuffer;

	SysAllocator<sint32, AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT> __AXTempFinalMixTVBuffer;
	SysAllocator<sint32, AX_SAMPLES_MAX * AX_DRC_CHANNEL_COUNT * 2> __AXTempFinalMixDRCBuffer;

	// 48KHz buffers
	SysAllocator<sint32, AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT> __AXTVBuffer48;
	SysAllocator<sint32, AX_SAMPLES_MAX * AX_DRC_CHANNEL_COUNT * 2> __AXDRCBuffer48;

	sint32 AXUpsampleLinear32To48(sint32* inputBuffer, sint32* outputBuffer, sint32* sampleHistory, sint32 sampleCount, bool shiftSamples, sint32 channelCount)
	{
		if (shiftSamples)
		{
			for (sint32 c = 0; c < channelCount; c++)
			{
				float samplePrev = (float)sampleHistory[c];
				for (sint32 i = 0; i < sampleCount; i += 2)
				{
					float sample0 = (float)_swapEndianS32(inputBuffer[0]);
					float sample1 = (float)_swapEndianS32(inputBuffer[1]);

					inputBuffer += 2;
					float s0 = samplePrev * 0.66666669f + sample0 * 0.33333331f;
					float s1 = sample0;
					float s2 = sample1 * 0.66666669f + sample0 * 0.33333331f;
					outputBuffer[0] = _swapEndianS32(((sint32)s0) >> 8);
					outputBuffer[1] = _swapEndianS32(((sint32)s1) >> 8);
					outputBuffer[2] = _swapEndianS32(((sint32)s2) >> 8);
					outputBuffer += 3;
					samplePrev = sample1;
				}
				sampleHistory[c] = (sint32)samplePrev;
			}
		}
		else
		{
			for (sint32 c = 0; c < channelCount; c++)
			{
				float samplePrev = (float)sampleHistory[c];
				for (sint32 i = 0; i < sampleCount; i += 2)
				{
					float sample0 = (float)_swapEndianS32(inputBuffer[0]);
					float sample1 = (float)_swapEndianS32(inputBuffer[1]);
					inputBuffer += 2;
					float s0 = samplePrev * 0.66666669f + sample0 * 0.33333331f;
					float s1 = sample0;
					float s2 = sample1 * 0.66666669f + sample0 * 0.33333331f;
					outputBuffer[0] = _swapEndianS32(((sint32)s0));
					outputBuffer[1] = _swapEndianS32(((sint32)s1));
					outputBuffer[2] = _swapEndianS32(((sint32)s2));
					outputBuffer += 3;
					samplePrev = sample1;
				}
				sampleHistory[c] = (sint32)samplePrev;
			}
		}
		return (sampleCount / 2) * 3;
	}

	void AXTransferSamples(sint32* input, sint32* output, sint32 count, bool shiftSamples)
	{
		if (shiftSamples)
		{
			for (sint32 i = 0; i < count; i++)
			{
				sint32 s = _swapEndianS32(input[i]);
				s >>= 8;
				output[i] = _swapEndianS32(s);
			}
		}
		else
		{
			for (sint32 i = 0; i < count; i++)
				output[i] = input[i];
		}
	}

	void AXIst_ProcessFinalMixCallback()
	{
		bool forceLinearUpsampler = true;
		bool isRenderer48 = sndGeneric.initParam.rendererFreq == AX_RENDERER_FREQ_48KHZ;
		sint32 inputSampleCount = AXGetInputSamplesPerFrame();
		// TV
		if (isRenderer48 || __AXDeviceUpsampleStage[AX_DEV_TV] != AX_UPSAMPLE_STAGE_BEFORE_FINALMIX)
		{
			// only copy
			sint32* inputBuffer = __AXTVOutputBuffer.GetPtr();
			sint32* outputBuffer = __AXTempFinalMixTVBuffer.GetPtr();
			sint32 sampleCount = inputSampleCount * AX_TV_CHANNEL_COUNT;
			for (sint32 i = 0; i < sampleCount; i++)
			{
				sint32 sample0 = _swapEndianS32(inputBuffer[0]);
				sample0 >>= 8;
				inputBuffer++;
				*outputBuffer = _swapEndianS32(sample0);
				outputBuffer++;
			}
			for (sint32 c = 0; c < AX_TV_CHANNEL_COUNT; c++)
			{
				__AXFinalMixCBStructTV_dataPtrArray[c] = __AXTempFinalMixTVBuffer.GetPtr() + c * inputSampleCount;
			}
			__AXFinalMixCBStructTV->numSamples = (uint16)inputSampleCount;
			__AXFinalMixCBStructTV->data = __AXFinalMixCBStructTV_dataPtrArray.GetPtr();
		}
		else
		{
			// upsample (Cemu only implements the linear upsampler and always uses it)
			sint32 upsampledSampleCount = AXUpsampleLinear32To48(__AXTVOutputBuffer.GetPtr(), __AXTVBuffer48.GetPtr(), __AXTVUpsamplerSampleHistory, inputSampleCount, true, AX_TV_CHANNEL_COUNT);
			for (sint32 c = 0; c < AX_TV_CHANNEL_COUNT; c++)
			{
				__AXFinalMixCBStructTV_dataPtrArray[c] = __AXTVBuffer48.GetPtr() + c * upsampledSampleCount;
			}
			__AXFinalMixCBStructTV->numSamples = (uint16)upsampledSampleCount;
			__AXFinalMixCBStructTV->data = __AXFinalMixCBStructTV_dataPtrArray.GetPtr();
		}
		// DRC
		if (isRenderer48 || __AXDeviceUpsampleStage[AX_DEV_DRC] != AX_UPSAMPLE_STAGE_BEFORE_FINALMIX)
		{
			// only copy
			sint32* inputBuffer = __AXDRCOutputBuffer.GetPtr();
			sint32* outputBuffer = __AXTempFinalMixDRCBuffer.GetPtr();
			sint32 sampleCount = inputSampleCount * AX_DRC_CHANNEL_COUNT * 2;
			for (sint32 i = 0; i < sampleCount; i++)
			{
				sint32 sample0 = _swapEndianS32(inputBuffer[0]);
				sample0 >>= 8;
				inputBuffer++;
				*outputBuffer = _swapEndianS32(sample0);
				outputBuffer++;
			}
			for (sint32 c = 0; c < AX_DRC_CHANNEL_COUNT*2; c++)
			{
				__AXFinalMixCBStructDRC_dataPtrArray[c] = __AXTempFinalMixDRCBuffer.GetPtr() + c * inputSampleCount;
			}
			__AXFinalMixCBStructDRC->numSamples = (uint16)inputSampleCount;
			__AXFinalMixCBStructDRC->data = __AXFinalMixCBStructDRC_dataPtrArray.GetPtr();
		}
		else
		{
			// upsample
			// DRC0
			sint32 upsampledSampleCount = AXUpsampleLinear32To48(__AXDRCOutputBuffer.GetPtr(), __AXDRCBuffer48.GetPtr(), __AXDRC0UpsamplerSampleHistory, inputSampleCount, true, AX_DRC_CHANNEL_COUNT);
			// DRC1
			// note: Cemu swaps the two offsets here (reads DRC1 at upsampled*4, writes at input*4, which overlaps DRC0's
			// channels 2/3). The layout used everywhere else is input*4 for the mix buffer and upsampled*4 for the 48KHz buffer.
			AXUpsampleLinear32To48(__AXDRCOutputBuffer.GetPtr() + inputSampleCount * AX_DRC_CHANNEL_COUNT, __AXDRCBuffer48.GetPtr() + upsampledSampleCount * AX_DRC_CHANNEL_COUNT, __AXDRC1UpsamplerSampleHistory, inputSampleCount, true, AX_DRC_CHANNEL_COUNT);
			for (sint32 c = 0; c < AX_DRC_CHANNEL_COUNT * 2; c++)
				__AXFinalMixCBStructDRC_dataPtrArray[c] = __AXDRCBuffer48.GetPtr() + c * upsampledSampleCount;
			__AXFinalMixCBStructDRC->numSamples = (uint16)upsampledSampleCount;
			__AXFinalMixCBStructDRC->data = __AXFinalMixCBStructDRC_dataPtrArray.GetPtr();
		}

		// do callbacks
		__AXFinalMixOutputChannelCount[0] = AX_TV_CHANNEL_COUNT;
		__AXFinalMixOutputChannelCount[1] = AX_DRC_CHANNEL_COUNT;
		__AXFinalMixOutputChannelCount[2] = AX_RMT_CHANNEL_COUNT;
		for (sint32 i = 0; i < AX_DEV_COUNT; i++)
		{
			MPTR callback = __AXDeviceFinalMixCallback[i];
			if (callback == MPTR_NULL)
				continue;
			MEMPTR<AXFINALMIXCBPARAM> cbStruct;
			if (i == AX_DEV_TV)
				cbStruct = __AXFinalMixCBStructTV.GetPtr();
			else if (i == AX_DEV_DRC)
				cbStruct = __AXFinalMixCBStructDRC.GetPtr();
			else
				cbStruct = __AXFinalMixCBStructRMT.GetPtr();

			PPCCoreCallback(callback, cbStruct);
			__AXFinalMixOutputChannelCount[i] = (sint32)cbStruct->numChannelOutput;
		}

		// handle upsampling
		if (isRenderer48)
		{
			// copy TV
			AXTransferSamples(__AXTempFinalMixTVBuffer.GetPtr(), __AXTVBuffer48.GetPtr(), AX_SAMPLES_PER_3MS_48KHZ*AX_TV_CHANNEL_COUNT, false);
			// copy DRC 0
			AXTransferSamples(__AXTempFinalMixDRCBuffer.GetPtr(), __AXDRCBuffer48.GetPtr(), AX_SAMPLES_PER_3MS_48KHZ*AX_DRC_CHANNEL_COUNT, false);
		}
		else
		{
			if (__AXDeviceUpsampleStage[AX_DEV_TV] == AX_UPSAMPLE_STAGE_BEFORE_FINALMIX)
			{
				// final mix is 48KHz
				// no need to copy since samples are already in right buffer
			}
			else
			{
				// final mix is 32KHz -> upsample
				// TV
				AXUpsampleLinear32To48(__AXTempFinalMixTVBuffer.GetPtr(), __AXTVBuffer48.GetPtr(), __AXTVUpsamplerSampleHistory, inputSampleCount, false, AX_TV_CHANNEL_COUNT);
				// DRC0
				AXUpsampleLinear32To48(__AXTempFinalMixDRCBuffer.GetPtr(), __AXDRCBuffer48.GetPtr(), __AXDRC0UpsamplerSampleHistory, inputSampleCount, false, AX_DRC_CHANNEL_COUNT);
			}
		}
	}

	void AXIst_SyncSingleVPB(AXVPB* vpb)
	{
		uint32 index = vpb->index;
		uint32 sync = vpb->sync;
		AXVPBInternal_t* internalVPB = __AXVPBInternalVoiceArray + index;
		AXVPBInternal_t* internalShadowCopy = __AXVPBInternalVoiceShadowCopyArrayPtr + index;

		// shadow copy -> internal data
		internalShadowCopy->nextAddrHigh = internalVPB->nextAddrHigh;
		internalShadowCopy->nextAddrLow = internalVPB->nextAddrLow;
		if (internalVPB->nextToProcess)
			internalShadowCopy->nextToProcess = __AXVPBInternalVoiceShadowCopyArrayPtr + (sint32)internalVPB->nextToProcess->index;
		else
			internalShadowCopy->nextToProcess = nullptr;

		internalShadowCopy->mixerSelect = internalVPB->mixerSelect;

		internalShadowCopy->ukn2A2 = internalVPB->ukn2A2;

		internalShadowCopy->reserved296 = internalVPB->reserved296;
		internalShadowCopy->reserved298 = internalVPB->reserved298;
		internalShadowCopy->reserved29A = internalVPB->reserved29A;
		internalShadowCopy->reserved29C = internalVPB->reserved29C;
		internalShadowCopy->reserved29E = internalVPB->reserved29E;

		// sync current playback state
		if ((sync&AX_SYNCFLAG_PLAYBACKSTATE) == 0)
		{
			uint32 playbackState = _swapEndianU16(internalShadowCopy->playbackState);
			vpb->playbackState = playbackState;
			internalVPB->playbackState = _swapEndianU16(playbackState);
		}
		// sync current offset
		if ((sync&(AX_SYNCFLAG_CURRENTOFFSET | AX_SYNCFLAG_OFFSETS)) == 0)
		{
			internalVPB->internalOffsets.currentOffsetPtrHigh = internalShadowCopy->internalOffsets.currentOffsetPtrHigh;
			internalVPB->internalOffsets.currentOffsetPtrLow = internalShadowCopy->internalOffsets.currentOffsetPtrLow;
		}
		// sync volume
		if ((sync&AX_SYNCFLAG_VE) == 0)
		{
			internalVPB->veVolume = internalShadowCopy->veVolume;
		}
		// sync adpcm data
		if ((sync&AX_SYNCFLAG_ADPCMDATA) == 0)
		{
			for (sint32 i = 0; i < 16; i++)
				internalVPB->adpcmData.coef[i] = internalShadowCopy->adpcmData.coef[i];
			internalVPB->adpcmData.gain = internalShadowCopy->adpcmData.gain;
			internalVPB->adpcmData.scale = internalShadowCopy->adpcmData.scale;
			internalVPB->adpcmData.yn1 = internalShadowCopy->adpcmData.yn1;
			internalVPB->adpcmData.yn2 = internalShadowCopy->adpcmData.yn2;
		}
		// sync src data
		if ((sync&AX_SYNCFLAG_SRCDATA) == 0)
		{
			internalVPB->src.currentFrac = internalShadowCopy->src.currentFrac;
			internalVPB->src.historySamples[0] = internalShadowCopy->src.historySamples[0];
			internalVPB->src.historySamples[1] = internalShadowCopy->src.historySamples[1];
			internalVPB->src.historySamples[2] = internalShadowCopy->src.historySamples[2];
			internalVPB->src.historySamples[3] = internalShadowCopy->src.historySamples[3];
		}
		if (AXVoiceProtection_IsProtectedByAnyThread(vpb))
		{
			// if voice is currently protected, dont sync remaining flags
			return;
		}
		// internal data -> shadow copy
		// sync src type
		if ((sync&AX_SYNCFLAG_SRCFILTER) != 0)
		{
			internalShadowCopy->srcFilterMode = internalVPB->srcFilterMode;
			internalShadowCopy->srcTapFilter = internalVPB->srcTapFilter;
		}
		// Sync device mix
		if ((sync&AX_SYNCFLAG_DEVICEMIXMASK) != 0)
		{
			memcpy(internalShadowCopy->deviceMixMaskTV, internalVPB->deviceMixMaskTV, 8);
			memcpy(internalShadowCopy->deviceMixMaskDRC, internalVPB->deviceMixMaskDRC, 0x10);
			memcpy(internalShadowCopy->deviceMixMaskRMT, internalVPB->deviceMixMaskRMT, 0x20);
		}
		// sync device mix
		if ((sync & AX_SYNCFLAG_DEVICEMIX) != 0)
		{
			memcpy(internalShadowCopy->deviceMixTV, internalVPB->deviceMixTV, 0x60);
			memcpy(internalShadowCopy->deviceMixDRC, internalVPB->deviceMixDRC, 0x80);
			memcpy(internalShadowCopy->deviceMixRMT, internalVPB->deviceMixRMT, 0x40);
		}
		// sync playback state
		if ((sync&AX_SYNCFLAG_PLAYBACKSTATE) != 0)
		{
			internalShadowCopy->playbackState = internalVPB->playbackState;
		}
		// sync voice type
		if ((sync&AX_SYNCFLAG_VOICETYPE) != 0)
		{
			internalShadowCopy->voiceType = internalVPB->voiceType;
		}
		// itd
		if ((sync&AX_SYNCFLAG_ITD40) == 0)
		{
			if ((sync&AX_SYNCFLAG_ITD20) != 0)
			{
				// sync PB itd (todo in Cemu)
			}
		}
		else
		{
			// sync itd
			internalShadowCopy->reserved176_itdRelated = internalVPB->reserved176_itdRelated;
			internalShadowCopy->reserved178_itdRelated = internalVPB->reserved178_itdRelated;
		}
		// sync volume envelope
		// the part below could be incorrect (it seems strange that the delta flag overwrites the full ve flag? But PPC code looks like this)
		if ((sync&AX_SYNCFLAG_VEDELTA) != 0)
		{
			internalShadowCopy->veDelta = internalVPB->veDelta;
		}
		else if ((sync&AX_SYNCFLAG_VE) != 0)
		{
			internalShadowCopy->veVolume = internalVPB->veVolume;
			internalShadowCopy->veDelta = internalVPB->veDelta;
		}
		// sync offsets
		if ((sync&AX_SYNCFLAG_OFFSETS) != 0)
		{
			// sync entire offsets block
			memcpy(&internalShadowCopy->internalOffsets, &internalVPB->internalOffsets, sizeof(axOffsetsInternal_t));
		}
		else
		{
			// sync individual offset fields
			if ((sync&AX_SYNCFLAG_LOOPFLAG) != 0)
			{
				// sync loop flag
				internalShadowCopy->internalOffsets.loopFlag = internalVPB->internalOffsets.loopFlag;
			}
			if ((sync&AX_SYNCFLAG_LOOPOFFSET) != 0)
			{
				// sync loop offset
				internalShadowCopy->internalOffsets.loopOffsetPtrLow = internalVPB->internalOffsets.loopOffsetPtrLow;
				internalShadowCopy->internalOffsets.loopOffsetPtrHigh = internalVPB->internalOffsets.loopOffsetPtrHigh;
			}
			if ((sync&AX_SYNCFLAG_ENDOFFSET) != 0)
			{
				// sync end offset
				internalShadowCopy->internalOffsets.endOffsetPtrLow = internalVPB->internalOffsets.endOffsetPtrLow;
				internalShadowCopy->internalOffsets.endOffsetPtrHigh = internalVPB->internalOffsets.endOffsetPtrHigh;
			}
			if ((sync&AX_SYNCFLAG_CURRENTOFFSET) != 0)
			{
				// sync current offset
				internalShadowCopy->internalOffsets.currentOffsetPtrLow = internalVPB->internalOffsets.currentOffsetPtrLow;
				internalShadowCopy->internalOffsets.currentOffsetPtrHigh = internalVPB->internalOffsets.currentOffsetPtrHigh;
			}
		}
		if ((sync&AX_SYNCFLAG_ADPCMDATA) != 0)
		{
			// sync adpcm data
			for (sint32 i = 0; i < 16; i++)
				internalShadowCopy->adpcmData.coef[i] = internalVPB->adpcmData.coef[i];
			internalShadowCopy->adpcmData.gain = internalVPB->adpcmData.gain;
			internalShadowCopy->adpcmData.scale = internalVPB->adpcmData.scale;
		}
		if ((sync&AX_SYNCFLAG_SRCDATA) != 0)
		{
			// sync voice all src data
			internalShadowCopy->src.ratioHigh = internalVPB->src.ratioHigh;
			internalShadowCopy->src.ratioLow = internalVPB->src.ratioLow;
			internalShadowCopy->src.currentFrac = internalVPB->src.currentFrac;
			internalShadowCopy->src.historySamples[0] = internalVPB->src.historySamples[0];
			internalShadowCopy->src.historySamples[1] = internalVPB->src.historySamples[1];
			internalShadowCopy->src.historySamples[2] = internalVPB->src.historySamples[2];
			internalShadowCopy->src.historySamples[3] = internalVPB->src.historySamples[3];
		}
		else
		{
			if ((sync&AX_SYNCFLAG_SRCRATIO) != 0)
			{
				// sync voice src ratio
				internalShadowCopy->src.ratioHigh = internalVPB->src.ratioHigh;
				internalShadowCopy->src.ratioLow = internalVPB->src.ratioLow;
			}
		}
		if ((sync&AX_SYNCFLAG_ADPCMLOOP) != 0)
		{
			// sync voice adpcm loop
			internalShadowCopy->adpcmLoop.loopScale = internalVPB->adpcmLoop.loopScale;
			internalShadowCopy->adpcmLoop.loopYn1 = internalVPB->adpcmLoop.loopYn1;
			internalShadowCopy->adpcmLoop.loopYn2 = internalVPB->adpcmLoop.loopYn2;
		}
		if ((sync&AX_SYNCFLAG_LPFCOEF) != 0)
		{
			// sync lpf coef
			internalShadowCopy->lpf.a0 = internalVPB->lpf.a0;
			internalShadowCopy->lpf.b0 = internalVPB->lpf.b0;
		}
		else
		{
			if ((sync&AX_SYNCFLAG_LPFDATA) != 0)
			{
				// sync lpf
				internalShadowCopy->lpf.on = internalVPB->lpf.on;
				internalShadowCopy->lpf.yn1 = internalVPB->lpf.yn1;
				internalShadowCopy->lpf.a0 = internalVPB->lpf.a0;
				internalShadowCopy->lpf.b0 = internalVPB->lpf.b0;
			}
		}
		if ((sync&AX_SYNCFLAG_BIQUADCOEF) != 0)
		{
			// sync biquad coef
			internalShadowCopy->biquad.b0 = internalVPB->biquad.b0;
			internalShadowCopy->biquad.b1 = internalVPB->biquad.b1;
			internalShadowCopy->biquad.b2 = internalVPB->biquad.b2;
			internalShadowCopy->biquad.a1 = internalVPB->biquad.a1;
			internalShadowCopy->biquad.a2 = internalVPB->biquad.a2;
		}
		else if ((sync&AX_SYNCFLAG_BIQUADDATA) != 0)
		{
			// sync biquad
			internalShadowCopy->biquad.on = internalVPB->biquad.on;
			internalShadowCopy->biquad.xn1 = internalVPB->biquad.xn1;
			internalShadowCopy->biquad.xn2 = internalVPB->biquad.xn2;
			internalShadowCopy->biquad.yn1 = internalVPB->biquad.yn1;
			internalShadowCopy->biquad.yn2 = internalVPB->biquad.yn2;
			internalShadowCopy->biquad.b0 = internalVPB->biquad.b0;
			internalShadowCopy->biquad.b1 = internalVPB->biquad.b1;
			internalShadowCopy->biquad.b2 = internalVPB->biquad.b2;
			internalShadowCopy->biquad.a1 = internalVPB->biquad.a1;
			internalShadowCopy->biquad.a2 = internalVPB->biquad.a2;
		}
		if ((sync&AX_SYNCFLAG_VOICEREMOTEON) != 0)
		{
			// sync VoiceRmtOn (AXSetVoiceRmtOn)
			internalShadowCopy->reserved148_voiceRmtOn = internalVPB->reserved148_voiceRmtOn;
		}
		if ((sync&AX_SYNCFLAG_4000000) != 0)
		{
			// todo
		}
		if ((sync&AX_SYNCFLAG_8000000) != 0)
		{
			// todo
			// AXSetVoiceRmtSrc
		}
		// other flags todo: 0x10000000, 0x20000000, 0x40000000 for RmtIIR
	}

	void AXIst_SyncVPB(AXVPBInternal_t** lastProcessedDSPShadowCopy, AXVPBInternal_t** lastProcessedPPCShadowCopy)
	{
		std::lock_guard<std::recursive_mutex> listLock(__AXVoiceListSpinlock);
		std::lock_guard<std::recursive_mutex> paramLock(__AXVoiceParamLock);

		AXVPBInternal_t* previousInternalDSP = nullptr;
		AXVPBInternal_t* previousInternalPPC = nullptr;
		for (sint32 priority = AX_PRIORITY_MAX - 1; priority >= AX_PRIORITY_LOWEST; priority--)
		{
			auto& voiceArray = AXVoiceList_GetListByPriority(priority);
			for(auto vpb : voiceArray)
			{
				sint32 index = vpb->index;
				sint32 depop = vpb->depop;
				AXVPBInternal_t* internalVPB = __AXVPBInternalVoiceArray + index;
				AXVPBInternal_t* internalShadowCopy = __AXVPBInternalVoiceShadowCopyArrayPtr + index;
				internalVPB->mixerSelect = vpb->mixerSelect;
				if (depop)
				{
					AXMix_DepopVoice(internalShadowCopy);
					vpb->depop = 0;
				}
				if (internalVPB->playbackState != _swapEndianU16(1) && vpb->sync == 0)
				{
					internalVPB->ukn2A2 = 2;
					internalShadowCopy->ukn2A2 = 2;
					internalVPB->nextAddrHigh = 0;
					internalVPB->nextAddrLow = 0;
				}
				else
				{
					internalVPB->ukn2A2 = 1;
					if (previousInternalPPC)
					{
						internalVPB->nextAddrHigh = previousInternalPPC->selfAddrHigh;
						internalVPB->nextAddrLow = previousInternalPPC->selfAddrLow;
						internalVPB->nextToProcess = previousInternalPPC;
						previousInternalPPC = internalVPB;
					}
					else
					{
						internalVPB->nextAddrHigh = 0;
						internalVPB->nextAddrLow = 0;
						internalVPB->nextToProcess = nullptr;
						previousInternalPPC = internalVPB;
					}
					AXIst_SyncSingleVPB(vpb);
					if (!AXVoiceProtection_IsProtectedByAnyThread(vpb))
					{
						vpb->depop = 0;
						vpb->sync = 0;
					}
				}
			}
		}
		// depop and reset voices which just stopped playing
		auto& freeVoicesArray = AXVoiceList_GetFreeVoices();
		for(auto vpb : freeVoicesArray)
		{
			AXVPBInternal_t* internalVPB = __AXVPBInternalVoiceArray + (sint32)vpb->index;
			AXVPBInternal_t* internalShadowCopy = __AXVPBInternalVoiceShadowCopyArrayPtr + (sint32)vpb->index;
			if (vpb->depop != (uint32be)0)
			{
				AXMix_DepopVoice(internalShadowCopy);
				vpb->depop = 0;
			}
			vpb->playbackState = 0;
			internalVPB->ukn2A2 = 2;
			internalShadowCopy->ukn2A2 = 2;
			internalVPB->playbackState = 0;
			internalShadowCopy->playbackState = 0;
		}
		// return last processed DSP/PPC voice internal shadow copy
		if (lastProcessedDSPShadowCopy)
		{
			if (previousInternalDSP)
			{
				AXVPBInternal_t* internalShadowCopy = __AXVPBInternalVoiceShadowCopyArrayPtr + (sint32)previousInternalDSP->index;
				*lastProcessedDSPShadowCopy = internalShadowCopy;
			}
			else
				*lastProcessedDSPShadowCopy = nullptr;
		}
		if (lastProcessedPPCShadowCopy)
		{
			if (previousInternalPPC)
			{
				AXVPBInternal_t* internalShadowCopy = __AXVPBInternalVoiceShadowCopyArrayPtr + (sint32)previousInternalPPC->index;
				*lastProcessedPPCShadowCopy = internalShadowCopy;
			}
			else
				*lastProcessedPPCShadowCopy = nullptr;
		}
	}

	void AXIst_HandleFrameCallbacks()
	{
		// snapshot under the lock so callbacks may (de)register without deadlocking
		MPTR frameCallback;
		MPTR appFrameCallbacks[AX_APP_FRAME_CALLBACK_MAX];
		{
			std::lock_guard<std::mutex> lock(__AXAppFrameCallbackMutex);
			frameCallback = __AXFrameCallback;
			memcpy(appFrameCallbacks, __AXAppFrameCallback, sizeof(appFrameCallbacks));
		}
		// frame callback
		if (frameCallback != MPTR_NULL)
		{
			// execute frame callback (no params)
			PPCCoreCallback(frameCallback);
		}
		// app frame callback
		for (sint32 i = 0; i < AX_APP_FRAME_CALLBACK_MAX; i++)
		{
			if (appFrameCallbacks[i] == MPTR_NULL)
				continue;
			// execute app frame callback (no params)
			PPCCoreCallback(appFrameCallbacks[i]);
		}
	}

	std::atomic_bool __AXIstIsProcessingFrame = false;

	void AXIst_GenerateFrame()
	{
		// generate one frame (3MS) of audio
		__AXIstIsProcessingFrame.store(true);

		memset(__AXTVOutputBuffer.GetPtr(), 0, AX_SAMPLES_PER_3MS_48KHZ * AX_TV_CHANNEL_COUNT * sizeof(sint32));
		memset(__AXDRCOutputBuffer.GetPtr(), 0, AX_SAMPLES_PER_3MS_48KHZ * AX_DRC_CHANNEL_COUNT * sizeof(sint32));

		AXVPBInternal_t* internalShadowCopyDSPHead = nullptr;
		AXVPBInternal_t* internalShadowCopyPPCHead = nullptr;

		AXIst_SyncVPB(&internalShadowCopyDSPHead, &internalShadowCopyPPCHead);

		AXMix_process(internalShadowCopyPPCHead);

		AXOut_ResetFinalMixCBData();
		AXIst_ProcessFinalMixCallback();

		if (diag_on())
		{
			// Did any voice reach the mixer, and did the mix write a non-zero sample?
			// peak_mix is the 32 kHz buffer before the 32->48 shift. peak_lr / peak_other are the
			// 48 kHz buffer AXOut_SubmitTVFrame hands to the host (stereo pair, then the other four).
			auto abs_s32 = [](sint32 s) -> uint32 {
				return s < 0 ? (uint32)(-(long long)s) : (uint32)s;
			};
			AXVPBInternal_t* shadow = __AXVPBInternalVoiceShadowCopyArrayPtr;
			uint32 linked_ppc = 0, linked_dsp = 0, mask_tv = 0;
			for (AXVPBInternal_t* v = internalShadowCopyPPCHead; v && linked_ppc < (uint32)AX_MAX_VOICES;)
			{
				if (v < shadow || v >= shadow + AX_MAX_VOICES)
					break;
				linked_ppc++;
				for (int b = 0; b < 4; b++)
					if (v->deviceMixMaskTV[b] != 0)
					{
						mask_tv++;
						break;
					}
				v = v->nextToProcess.GetPtr();
			}
			for (AXVPBInternal_t* v = internalShadowCopyDSPHead; v && linked_dsp < (uint32)AX_MAX_VOICES;)
			{
				if (v < shadow || v >= shadow + AX_MAX_VOICES)
					break;
				linked_dsp++;
				v = v->nextToProcess.GetPtr();
			}
			uint32 playing = 0;
			for (int i = 0; i < AX_MAX_VOICES; i++)
				if (_swapEndianU16(__AXVPBInternalVoiceArray[i].playbackState) == 1)
					playing++;
			uint32 peak_mix = 0, peak_lr = 0, peak_other = 0;
			sint32* mix = __AXTVOutputBuffer.GetPtr();
			for (int i = 0; i < AX_SAMPLES_PER_3MS_48KHZ * AX_TV_CHANNEL_COUNT; i++)
			{
				uint32 a = abs_s32(_swapEndianS32(mix[i]));
				if (a > peak_mix)
					peak_mix = a;
			}
			sint32* out = __AXTVBuffer48.GetPtr();
			for (int c = 0; c < AX_TV_CHANNEL_COUNT; c++)
			{
				for (int i = 0; i < AX_SAMPLES_PER_3MS_48KHZ; i++)
				{
					uint32 a = abs_s32(_swapEndianS32(out[c * AX_SAMPLES_PER_3MS_48KHZ + i]));
					if (c < 2)
					{
						if (a > peak_lr)
							peak_lr = a;
					}
					else if (a > peak_other)
						peak_other = a;
				}
			}
			uint32 flags = 0;
			if (__AXDeviceFinalMixCallback[AX_DEV_TV])
				flags |= 1;
			if (__AXFrameCallback)
				flags |= 2;
			for (int i = 0; i < AX_APP_FRAME_CALLBACK_MAX; i++)
				if (__AXAppFrameCallback[i])
				{
					flags |= 4;
					break;
				}
			title_diag_ax(playing, linked_ppc, linked_dsp, mask_tv, peak_mix, peak_lr, peak_other, flags);
		}

		// todo - additional phases. See unimplemented API:
		// AXSetDRCVSMode
		// AXSetDeviceCompressor
		// AXRegisterPostFinalMixCallback

		AXOut_SubmitTVFrame(0);
		AXOut_SubmitDRCFrame(0);

		__AXIstIsProcessingFrame.store(false);
	}

	bool AXIst_IsFrameBeingProcessed()
	{
		return __AXIstIsProcessingFrame.load();
	}

	// ---- AX frame thread ----
	// Replaces Cemu's AXIst_ThreadEntry (a guest thread waiting on an OSMessageQueue) plus the
	// AXOut_update() pacing Cemu runs from its CPU scheduler: a host thread, adopted as a guest
	// thread so callbacks can run guest code, generates one frame every 3 ms of host time.
	namespace
	{
		std::mutex s_istLock;
		bool s_istStopRequested = false;
		bool s_istRunning = false;
		pthread_t s_istHostThread;
		constexpr size_t kIstHostStackSize = 64u << 20; // guest call depth maps onto the host stack
		constexpr uint32 kIstGuestStackSize = 0x10000;  // Cemu: 0x4000

		void AXIst_ThreadEntry()
		{
			threads::adopt_host_thread("AX", kIstGuestStackSize, 1);

			using clock = std::chrono::steady_clock;
			// Cemu AXOut_update: 3 ms per frame, 2.9 ms while the output device is running low,
			// at least 1.7 ms between frames, skip ahead instead of catching up when far behind
			constexpr auto kWaitDuration = std::chrono::microseconds(3000);
			constexpr auto kWaitDurationFast = std::chrono::microseconds(2900);
			constexpr auto kWaitDurationSlow = std::chrono::microseconds(3100); // host addition: device clock drift
			constexpr auto kWaitDurationMinimum = std::chrono::microseconds(1700);
			constexpr auto kTimeout = std::chrono::milliseconds(100);

			auto nextFrame = clock::now();
			auto lastFrame = nextFrame - kWaitDuration;
			std::unique_lock<std::mutex> lock(s_istLock);
			while (!s_istStopRequested)
			{
				sint32 pacing = AXOut_GetFramePacingAdjustment();
				nextFrame += pacing < 0 ? kWaitDurationFast : (pacing > 0 ? kWaitDurationSlow : kWaitDuration);
				auto now = clock::now();
				if (now - nextFrame >= kTimeout)
					nextFrame = now; // too far behind (stall, debugger): skip forward
				auto wakeTime = std::max(nextFrame, lastFrame + kWaitDurationMinimum);
				if (wakeTime > now)
				{
					auto waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(wakeTime - now).count();
					lock.unlock();
					SDL_DelayPrecise((Uint64)waitNs);
					lock.lock();
				}
				if (s_istStopRequested)
					break;
				lock.unlock();
				lastFrame = clock::now();
				const bool measure = diag_on();
				auto workStart = measure ? clock::now() : clock::time_point{};
				AXIst_GenerateFrame();
				numProcessedFrames++;
				if (measure)
				{
					static auto reportStart = clock::now();
					static uint64_t reportFrames = 0;
					static auto reportWork = clock::duration::zero();
					static auto maxWork = clock::duration::zero();
					auto work = clock::now() - workStart;
					reportFrames++;
					reportWork += work;
					maxWork = std::max(maxWork, work);
					auto elapsed = clock::now() - reportStart;
					if (elapsed >= std::chrono::seconds(5))
					{
						double seconds = std::chrono::duration<double>(elapsed).count();
						LOG("audio: AX %.1f frames/s, mix avg %.2f ms max %.2f ms",
						    reportFrames / seconds,
						    std::chrono::duration<double, std::milli>(reportWork).count() / reportFrames,
						    std::chrono::duration<double, std::milli>(maxWork).count());
						reportStart = clock::now();
						reportFrames = 0;
						reportWork = clock::duration::zero();
						maxWork = clock::duration::zero();
					}
				}
				lock.lock();
			}
		}
	}

	void AXIst_InitThread()
	{
		__AXIstIsProcessingFrame = false;
		std::lock_guard<std::mutex> lock(s_istLock);
		if (s_istRunning)
			return;
		s_istStopRequested = false;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
#if TARGET_OS_IOS  // iOS port: see ios/src/vm_ios.cpp
		pthread_attr_setstacksize(&attr, botw_ios_stack_size(kIstHostStackSize));
#else
		pthread_attr_setstacksize(&attr, kIstHostStackSize);
#endif
		int r = pthread_create(&s_istHostThread, &attr, [](void*) -> void* {
			botw_set_thread_name("AX");
			AXIst_ThreadEntry();
			return nullptr;
		}, nullptr);
		pthread_attr_destroy(&attr);
		if (r != 0)
		{
			LOG("snd_core: failed to create the AX thread (%d)", r);
			return;
		}
		s_istRunning = true;
	}

	void AXIst_StopThread()
	{
		{
			std::lock_guard<std::mutex> lock(s_istLock);
			if (!s_istRunning)
				return;
			s_istStopRequested = true;
			s_istRunning = false;
		}
		if (pthread_equal(pthread_self(), s_istHostThread))
			pthread_detach(s_istHostThread); // AXQuit from inside an AX callback: the loop exits after this frame
		else
			pthread_join(s_istHostThread, nullptr);
	}

	// ---- exports BotW imports that Cemu does not implement ----

	// Cemu always uses the linear upsampler (forceLinearUpsampler); the setting is stored for completeness.
	sint32 AXSetDeviceLinearUpsampler(uint32 device, uint32 deviceIndex, uint32 useLinear)
	{
		if (device == AX_DEV_TV)
		{
			if (deviceIndex != 0)
				return -2;
			__AXTVUpsampler.useLinearUpsampler = useLinear != 0;
		}
		else if (device == AX_DEV_DRC)
		{
			if (deviceIndex > 1)
				return -2;
			__AXDRCUpsampler[deviceIndex].useLinearUpsampler = useLinear != 0;
		}
		else
			return -1;
		return 0;
	}

	// Output compressor stage: not implemented (Cemu lists it as a missing frame phase).
	uint32 __AXDeviceCompressor[AX_DEV_COUNT];

	sint32 AXSetDeviceCompressor(uint32 device, uint32 mode)
	{
		if (device >= AX_DEV_COUNT)
			return -1;
		__AXDeviceCompressor[device] = mode;
		return 0;
	}

	// DSP/PPC load "exceed" callback: emulated mixing never runs over budget, so it never fires.
	MPTR __AXExceedCallback = MPTR_NULL;

	MPTR AXRegisterExceedCallback(MPTR funcAddr)
	{
		MPTR prev = __AXExceedCallback;
		__AXExceedCallback = funcAddr;
		return prev;
	}

	// Profiling: no swap profiles are recorded.
	void AXInitProfile(void* profile, uint32 maxProfiles)
	{
	}

	uint32 AXGetSwapProfile(void* profile, uint32 maxProfiles)
	{
		return 0;
	}
}
