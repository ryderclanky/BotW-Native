// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2_Surface_Copy.cpp), MPL-2.0. GX2CopySurface & co. The legacy
// gx2Export_* functions are converted to typed GX2:: functions with the same argument order. GPU copies emit the
// IT_HLE_COPY_SURFACE_NEW packet exactly like Cemu. CPU copies (LINEAR_SPECIAL surfaces) differ, see
// GX2CopySurfaceInternal. gx2SurfaceCopySoftware stays a global function with Cemu's signature: the vendored Latte
// tree (runtime/cemu/Cafe/HW/Latte/Core/LatteSurfaceCopy.cpp) calls it. gx2CopySurfaceTest (benchmark) dropped.
#include <algorithm>

#include "../compat/cemu_export.h"
#include "../latte/addrlib/LatteAddrLib.h"
#include "../latte/isa/LatteReg.h"
#include "../latte/isa/RegDefines.h"
#include "GX2.h"
#include "GX2_Resource.h"
#include "GX2_Surface.h"

template<uint32 copyBpp>
void gx2SurfaceCopySoftware_specialized(
	uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice, uint32 srcSwizzle, uint32 srcHwTileMode,
	uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth, uint32 dstSlice, uint32 dstSwizzle, uint32 dstHwTileMode,
	uint32 copyWidth, uint32 copyHeight)
{
	uint32 srcPipeSwizzle = (srcSwizzle >> 8) & 1;
	uint32 srcBankSwizzle = ((srcSwizzle >> 9) & 3);
	uint32 dstPipeSwizzle = (dstSwizzle >> 8) & 1;
	uint32 dstBankSwizzle = ((dstSwizzle >> 9) & 3);

	for (uint32 y = 0; y < copyHeight; y++)
	{
		for (uint32 x = 0; x < copyWidth; x++)
		{
			// calculate address of input block
			uint32 srcOffset = 0;
			if (srcHwTileMode == 0 || srcHwTileMode == 1)
				srcOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, srcSlice, 0, copyBpp, srcPitch, surfSrcHeight, srcDepth);
			else if (srcHwTileMode == 2 || srcHwTileMode == 3)
				srcOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, srcSlice, copyBpp, srcPitch, surfSrcHeight, (Latte::E_HWTILEMODE)srcHwTileMode, false);
			else
				srcOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, srcSlice, 0, copyBpp, srcPitch, surfSrcHeight, 1 * 1, (Latte::E_HWTILEMODE)srcHwTileMode, false, srcPipeSwizzle, srcBankSwizzle);
			uint8* inputBlockData = inputData + srcOffset;
			// calculate address of output block
			uint32 dstOffset = 0;
			if (dstHwTileMode == 0 || dstHwTileMode == 1)
				dstOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, dstSlice, 0, copyBpp, dstPitch, surfDstHeight, dstDepth);
			else if (dstHwTileMode == 2 || dstHwTileMode == 3)
				dstOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, dstSlice, copyBpp, dstPitch, surfDstHeight, (Latte::E_HWTILEMODE)dstHwTileMode, false);
			else
				dstOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, dstSlice, 0, copyBpp, dstPitch, surfDstHeight, 1 * 1, (Latte::E_HWTILEMODE)dstHwTileMode, false, dstPipeSwizzle, dstBankSwizzle);
			uint8* outputBlockData = outputData + dstOffset;

			if constexpr (copyBpp == 8)
			{
				outputBlockData[0] = inputBlockData[0];
			}
			else if constexpr (copyBpp == 16)
			{
				*(uint16*)outputBlockData = *(uint16*)inputBlockData;
			}
			else if constexpr (copyBpp == 32)
			{
				*(uint32*)outputBlockData = *(uint32*)inputBlockData;
			}
			else if constexpr (copyBpp == 64)
			{
				*(uint32*)(outputBlockData + 0) = *(uint32*)(inputBlockData + 0);
				*(uint32*)(outputBlockData + 4) = *(uint32*)(inputBlockData + 4);
			}
			else if constexpr (copyBpp == 128)
			{
				*(uint32*)(outputBlockData + 0) = *(uint32*)(inputBlockData + 0);
				*(uint32*)(outputBlockData + 4) = *(uint32*)(inputBlockData + 4);
				*(uint32*)(outputBlockData + 8) = *(uint32*)(inputBlockData + 8);
				*(uint32*)(outputBlockData + 12) = *(uint32*)(inputBlockData + 12);
			}
		}
	}
}

// fast copy for tilemode 4 to tilemode 4
// assumes aa 1
// this only supports the cases where every micro tile fits within 256 bytes (group size)
// we could accelerate this even further if we copied whole macro blocks
void gx2SurfaceCopySoftware_fastPath_tm4Copy(uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice, uint32 srcSwizzle,
	uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth, uint32 dstSlice, uint32 dstSwizzle,
	uint32 copyWidth, uint32 copyHeight, uint32 copyBpp)
{
	cemu_assert_debug((copyWidth & 7) == 0);
	cemu_assert_debug((copyHeight & 7) == 0);

	uint32 srcPipeSwizzle = (srcSwizzle >> 8) & 1;
	uint32 srcBankSwizzle = ((srcSwizzle >> 9) & 3);
	uint32 dstPipeSwizzle = (dstSwizzle >> 8) & 1;
	uint32 dstBankSwizzle = ((dstSwizzle >> 9) & 3);

	uint32 texelBytes = copyBpp / 8;

	if (srcSlice == dstSlice && srcSwizzle == dstSwizzle && surfSrcHeight == surfDstHeight && srcPitch == dstPitch)
	{
		// shared tile offsets
		for (uint32 y = 0; y < copyHeight; y += 8)
		{
			for (uint32 x = 0; x < copyWidth; x += 8)
			{
				// copy 8x8 micro tile
				uint32 offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, srcSlice, 0, copyBpp, srcPitch, surfSrcHeight, 1 * 1, Latte::E_HWTILEMODE::TM_2D_TILED_THIN1, false, srcPipeSwizzle, srcBankSwizzle);
				uint8* inputBlockData = inputData + offset;
				uint8* outputBlockData = outputData + offset;
				memcpy(outputBlockData, inputBlockData, texelBytes * (8 * 8));
			}
		}
	}
	else
	{
		// separate tile offsets
		for (uint32 y = 0; y < copyHeight; y += 8)
		{
			for (uint32 x = 0; x < copyWidth; x += 8)
			{
				// copy 8x8 micro tile
				uint32 srcOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, srcSlice, 0, copyBpp, srcPitch, surfSrcHeight, 1 * 1, Latte::E_HWTILEMODE::TM_2D_TILED_THIN1, false, srcPipeSwizzle, srcBankSwizzle);
				uint8* inputBlockData = inputData + srcOffset;
				uint32 dstOffset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, dstSlice, 0, copyBpp, dstPitch, surfDstHeight, 1 * 1, Latte::E_HWTILEMODE::TM_2D_TILED_THIN1, false, dstPipeSwizzle, dstBankSwizzle);
				uint8* outputBlockData = outputData + dstOffset;
				memcpy(outputBlockData, inputBlockData, texelBytes * (8 * 8));
			}
		}
	}
}

void gx2SurfaceCopySoftware(
	uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice, uint32 srcSwizzle, uint32 srcHwTileMode,
	uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth, uint32 dstSlice, uint32 dstSwizzle, uint32 dstHwTileMode,
	uint32 copyWidth, uint32 copyHeight, uint32 copyBpp)
{
	if (srcHwTileMode == 16)
		srcHwTileMode = 0;
	if (dstHwTileMode == 16)
		dstHwTileMode = 0;

	if (srcHwTileMode == 4 && dstHwTileMode == 4 && (copyWidth & 7) == 0 && (copyHeight & 7) == 0 && copyBpp <= 32) // todo - check sample == 1
	{
		gx2SurfaceCopySoftware_fastPath_tm4Copy(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, copyWidth, copyHeight, copyBpp);
		return;
	}

	if (copyBpp == 8)
		gx2SurfaceCopySoftware_specialized<8>(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, srcHwTileMode, outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, dstHwTileMode, copyWidth, copyHeight);
	else if (copyBpp == 16)
		gx2SurfaceCopySoftware_specialized<16>(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, srcHwTileMode, outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, dstHwTileMode, copyWidth, copyHeight);
	else if (copyBpp == 32)
		gx2SurfaceCopySoftware_specialized<32>(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, srcHwTileMode, outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, dstHwTileMode, copyWidth, copyHeight);
	else if (copyBpp == 64)
		gx2SurfaceCopySoftware_specialized<64>(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, srcHwTileMode, outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, dstHwTileMode, copyWidth, copyHeight);
	else if (copyBpp == 128)
		gx2SurfaceCopySoftware_specialized<128>(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, srcHwTileMode, outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, dstHwTileMode, copyWidth, copyHeight);
	else
		cemu_assert_debug(false);
}

namespace GX2
{


// Surface copy handling is complicated because of having to simulate unified memory
// GX2CopySurface supports two modes:
// - When source or destination surface have a tilemode of LINEAR_SPECIAL (16) -> Copy is done synchronously on the CPU
// - In all other cases -> Copy is done on the GPU via draw commands

// But in Cemu things are more complicated, we generally can't do pure CPU copies because a surface's texture data
// may only exist in VRAM right now. So we always need to delegate copying to the renderer thread (which has access to the texture cache)

// In Cemu we thus handle it like this:
// For GX2 CPU copies -> Submit as async command to the renderer (will be processed asap) and stall until completed
// For GX2 GPU copies -> Submit as HLE command to Latte's command queue to be executed in order

void GX2CopySurfaceInternal(GX2Surface* srcSurface, uint32 srcMip, uint32 srcSlice, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice)
{
	sint32 dstWidth = dstSurface->width;
	sint32 dstHeight = dstSurface->height;
	sint32 srcWidth = srcSurface->width;
	sint32 srcHeight = srcSurface->height;

	sint32 dstMipWidth = std::max(dstWidth>>dstMip, 1);
	sint32 dstMipHeight = std::max(dstHeight>>dstMip, 1);
	sint32 srcMipWidth = std::max(srcWidth>>srcMip, 1);
	sint32 srcMipHeight = std::max(srcHeight>>srcMip, 1);

	if( dstMipWidth != srcMipWidth || dstMipHeight != srcMipHeight )
	{
		cemuLog_logDebugOnce(LogType::Force, "GX2CopySurface: Mismatching mip resolution");
		return;
	}
	// handle format
	Latte::E_GX2SURFFMT srcFormat = srcSurface->format;
	Latte::E_GX2SURFFMT dstFormat = dstSurface->format;
	auto srcHwFormat = Latte::GetHWFormat(srcFormat);
	auto dstHwFormat = Latte::GetHWFormat(dstFormat);
	// get texture info
	LatteAddrLib::AddrSurfaceInfo_OUT surfOutSrc = {0};
	GX2::GX2CalculateSurfaceInfo(srcSurface, srcMip, &surfOutSrc);
	LatteAddrLib::AddrSurfaceInfo_OUT surfOutDst = {0};
	GX2::GX2CalculateSurfaceInfo(dstSurface, dstMip, &surfOutDst);
	// check parameters
	if (srcSurface->numLevels == 0)
	{
		debug_printf("GX2CopySurface(): mip count is 0\n");
		return;
	}
	// make sure formats are compatible
	if( surfOutSrc.bpp != surfOutDst.bpp )
	{
		cemuLog_logDebug(LogType::Force, "GX2CopySurface(): Format bpp mismatch (src=0x{:04x} dst=0x{:04x})", (sint32)srcFormat, (sint32)dstFormat);
		return;
	}
	// get input pointer
	cemu_assert(srcMip < srcSurface->numLevels);
	uint8* srcDataPtr = nullptr;
	if( srcMip == 0 )
		srcDataPtr = (uint8*)memory_getPointerFromVirtualOffset(srcSurface->imagePtr);
	else if( srcMip == 1 )
		srcDataPtr = (uint8*)memory_getPointerFromVirtualOffset(srcSurface->mipPtr);
	else
		srcDataPtr = (uint8*)memory_getPointerFromVirtualOffset(srcSurface->mipPtr + srcSurface->mipOffset[srcMip - 1]);
	// get output pointer
	cemu_assert(dstMip < dstSurface->numLevels);
	uint8* dstDataPtr = nullptr;
	if( dstMip == 0 )
		dstDataPtr = (uint8*)memory_getPointerFromVirtualOffset(dstSurface->imagePtr);
	else if( dstMip == 1 )
		dstDataPtr = (uint8*)memory_getPointerFromVirtualOffset(dstSurface->mipPtr);
	else
		dstDataPtr = (uint8*)memory_getPointerFromVirtualOffset(dstSurface->mipPtr + dstSurface->mipOffset[dstMip - 1]);
	// note: pitch is taken from surfOutSrc/surfOutDst, which may different from the pitch stored in the input surface structs
	uint32 srcPitch = surfOutSrc.pitch;
	Latte::E_GX2TILEMODE srcTilemode = (srcSurface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL) ? srcSurface->tileMode.value() : (Latte::E_GX2TILEMODE)surfOutSrc.hwTileMode;
	Latte::E_GX2TILEMODE dstTilemode = (dstSurface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL) ? dstSurface->tileMode.value() : (Latte::E_GX2TILEMODE)surfOutDst.hwTileMode;

	if (srcTilemode == Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL)
	{
		// todo - why is this necessary? GX2CalculateSurfaceInfo should already handle this
		srcPitch = srcSurface->pitch >> srcMip;
		srcPitch = std::max<uint32>(srcPitch, 1);
	}
	uint32 copyWidth = std::max<uint32>(dstWidth>>dstMip, 1);
	uint32 copyHeight = std::max<uint32>(dstHeight>>dstMip, 1);

	cemu_assert(copyWidth <= std::max<uint32>(srcWidth>>srcMip, 1));
	cemu_assert(copyHeight <= std::max<uint32>(srcHeight>>srcMip, 1));

	cemuLog_log(LogType::GX2, "GX2CopySurface:");
	cemuLog_log(LogType::GX2,"srcSurface: imagePtr=0x{:08x} mipPtr=0x{:08x} swizzle=0x{:06x} width={} height={} depth={} pitch=0x{:x} tilemode=0x{:x} format=0x{:x} mip={} slice={}",
		(uint32)srcSurface->imagePtr, (uint32)srcSurface->mipPtr, (uint32)srcSurface->swizzle, (uint32)srcSurface->width, (uint32)srcSurface->height, (uint32)srcSurface->depth, (uint32)srcSurface->pitch,
		(uint32)srcSurface->tileMode.value(), (uint32)srcSurface->format.value(), srcMip, srcSlice);
	cemuLog_log(LogType::GX2, "dstSurface: imagePtr=0x{:08x} mipPtr=0x{:08x} swizzle=0x{:06x} width={} height={} depth={} pitch=0x{:x} tilemode=0x{:x} format=0x{:x} mip={} slice={}",
		(uint32)dstSurface->imagePtr, (uint32)dstSurface->mipPtr, (uint32)dstSurface->swizzle, (uint32)dstSurface->width, (uint32)dstSurface->height, (uint32)dstSurface->depth, (uint32)dstSurface->pitch,
		(uint32)dstSurface->tileMode.value(), (uint32)dstSurface->format.value(), dstMip, dstSlice);

	bool isGX2CPUCopy = srcSurface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL || dstSurface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL;
	// BotW-Native: for GX2 CPU copies Cemu fills two LatteSurfaceCopyParam structs, queues them as an async command
	// for its GPU thread (LatteAsyncCommand_queueTextureCopy -> LatteSurfaceCopy_copySurfaceNew) and spins until the
	// async queue is empty. This runtime has no such side channel, so the CPU copy uses the IT_HLE_COPY_SURFACE_NEW
	// packet below (its GPU handler calls the same LatteSurfaceCopy_copySurfaceNew, and the packet carries exactly the
	// values Cemu puts into srcCopy/dstCopy/copyRect), followed by GX2DrawDone so the copy has finished when
	// GX2CopySurface returns. Difference: it runs in order after already queued commands instead of "asap"; while a
	// display list is being recorded the packet is only recorded (no wait).

	// copy via GPU commands
	// for simplicity and performance Cemu uses a HLE command to handle surface copies,
	// a more accurate implementation would setup actual drawcalls to copy the texture data
	GX2::GX2ReserveCmdSpace(23);
	gx2WriteGather_submit(pm4HeaderType3(IT_HLE_COPY_SURFACE_NEW, 4+9*2),
	// copy rect
	(uint32)0, // x
	(uint32)0, // y
	(uint32)copyWidth,
	(uint32)copyHeight,
	// src
	(uint32)MEMPTR<void>(srcDataPtr).GetMPTR(),
	(uint32)srcSurface->swizzle,
	(uint32)srcSurface->format.value(),
	(uint32)surfOutSrc.pitch,
	(uint32)surfOutSrc.height,
	srcSlice,
	(uint32)srcSurface->dim.value(),
	(uint32)srcTilemode,
	(uint32)srcSurface->aa,
	// dst
	(uint32)MEMPTR<void>(dstDataPtr).GetMPTR(),
	(uint32)dstSurface->swizzle,
	(uint32)dstSurface->format.value(),
	(uint32)surfOutDst.pitch,
	(uint32)surfOutDst.height,
	dstSlice,
	(uint32)dstSurface->dim.value(),
	(uint32)dstTilemode,
	(uint32)dstSurface->aa
	);

	if (isGX2CPUCopy && !GX2::GX2GetDisplayListWriteStatus())
		GX2::GX2DrawDone();
}

void gx2Surface_GX2CopySurface(GX2Surface* srcSurface, uint32 srcMip, uint32 srcSlice, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice)
{
	GX2CopySurfaceInternal(srcSurface, srcMip, srcSlice, dstSurface, dstMip, dstSlice);
}

// gx2Export_GX2CopySurface: r3 = srcSurface, r4 = srcMip, r5 = srcSlice, r6 = dstSurface, r7 = dstMip, r8 = dstSlice
void GX2CopySurface(GX2Surface* srcSurface, uint32 srcMip, uint32 srcSlice, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice)
{
	gx2Surface_GX2CopySurface(srcSurface, srcMip, srcSlice, dstSurface, dstMip, dstSlice);
}

typedef struct
{
	sint32 left;
	sint32 top;
	sint32 right;
	sint32 bottom;
}GX2Rect_t;

typedef struct  
{
	sint32 x;
	sint32 y;
}GX2Point_t;

// gx2Export_GX2CopySurfaceEx: r3 = srcSurface, r4 = srcMip, r5 = srcSlice, r6 = dstSurface, r7 = dstMip, r8 = dstSlice,
// r9 = rectCount, r10 = rectSrcArray, [r1+8] = pointDstArray
void GX2CopySurfaceEx(GX2Surface* srcSurface, uint32 srcMip, uint32 srcSlice, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice, sint32 rectCount, MPTR rectSrcArrayMPTR, MPTR pointDstArrayMPTR)
{
	GX2Rect_t* rectSrc = (GX2Rect_t*)memory_getPointerFromVirtualOffset(rectSrcArrayMPTR);
	GX2Point_t* rectDst = (GX2Point_t*)memory_getPointerFromVirtualOffset(pointDstArrayMPTR);
	(void)rectDst;

	for(sint32 i=0; i<rectCount; i++)
	{
		uint32 srcWidth = srcSurface->width;
		uint32 srcHeight = srcSurface->height;
		// calculate rect size
		sint32 rectSrcX = (sint32)_swapEndianU32((uint32)rectSrc[i].left);
		sint32 rectSrcY = (sint32)_swapEndianU32((uint32)rectSrc[i].top);
		sint32 rectWidth = (sint32)_swapEndianU32((uint32)rectSrc[i].right) - rectSrcX;
		sint32 rectHeight = (sint32)_swapEndianU32((uint32)rectSrc[i].bottom) - rectSrcY;
		if( rectSrcX == 0 && rectSrcY == 0 && rectWidth == srcWidth && rectHeight == srcHeight )
		{
			// special case in which GX2CopySurfaceEx acts like GX2CopySurface()
			gx2Surface_GX2CopySurface(srcSurface, srcMip, srcSlice, dstSurface, dstMip, dstSlice);
		}
	}
}

// gx2Export_GX2ResolveAAColorBuffer: r3 = srcColorBuffer, r4 = dstSurface, r5 = dstMip, r6 = dstSlice
void GX2ResolveAAColorBuffer(GX2ColorBuffer* srcColorBuffer, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice)
{
	GX2Surface* srcSurface = &srcColorBuffer->surface;
	uint32 srcMip = srcColorBuffer->viewMip;
	uint32 srcSlice = srcColorBuffer->viewFirstSlice;

	// allocate pixel buffer
	sint32 dstWidth = dstSurface->width;
	sint32 dstHeight = dstSurface->height;
	sint32 srcWidth = srcSurface->width;
	sint32 srcHeight = srcSurface->height;

	uint32 dstMipWidth = std::max(dstWidth>>dstMip, 1);
	uint32 dstMipHeight = std::max(dstHeight>>dstMip, 1);
	uint32 srcMipWidth = std::max(srcWidth>>srcMip, 1);
	uint32 srcMipHeight = std::max(srcHeight>>srcMip, 1);

	// check if surface properties match
	if( srcSurface->width != dstSurface->width || srcSurface->height != dstSurface->height )
	{
		return;
	}
	if( dstMipWidth != srcMipWidth || dstMipHeight != srcMipHeight )
	{
		cemu_assert_suspicious();
		return;
	}
	// handle format
	Latte::E_GX2SURFFMT srcFormat = srcSurface->format;
	Latte::E_GX2SURFFMT dstFormat = dstSurface->format;
	sint32 srcStepX = 1;
	sint32 srcStepY = 1;
	sint32 dstStepX = 1;
	sint32 dstStepY = 1;
	auto srcHwFormat = Latte::GetHWFormat(srcFormat);
	auto dstHwFormat = Latte::GetHWFormat(dstFormat);
	// calculate step size for compressed textures
	if( Latte::IsCompressedFormat(srcHwFormat) )
	{
		srcStepX = 4;
		srcStepY = 4;
	}
	if(Latte::IsCompressedFormat(dstHwFormat) )
	{
		dstStepX = 4;
		dstStepY = 4;
	}
	cemu_assert_debug(srcStepX == dstStepX && srcStepY == dstStepY);

	if( srcHwFormat != dstHwFormat )
	{
		// mismatching format
		debug_printf("GX2ResolveAAColorBuffer(): Format mismatch\n");
		cemu_assert_unimplemented();
		return;
	}
	GX2CopySurfaceInternal(srcSurface, srcMip, srcSlice, dstSurface, dstMip, dstSlice);
}

// gx2Export_GX2ConvertDepthBufferToTextureSurface: r3 = depthBuffer, r4 = dstSurface, r5 = dstMip, r6 = dstSlice
void GX2ConvertDepthBufferToTextureSurface(GX2DepthBuffer* depthBuffer, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice)
{
	if (dstMip != 0 || dstSlice != 0)
		debugBreakpoint();
	// get texture info
	LatteAddrLib::AddrSurfaceInfo_OUT surfOutSrc = { 0 };
	GX2::GX2CalculateSurfaceInfo(&depthBuffer->surface, 0, &surfOutSrc);
	LatteAddrLib::AddrSurfaceInfo_OUT surfOutDst = { 0 };
	GX2::GX2CalculateSurfaceInfo(dstSurface, 0, &surfOutDst);

	if (depthBuffer->surface.imagePtr == dstSurface->imagePtr)
	{
		// in-place re-tiling doesn't need any actual copy operation?
		if (dstMip != 0 || dstSlice != 0)
			debugBreakpoint();
		debug_printf("In-place re-tiling\n");
		return;
	}

	uint32 numSlices = std::max<uint32>(depthBuffer->viewNumSlices, 1);
	for (uint32 subSliceIndex = 0; subSliceIndex < numSlices; subSliceIndex++)
	{
		// send copy command to GPU
		GX2CopySurfaceInternal(&depthBuffer->surface, depthBuffer->viewMip.value(), depthBuffer->viewFirstSlice.value() + subSliceIndex, dstSurface, dstMip, dstSlice + subSliceIndex);
	}
}

} // namespace GX2

// registered with osLib_addFunction in Cemu's GX2SurfaceCopyInit
CAFE_EXPORT(gx2, GX2CopySurface, GX2::GX2CopySurface);
CAFE_EXPORT(gx2, GX2CopySurfaceEx, GX2::GX2CopySurfaceEx);
CAFE_EXPORT(gx2, GX2ResolveAAColorBuffer, GX2::GX2ResolveAAColorBuffer);
CAFE_EXPORT(gx2, GX2ConvertDepthBufferToTextureSurface, GX2::GX2ConvertDepthBufferToTextureSurface);
