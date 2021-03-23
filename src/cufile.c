/*
 * cufile.c
 *
 * A thin wrapper to call cuFile library functions.
 * ----
 * Copyright 2011-2020 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2020 (C) The PG-Strom Development Team
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
#include "pg_strom.h"
#include <dlfcn.h>

#ifdef WITH_CUFILE
#define CUFILE_ERROR__DRIVER_NOT_INITIALIZED	\
	((CUfileError_t){CU_FILE_DRIVER_NOT_INITIALIZED,CUDA_SUCCESS})

/*
 * cuFileDriverOpen
 */
static CUfileError_t (*p_cuFileDriverOpen)(void) = NULL;

CUfileError_t
cuFileDriverOpen(void)
{
	if (!p_cuFileDriverOpen)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverOpen();
}

/*
 * cuFileDriverClose
 */
static CUfileError_t (*p_cuFileDriverClose)(void) = NULL;

CUfileError_t
cuFileDriverClose(void)
{
	if (!p_cuFileDriverClose)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverClose();
}

/*
 * cuFileDriverGetProperties
 */
static CUfileError_t (*p_cuFileDriverGetProperties)(
	CUfileDrvProps_t *props) = NULL;

CUfileError_t
cuFileDriverGetProperties(CUfileDrvProps_t *props)
{
	if (!p_cuFileDriverGetProperties)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverGetProperties(props);
}

/*
 * cuFileDriverSetPollMode
 */
static CUfileError_t (*p_cuFileDriverSetPollMode)(
	bool poll,
	size_t poll_threshold_size) = NULL;

CUfileError_t
cuFileDriverSetPollMode(bool poll, size_t poll_threshold_size)
{
	if (!p_cuFileDriverSetPollMode)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverSetPollMode(poll, poll_threshold_size);
}

/*
 * cuFileDriverSetMaxDirectIOSize
 */
static CUfileError_t (*p_cuFileDriverSetMaxDirectIOSize)(
	size_t max_direct_io_size) = NULL;

CUfileError_t
cuFileDriverSetMaxDirectIOSize(size_t max_direct_io_size)
{
	if (!p_cuFileDriverSetMaxDirectIOSize)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverSetMaxDirectIOSize(max_direct_io_size);
}

/*
 * cuFileDriverSetMaxCacheSize
 */
static CUfileError_t (*p_cuFileDriverSetMaxCacheSize)(
	size_t max_cache_size) = NULL;

CUfileError_t
cuFileDriverSetMaxCacheSize(size_t max_cache_size)
{
	if (!p_cuFileDriverSetMaxCacheSize)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverSetMaxCacheSize(max_cache_size);
}

/*
 * cuFileDriverSetMaxPinnedMemSize
 */
static CUfileError_t (*p_cuFileDriverSetMaxPinnedMemSize)(
	size_t max_pinned_size) = NULL;

CUfileError_t
cuFileDriverSetMaxPinnedMemSize(size_t max_pinned_size)
{
	if (!p_cuFileDriverSetMaxPinnedMemSize)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileDriverSetMaxPinnedMemSize(max_pinned_size);
}

/*
 * cuFileHandleRegister
 */
static CUfileError_t (*p_cuFileHandleRegister)(
	CUfileHandle_t *fh,
	CUfileDescr_t *descr) = NULL;

CUfileError_t
cuFileHandleRegister(CUfileHandle_t *fh, CUfileDescr_t *descr)
{
	if (!p_cuFileHandleRegister)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileHandleRegister(fh, descr);
}

/*
 * cuFileHandleDeregister
 */
static void (*p_cuFileHandleDeregister)(
	CUfileHandle_t fh) = NULL;

void
cuFileHandleDeregister(CUfileHandle_t fh)
{
	if (p_cuFileHandleDeregister)
		return p_cuFileHandleDeregister(fh);
}

/*
 * cuFileBufRegister
 */
static CUfileError_t (*p_cuFileBufRegister)(
	const void *devPtr_base,
	size_t length,
	int flags) = NULL;

CUfileError_t
cuFileBufRegister(const void *devPtr_base, size_t length, int flags)
{
	if (!p_cuFileBufRegister)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileBufRegister(devPtr_base, length, flags);
}

/*
 * cuFileBufDeregister
 */
static CUfileError_t (*p_cuFileBufDeregister)(
	const void *devPtr_base) = NULL;

CUfileError_t cuFileBufDeregister(const void *devPtr_base)
{
	if (!p_cuFileBufDeregister)
		return CUFILE_ERROR__DRIVER_NOT_INITIALIZED;
	return p_cuFileBufDeregister(devPtr_base);
}

/*
 * cuFileRead
 */
static ssize_t (*p_cuFileRead)(
	CUfileHandle_t fh,
	void *devPtr_base,
	size_t size,
	off_t file_offset,
	off_t devPtr_offset) = NULL;

ssize_t
cuFileRead(CUfileHandle_t fh,
		   void *devPtr_base,
		   size_t size,
		   off_t file_offset,
		   off_t devPtr_offset)
{
	if (!p_cuFileRead)
		return -CU_FILE_DRIVER_NOT_INITIALIZED;
	return p_cuFileRead(fh, devPtr_base, size, file_offset, devPtr_offset);
}

/*
 * cuFileWrite
 */
static ssize_t (*p_cuFileWrite)(
	CUfileHandle_t fh,
	const void *devPtr_base,
	size_t size,
	off_t file_offset,
	off_t devPtr_offset) = NULL;

ssize_t cuFileWrite(CUfileHandle_t fh,
					const void *devPtr_base,
					size_t size,
					off_t file_offset,
					off_t devPtr_offset)
{
	if (!p_cuFileWrite)
		return -CU_FILE_DRIVER_NOT_INITIALIZED;
	return p_cuFileWrite(fh,devPtr_base,size,file_offset,devPtr_offset);
}

/*
 * lookup_cufile_function
 */
static void *
lookup_cufile_function(void *handle, const char *func_name)
{
	void   *func_addr = dlsym(handle, func_name);

	if (!func_addr)
		elog(ERROR, "could not find cuFile symbol \"%s\" - %s",
			 func_name, dlerror());
	return func_addr;
}
#endif

#define LOOKUP_CUFILE_FUNCTION(func_name)		\
	p_##func_name = lookup_cufile_function(handle, #func_name)

/*
 * cuFileDriverLoaded
 */
bool
cuFileDriverLoaded(void)
{
#ifdef WITH_CUFILE
	if (p_cuFileDriverOpen != NULL &&		/* libcufile.so */
		access("/proc/driver/nvidia-fs/version", F_OK) == 0)	/* nvidia_fs */
	{
		return true;
	}
#endif
	return false;
}

/*
 * pgstrom_init_cufile
 */
void
pgstrom_init_cufile(void)
{
#ifdef WITH_CUFILE
	char		namebuf[MAXPGPATH];
	void	   *handle;

	/* version attached on the production release? */
	snprintf(namebuf, sizeof(namebuf), "libcufile.so");
	handle = dlopen(namebuf, RTLD_NOW | RTLD_LOCAL);
	if (!handle)
	{
		snprintf(namebuf, sizeof(namebuf),
				 CUDA_LIBRARY_PATH "/libcufile.so");
		handle = dlopen(namebuf, RTLD_NOW | RTLD_LOCAL);
		if (!handle)
			elog(ERROR, "failed on dlopen('libcufile.so'): %m");
	}

	PG_TRY();
	{
		LOOKUP_CUFILE_FUNCTION(cuFileDriverOpen);
		LOOKUP_CUFILE_FUNCTION(cuFileDriverClose);
		LOOKUP_CUFILE_FUNCTION(cuFileDriverGetProperties);
		LOOKUP_CUFILE_FUNCTION(cuFileDriverSetPollMode);
		LOOKUP_CUFILE_FUNCTION(cuFileDriverSetMaxDirectIOSize);
		LOOKUP_CUFILE_FUNCTION(cuFileDriverSetMaxCacheSize);
		LOOKUP_CUFILE_FUNCTION(cuFileDriverSetMaxPinnedMemSize);

		LOOKUP_CUFILE_FUNCTION(cuFileHandleRegister);
		LOOKUP_CUFILE_FUNCTION(cuFileHandleDeregister);
		LOOKUP_CUFILE_FUNCTION(cuFileBufRegister);
		LOOKUP_CUFILE_FUNCTION(cuFileBufDeregister);
		LOOKUP_CUFILE_FUNCTION(cuFileRead);
		LOOKUP_CUFILE_FUNCTION(cuFileWrite);
	}
	PG_CATCH();
	{
		dlclose(handle);

		p_cuFileDriverOpen = NULL;
		p_cuFileDriverClose = NULL;
		p_cuFileDriverGetProperties = NULL;
		p_cuFileDriverSetPollMode = NULL;
		p_cuFileDriverSetMaxDirectIOSize = NULL;
		p_cuFileDriverSetMaxCacheSize = NULL;
		p_cuFileDriverSetMaxPinnedMemSize = NULL;
		p_cuFileHandleRegister = NULL;
		p_cuFileHandleDeregister = NULL;
		p_cuFileBufRegister = NULL;
		p_cuFileBufDeregister = NULL;
		p_cuFileRead = NULL;
		p_cuFileWrite = NULL;

		elog(LOG, "failed on lookup cuFile symbols, cuFile is disabled.");
		FlushErrorState();
	}
	PG_END_TRY();
#endif /* WITH_CUFILE */
}
