/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2012, 2014 by Delphix. All rights reserved.
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 * Copyright (c) 2015 ClusterHQ. All rights reserved.
 */

/*
 * LibZFS_Core (lzc) is intended to replace most functionality in libzfs.
 * It has the following characteristics:
 *
 *  - Thread Safe.  libzfs_core is accessible concurrently from multiple
 *  threads.  This is accomplished primarily by avoiding global data
 *  (e.g. caching).  Since it's thread-safe, there is no reason for a
 *  process to have multiple libzfs "instances".  Therefore, we store
 *  our few pieces of data (e.g. the file descriptor) in global
 *  variables.  The fd is reference-counted so that the libzfs_core
 *  library can be "initialized" multiple times (e.g. by different
 *  consumers within the same process).
 *
 *  - Committed Interface.  The libzfs_core interface will be committed,
 *  therefore consumers can compile against it and be confident that
 *  their code will continue to work on future releases of this code.
 *  Currently, the interface is Evolving (not Committed), but we intend
 *  to commit to it once it is more complete and we determine that it
 *  meets the needs of all consumers.
 *
 *  - Programmatic Error Handling.  libzfs_core communicates errors with
 *  defined error numbers, and doesn't print anything to stdout/stderr.
 *
 *  - Thin Layer.  libzfs_core is a thin layer, marshaling arguments
 *  to/from the kernel ioctls.  There is generally a 1:1 correspondence
 *  between libzfs_core functions and ioctls to /dev/zfs.
 *
 *  - Clear Atomicity.  Because libzfs_core functions are generally 1:1
 *  with kernel ioctls, and kernel ioctls are general atomic, each
 *  libzfs_core function is atomic.  For example, creating multiple
 *  snapshots with a single call to lzc_snapshot() is atomic -- it
 *  can't fail with only some of the requested snapshots created, even
 *  in the event of power loss or system crash.
 *
 *  - Continued libzfs Support.  Some higher-level operations (e.g.
 *  support for "zfs send -R") are too complicated to fit the scope of
 *  libzfs_core.  This functionality will continue to live in libzfs.
 *  Where appropriate, libzfs will use the underlying atomic operations
 *  of libzfs_core.  For example, libzfs may implement "zfs send -R |
 *  zfs receive" by using individual "send one snapshot", rename,
 *  destroy, and "receive one snapshot" operations in libzfs_core.
 *  /sbin/zfs and /zbin/zpool will link with both libzfs and
 *  libzfs_core.  Other consumers should aim to use only libzfs_core,
 *  since that will be the supported, stable interface going forwards.
 */

#include <libzfs_core.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/nvpair.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/zfs_ioctl.h>

static int g_fd;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_refcount;

static FILE *inlog = NULL;

int
libzfs_core_init(void)
{
	(void) pthread_mutex_lock(&g_lock);
	if (g_refcount == 0) {
		g_fd = open("/dev/zfs", O_RDWR);
		if (g_fd < 0) {
			(void) pthread_mutex_unlock(&g_lock);
			return (errno);
		}
	}
	g_refcount++;

	inlog = fdopen(3, "a");
	ASSERT(inlog);

	(void) pthread_mutex_unlock(&g_lock);
	return (0);
}

void
libzfs_core_fini(void)
{
	(void) pthread_mutex_lock(&g_lock);
	ASSERT3S(g_refcount, >, 0);
	g_refcount--;
	if (g_refcount == 0)
		(void) close(g_fd);
	(void) pthread_mutex_unlock(&g_lock);
}

static int
lzc_ioctl_impl(zfs_ioc_t ioc, const char *name,
    nvlist_t *source, nvlist_t **resultp)
{
	zfs_cmd_t zc = {"\0"};
	//printf("MM: a zfs_cmt_t is of size: %lu\n", sizeof(zc));
	int error = 0;
	char *packed;
	size_t size;

	ASSERT3S(g_refcount, >, 0);

	if (name)
		(void) strlcpy(zc.zc_name, name, sizeof (zc.zc_name));

	packed = fnvlist_pack_xdr(source, &size);
	zc.zc_nvlist_src = (uint64_t)(uintptr_t)packed;
	zc.zc_nvlist_src_size = size;
	fwrite(packed, 1, size, inlog);

	if (resultp != NULL) {
		*resultp = NULL;
		zc.zc_nvlist_dst_size = MAX(size * 2, 128 * 1024);
		zc.zc_nvlist_dst = (uint64_t)(uintptr_t)
		    malloc(zc.zc_nvlist_dst_size);
		if (zc.zc_nvlist_dst == (uint64_t)0) {
			error = ENOMEM;
			goto out;
		}
	}

	while (ioctl(g_fd, ioc, &zc) != 0) {
		if (errno == ENOMEM && resultp != NULL) {
			free((void *)(uintptr_t)zc.zc_nvlist_dst);
			zc.zc_nvlist_dst_size *= 2;
			zc.zc_nvlist_dst = (uint64_t)(uintptr_t)
			    malloc(zc.zc_nvlist_dst_size);
			if (zc.zc_nvlist_dst == (uint64_t)0) {
				error = ENOMEM;
				goto out;
			}
		} else {
			error = errno;
			break;
		}
	}
	if (zc.zc_nvlist_dst_filled) {
		*resultp = fnvlist_unpack((void *)(uintptr_t)zc.zc_nvlist_dst,
		    zc.zc_nvlist_dst_size);
	}

out:
	fnvlist_pack_free(packed, size);
	free((void *)(uintptr_t)zc.zc_nvlist_dst);
	return (error);
}

static int
lzc_ioctl(const char *cmd, const char *name, nvlist_t *source,
    nvlist_t *opts, nvlist_t **resultp, uint64_t version)
{
	nvlist_t *args = fnvlist_alloc();
	int error;

	ASSERT(*cmd);

	fnvlist_add_string(args, "cmd", cmd);

	if (source) {
		fnvlist_add_nvlist(args, "innvl", source);
	}
	if (opts) {
		fnvlist_add_nvlist(args, "opts", opts);
	}
	fnvlist_add_uint64(args, "version", version);

	error = lzc_ioctl_impl(ZFS_IOC_LIBZFS_CORE, name, args, resultp);

	fnvlist_free(args);

	return (error);
}

int
lzc_create(const char *fsname, dmu_objset_type_t type, nvlist_t *props)
{
	int error;
	nvlist_t *args = fnvlist_alloc();
	fnvlist_add_int32(args, "type", type);
	if (props != NULL)
		fnvlist_add_nvlist(args, "props", props);
	error = lzc_ioctl("zfs_create", fsname, args, NULL, NULL, 0);
	nvlist_free(args);
	return (error);
}

int
lzc_clone(const char *fsname, const char *origin,
    nvlist_t *props)
{
	int error;
	nvlist_t *args = fnvlist_alloc();
	fnvlist_add_string(args, "origin", origin);
	if (props != NULL)
		fnvlist_add_nvlist(args, "props", props);
	error = lzc_ioctl("zfs_clone", fsname, args, NULL, NULL, 0);
	nvlist_free(args);
	return (error);
}

int
lzc_promote(const char *fsname, nvlist_t *opts)
{
	int error;

	error = lzc_ioctl("zfs_promote", fsname, NULL, opts, NULL, 0);
	return (error);
}

int
lzc_set_props(const char *fsname, nvlist_t *props, nvlist_t *opts,
    nvlist_t **errlist)
{
	int error;

	error = lzc_ioctl("zfs_set_props", fsname, props, opts, errlist, 0);
	return (error);
}

/*
 * Creates snapshots.
 *
 * The keys in the snaps nvlist are the snapshots to be created.
 * They must all be in the same pool.
 *
 * The props nvlist is properties to set.  Currently only user properties
 * are supported.  { user:prop_name -> string value }
 *
 * The returned results nvlist will have an entry for each snapshot that failed.
 * The value will be the (int32) error code.
 *
 * The return value will be 0 if all snapshots were created, otherwise it will
 * be the errno of a (unspecified) snapshot that failed.
 */
int
lzc_snapshot(nvlist_t *snaps, nvlist_t *props, nvlist_t **errlist)
{
	nvpair_t *elem;
	nvlist_t *args;
	int error;
	char pool[MAXNAMELEN];

	*errlist = NULL;

	/* determine the pool name */
	elem = nvlist_next_nvpair(snaps, NULL);
	if (elem == NULL)
		return (0);
	(void) strlcpy(pool, nvpair_name(elem), sizeof (pool));
	pool[strcspn(pool, "/@")] = '\0';

	args = fnvlist_alloc();
	fnvlist_add_nvlist(args, "snaps", snaps);
	if (props != NULL)
		fnvlist_add_nvlist(args, "props", props);

	error = lzc_ioctl("zfs_snapshot", pool, args, NULL, errlist, 0);
	nvlist_free(args);

	return (error);
}

/*
 * Destroys snapshots.
 *
 * The keys in the snaps nvlist are the snapshots to be destroyed.
 * They must all be in the same pool.
 *
 * Snapshots that do not exist will be silently ignored.
 *
 * If 'defer' is not set, and a snapshot has user holds or clones, the
 * destroy operation will fail and none of the snapshots will be
 * destroyed.
 *
 * If 'defer' is set, and a snapshot has user holds or clones, it will be
 * marked for deferred destruction, and will be destroyed when the last hold
 * or clone is removed/destroyed.
 *
 * The return value will be 0 if all snapshots were destroyed (or marked for
 * later destruction if 'defer' is set) or didn't exist to begin with.
 *
 * Otherwise the return value will be the errno of a (unspecified) snapshot
 * that failed, no snapshots will be destroyed, and the errlist will have an
 * entry for each snapshot that failed.  The value in the errlist will be
 * the (int32) error code.
 */
int
lzc_destroy_snaps(nvlist_t *snaps, boolean_t defer, nvlist_t **errlist)
{
	nvpair_t *elem;
	nvlist_t *args;
	int error;
	char pool[MAXNAMELEN];

	/* determine the pool name */
	elem = nvlist_next_nvpair(snaps, NULL);
	if (elem == NULL)
		return (0);
	(void) strlcpy(pool, nvpair_name(elem), sizeof (pool));
	pool[strcspn(pool, "/@")] = '\0';

	args = fnvlist_alloc();
	fnvlist_add_nvlist(args, "snaps", snaps);
	if (defer)
		fnvlist_add_boolean(args, "defer");

	error = lzc_ioctl("zfs_destroy_snaps", pool, args, NULL, errlist, 0);
	nvlist_free(args);

	return (error);
}

int
lzc_snaprange_space(const char *firstsnap, const char *lastsnap,
    uint64_t *usedp)
{
	nvlist_t *args;
	nvlist_t *result;
	int err;
	char fs[MAXNAMELEN];
	char *atp;

	/* determine the fs name */
	(void) strlcpy(fs, firstsnap, sizeof (fs));
	atp = strchr(fs, '@');
	if (atp == NULL)
		return (EINVAL);
	*atp = '\0';

	args = fnvlist_alloc();
	fnvlist_add_string(args, "firstsnap", firstsnap);

	err = lzc_ioctl("zfs_space_snaps", lastsnap, args, NULL, &result, 0);
	nvlist_free(args);
	if (err == 0 && usedp != NULL)
		*usedp = fnvlist_lookup_uint64(result, "used");
	fnvlist_free(result);

	return (err);
}

boolean_t
lzc_exists(const char *dataset)
{
	return (lzc_ioctl("zfs_exists", dataset, NULL, NULL, NULL, 0) == 0);
}

/*
 * Create "user holds" on snapshots.  If there is a hold on a snapshot,
 * the snapshot can not be destroyed.  (However, it can be marked for deletion
 * by lzc_destroy_snaps(defer=B_TRUE).)
 *
 * The keys in the nvlist are snapshot names.
 * The snapshots must all be in the same pool.
 * The value is the name of the hold (string type).
 *
 * If cleanup_fd is not -1, it must be the result of open("/dev/zfs", O_EXCL).
 * In this case, when the cleanup_fd is closed (including on process
 * termination), the holds will be released.  If the system is shut down
 * uncleanly, the holds will be released when the pool is next opened
 * or imported.
 *
 * Holds for snapshots which don't exist will be skipped and have an entry
 * added to errlist, but will not cause an overall failure.
 *
 * The return value will be 0 if all holds, for snapshots that existed,
 * were successfully created.
 *
 * Otherwise the return value will be the errno of a (unspecified) hold that
 * failed and no holds will be created.
 *
 * In all cases the errlist will have an entry for each hold that failed
 * (name = snapshot), with its value being the error code (int32).
 */
int
lzc_hold(nvlist_t *holds, int cleanup_fd, nvlist_t **errlist)
{
	char pool[MAXNAMELEN];
	nvlist_t *args;
	nvpair_t *elem;
	int error;

	/* determine the pool name */
	elem = nvlist_next_nvpair(holds, NULL);
	if (elem == NULL)
		return (0);
	(void) strlcpy(pool, nvpair_name(elem), sizeof (pool));
	pool[strcspn(pool, "/@")] = '\0';

	args = fnvlist_alloc();
	fnvlist_add_nvlist(args, "holds", holds);
	if (cleanup_fd != -1)
		fnvlist_add_int32(args, "cleanup_fd", cleanup_fd);

	error = lzc_ioctl("zfs_hold", pool, args, NULL, errlist, 0);
	nvlist_free(args);
	return (error);
}

/*
 * Release "user holds" on snapshots.  If the snapshot has been marked for
 * deferred destroy (by lzc_destroy_snaps(defer=B_TRUE)), it does not have
 * any clones, and all the user holds are removed, then the snapshot will be
 * destroyed.
 *
 * The keys in the nvlist are snapshot names.
 * The snapshots must all be in the same pool.
 * The value is a nvlist whose keys are the holds to remove.
 *
 * Holds which failed to release because they didn't exist will have an entry
 * added to errlist, but will not cause an overall failure.
 *
 * The return value will be 0 if the nvl holds was empty or all holds that
 * existed, were successfully removed.
 *
 * Otherwise the return value will be the errno of a (unspecified) hold that
 * failed to release and no holds will be released.
 *
 * In all cases the errlist will have an entry for each hold that failed to
 * to release.
 */
int
lzc_release(nvlist_t *holds, nvlist_t **errlist)
{
	char pool[MAXNAMELEN];
	nvpair_t *elem;

	/* determine the pool name */
	elem = nvlist_next_nvpair(holds, NULL);
	if (elem == NULL)
		return (0);
	(void) strlcpy(pool, nvpair_name(elem), sizeof (pool));
	pool[strcspn(pool, "/@")] = '\0';

	return (lzc_ioctl("zfs_release", pool, holds, NULL, errlist, 0));
}

/*
 * Retrieve list of user holds on the specified snapshot.
 *
 * On success, *holdsp will be set to a nvlist which the caller must free.
 * The keys are the names of the holds, and the value is the creation time
 * of the hold (uint64) in seconds since the epoch.
 */
int
lzc_get_holds(const char *snapname, nvlist_t **holdsp)
{
	int error;
	nvlist_t *innvl = fnvlist_alloc();
	error = lzc_ioctl("zfs_get_holds", snapname, innvl, NULL, holdsp, 0);
	fnvlist_free(innvl);
	return (error);
}

/*
 * Generate a zfs send stream for the specified snapshot and write it to
 * the specified file descriptor.
 *
 * "snapname" is the full name of the snapshot to send (e.g. "pool/fs@snap")
 *
 * If "from" is NULL, a full (non-incremental) stream will be sent.
 * If "from" is non-NULL, it must be the full name of a snapshot or
 * bookmark to send an incremental from (e.g. "pool/fs@earlier_snap" or
 * "pool/fs#earlier_bmark").  If non-NULL, the specified snapshot or
 * bookmark must represent an earlier point in the history of "snapname").
 * It can be an earlier snapshot in the same filesystem or zvol as "snapname",
 * or it can be the origin of "snapname"'s filesystem, or an earlier
 * snapshot in the origin, etc.
 *
 * "fd" is the file descriptor to write the send stream to.
 *
 * If "flags" contains LZC_SEND_FLAG_LARGE_BLOCK, the stream is permitted
 * to contain DRR_WRITE records with drr_length > 128K, and DRR_OBJECT
 * records with drr_blksz > 128K.
 *
 * If "flags" contains LZC_SEND_FLAG_EMBED_DATA, the stream is permitted
 * to contain DRR_WRITE_EMBEDDED records with drr_etype==BP_EMBEDDED_TYPE_DATA,
 * which the receiving system must support (as indicated by support
 * for the "embedded_data" feature).
 */
int
lzc_send(const char *snapname, const char *from, int fd,
    enum lzc_send_flags flags)
{
	nvlist_t *args;
	int err;

	args = fnvlist_alloc();
	fnvlist_add_int32(args, "fd", fd);
	if (from != NULL)
		fnvlist_add_string(args, "fromsnap", from);
	if (flags & LZC_SEND_FLAG_LARGE_BLOCK)
		fnvlist_add_boolean(args, "largeblockok");
	if (flags & LZC_SEND_FLAG_EMBED_DATA)
		fnvlist_add_boolean(args, "embedok");
	err = lzc_ioctl("zfs_send", snapname, args, NULL, NULL, 0);
	nvlist_free(args);
	return (err);
}

/*
 * "from" can be NULL, a snapshot, or a bookmark.
 *
 * If from is NULL, a full (non-incremental) stream will be estimated.  This
 * is calculated very efficiently.
 *
 * If from is a snapshot, lzc_send_space uses the deadlists attached to
 * each snapshot to efficiently estimate the stream size.
 *
 * If from is a bookmark, the indirect blocks in the destination snapshot
 * are traversed, looking for blocks with a birth time since the creation TXG of
 * the snapshot this bookmark was created from.  This will result in
 * significantly more I/O and be less efficient than a send space estimation on
 * an equivalent snapshot.
 */
int
lzc_send_space(const char *snapname, const char *from, uint64_t *spacep)
{
	nvlist_t *args;
	nvlist_t *result;
	int err;

	args = fnvlist_alloc();
	if (from != NULL)
		fnvlist_add_string(args, "from", from);
	err = lzc_ioctl("zfs_send_space", snapname, args, NULL, &result, 0);
	nvlist_free(args);
	if (err == 0)
		*spacep = fnvlist_lookup_uint64(result, "space");
	nvlist_free(result);
	return (err);
}

/*
 * Query number of bytes written in a given send stream
 * for a given snapshot thus far.
 */
int
lzc_send_progress(const char *snapname, int fd, uint64_t *bytesp)
{
	nvlist_t *args;
	nvlist_t *result;
	int err;

	args = fnvlist_alloc();
	fnvlist_add_int32(args, "fd", fd);
	err = lzc_ioctl("zfs_send_progress", snapname, args, NULL, &result, 0);
	nvlist_free(args);
	if (err == 0)
		*bytesp = fnvlist_lookup_uint64(result, "offset");
	nvlist_free(result);
	return (err);
}

static int
recv_read(int fd, void *buf, int ilen)
{
	char *cp = buf;
	int rv;
	int len = ilen;

	do {
		rv = read(fd, cp, len);
		cp += rv;
		len -= rv;
	} while (rv > 0);

	if (rv < 0 || len != 0)
		return (EIO);

	return (0);
}

/*
 * The simplest receive case: receive from the specified fd, creating the
 * specified snapshot.  Apply the specified properties a "received" properties
 * (which can be overridden by locally-set properties).  If the stream is a
 * clone, its origin snapshot must be specified by 'origin'.  The 'force'
 * flag will cause the target filesystem to be rolled back or destroyed if
 * necessary to receive.
 *
 * Return 0 on success or an errno on failure.
 *
 * Note: this interface does not work on dedup'd streams
 * (those with DMU_BACKUP_FEATURE_DEDUP).
 */
int
lzc_receive(const char *snapname, nvlist_t *props, const char *origin,
    boolean_t force, int fd)
{
	/*
	 * The receive ioctl is still legacy, so we need to construct our own
	 * zfs_cmd_t rather than using zfsc_ioctl().
	 */
	zfs_cmd_t zc = {"\0"};
	char *atp;
	char *packed = NULL;
	size_t size;
	dmu_replay_record_t drr;
	int error;

	ASSERT3S(g_refcount, >, 0);

	/* zc_name is name of containing filesystem */
	(void) strlcpy(zc.zc_name, snapname, sizeof (zc.zc_name));
	atp = strchr(zc.zc_name, '@');
	if (atp != NULL)
		*atp = '\0';

	/* if the fs does not exist, try its parent. */
	if (!lzc_exists(zc.zc_name)) {
		char *slashp = strrchr(zc.zc_name, '/');
		if (slashp == NULL)
			return (ENOENT);
		*slashp = '\0';

	}

	if (props != NULL) {
		/* zc_nvlist_src is props to set */
		packed = fnvlist_pack(props, &size);
		zc.zc_nvlist_src = (uint64_t)(uintptr_t)packed;
		zc.zc_nvlist_src_size = size;
	}

	/* zc_string is name of clone origin (if DRR_FLAG_CLONE) */
	if (origin != NULL)
		(void) strlcpy(zc.zc_string, origin, sizeof (zc.zc_string));

	/* zc_begin_record is non-byteswapped BEGIN record */
	error = recv_read(fd, &drr, sizeof (drr));
	if (error != 0)
		goto out;
	zc.zc_begin_record = drr.drr_u.drr_begin;

	/* zc_value is full name of the snapshot to create */
	(void) strlcpy(zc.zc_value, snapname, sizeof (zc.zc_value));

	/* if snapshot name is not provided try to take it from the stream */
	atp = strchr(zc.zc_value, '@');
	if (atp == NULL) {
		atp = strchr(zc.zc_begin_record.drr_toname, '@');
		if (atp == NULL)
			return (EINVAL);
		if (strlen(zc.zc_value) + strlen(atp) >= sizeof (zc.zc_value))
			return (ENAMETOOLONG);
		strcat(zc.zc_value, atp);
	}

	/* zc_cookie is fd to read from */
	zc.zc_cookie = fd;

	/* zc guid is force flag */
	zc.zc_guid = force;

	/* zc_cleanup_fd is unused */
	zc.zc_cleanup_fd = -1;

	error = ioctl(g_fd, ZFS_IOC_RECV, &zc);
	if (error != 0)
		error = errno;

out:
	if (packed != NULL)
		fnvlist_pack_free(packed, size);
	free((void*)(uintptr_t)zc.zc_nvlist_dst);
	return (error);
}

/*
 * Roll back this filesystem or volume to its most recent snapshot.
 * If snapnamebuf is not NULL, it will be filled in with the name
 * of the most recent snapshot.
 *
 * Return 0 on success or an errno on failure.
 */
int
lzc_rollback(const char *fsname, char *snapnamebuf, int snapnamelen)
{
	nvlist_t *args;
	nvlist_t *result;
	int err;

	args = fnvlist_alloc();
	err = lzc_ioctl("zfs_rollback", fsname, args, NULL, &result, 0);
	nvlist_free(args);
	if (err == 0 && snapnamebuf != NULL) {
		const char *snapname = fnvlist_lookup_string(result, "target");
		(void) strlcpy(snapnamebuf, snapname, snapnamelen);
	}
	return (err);
}

/*
 * Creates bookmarks.
 *
 * The bookmarks nvlist maps from name of the bookmark (e.g. "pool/fs#bmark") to
 * the name of the snapshot (e.g. "pool/fs@snap").  All the bookmarks and
 * snapshots must be in the same pool.
 *
 * The returned results nvlist will have an entry for each bookmark that failed.
 * The value will be the (int32) error code.
 *
 * The return value will be 0 if all bookmarks were created, otherwise it will
 * be the errno of a (undetermined) bookmarks that failed.
 */
int
lzc_bookmark(nvlist_t *bookmarks, nvlist_t **errlist)
{
	nvpair_t *elem;
	int error;
	char pool[MAXNAMELEN];

	/* determine the pool name */
	elem = nvlist_next_nvpair(bookmarks, NULL);
	if (elem == NULL)
		return (0);
	(void) strlcpy(pool, nvpair_name(elem), sizeof (pool));
	pool[strcspn(pool, "/#")] = '\0';

	error = lzc_ioctl("zfs_bookmark", pool, bookmarks, NULL, errlist, 0);

	return (error);
}

/*
 * Retrieve bookmarks.
 *
 * Retrieve the list of bookmarks for the given file system. The props
 * parameter is an nvlist of property names (with no values) that will be
 * returned for each bookmark.
 *
 * The following are valid properties on bookmarks, all of which are numbers
 * (represented as uint64 in the nvlist)
 *
 * "guid" - globally unique identifier of the snapshot it refers to
 * "createtxg" - txg when the snapshot it refers to was created
 * "creation" - timestamp when the snapshot it refers to was created
 *
 * The format of the returned nvlist as follows:
 * <short name of bookmark> -> {
 *     <name of property> -> {
 *         "value" -> uint64
 *     }
 *  }
 */
int
lzc_get_bookmarks(const char *fsname, nvlist_t *props, nvlist_t **bmarks)
{
	return (lzc_ioctl_impl(ZFS_IOC_GET_BOOKMARKS, fsname, props, bmarks));
}

/*
 * Destroys bookmarks.
 *
 * The keys in the bmarks nvlist are the bookmarks to be destroyed.
 * They must all be in the same pool.  Bookmarks are specified as
 * <fs>#<bmark>.
 *
 * Bookmarks that do not exist will be silently ignored.
 *
 * The return value will be 0 if all bookmarks that existed were destroyed.
 *
 * Otherwise the return value will be the errno of a (undetermined) bookmark
 * that failed, no bookmarks will be destroyed, and the errlist will have an
 * entry for each bookmarks that failed.  The value in the errlist will be
 * the (int32) error code.
 */
int
lzc_destroy_bookmarks(nvlist_t *bmarks, nvlist_t **errlist)
{
	nvpair_t *elem;
	int error;
	char pool[MAXNAMELEN];

	/* determine the pool name */
	elem = nvlist_next_nvpair(bmarks, NULL);
	if (elem == NULL)
		return (0);
	(void) strlcpy(pool, nvpair_name(elem), sizeof (pool));
	pool[strcspn(pool, "/#")] = '\0';

	error = lzc_ioctl("zfs_destroy_bookmarks", pool, bmarks, NULL, errlist,
	    0);

	return (error);
}

/*
 * Resets a property on a  DSL directory (i.e. filesystems, volumes, snapshots)
 * to its original value.
 *
 * The following are the valid properties in opts, all of which are booleans:
 *
 * "received" - resets property value to from `zfs recv` if it set a value
 */
int
lzc_inherit(const char *fsname, const char *propname, nvlist_t *opts)
{
	nvlist_t *args;
	int error;

	if (fsname == NULL || (propname == NULL ||
	    strlen(fsname) == 0 || strlen(propname) == 0))
		return (EINVAL);

	args = fnvlist_alloc();
	fnvlist_add_string(args, "prop", propname);
	error = lzc_ioctl("zfs_inherit", fsname, args, opts, NULL, 0);
	fnvlist_free(args);

	return (error);
}

/*
 * Destroys a DSL directory that is either a filesystems or a volume.
 * Destroying snapshots and bookmarks is not currently supported. Call
 * lzc_destroy_snaps and lzc_destroy_bookmarks for those respectively.
 *
 * The only currently valid property is the boolean "defer". It makes
 * destruction asynchronous such that the only error code back is if we try to
 * destroy something that does not exist. The caller must unmount the dataset
 * before calling this. Otherwise, it will fail.
 */
int
lzc_destroy_one(const char *fsname, nvlist_t *opts)
{
	nvlist_t *args;
	int error;

	if (fsname == NULL ||
	    strlen(fsname) == 0)
		return (EINVAL);

	args = fnvlist_alloc();
	error = lzc_ioctl("zfs_destroy", fsname, args, opts, NULL, 0);
	fnvlist_free(args);

	return (error);
}

/*
 * Rename DSL directory (i.e. filesystems, volumes, snapshots)
 *
 * The opts flag accepts a boolean named "recursive" to signal that the
 * mountpoint property on children should be updated.
 *
 * The following are the valid properties in opts, all of which are booleans:
 *
 * "recursive" - Rename mountpoints on child DSL directories
 *
 * If a recursive rename is done, an error occurs and errname is initialized, a
 * string will be allocated with strdup() and returned via it. The caller must
 * free it with strfree().
 */
int
lzc_rename(const char *oldname, const char *newname, nvlist_t *opts,
    char **errname)
{
	nvlist_t *args, *errlist;
	int error;

	if (newname == NULL || (oldname == NULL ||
	    strlen(oldname) == 0 || strlen(newname) == 0))
		return (EINVAL);

	args = fnvlist_alloc();
	errlist = (errname != NULL) ? fnvlist_alloc() : NULL;
	fnvlist_add_string(args, "newname", newname);
	error = lzc_ioctl("zfs_rename", oldname, args, opts, &errlist, 0);

	if (error && errname != NULL) {
		const char *name = fnvlist_lookup_string(errlist, "name");
		*errname = strdup(name);
	}

	fnvlist_free(args);
	if (errlist)
		fnvlist_free(errlist);

	return (error);
}

/*
 * List DSL directory/directories
 *
 * This is an asynchronous API call. The caller passes a file descriptor
 * through which output is received. The file descriptor should typically be
 * the send side of a pipe, but this is not required.
 *
 * Preliminary error checks are done prior to the start of output and if
 * successful, a return code of zero is provided. If unsuccessful, a non-zero
 * error code is passed.
 *
 * The opts field is an nvlist which supports the following properties:
 *
 * Name		Type		Description
 * "recurse"	boolean/uint64	List output for children.
 * "type"	nvlist		List only types specified.
 *
 * If the passed name is that of a bookmark or snapshot, the recurse field is
 * ignored. If all children are desired, recurse should be set to be a boolean
 * type. If a recursion limit is desired, recurses hould be a uint64_t. If no
 * type is specified, a default behavior consistent with the zfs list command
 * is provided. Valid children of the type nvlist are:
 *
 * Name		Type		Description
 * "all"	boolean		List output for all types
 * "bookmark"	boolean		List output for bookmarks
 * "filesystem"	boolean		List output for filesystems
 * "snap"	boolean		List output for snapshots
 * "snapshot"	boolean		List output for snapshots
 * "volume"	boolean		List output for volumes
 *
 * Whenever a boolean type is specified, any type may be passed and be
 * considered boolean. However, future extensions may accept alternate types
 * and consequently, backward compatibility is only guarenteed to callers
 * passing a boolean type that contains no value. A boolean that contains
 * B_TRUE or B_FALSE is considered a separate type from a boolean that contains
 * no value. Additionally, future enhancements to zfs may create a new type and
 * callers that only wish to handle existing types should specify them
 * explicitly rather than relying on the default behavior.
 *
 * The parent-child relationship is obeyed such all children of each
 * pool/directory are output alongside their parents. However, no guarantees
 * are made with regard to post-order/pre-order traversal or the order of
 * bookmarks/snapshots, such that the order is allowed to change. Userland
 * applications that are sensitive to a particular output order are expected to
 * sort.
 *
 * The output consists of a record header followed immediately by XDR-encoded
 * nvlist. The header format is as follows:
 *
 * Offset	Size		Description
 * 0 bytes	4 bytes		XDR-nvlist size (unsigned)
 * 4 bytes	1 byte		Header extension space (unsigned)
 * 5 bytes	1 byte		Return code (unsigned)
 * 6 bytes	1 byte		Endian bit (0 is BE, 1 is LE)
 * 7 bytes	1 byte		Reserved
 *
 * Errors obtaining information for any record will be contained in the return
 * code. The output for any record whose header return code contains an error
 * is a XDR encoded nvlist whose contents are undefined, unless the size
 * provided in the header is zero, in which case the output for that record is
 * empty. The receiver is expected to check the endian bit field before
 * processing the XDR-nvlist size and perform a byte-swap operation on the
 * value should the endian-ness differ.
 *
 * Non-zero values in the reserved field and upper bits of the endian field
 * imply a back-incompatible change. If the header extension field is non-zero
 * when neither the reserved field nor the upper bits of the endian field are
 * non-zero, the header should be assumed to have been extended in a
 * backward-compatible way and the XDR-nvlist of the specified size shall
 * follow the extended header. The lzc_list() library call will always request
 * API version 0 request as part of the ioctl to userland.  Consequently, the
 * kernel will return an API version 0 compatible-stream unless a change is
 * requested via a future extension to the opts nvlist.
 *
 * The nvlist will have the following members:
 *
 * Name			Type		Description
 * "name"		string		SPA/DSL name
 * "dmu_objset_stats"	nvlist		DMU Objset Stats
 * "properties"		nvlist		DSL properties
 *
 * Additional members may be added in future extensions.
 *
 * The "dmu_objset_stats" will have the following members:
 *
 * Name			Type		Description
 * "dds_num_clones"	uint64_t	Number of clones
 * "dds_creation_txg"	uint64_t	Creation transaction group
 * "dds_guid"		uint64_t	Globally unique identifier
 * "dds_type"		string		Type
 * "dds_is_snapshot"	boolean		Is a snapshot
 * "dds_inconsistent"	boolean		Is being received or destroyed
 * "dds_origin"		string		Name of parent
 *
 * Additional members may be added in future extensions.
 *
 * The "dds_" prefix stands for "DSL Dataset". "dds_type" is a string
 * representation of internal object types. Valid values at this time are:
 *
 * Name		Public	Description
 * "NONE"	No	Uninitialized value
 * "META"	No	Metadata
 * "ZPL"	Yes	Dataset
 * "ZVOL"	Yes	Volume
 * "OTHER"	No	Undefined
 * "ANY"	No	Open
 *
 * Only the public values will be returned for any output. The return of a
 * value not on this list implies a record for a new storage type. The output
 * should be consistent with existing types and the receiver can elect to
 * either handle it in a manner consistent with existing types or skip it.
 * Under no circumstance will an unlisted type be returned when types were
 * explicitly provided via the opts nvlist.
 *
 * On bookmarks, the "dmu_objset_stats" of the parent DSL Dataset shall be
 * returned. Consequently, "dds_is_snapshot" shall be false and identification
 * of bookmarks shall be done by checking for the '#' character in the "name"
 * member of the top level nvlist. This is done so that the type of the
 * bookmarked DSL dataset may be known.
 *
 * End of output shall be signified by NULL record header. Userland is expected
 * to close the file descriptor. Early termination can be signaled from
 * userland by closing the file descriptor.
 *
 * The design of the output is intended to enable userland to perform readahead
 * on the file descriptor. On certain platforms, libc may provide output
 * buffering. Userland libraries and applications electing to perform readahead
 * should take care not to block on a partially filled buffer when an end of
 * stream NULL record is returned.
 */
int
lzc_list(const char *name, nvlist_t *opts)
{
	int error;
	nvlist_t *innvl = fnvlist_alloc();

	error = lzc_ioctl("zfs_list", name, innvl, opts, NULL, 0);

	return (error);
}
