/*
 * gpudevice.h
 *
 * Definition of GPU device properties
 * --
 * Copyright 2011-2016 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2016 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef GPUDEVICE_H
#define GPUDEVICE_H

/*
 * Attribute of the available GPU devices
 */
typedef struct DevAttributes
{
	cl_int		DEV_ID;
	char		DEV_NAME[256];
	size_t		DEV_TOTAL_MEMSZ;
	cl_int		CORES_PER_MPU;
	cl_int		MAX_THREADS_PER_BLOCK;
	cl_int		MAX_BLOCK_DIM_X;
	cl_int		MAX_BLOCK_DIM_Y;
	cl_int		MAX_BLOCK_DIM_Z;
	cl_int		MAX_GRID_DIM_X;
	cl_int		MAX_GRID_DIM_Y;
	cl_int		MAX_GRID_DIM_Z;
	cl_int		MAX_SHARED_MEMORY_PER_BLOCK;
	cl_int		TOTAL_CONSTANT_MEMORY;
	cl_int		WARP_SIZE;
	cl_int		MAX_PITCH;
	cl_int		MAX_REGISTERS_PER_BLOCK;
	cl_int		REGISTERS_PER_BLOCK;
	cl_int		CLOCK_RATE;
	cl_int		TEXTURE_ALIGNMENT;
	cl_int		MULTIPROCESSOR_COUNT;
	cl_int		KERNEL_EXEC_TIMEOUT;
	cl_int		INTEGRATED;
	cl_int		CAN_MAP_HOST_MEMORY;
	cl_int		COMPUTE_MODE;
	cl_int		MAXIMUM_TEXTURE1D_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_HEIGHT;
	cl_int		MAXIMUM_TEXTURE3D_WIDTH;
	cl_int		MAXIMUM_TEXTURE3D_HEIGHT;
	cl_int		MAXIMUM_TEXTURE3D_DEPTH;
	cl_int		MAXIMUM_TEXTURE2D_LAYERED_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_LAYERED_HEIGHT;
	cl_int		MAXIMUM_TEXTURE2D_LAYERED_LAYERS;
	cl_int		SURFACE_ALIGNMENT;
	cl_int		CONCURRENT_KERNELS;
	cl_int		ECC_ENABLED;
	cl_int		PCI_BUS_ID;
	cl_int		PCI_DEVICE_ID;
	cl_int		TCC_DRIVER;
	cl_int		MEMORY_CLOCK_RATE;
	cl_int		GLOBAL_MEMORY_BUS_WIDTH;
	cl_int		L2_CACHE_SIZE;
	cl_int		MAX_THREADS_PER_MULTIPROCESSOR;
	cl_int		ASYNC_ENGINE_COUNT;
	cl_int		UNIFIED_ADDRESSING;
	cl_int		MAXIMUM_TEXTURE1D_LAYERED_WIDTH;
	cl_int		MAXIMUM_TEXTURE1D_LAYERED_LAYERS;
	cl_int		MAXIMUM_TEXTURE2D_GATHER_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_GATHER_HEIGHT;
	cl_int		MAXIMUM_TEXTURE3D_WIDTH_ALTERNATE;
	cl_int		MAXIMUM_TEXTURE3D_HEIGHT_ALTERNATE;
	cl_int		MAXIMUM_TEXTURE3D_DEPTH_ALTERNATE;
	cl_int		PCI_DOMAIN_ID;
	cl_int		TEXTURE_PITCH_ALIGNMENT;
	cl_int		MAXIMUM_TEXTURECUBEMAP_WIDTH;
	cl_int		MAXIMUM_TEXTURECUBEMAP_LAYERED_WIDTH;
	cl_int		MAXIMUM_TEXTURECUBEMAP_LAYERED_LAYERS;
	cl_int		MAXIMUM_SURFACE1D_WIDTH;
	cl_int		MAXIMUM_SURFACE2D_WIDTH;
	cl_int		MAXIMUM_SURFACE2D_HEIGHT;
	cl_int		MAXIMUM_SURFACE3D_WIDTH;
	cl_int		MAXIMUM_SURFACE3D_HEIGHT;
	cl_int		MAXIMUM_SURFACE3D_DEPTH;
	cl_int		MAXIMUM_SURFACE1D_LAYERED_WIDTH;
	cl_int		MAXIMUM_SURFACE1D_LAYERED_LAYERS;
	cl_int		MAXIMUM_SURFACE2D_LAYERED_WIDTH;
	cl_int		MAXIMUM_SURFACE2D_LAYERED_HEIGHT;
	cl_int		MAXIMUM_SURFACE2D_LAYERED_LAYERS;
	cl_int		MAXIMUM_SURFACECUBEMAP_WIDTH;
	cl_int		MAXIMUM_SURFACECUBEMAP_LAYERED_WIDTH;
	cl_int		MAXIMUM_SURFACECUBEMAP_LAYERED_LAYERS;
	cl_int		MAXIMUM_TEXTURE1D_LINEAR_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_LINEAR_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_LINEAR_HEIGHT;
	cl_int		MAXIMUM_TEXTURE2D_LINEAR_PITCH;
	cl_int		MAXIMUM_TEXTURE2D_MIPMAPPED_WIDTH;
	cl_int		MAXIMUM_TEXTURE2D_MIPMAPPED_HEIGHT;
	cl_int		COMPUTE_CAPABILITY_MAJOR;
	cl_int		COMPUTE_CAPABILITY_MINOR;
	cl_int		MAXIMUM_TEXTURE1D_MIPMAPPED_WIDTH;
	cl_int		STREAM_PRIORITIES_SUPPORTED;
	cl_int		GLOBAL_L1_CACHE_SUPPORTED;
	cl_int		LOCAL_L1_CACHE_SUPPORTED;
	cl_int		MAX_SHARED_MEMORY_PER_MULTIPROCESSOR;
	cl_int		MAX_REGISTERS_PER_MULTIPROCESSOR;
	cl_int		MANAGED_MEMORY;
	cl_int		MULTI_GPU_BOARD;
	cl_int		MULTI_GPU_BOARD_GROUP_ID;
	cl_int		HOST_NATIVE_ATOMIC_SUPPORTED;
	cl_int		SINGLE_TO_DOUBLE_PRECISION_PERF_RATIO;
	cl_int		PAGEABLE_MEMORY_ACCESS;
	cl_int		CONCURRENT_MANAGED_ACCESS;
	cl_int		COMPUTE_PREEMPTION_SUPPORTED;
	cl_int		CAN_USE_HOST_POINTER_FOR_REGISTERED_MEM;
} DevAttributes;

extern DevAttributes   *devAttrs;
extern cl_int			numDevAttrs;
extern cl_ulong			devComputeCapability;

extern bool	gpu_scoreboard_mem_alloc(size_t nbytes);
extern void	gpu_scoreboard_mem_free(size_t nbytes);

extern void pgstrom_init_gpu_device(void);
extern Datum pgstrom_device_info(PG_FUNCTION_ARGS);

#endif	/* GPUDEVICE_H */