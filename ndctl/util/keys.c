// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2018 Intel Corporation. All rights reserved. */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/param.h>
#include <keyutils.h>
#include <syslog.h>

#include <ndctl.h>
#include <ndctl/config.h>
#include <ndctl/libndctl.h>
#include <util/keys.h>

static int get_key_path(struct ndctl_dimm *dimm, char *path,
		enum ndctl_key_type key_type)
{
	char hostname[HOST_NAME_MAX];
	int rc;

	rc = gethostname(hostname, HOST_NAME_MAX);
	if (rc < 0) {
		fprintf(stderr, "gethostname: %s\n", strerror(errno));
		return -errno;
	}

	if (key_type == ND_USER_OLD_KEY) {
		rc = sprintf(path, "%s/nvdimmold_%s_%s.blob",
				NDCTL_KEYS_DIR,
				ndctl_dimm_get_unique_id(dimm),
				hostname);
	} else {
		rc = sprintf(path, "%s/nvdimm_%s_%s.blob",
				NDCTL_KEYS_DIR,
				ndctl_dimm_get_unique_id(dimm),
				hostname);
	}

	if (rc < 0) {
		fprintf(stderr, "error setting path: %s\n", strerror(errno));
		return -errno;
	}

	return 0;
}

static int get_key_desc(struct ndctl_dimm *dimm, char *desc,
		enum ndctl_key_type key_type)
{
	int rc;

	if (key_type == ND_USER_OLD_KEY)
		rc = sprintf(desc, "nvdimm-old:%s",
				ndctl_dimm_get_unique_id(dimm));
	else
		rc = sprintf(desc, "nvdimm:%s",
				ndctl_dimm_get_unique_id(dimm));

	if (rc < 0) {
		fprintf(stderr, "error setting key description: %s\n",
				strerror(errno));
		return -errno;
	}

	return 0;
}

char *ndctl_load_key_blob(const char *path, int *size, const char *postfix,
		int dirfd)
{
	struct stat st;
	ssize_t read_bytes = 0;
	int rc, fd;
	char *blob, *pl, *rdptr;
	char prefix[] = "load ";

	fd = openat(dirfd, path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "failed to open file %s: %s\n",
				path, strerror(errno));
		return NULL;
	}

	rc = fstat(fd, &st);
	if (rc < 0) {
		fprintf(stderr, "stat: %s\n", strerror(errno));
		return NULL;
	}
	if ((st.st_mode & S_IFMT) != S_IFREG) {
		fprintf(stderr, "%s not a regular file\n", path);
		return NULL;
	}

	if (st.st_size == 0 || st.st_size > 4096) {
		fprintf(stderr, "Invalid blob file size\n");
		return NULL;
	}

	*size = st.st_size + sizeof(prefix) - 1;
	/*
	 * We need to increment postfix and space.
	 * "keyhandle=" is 10 bytes, plus null termination.
	 */
	if (postfix)
		*size += strlen(postfix) + 10 + 1;
	blob = malloc(*size);
	if (!blob) {
		fprintf(stderr, "Unable to allocate memory for blob\n");
		return NULL;
	}

	memcpy(blob, prefix, sizeof(prefix) - 1);
	pl = blob + sizeof(prefix) - 1;

	rdptr = pl;
	do {
		rc = read(fd, rdptr, st.st_size - read_bytes);
		if (rc < 0) {
			fprintf(stderr, "Failed to read from blob file: %s\n",
					strerror(errno));
			free(blob);
			close(fd);
			return NULL;
		}
		read_bytes += rc;
		rdptr += rc;
	} while (read_bytes != st.st_size);

	close(fd);

	if (postfix) {
		pl += read_bytes;
		*pl = ' ';
		pl++;
		rc = sprintf(pl, "keyhandle=%s", postfix);
	}

	return blob;
}

static key_serial_t dimm_check_key(struct ndctl_dimm *dimm,
		enum ndctl_key_type key_type)
{
	char desc[ND_KEY_DESC_SIZE];
	int rc;

	rc = get_key_desc(dimm, desc, key_type);
	if (rc < 0)
		return rc;

	return keyctl_search(KEY_SPEC_USER_KEYRING, "encrypted", desc, 0);
}

static key_serial_t dimm_create_key(struct ndctl_dimm *dimm,
		const char *kek)
{
	char desc[ND_KEY_DESC_SIZE];
	char path[PATH_MAX];
	char cmd[ND_KEY_CMD_SIZE];
	key_serial_t key;
	void *buffer;
	int rc;
	ssize_t size;
	FILE *fp;
	ssize_t wrote;
	struct stat st;

	if (ndctl_dimm_is_active(dimm)) {
		fprintf(stderr, "regions active on %s, op failed\n",
				ndctl_dimm_get_devname(dimm));
		return -EBUSY;
	}

	rc = get_key_desc(dimm, desc, ND_USER_KEY);
	if (rc < 0)
		return rc;

	/* make sure it's not already in the key ring */
	key = keyctl_search(KEY_SPEC_USER_KEYRING, "encrypted", desc, 0);
	if (key > 0) {
		fprintf(stderr, "Error: key already present in user keyring\n");
		return -EEXIST;
	}

	rc = get_key_path(dimm, path, ND_USER_KEY);
	if (rc < 0)
		return rc;

	rc = stat(path, &st);
	if (rc == 0) {
		fprintf(stderr, "%s already exists!\n", path);
		return -EEXIST;
	}

	rc = sprintf(cmd, "new enc32 %s 32", kek);
	if (rc < 0) {
		fprintf(stderr, "sprintf: %s\n", strerror(errno));
		return -errno;
	}

	key = add_key("encrypted", desc, cmd, strlen(cmd),
			KEY_SPEC_USER_KEYRING);
	if (key < 0) {
		fprintf(stderr, "add_key failed: %s\n", strerror(errno));
		return -errno;
	}

	size = keyctl_read_alloc(key, &buffer);
	if (size < 0) {
		fprintf(stderr, "keyctl_read_alloc failed: %s\n", strerror(errno));
		keyctl_unlink(key, KEY_SPEC_USER_KEYRING);
		return rc;
	}

	fp = fopen(path, "w");
	if (!fp) {
		rc = -errno;
		fprintf(stderr, "Unable to open file %s: %s\n",
				path, strerror(errno));
		free(buffer);
		return rc;
	}

	 wrote = fwrite(buffer, 1, size, fp);
	 if (wrote != size) {
		 if (wrote == -1)
			 rc = -errno;
		 else
			 rc = -EIO;
		 fprintf(stderr, "Failed to write to %s: %s\n",
				 path, strerror(-rc));
		 free(buffer);
		 return rc;
	 }

	 fclose(fp);
	 free(buffer);
	 return key;
}

static key_serial_t dimm_load_key(struct ndctl_dimm *dimm,
		enum ndctl_key_type key_type)
{
	key_serial_t key;
	char desc[ND_KEY_DESC_SIZE];
	char path[PATH_MAX];
	int rc;
	char *blob;
	int size;

	if (ndctl_dimm_is_active(dimm)) {
		fprintf(stderr, "regions active on %s, op failed\n",
				ndctl_dimm_get_devname(dimm));
		return -EBUSY;
	}

	rc = get_key_desc(dimm, desc, key_type);
	if (rc < 0)
		return rc;

	rc = get_key_path(dimm, path, key_type);
	if (rc < 0)
		return rc;

	blob = ndctl_load_key_blob(path, &size, NULL, -1);
	if (!blob)
		return -ENOMEM;

	key = add_key("encrypted", desc, blob, size, KEY_SPEC_USER_KEYRING);
	free(blob);
	if (key < 0) {
		fprintf(stderr, "add_key failed: %s\n", strerror(errno));
		return -errno;
	}

	return key;
}

/*
 * The function will check to see if the existing key is there and remove
 * from user key ring if it is. Rename the existing key blob to old key
 * blob, and then attempt to inject the key as old key into the user key
 * ring.
 */
static key_serial_t move_key_to_old(struct ndctl_dimm *dimm)
{
	int rc;
	key_serial_t key;
	char old_path[PATH_MAX];
	char new_path[PATH_MAX];

	if (ndctl_dimm_is_active(dimm)) {
		fprintf(stderr, "regions active on %s, op failed\n",
				ndctl_dimm_get_devname(dimm));
		return -EBUSY;
	}

	key = dimm_check_key(dimm, ND_USER_KEY);
	if (key > 0)
		keyctl_unlink(key, KEY_SPEC_USER_KEYRING);

	rc = get_key_path(dimm, old_path, ND_USER_KEY);
	if (rc < 0)
		return rc;

	rc = get_key_path(dimm, new_path, ND_USER_OLD_KEY);
	if (rc < 0)
		return rc;

	rc = rename(old_path, new_path);
	if (rc < 0) {
		fprintf(stderr, "rename failed from %s to %s: %s\n",
				old_path, new_path, strerror(errno));
		return -errno;
	}

	return dimm_load_key(dimm, ND_USER_OLD_KEY);
}

static int dimm_remove_key(struct ndctl_dimm *dimm,
		enum ndctl_key_type key_type)
{
	key_serial_t key;
	char path[PATH_MAX];
	int rc;

	key = dimm_check_key(dimm, key_type);
	if (key > 0)
		keyctl_unlink(key, KEY_SPEC_USER_KEYRING);

	rc = get_key_path(dimm, path, key_type);
	if (rc < 0)
		return rc;

	rc = unlink(path);
	if (rc < 0) {
		fprintf(stderr, "delete file %s failed: %s\n",
				path, strerror(errno));
		return -errno;
	}

	return 0;
}

static int verify_kek(struct ndctl_dimm *dimm, const char *kek)
{
	char *type, *desc, *key_handle;
	key_serial_t key;

	key_handle = strdup(kek);
	if (!key_handle)
		return -ENOMEM;

	type = strtok(key_handle, ":");
	if (!type) {
		fprintf(stderr, "No key type found for kek handle\n");
		return -EINVAL;
	}

	if (strcmp(type, "trusted") != 0 &&
			strcmp(type, "user") != 0) {
		fprintf(stderr, "No such key type: %s", type);
		return -EINVAL;
	}

	desc = strtok(NULL, ":");
	if (!desc) {
		fprintf(stderr, "No description found for kek handle\n");
		return -EINVAL;
	}

	key = keyctl_search(KEY_SPEC_USER_KEYRING, type, desc, 0);
	if (key < 0) {
		fprintf(stderr, "No key encryption key found\n");
		return key;
	}

	free(key_handle);
	return 0;
}

int ndctl_dimm_setup_key(struct ndctl_dimm *dimm, const char *kek)
{
	key_serial_t key;
	int rc;

	rc = verify_kek(dimm, kek);
	if (rc < 0)
		return rc;

	key = dimm_create_key(dimm, kek);
	if (key < 0)
		return key;

	rc = ndctl_dimm_update_passphrase(dimm, 0, key);
	if (rc < 0) {
		dimm_remove_key(dimm, ND_USER_KEY);
		return rc;
	}

	return 0;
}

static char *get_current_kek(struct ndctl_dimm *dimm)
{
	key_serial_t key;
	char *key_buf;
	long rc;
	char *type, *desc;

	key = dimm_check_key(dimm, ND_USER_KEY);
	if (key < 0)
		return NULL;

	rc = keyctl_read_alloc(key, (void **)&key_buf);
	if (rc < 0)
		return NULL;

	rc = sscanf(key_buf, "%ms %ms", &type, &desc);
	if (rc < 0)
		return NULL;

	free(key_buf);
	free(type);

	return desc;
}

int ndctl_dimm_update_key(struct ndctl_dimm *dimm, const char *kek)
{
	int rc;
	key_serial_t old_key, new_key;
	char *current_kek = NULL;

	if (kek) {
		rc = verify_kek(dimm, kek);
		if (rc < 0)
			return rc;
	} else { /* find current kek */
		current_kek = get_current_kek(dimm);
		if (!current_kek)
			return -ENOKEY;
	}

	/*
	 * 1. check if current key is loaded and remove
	 * 2. move current key blob to old key blob
	 * 3. load old key blob
	 * 4. trigger change key with old and new key
	 * 5. remove old key
	 * 6. remove old key blob
	 */
	old_key = move_key_to_old(dimm);
	if (old_key < 0)
		return old_key;

	new_key = dimm_create_key(dimm, current_kek ? current_kek : kek);
	free(current_kek);
	/* need to create new key here */
	if (new_key < 0) {
		new_key = dimm_load_key(dimm, ND_USER_KEY);
		if (new_key < 0)
			return new_key;
	}

	rc = ndctl_dimm_update_passphrase(dimm, old_key, new_key);
	if (rc < 0)
		return rc;

	rc = dimm_remove_key(dimm, ND_USER_OLD_KEY);
	if (rc < 0)
		return rc;

	return 0;
}

static key_serial_t check_dimm_key(struct ndctl_dimm *dimm, bool need_key)
{
	key_serial_t key;

	key = dimm_check_key(dimm, false);
	if (key < 0) {
		key = dimm_load_key(dimm, false);
		if (key < 0 && need_key) {
			fprintf(stderr, "Unable to load key\n");
			return -ENOKEY;
		} else
			key = 0;
	}
	return key;
}

static int run_key_op(struct ndctl_dimm *dimm,
		key_serial_t key,
		int (*run_op)(struct ndctl_dimm *, long), const char *name)
{
	int rc;

	rc = run_op(dimm, key);
	if (rc < 0) {
		fprintf(stderr, "Failed %s for %s\n", name,
				ndctl_dimm_get_devname(dimm));
		return rc;
	}

	return 0;
}

static int discard_key(struct ndctl_dimm *dimm)
{
	int rc;

	rc = dimm_remove_key(dimm, false);
	if (rc < 0) {
		fprintf(stderr, "Unable to cleanup key.\n");
		return rc;
	}

	return 0;
}

int ndctl_dimm_remove_key(struct ndctl_dimm *dimm)
{
	key_serial_t key;
	int rc;

	key = check_dimm_key(dimm, true);
	if (key < 0)
		return key;

	rc = run_key_op(dimm, key, ndctl_dimm_disable_passphrase,
			"remove passphrase");
	if (rc < 0)
		return rc;

	return discard_key(dimm);
}

int ndctl_dimm_secure_erase_key(struct ndctl_dimm *dimm)
{
	key_serial_t key;
	int rc;

	key = check_dimm_key(dimm, true);
	if (key < 0)
		return key;

	rc = run_key_op(dimm, key, ndctl_dimm_secure_erase,
			"crypto erase");
	if (rc < 0)
		return rc;

	return discard_key(dimm);
}

int ndctl_dimm_overwrite_key(struct ndctl_dimm *dimm)
{
	key_serial_t key;
	int rc;

	key = check_dimm_key(dimm, false);
	if (key < 0)
		return key;

	rc = run_key_op(dimm, key, ndctl_dimm_overwrite,
			"overwrite");
	if (rc < 0)
		return rc;

	if (key > 0)
		return discard_key(dimm);

	return 0;
}