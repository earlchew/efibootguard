/*
 * EFI Boot Guard
 *
 * Copyright (c) Siemens AG, 2017
 *
 * Authors:
 *  Andreas Reichel <andreas.reichel.ext@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <check.h>
#include <configuration.h>
#include <env_api.h>
#include <fff.h>
#include <syspart.h>
#include <utils.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

DEFINE_FFF_GLOBALS;

UINTN volume_count;
VOLUME_DESC *volumes;

static void
shuffle_volumes()
{
	/* Shuffle the volumes so that the tests will cover
	 * a variety of permutations. */

	for (unsigned vx = 0; vx < volume_count; ++vx) {

		unsigned wx = vx + random() % (volume_count - vx);

		if (vx != wx) {
			VOLUME_DESC swap = volumes[vx];
			volumes[vx] = volumes[wx];
			volumes[wx] = swap;
		}
	}
}

/* Error injection
 *
 * Errors are optionally injected to cover the failure paths, primarily
 * to check for aborts and leaks. Resetting error_census and running with
 * error_injection disabled will count the number of injection points.
 * Running with non-zero error_injection will inject EFI status error
 * at the designated injection point. */

static unsigned error_census;
static unsigned error_injection;

#define INJECT_ERROR() \
	do \
		switch (error_injection) { \
		default: --error_injection; \
		case 0: ++error_census; break; \
		case 1: error_injection = 0; return EFI_INVALID_PARAMETER; \
		} \
	while (0)

static EFI_STATUS
inject_error()
{
	INJECT_ERROR();
	return EFI_SUCCESS;
}

/* Boot volume
 *
 * Simulate a boot volume by using a unique EFI_DEVICE_PATH instance.
 * This avoids having to create a proper device path data set to
 * represent the path to the volume. */

static EFI_DEVICE_PATH boot_volume;
static EFI_DEVICE_PATH non_boot_volume;
static EFI_DEVICE_PATH non_boot_disk;

/* Cpnfig file IO
 *
 * Use a struct fatvars_scenario instance to direct simulation
 * of config file IO. The scenario provides the config data
 * when reading, and the static variable config_file_protocol_wrote
 * captures the content when writing. */

static BG_ENVDATA config_file_protocol_wrote;

struct fatvars_scenario {
	BG_ENVDATA *envdata;
};

struct config_file_protocol {
	EFI_FILE_PROTOCOL file_protocol;
	struct fatvars_scenario *scenario;
};

static EFI_STATUS
EFIAPI config_file_protocol_read(
    IN EFI_FILE_PROTOCOL *File,
    IN OUT UINTN         *BufferSize,
    OUT VOID             *Buffer)
{
	INJECT_ERROR();

	struct config_file_protocol *self = (void *) File;

	size_t envdata_size = sizeof(*self->scenario->envdata);

	if (!self->scenario->envdata || envdata_size > *BufferSize)
		*BufferSize = 0;
	else {
		*BufferSize = envdata_size;
		memcpy(Buffer, self->scenario->envdata, envdata_size);
	}

	return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI config_file_protocol_write(
    IN struct _EFI_FILE_HANDLE  *File,
    IN OUT UINTN                *BufferSize,
    IN VOID                     *Buffer)
{
	INJECT_ERROR();

	if (*BufferSize == sizeof(config_file_protocol_wrote))
		memcpy(&config_file_protocol_wrote, Buffer, *BufferSize);
	else
		*BufferSize = 0;

	return *BufferSize ? EFI_SUCCESS : EFI_BAD_BUFFER_SIZE;
}

static EFI_FILE_PROTOCOL config_file_protocol = {
	.Read = config_file_protocol_read,
	.Write = config_file_protocol_write,
};

/* Volume IO
 *
 * The config file is opened for reading and writing by using
 * a struct volume_file_protocol instance. The struct fatvars_scenario
 * is passed along to the new struct volume_file_protocol to
 * drive the reading/writing simulation. */

struct volume_file_protocol {
	EFI_FILE_PROTOCOL file_protocol;
	struct fatvars_scenario *scenario;
};

static EFI_STATUS
EFIAPI volume_file_protocol_close(
    IN EFI_FILE_PROTOCOL *File)
{
	struct volume_file_protocol *self = (void *) File;

	free(self);

	INJECT_ERROR();

	return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI volume_file_protocol_open(
    IN EFI_FILE_PROTOCOL   *File,
    OUT EFI_FILE_PROTOCOL **NewHandle,
    IN CHAR16              *FileName,
    IN UINT64               OpenMode,
    IN UINT64               Attributes)
{
	INJECT_ERROR();

	struct volume_file_protocol *self = (void *) File;

	EFI_STATUS status = EFI_OUT_OF_RESOURCES;

	struct config_file_protocol *handle = malloc(sizeof(*handle));

	if (handle) {
		handle->file_protocol = config_file_protocol;
		handle->scenario = self->scenario;

		*NewHandle = &handle->file_protocol;
		status = EFI_SUCCESS;
	}

	return status;
}

static EFI_FILE_PROTOCOL volume_file_protocol = {
	.Open = volume_file_protocol_open,
	.Close = volume_file_protocol_close,
};

static void
close_volume_file_protocol(EFI_FILE_HANDLE handle)
{
	struct volume_file_protocol *self = (void *) handle;

	free(self);
}

static EFI_FILE_HANDLE
create_volume_file_protocol(struct fatvars_scenario *scenario)
{
	struct volume_file_protocol *self = malloc(sizeof(*self));

	if (self) {
		self->file_protocol = volume_file_protocol;
		self->scenario = scenario;
	}

	return &self->file_protocol;
}

/* Test volumes
 *
 * A set of test volumes are created during each test to simulation
 * the partitions available during boot. Each volume is associated
 * with a struct fatvars_scenario instance. One volume is distinguished
 * as being the first, and the remainder are share common characteristics.
 * As a final step, the set is shuffled to obscure the location of
 * the first volume. */

static void
create_test_volumes(
	UINTN config_parts,
	struct fatvars_scenario *first,
	EFI_DEVICE_PATH *first_devpath,
	struct fatvars_scenario *rest,
	EFI_DEVICE_PATH *rest_devpath,
	...)
{
	va_list argp;

	struct fatvars_scenario *scenario = first;
	EFI_DEVICE_PATH *devpath = first_devpath;

	va_start(argp, rest_devpath);

	struct fatvars_scenario *list =
		va_arg(argp, struct fatvars_scenario *);

	/* Offset the usable volume index to improve
	 * detection of errors indexing the volume
	 * array, and config array.  */

	const unsigned volumeOffset = 997;

	volume_count = volumeOffset + config_parts;
	volumes = malloc(sizeof(*volumes) * volume_count);
	if (!volumes)
		ck_abort();

	for (unsigned vx = 0; vx < volume_count; ++vx) {
		volumes[vx] = (VOLUME_DESC) { };

		if (vx < volumeOffset)
			continue;

		volumes[vx].devpath = devpath;
		volumes[vx].fslabel = NULL;
		volumes[vx].fscustomlabel = NULL;
		volumes[vx].root = create_volume_file_protocol(scenario);

		if (!volumes[vx].root)
			ck_abort();

		scenario = rest;
		devpath = rest_devpath;

		if (list) {
			rest = list;
			rest_devpath = va_arg(argp, EFI_DEVICE_PATH *);
			list = va_arg(argp, struct fatvars_scenario *);
		}
	}

	shuffle_volumes();

	va_end(argp);
}

static void
close_test_volumes()
{
	for (unsigned vx = 0; vx < volume_count; ++vx)
		close_volume_file_protocol(volumes[vx].root);

	free(volumes);

	volume_count = 0;
	volumes = NULL;
}

static void
envdata_init(BG_ENVDATA *env)
{
	env->crc32 = bgenv_crc32(0, env, sizeof(*env) - sizeof(env->crc32));
}

Suite *ebg_test_suite(void);

static void
print_part(const CHAR16 *fmtbegin, const CHAR16 *fmtend, va_list *argp)
{
	char fmt[fmtend-fmtbegin+1];

	for (unsigned ix = 0; ix < sizeof(fmt)-1; ++ix)
		fmt[ix] = fmtbegin[ix];
	fmt[sizeof(fmt)-1] = 0;
	vfprintf(stderr, fmt, *argp);
}

void
PrintC(const UINT8 color, const CHAR16 *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);

	/* Host libraries are built using unsigned int wchar_t, whereas UEFI
	 * runtime requires CHAR16. Use a simple analysis of the format
	 * to locate %s so that the string can be treated as char*, instead
	 * of CHAR16*, by printf(3). */

	while (*fmt) {
		if (*fmt != '%') {
			fputc(*fmt++, stderr);
			continue;
		}

		const CHAR16 *endp = fmt;
		while (1) {

			switch (*endp) {
			default:
				++endp;
				continue;
			case 0:
				print_part(fmt, endp, &argp);
				break;
			case 'd': case 'i':
			case 'o': case 'u': case 'x': case 'X':
				print_part(fmt, ++endp, &argp);
				break;
			case 's':
				{
				const CHAR16 *str = va_arg(argp, CHAR16 *);

				char cstr[StrLen(str) + 1];
				char cfmt[endp - fmt + 1];

				for (unsigned ix = 0; ix < sizeof(fmt)-1; ++ix)
					cfmt[ix] = fmt[ix];
				cfmt[sizeof(cfmt)-1] = 0;

				for (unsigned ix = 0; ix < sizeof(cstr)-1; ++ix)
					cstr[ix] = str[ix];
				cstr[sizeof(cstr)-1] = 0;

				fprintf(stderr, cfmt, cstr);

				++endp;
				}
			}

			fmt = endp;
			break;
		}
	}

	va_end(argp);
}

/* UEFI API
 *
 * Provide simple implementations of the functions referenced
 * by load_config(). Each will typically support error injection
 * to allow testing of the failure paths. */

VOID *
AllocatePool(IN UINTN size)
{
	if (EFI_ERROR(inject_error()))
		return NULL;

	return malloc(size);
}

VOID
FreePool(IN VOID *p)
{
	free(p);
}

CHAR16 *
StrDuplicate(IN CONST CHAR16 *src)
{
	size_t len = 0;

	while (src[len++])
		continue;

	size_t size = sizeof(*src) * len;

	CHAR16 *dst = malloc(size);

	if (dst)
		memcpy(dst, src, size);

	return dst;
}

INTN
StrCmp(IN CONST CHAR16 *s1, IN CONST CHAR16 *s2)
{
	INTN rank;

	do {
		UINT16 s1c = *s1++;
		UINT16 s2c = *s2++;

		rank = s1c - s2c;
		if (!s1c)
			break;

	} while (!rank);

	return rank;
}

UINTN
StrLen (IN CONST CHAR16 *s)
{
	const CHAR16 *p = s;

	while (*p)
		++p;
	return p - s;
}

BOOLEAN
IsOnBootVolume(EFI_DEVICE_PATH *dp)
{
	/* Prevent cppcheck ConstParameter warning. */
	if (dp)
		memcpy(dp, dp, sizeof(*dp));
	return dp == &boot_volume;
}

EFI_STATUS
CalculateCrc32(VOID *pt, UINTN size, UINT32 *crc)
{
	INJECT_ERROR();

	*crc = bgenv_crc32(0, pt, size);
	return EFI_SUCCESS;
}

EFI_STATUS
enumerate_cfg_parts(UINTN *config_volumes, UINTN *maxHandles)
{
	INJECT_ERROR();

	UINTN numHandles = 0;

	for (UINTN vx = 0; vx < volume_count; ++vx) {
		if (volumes[vx].root)
			config_volumes[numHandles++] = vx;
	}
	*maxHandles = numHandles;

	return EFI_SUCCESS;
}

UINTN
filter_cfg_parts(UINTN *config_volumes, UINTN maxHandles)
{
	UINTN numHandles = 0;

	for (UINTN vx = 0; vx < maxHandles; ++vx) {
		if (volumes[config_volumes[vx]].devpath != &non_boot_disk)
			config_volumes[numHandles++] = config_volumes[vx];
	}

	return numHandles;
}

START_TEST(load_config_empty)
{
	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	volume_count = 0;
	volumes = NULL;

	status = load_config(&bglp);
	ck_assert(status == BG_CONFIG_ERROR);
}
END_TEST

START_TEST(load_config_no_cfg_parts)
{
	/* Test the scenario where no config data is found across
	 * all the boot devices. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	struct fatvars_scenario scenario = {
		.envdata = NULL,
	};

	create_test_volumes(
		ENV_NUM_CONFIG_PARTS, &scenario, NULL, &scenario, NULL, NULL);

	status = load_config(&bglp);
	ck_assert(status == BG_CONFIG_ERROR);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_num_cfg_parts)
{
	/* Test the scenario where there is the expected number of
	 * config environment files. Verify that the most recent
	 * revision is selected. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 2,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
		.kernelfile = L"kernelfile",
		.kernelfile = L"kernelparams",
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	BG_ENVDATA inactive = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&inactive);

	struct fatvars_scenario rest = {
		.envdata = &inactive,
	};

	create_test_volumes(
		ENV_NUM_CONFIG_PARTS, &first, NULL, &rest, NULL, NULL);

	status = load_config(&bglp);
	ck_assert(status == BG_SUCCESS);
	ck_assert(StrCmp(bglp.payload_path, active.kernelfile) == 0);
	ck_assert(StrCmp(bglp.payload_options, active.kernelparams) == 0);
	ck_assert(bglp.timeout == active.watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_num_cfg_parts_error)
{
	/* Test the scenario where there is the expected number of
	 * config environment files, but an error is encountered.
	 * Verify that BG_SUCCESS is not returned. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 2,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
		.kernelfile = L"kernelfile",
		.kernelfile = L"kernelparams",
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	BG_ENVDATA inactive = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&inactive);

	struct fatvars_scenario rest = {
		.envdata = &inactive,
	};

	/* First make a pass to take a census of the number of
	 * potential errors that can be injected. */

	create_test_volumes(
		ENV_NUM_CONFIG_PARTS, &first, NULL, &rest, NULL, NULL);

	error_census = 0;
	status = load_config(&bglp);
	ck_assert(status == BG_SUCCESS);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();

	/* Knowing the total number of potential errors, inject
	 * one error at a time to verify that each can be detected. */

	ck_assert(error_census);
	for (unsigned ex = error_census; ex; --ex) {

		error_injection = ex;

		create_test_volumes(
			ENV_NUM_CONFIG_PARTS, &first, NULL, &rest, NULL, NULL);

		bglp = (BG_LOADER_PARAMS) { };

		status = load_config(&bglp);
		ck_assert(status != BG_SUCCESS);

		FreePool(bglp.payload_path);
		FreePool(bglp.payload_options);

		close_test_volumes();
	}

	if (error_injection)
		ck_abort();
}
END_TEST

START_TEST(load_config_one_cfg_part)
{
	/* Test the scenario where there is only one config environment file.
	 * Verify that this is the config that is selected. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 2,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
		.kernelfile = L"kernelfile",
		.kernelfile = L"kernelparams",
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	create_test_volumes(1, &first, NULL, NULL, NULL, NULL);

	status = load_config(&bglp);
	if (ENV_NUM_CONFIG_PARTS == 1)
		ck_assert(status == BG_SUCCESS);
	else
		ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);
	ck_assert(StrCmp(bglp.payload_path, active.kernelfile) == 0);
	ck_assert(StrCmp(bglp.payload_options, active.kernelparams) == 0);
	ck_assert(bglp.timeout == active.watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_extra_cfg_part)
{
	/* Test the scenario where there is one more config environment file
	 * than expected. Verify that the most recent config environment
	 * file is selected, and that BG_CONFIG_PARTIALLY_CORRUPTED is
	 * returned. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 2,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
		.kernelfile = L"kernelfile",
		.kernelfile = L"kernelparams",
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	BG_ENVDATA inactive = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&inactive);

	struct fatvars_scenario rest = {
		.envdata = &inactive,
	};

	create_test_volumes(
		ENV_NUM_CONFIG_PARTS+1, &first, NULL, &rest, NULL, NULL);

	status = load_config(&bglp);
	ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);

	ck_assert(StrCmp(bglp.payload_path, active.kernelfile) == 0);
	ck_assert(StrCmp(bglp.payload_options, active.kernelparams) == 0);
	ck_assert(bglp.timeout == active.watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_extra_cfg_disk)
{
	/* Test the scenario where there is one more config
	 * environment file than expected, and an extra disk.
	 * Verify that the most recent config environment file
	 * is selected, and that BG_CONFIG_PARTIALLY_CORRUPTED
	 * is returned. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 2,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
		.kernelfile = L"kernelfile",
		.kernelfile = L"kernelparams",
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	BG_ENVDATA other = {
		.revision = 3,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 999,
	};
	envdata_init(&other);

	struct fatvars_scenario peer = {
		.envdata = &other,
	};

	BG_ENVDATA inactive = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&inactive);

	struct fatvars_scenario sibling = {
		.envdata = &inactive,
	};

	create_test_volumes(
		ENV_NUM_CONFIG_PARTS+1,
		&first, &non_boot_volume,
		&sibling, &non_boot_volume,
		&peer, &non_boot_disk,
		NULL);

	status = load_config(&bglp);
	if (ENV_NUM_CONFIG_PARTS == 2)
		ck_assert(status == BG_SUCCESS);
	else
		ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);

	ck_assert(StrCmp(bglp.payload_path, active.kernelfile) == 0);
	ck_assert(StrCmp(bglp.payload_options, active.kernelparams) == 0);
	ck_assert(bglp.timeout == active.watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_rank_inprogress)
{
	/* Test the scenario where there are two config environment files
	 * but one of them is in_progress. Verify that the other is
	 * selected. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA inprogress = {
		.revision = 2,
		.in_progress = 1,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
	};
	envdata_init(&inprogress);

	struct fatvars_scenario first = {
		.envdata = &inprogress,
	};

	BG_ENVDATA active = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&active);

	struct fatvars_scenario rest = {
		.envdata = &active,
	};

	create_test_volumes(2, &first, NULL, &rest, NULL, NULL);

	status = load_config(&bglp);
	if (ENV_NUM_CONFIG_PARTS == 2)
		ck_assert(status == BG_SUCCESS);
	else
		ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);
	ck_assert(bglp.timeout == active.watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_rank_ustate)
{
	/* Test that the ranking of the states meets expectations.
	 * INSTALLED is preferred over TESTING, which in turn is
	 * preferred over OK. Also verify that the state is updated
	 * when necessary. */

	static const uint16_t states[] = {
		USTATE_INSTALLED,
		USTATE_TESTING,
		USTATE_OK,
		USTATE_FAILED,
	};

	int steps = sizeof(states)/sizeof(states[0]) - 1;

	for (int ix = 0; ix < steps; ++ix) {
		BG_LOADER_PARAMS bglp = { };
		BG_STATUS status;

		uint16_t state1 = states[ix+0];
		uint16_t state2 = states[ix+1];

		BG_ENVDATA active = {
			.revision = 1,
			.in_progress = 0,
			.ustate = state1,
			.kernelfile = L"first",
		};
		envdata_init(&active);

		struct fatvars_scenario first = {
			.envdata = &active,
		};

		BG_ENVDATA inactive = {
			.revision = 1,
			.in_progress = 0,
			.ustate = state2,
			.kernelfile = L"second",
		};
		envdata_init(&inactive);

		struct fatvars_scenario rest = {
			.envdata = &inactive,
		};

		create_test_volumes(2, &first, NULL, &rest, NULL, NULL);

		memset(&config_file_protocol_wrote,
			0, sizeof(config_file_protocol_wrote));

		status = load_config(&bglp);
		if (ENV_NUM_CONFIG_PARTS == 2)
			ck_assert(status == BG_SUCCESS);
		else
			ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);

		const CHAR16 *kernelfile;

		if (state1 == USTATE_TESTING)
			kernelfile = inactive.kernelfile;
		else
			kernelfile = active.kernelfile;

		ck_assert(StrCmp(bglp.payload_path, kernelfile) == 0);

		switch (state1) {
		case USTATE_INSTALLED:
			ck_assert(config_file_protocol_wrote.revision
				== 1);
			ck_assert(config_file_protocol_wrote.ustate
				== USTATE_TESTING);
			break;
		case USTATE_TESTING:
			ck_assert(config_file_protocol_wrote.revision
				== REVISION_FAILED);
			ck_assert(config_file_protocol_wrote.ustate
				== USTATE_FAILED);
			break;
		}

		FreePool(bglp.payload_path);
		FreePool(bglp.payload_options);

		close_test_volumes();
	}
}
END_TEST

START_TEST(load_config_rank_bootvolume)
{
	/* Test the scenario where the boot volume must be used to
	 * discriminate between two otherwise equally ranked config
	 * environment files. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	BG_ENVDATA inactive = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&inactive);

	struct fatvars_scenario rest = {
		.envdata = &inactive,
	};

	create_test_volumes(
		2, &first, &boot_volume, &rest, &non_boot_volume, NULL);

	status = load_config(&bglp);
	if (ENV_NUM_CONFIG_PARTS == 2)
		ck_assert(status == BG_SUCCESS);
	else
		ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);
	ck_assert(bglp.timeout == active.watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_rank_volumeindex)
{
	/* Test the scenario where the volume index must be used to
	 * discriminate between two otherwise equally ranked config
	 * environment files. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA active = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
	};
	envdata_init(&active);

	struct fatvars_scenario first = {
		.envdata = &active,
	};

	BG_ENVDATA inactive = {
		.revision = 1,
		.in_progress = 0,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 99,
	};
	envdata_init(&inactive);

	struct fatvars_scenario rest = {
		.envdata = &inactive,
	};

	create_test_volumes(
		2, &first, &boot_volume, &rest, &boot_volume, NULL);

	const struct volume_file_protocol *selected_volume = NULL;
	for (UINTN vx = 0; vx < volume_count; ++vx) {
		if (volumes[vx].root) {
			selected_volume = (void *) volumes[vx].root;
			break;
		}
	}

	int watchdog_timeout_sec = -1;

	if (!selected_volume)
		ck_abort();
	else if (selected_volume->scenario == &first)
		watchdog_timeout_sec = active.watchdog_timeout_sec;
	else
		watchdog_timeout_sec = inactive.watchdog_timeout_sec;

	status = load_config(&bglp);
	if (ENV_NUM_CONFIG_PARTS == 2)
		ck_assert(status == BG_SUCCESS);
	else
		ck_assert(status == BG_CONFIG_PARTIALLY_CORRUPTED);
	ck_assert(bglp.timeout == watchdog_timeout_sec);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

START_TEST(load_config_fail_inprogress)
{
	/* Test the scenario where all the config environment files
	 * are marked in_progress. In this case, there are no
	 * viable candidates, and load_config() should return
	 * an BG_CONFIG_ERROR. */

	BG_LOADER_PARAMS bglp = { };
	BG_STATUS status;

	BG_ENVDATA inprogress = {
		.revision = 1,
		.in_progress = 1,
		.ustate = USTATE_OK,
		.watchdog_timeout_sec = 11,
	};
	envdata_init(&inprogress);

	struct fatvars_scenario active = {
		.envdata = &inprogress,
	};

	create_test_volumes(2, &active, NULL, &active, NULL, NULL);

	status = load_config(&bglp);
	ck_assert(status == BG_CONFIG_ERROR);

	FreePool(bglp.payload_path);
	FreePool(bglp.payload_options);

	close_test_volumes();
}
END_TEST

Suite *ebg_test_suite(void)
{
	Suite *s;
	TCase *tc_core;

	s = suite_create("env_fatvars");

	/* Run each test 10 times so that shuffle_volumes() will
	 * introduce variation to verify the outcome using different
	 * input  permutations. */

	tc_core = tcase_create("Core");
	tcase_add_loop_test(tc_core, load_config_empty, 0, 10);
	tcase_add_loop_test(tc_core, load_config_no_cfg_parts, 0, 10);
	tcase_add_loop_test(tc_core, load_config_num_cfg_parts, 0, 10);
	tcase_add_loop_test(tc_core, load_config_num_cfg_parts_error, 0, 10);
	tcase_add_loop_test(tc_core, load_config_one_cfg_part, 0, 10);
	tcase_add_loop_test(tc_core, load_config_extra_cfg_part, 0, 10);
	tcase_add_loop_test(tc_core, load_config_extra_cfg_disk, 0, 10);
	tcase_add_loop_test(tc_core, load_config_rank_inprogress, 0, 10);
	tcase_add_loop_test(tc_core, load_config_rank_ustate, 0, 10);
	tcase_add_loop_test(tc_core, load_config_rank_bootvolume, 0, 10);
	tcase_add_loop_test(tc_core, load_config_rank_volumeindex, 0, 10);
	tcase_add_loop_test(tc_core, load_config_fail_inprogress, 0, 10);
	suite_add_tcase(s, tc_core);

	return s;
}
