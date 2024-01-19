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

#include <efi.h>
#include <efilib.h>
#include <efiapi.h>
#include <bootguard.h>
#include <configuration.h>
#include <utils.h>
#include <syspart.h>
#include <envdata.h>

struct EnvDataVolume {
	int volume_index;
	BG_ENVDATA envdata;
};

static unsigned config_state_ranking(const BG_ENVDATA *envdata)
{
	unsigned rank = -1;

	/* Assign a rank to each of the states. Prefer INSTALLED,
	 * then TESTING, over OK, but eschew FAILED and unknown. */

	if (envdata) {
		switch (envdata->ustate) {
		case USTATE_INSTALLED: rank = 0; break;
		case USTATE_TESTING:   rank = 1; break;
		case USTATE_OK:        rank = 2; break;
		default:               rank = 3; break;
		}
	}

	return rank;
}

static void sift_envdata_volume(
	struct EnvDataVolume **lhsp, struct EnvDataVolume **rhsp)
{
	struct EnvDataVolume *lhs = *lhsp;
	struct EnvDataVolume *rhs = *rhsp;

	unsigned lstaterank = config_state_ranking(lhs ? &lhs->envdata : NULL);
	unsigned rstaterank = config_state_ranking(rhs ? &rhs->envdata : NULL);

	/* Compare the lhs and rhs, swapping to ensure that the lhs is
	 * preferred. Preferred the configuration that is not in_progress,
	 * has the highest revision, and has the lower ranked state.
	 *
	 * If lhs and rhs are equal, prefer the copy on the boot volume,
	 * otherwise prefer the copy on the first occurring partition.
	 * This is relevant for scenarios where a backup is taken of
	 * EFI System Partition, and the config is stored on the ESP. */

	BOOLEAN swap;

	if (!rhs)
		swap = FALSE;
	else if (!lhs)
		swap = TRUE;
	else {
		BOOLEAN lbootvolume = IsOnBootVolume(
			volumes[lhs->volume_index].devpath);

		BOOLEAN rbootvolume = IsOnBootVolume(
			volumes[rhs->volume_index].devpath);

		if (lhs->envdata.in_progress != rhs->envdata.in_progress)
			swap = (lhs->envdata.in_progress >
					rhs->envdata.in_progress);
		else if (lhs->envdata.revision != rhs->envdata.revision)
			swap = (lhs->envdata.revision < rhs->envdata.revision);
		else if (lstaterank != rstaterank)
			swap = (lstaterank > rstaterank);
		else if (lbootvolume != rbootvolume)
			swap = (lbootvolume < rbootvolume);
		else if (lhs->volume_index != rhs->volume_index)
			swap = (lhs->volume_index > rhs->volume_index);
		else
			swap = FALSE;
	}

	if (swap) {
		*lhsp = rhs;
		*rhsp = lhs;
	}
}

static BG_STATUS save_current_config(struct EnvDataVolume *env)
{
	BG_STATUS result = BG_CONFIG_ERROR;
	EFI_STATUS efistatus;

	VOLUME_DESC *v = &volumes[env->volume_index];
	EFI_FILE_HANDLE fh = NULL;
	efistatus = open_cfg_file(v->root, &fh, EFI_FILE_MODE_WRITE |
				  EFI_FILE_MODE_READ);
	if (EFI_ERROR(efistatus)) {
		ERROR(L"Could not open environment file on system partition %d: %r\n",
		      env->volume_index, efistatus);
		goto scc_cleanup;
	}

	uint32_t crc32 = -1;

	efistatus = CalculateCrc32(
	    &env->envdata,
	    sizeof(BG_ENVDATA) - sizeof(env->envdata.crc32), &crc32);

	if (!EFI_ERROR(efistatus)) {
		UINTN writelen = sizeof(BG_ENVDATA);

		env->envdata.crc32 = crc32;
		efistatus = fh->Write(fh, &writelen, (VOID *)&env->envdata);
	}

	if (EFI_ERROR(efistatus)) {
		ERROR(L"Cannot write environment to file: %r\n", efistatus);
		(VOID) close_cfg_file(v->root, fh);
		goto scc_cleanup;
	}

	if (EFI_ERROR(close_cfg_file(v->root, fh))) {
		ERROR(L"Could not close environment config file.\n");
		goto scc_cleanup;
	}

	result = BG_SUCCESS;
scc_cleanup:
	return result;
}

static EFI_STATUS read_config(
	BOOLEAN *errored, const VOLUME_DESC *volume, BG_ENVDATA *envdata)
{
	EFI_STATUS result;

	EFI_FILE_HANDLE fh = NULL;

	result = open_cfg_file(volume->root, &fh, EFI_FILE_MODE_READ);
	if (EFI_ERROR(result)) {

		ERROR(L"Could not open environment file\n");
		*errored = TRUE;
		goto failed;
	}

	UINTN readlen = sizeof(*envdata);

	result = read_cfg_file(fh, &readlen, (VOID *) envdata);

	if (EFI_ERROR(close_cfg_file(volume->root, fh))) {
		WARNING(L"Could not close environment config file\n");
		*errored = TRUE;
		/* Only fail if the read did not succeed */
	}

	if (EFI_ERROR(result)) {
		ERROR(L"Cannot read environment file\n");
		*errored = TRUE;
		goto failed;
	}

	if (readlen != sizeof(BG_ENVDATA)) {
		result = EFI_BAD_BUFFER_SIZE;
		ERROR(L"Environment file has wrong size\n");
		*errored = TRUE;
		goto failed;
	}

	uint32_t crc32 = -1;

	result = CalculateCrc32(
	    envdata,
	    sizeof(*envdata) - sizeof(envdata->crc32),
	    &crc32);

	if (EFI_ERROR(result)) {
		ERROR(L"Unable to compute CRC32\n");
		*errored = TRUE;
		goto failed;
	}

	if (crc32 != envdata->crc32) {
		result = EFI_CRC_ERROR;
		ERROR(L"CRC32 error in environment data\n");
		INFO(L"calculated: %lx\n", crc32);
		INFO(L"stored: %lx\n", envdata->crc32);
		*errored = TRUE;
		goto failed;
	}

failed:
	return result;
}

BG_STATUS load_config(BG_LOADER_PARAMS *bglp)
{
	BG_STATUS result = BG_CONFIG_ERROR;
	UINTN numHandles = volume_count;
	UINTN *config_volumes = NULL;
	BOOLEAN errored = FALSE;

	/* Find all the viable configs, and place the most preferred
	 * in rank_envdata[0], with the next preferred in rank_envdata[1]. */

	const unsigned ENVSLOTS = 3;

	struct EnvDataVolume env[ENVSLOTS];
	struct EnvDataVolume *rank_envdata[ENVSLOTS];
	struct EnvDataVolume *rsrv_envdata[ENVSLOTS];

	for (unsigned edx = 0; edx < ENVSLOTS; ++edx) {
		rank_envdata[edx] = NULL;
		rsrv_envdata[edx] = env + edx;
	}

	if (!volume_count) {
		ERROR(L"No volumes available for config partitions.\n");
		goto failed;
	}

	config_volumes = (UINTN *)AllocatePool(sizeof(UINTN) * volume_count);
	if (!config_volumes) {
		ERROR(L"Could not allocate memory for config partition mapping.\n");
		goto failed;
	}

	if (EFI_ERROR(enumerate_cfg_parts(config_volumes, &numHandles))) {
		ERROR(L"Could not enumerate config partitions.\n");
		goto failed;
	}

	numHandles = filter_cfg_parts(config_volumes, numHandles);

	if (numHandles != ENV_NUM_CONFIG_PARTS) {
		WARNING(L"Unexpected config partitions: found: %d, but expected %d.\n",
			numHandles, ENV_NUM_CONFIG_PARTS);
		/* Don't treat this as error because we may still be able to
		 * find a valid config */
		errored = TRUE;
	}

	struct EnvDataVolume **envrsrv = rsrv_envdata + ENVSLOTS;

	/* Load the most recent config data */
	for (UINTN ix = 0; ix < numHandles; ix++) {

		struct EnvDataVolume **envrank = rank_envdata + ENVSLOTS;

		if (!*--envrank)
			*envrank = *--envrsrv;

		envrank[0]->volume_index = config_volumes[ix];

		INFO(L"Reading config file on volume %d.\n",
			envrank[0]->volume_index);

		BOOLEAN read_error = FALSE;
		EFI_STATUS read_status = read_config(
			&read_error,
			&volumes[envrank[0]->volume_index],
			&envrank[0]->envdata);

		errored |= read_error;

		if (read_error)
			WARNING(L"Could not read environment file "
				"on config partition %d\n", ix);

		if (EFI_ERROR(read_status))
			continue;

		/* enforce NUL-termination of strings */
		envrank[0]->envdata.kernelfile[ENV_STRING_LENGTH - 1] = 0;
		envrank[0]->envdata.kernelparams[ENV_STRING_LENGTH - 1] = 0;

		/* Sift the most recently read config data to compare it
		 * to the ones already read. */

		do {
                    --envrank;
                    sift_envdata_volume(envrank+0, envrank+1);
                } while (envrank != rank_envdata);
	}

	/* Assume we boot with the latest configuration. Environments
	 * that are in_progress are ranked lower. Ensure that there is
	 * a most preferred environment, and it is not still in_progress. */

	struct EnvDataVolume *next = rank_envdata[0];
	struct EnvDataVolume *prev = rank_envdata[1];

	if (!next || next->envdata.in_progress) {
		ERROR(L"Could not find any valid config partition.\n");
		goto failed;
	}

	struct EnvDataVolume *latest = next;

	if (latest->envdata.ustate == USTATE_TESTING) {
		/* If it has already been booted, this indicates a failed
		 * update. In this case, mark it as failed by giving a
		 * zero-revision */
		latest->envdata.ustate = USTATE_FAILED;
		latest->envdata.revision = REVISION_FAILED;
		save_current_config(latest);
		/* We must boot with the configuration that was active before
		 * if possible, otherwise try again with what is available.
		 */
		if (!prev) {
			ERROR(L"Could not find previous valid config partition.\n");
			goto failed;
		}
		latest = prev;

	} else if (latest->envdata.ustate == USTATE_INSTALLED) {
		/* If this configuration has never been booted with, set ustate
		 * to indicate that this configuration is now being tested */
		latest->envdata.ustate = USTATE_TESTING;
		save_current_config(latest);
	}

	bglp->payload_path = StrDuplicate(latest->envdata.kernelfile);
	bglp->payload_options = StrDuplicate(latest->envdata.kernelparams);
	bglp->timeout = latest->envdata.watchdog_timeout_sec;

	INFO(L"Choosing config on volume %d.\n", latest->volume_index);
	INFO(L"Config Revision: %d:\n", latest->envdata.revision);
	INFO(L" ustate: %d\n", latest->envdata.ustate);
	INFO(L" kernel: %s\n", bglp->payload_path);
	INFO(L" args: %s\n", bglp->payload_options);
	INFO(L" timeout: %d seconds\n", bglp->timeout);

	result = errored ? BG_CONFIG_PARTIALLY_CORRUPTED : BG_SUCCESS;

failed:
	FreePool(config_volumes);
	return result;
}

BG_STATUS save_config(BG_LOADER_PARAMS *bglp)
{
	(VOID)bglp;
	return BG_NOT_IMPLEMENTED;
}
