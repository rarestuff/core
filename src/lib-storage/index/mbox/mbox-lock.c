/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "mbox-storage.h"
#include "mbox-file.h"
#include "mbox-lock.h"

#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef HAVE_FLOCK
#  include <sys/file.h>
#endif

/* 0.1 .. 0.2msec */
#define LOCK_RANDOM_USLEEP_TIME (100000 + (unsigned int)rand() % 100000)

/* lock methods to use in wanted order */
#define DEFAULT_READ_LOCK_METHODS "fcntl"
#define DEFAULT_WRITE_LOCK_METHODS "dotlock fcntl"
/* lock timeout */
#define DEFAULT_LOCK_TIMEOUT (10*60)
/* assume stale dotlock if mbox file hasn't changed for n seconds */
#define DEFAULT_DOTLOCK_CHANGE_TIMEOUT (5*60)

enum mbox_lock_type {
	MBOX_LOCK_DOTLOCK,
	MBOX_LOCK_FCNTL,
	MBOX_LOCK_FLOCK,
	MBOX_LOCK_LOCKF,

	MBOX_LOCK_COUNT
};

struct mbox_lock_context {
	struct index_mailbox *ibox;
	int lock_status[MBOX_LOCK_COUNT];
	int checked_file;

	int lock_type;
	int dotlock_last_stale;
};

struct mbox_lock_data {
	enum mbox_lock_type type;
	const char *name;
	int (*func)(struct mbox_lock_context *ctx, int lock_type,
		    time_t max_wait_time);
};

static int mbox_lock_dotlock(struct mbox_lock_context *ctx, int lock_type,
			     time_t max_wait_time);
static int mbox_lock_fcntl(struct mbox_lock_context *ctx, int lock_type,
			   time_t max_wait_time);
#ifdef HAVE_FLOCK
static int mbox_lock_flock(struct mbox_lock_context *ctx, int lock_type,
			   time_t max_wait_time);
#else
#  define mbox_lock_flock NULL
#endif
#ifdef HAVE_LOCKF
static int mbox_lock_lockf(struct mbox_lock_context *ctx, int lock_type,
			   time_t max_wait_time);
#else
#  define mbox_lock_lockf NULL
#endif

struct mbox_lock_data lock_data[] = {
	{ MBOX_LOCK_DOTLOCK, "dotlock", mbox_lock_dotlock },
	{ MBOX_LOCK_FCNTL, "fcntl", mbox_lock_fcntl },
	{ MBOX_LOCK_FLOCK, "flock", mbox_lock_flock },
	{ MBOX_LOCK_LOCKF, "lockf", mbox_lock_lockf },
	{ 0, NULL, NULL }
};

static int lock_settings_initialized = FALSE;
static enum mbox_lock_type read_locks[MBOX_LOCK_COUNT+1];
static enum mbox_lock_type write_locks[MBOX_LOCK_COUNT+1];
static int lock_timeout, dotlock_change_timeout;

static int mbox_lock_list(struct mbox_lock_context *ctx, int lock_type,
			  time_t max_wait_time, int idx);
static int mbox_unlock_files(struct mbox_lock_context *ctx);

static void mbox_read_lock_methods(const char *str, const char *env,
				   enum mbox_lock_type *locks)
{
        enum mbox_lock_type type;
	const char *const *lock;
	int i, dest;

	for (lock = t_strsplit(str, " "), dest = 0; *lock != NULL; lock++) {
		for (type = 0; lock_data[type].name != NULL; type++) {
			if (strcasecmp(*lock, lock_data[type].name) == 0) {
				type = lock_data[type].type;
				break;
			}
		}
		if (lock_data[type].name == NULL)
			i_fatal("%s: Invalid value %s", env, *lock);
		if (lock_data[type].func == NULL) {
			i_fatal("%s: Support for lock type %s "
				"not compiled into binary", env, *lock);
		}

		for (i = 0; i < dest; i++) {
			if (locks[i] == type)
				i_fatal("%s: Duplicated value %s", env, *lock);
		}

		/* @UNSAFE */
		locks[dest++] = type;
	}
	locks[dest] = (enum mbox_lock_type)-1;
}

static void mbox_init_lock_settings(void)
{
	const char *str;
	int r, w;

	str = getenv("MBOX_READ_LOCKS");
	if (str == NULL) str = DEFAULT_READ_LOCK_METHODS;
	mbox_read_lock_methods(str, "MBOX_READ_LOCKS", read_locks);

	str = getenv("MBOX_WRITE_LOCKS");
	if (str == NULL) str = DEFAULT_WRITE_LOCK_METHODS;
	mbox_read_lock_methods(str, "MBOX_WRITE_LOCKS", write_locks);

	/* check that read/write list orders match. write_locks must contain
	   at least read_locks and possibly more. */
	for (r = w = 0; write_locks[w] != (enum mbox_lock_type)-1; w++) {
		if (read_locks[r] == (enum mbox_lock_type)-1)
			break;
		if (read_locks[r] == write_locks[w])
			r++;
	}
	if (read_locks[r] != (enum mbox_lock_type)-1) {
		i_fatal("mbox read/write lock list settings are invalid. "
			"Lock ordering must be the same with both, "
			"and write locks must contain all read locks "
			"(and possibly more)");
	}

	str = getenv("MBOX_LOCK_TIMEOUT");
	lock_timeout = str == NULL ? DEFAULT_LOCK_TIMEOUT : atoi(str);

	str = getenv("MBOX_DOTLOCK_CHANGE_TIMEOUT");
	dotlock_change_timeout = str == NULL ?
		DEFAULT_DOTLOCK_CHANGE_TIMEOUT : atoi(str);

        lock_settings_initialized = TRUE;
}

static int mbox_file_open_latest(struct mbox_lock_context *ctx, int lock_type)
{
	struct index_mailbox *ibox = ctx->ibox;
	struct stat st;

	if (ctx->checked_file || lock_type == F_UNLCK)
		return 0;

	if (ibox->mbox_fd != -1) {
		if (stat(ibox->path, &st) < 0) {
			mbox_set_syscall_error(ibox, "stat()");
			return -1;
		}

		if (st.st_ino != ibox->mbox_ino ||
		    !CMP_DEV_T(st.st_dev, ibox->mbox_dev))
			mbox_file_close(ibox);
	}

	if (ibox->mbox_fd == -1) {
		if (mbox_file_open(ibox) < 0)
			return -1;
	}

	ctx->checked_file = TRUE;
	return 0;
}

static int dotlock_callback(unsigned int secs_left, int stale, void *context)
{
        struct mbox_lock_context *ctx = context;
	int idx;

	if (stale && !ctx->dotlock_last_stale) {
		/* get next index we wish to try locking */
		for (idx = MBOX_LOCK_COUNT; idx > 0; idx--) {
			if (ctx->lock_status[idx-1])
				break;
		}

		if (mbox_lock_list(ctx, ctx->lock_type, 0, idx) <= 0) {
			/* we couldn't get fcntl/flock - it's really locked */
			ctx->dotlock_last_stale = TRUE;
			return FALSE;
		}
		(void)mbox_lock_list(ctx, F_UNLCK, 0, idx);
	}
	ctx->dotlock_last_stale = stale;

	index_storage_lock_notify(ctx->ibox, stale ?
				  MAILBOX_LOCK_NOTIFY_MAILBOX_OVERRIDE :
				  MAILBOX_LOCK_NOTIFY_MAILBOX_ABORT,
				  secs_left);
	return TRUE;
}

static int mbox_lock_dotlock(struct mbox_lock_context *ctx, int lock_type,
			     time_t max_wait_time __attr_unused__)
{
	struct index_mailbox *ibox = ctx->ibox;
	int ret;

	if (lock_type == F_UNLCK) {
		if (ibox->mbox_dotlock.ino == 0)
			return 1;

		if (file_unlock_dotlock(ibox->path, &ibox->mbox_dotlock) <= 0) {
			mbox_set_syscall_error(ibox, "file_unlock_dotlock()");
			ret = -1;
		}
                ibox->mbox_dotlock.ino = 0;
		return 1;
	}

	if (ibox->mbox_dotlock.ino != 0)
		return 1;

        ctx->dotlock_last_stale = -1;

	ret = file_lock_dotlock(ibox->path, NULL, FALSE, lock_timeout,
				dotlock_change_timeout, 0,
				dotlock_callback, ctx, &ibox->mbox_dotlock);

	if (ret < 0) {
		mbox_set_syscall_error(ibox, "file_lock_dotlock()");
		return -1;
	}
	if (ret == 0) {
		mail_storage_set_error(ibox->box.storage,
				       "Timeout while waiting for lock");
		return 0;
	}
	if (mbox_file_open_latest(ctx, lock_type) < 0)
		return -1;
	return 1;
}

#ifdef HAVE_FLOCK
static int mbox_lock_flock(struct mbox_lock_context *ctx, int lock_type,
			   time_t max_wait_time)
{
	time_t now, last_notify;

	if (mbox_file_open_latest(ctx, lock_type) < 0)
		return -1;

	if (lock_type == F_UNLCK && ctx->ibox->mbox_fd == -1)
		return 1;

	if (lock_type == F_WRLCK)
		lock_type = LOCK_EX;
	else if (lock_type == F_RDLCK)
		lock_type = LOCK_SH;
	else
		lock_type = LOCK_UN;

        last_notify = 0;
	while (flock(ctx->ibox->mbox_fd, lock_type | LOCK_NB) < 0) {
		if (errno != EWOULDBLOCK) {
			mbox_set_syscall_error(ctx->ibox, "flock()");
			return -1;
		}

		if (max_wait_time == 0)
			return 0;

		now = time(NULL);
		if (now >= max_wait_time)
			return 0;

		if (now != last_notify) {
			index_storage_lock_notify(ctx->ibox,
				MAILBOX_LOCK_NOTIFY_MAILBOX_ABORT,
				max_wait_time - now);
		}

		usleep(LOCK_RANDOM_USLEEP_TIME);
	}

	return 1;
}
#endif

#ifdef HAVE_LOCKF
static int mbox_lock_lockf(struct mbox_lock_context *ctx, int lock_type,
			   time_t max_wait_time)
{
	time_t now, last_notify;

	if (mbox_file_open_latest(ctx, lock_type) < 0)
		return -1;

	if (lock_type == F_UNLCK && ctx->ibox->mbox_fd == -1)
		return 1;

	if (lock_type != F_UNLCK)
		lock_type = F_TLOCK;
	else
		lock_type = F_ULOCK;

        last_notify = 0;
	while (lockf(ctx->ibox->mbox_fd, lock_type, 0) < 0) {
		if (errno != EAGAIN) {
			mbox_set_syscall_error(ctx->ibox, "lockf()");
			return -1;
		}

		if (max_wait_time == 0)
			return 0;

		now = time(NULL);
		if (now >= max_wait_time)
			return 0;

		if (now != last_notify) {
			index_storage_lock_notify(ctx->ibox,
				MAILBOX_LOCK_NOTIFY_MAILBOX_ABORT,
				max_wait_time - now);
		}

		usleep(LOCK_RANDOM_USLEEP_TIME);
	}

	return 1;
}
#endif

static int mbox_lock_fcntl(struct mbox_lock_context *ctx, int lock_type,
			   time_t max_wait_time)
{
	struct flock fl;
	time_t now;
	unsigned int next_alarm;
	int wait_type;

	if (mbox_file_open_latest(ctx, lock_type) < 0)
		return -1;

	if (lock_type == F_UNLCK && ctx->ibox->mbox_fd == -1)
		return 1;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = lock_type;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;

	if (max_wait_time == 0)
		wait_type = F_SETLK;
	else {
		wait_type = F_SETLKW;
		alarm(I_MIN(max_wait_time, 5));
	}

	while (fcntl(ctx->ibox->mbox_fd, wait_type, &fl) < 0) {
		if (errno != EINTR) {
			if (errno != EAGAIN && errno != EACCES)
				mbox_set_syscall_error(ctx->ibox, "fcntl()");
			alarm(0);
			return -1;
		}

		now = time(NULL);
		if (max_wait_time != 0 && now >= max_wait_time) {
			alarm(0);
			return 0;
		}

		/* notify locks once every 5 seconds.
		   try to use rounded values. */
		next_alarm = (max_wait_time - now) % 5;
		if (next_alarm == 0)
			next_alarm = 5;
		alarm(next_alarm);

		index_storage_lock_notify(ctx->ibox,
					  MAILBOX_LOCK_NOTIFY_MAILBOX_ABORT,
					  max_wait_time - now);
	}

	alarm(0);
	return 1;
}

static int mbox_lock_list(struct mbox_lock_context *ctx, int lock_type,
			  time_t max_wait_time, int idx)
{
	enum mbox_lock_type *lock_types;
        enum mbox_lock_type type;
	int i, ret = 0, lock_status;

	ctx->lock_type = lock_type;

	lock_types = lock_type == F_WRLCK ||
		(lock_type == F_UNLCK && ctx->ibox->mbox_lock_type == F_WRLCK) ?
		write_locks : read_locks;
	for (i = idx; lock_types[i] != (enum mbox_lock_type)-1; i++) {
		type = lock_types[i];
		lock_status = lock_type != F_UNLCK;

		if (ctx->lock_status[type] == lock_status)
			continue;
		ctx->lock_status[type] = lock_status;

		ret = lock_data[type].func(ctx, lock_type, max_wait_time);
		if (ret <= 0)
			break;
	}
	return ret;
}

static int mbox_update_locking(struct index_mailbox *ibox, int lock_type)
{
	struct mbox_lock_context ctx;
	time_t max_wait_time;
	int ret, i, drop_locks;

        index_storage_lock_notify_reset(ibox);

	if (!lock_settings_initialized)
                mbox_init_lock_settings();

	max_wait_time = time(NULL) + lock_timeout;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ibox = ibox;

	if (ibox->mbox_lock_type == F_WRLCK) {
		/* dropping to shared lock. first drop those that we
		   don't remove completely. */
		for (i = 0; i < MBOX_LOCK_COUNT; i++)
			ctx.lock_status[i] = 1;
		for (i = 0; read_locks[i] != (enum mbox_lock_type)-1; i++)
			ctx.lock_status[read_locks[i]] = 0;
		drop_locks = TRUE;
	} else {
		drop_locks = FALSE;
	}

	ibox->mbox_lock_type = lock_type;
	ret = mbox_lock_list(&ctx, lock_type, max_wait_time, 0);
	if (ret <= 0) {
		if (!drop_locks)
			(void)mbox_unlock_files(&ctx);
		if (ret == 0) {
			mail_storage_set_error(ibox->box.storage,
				"Timeout while waiting for lock");
		}
		return ret;
	}

	if (drop_locks) {
		/* dropping to shared lock: drop the locks that are only
		   in write list */
		memset(ctx.lock_status, 0, sizeof(ctx.lock_status));
		for (i = 0; write_locks[i] != (enum mbox_lock_type)-1; i++)
			ctx.lock_status[write_locks[i]] = 1;
		for (i = 0; read_locks[i] != (enum mbox_lock_type)-1; i++)
			ctx.lock_status[read_locks[i]] = 0;

		ibox->mbox_lock_type = F_WRLCK;
		(void)mbox_lock_list(&ctx, F_UNLCK, 0, 0);
		ibox->mbox_lock_type = F_RDLCK;
	}

	return 1;
}

int mbox_lock(struct index_mailbox *ibox, int lock_type,
	      unsigned int *lock_id_r)
{
	int ret;

	/* allow only unlock -> shared/exclusive or exclusive -> shared */
	i_assert(lock_type == F_RDLCK || lock_type == F_WRLCK);
	i_assert(lock_type == F_RDLCK || ibox->mbox_lock_type != F_RDLCK);

	if (ibox->mbox_lock_type == F_UNLCK) {
		ret = mbox_update_locking(ibox, lock_type);
		if (ret <= 0)
			return ret;

		ibox->mbox_lock_id += 2;
	}

	if (lock_type == F_RDLCK) {
		ibox->mbox_shared_locks++;
		*lock_id_r = ibox->mbox_lock_id;
	} else {
		ibox->mbox_excl_locks++;
		*lock_id_r = ibox->mbox_lock_id + 1;
	}
	return 1;
}

static int mbox_unlock_files(struct mbox_lock_context *ctx)
{
	int ret = 0;

	if (mbox_lock_list(ctx, F_UNLCK, 0, 0) < 0)
		ret = -1;

	/* make sure we don't keep mmap() between locks */
	mbox_file_close_stream(ctx->ibox);

	ctx->ibox->mbox_lock_id += 2;
	ctx->ibox->mbox_lock_type = F_UNLCK;
	return ret;
}

int mbox_unlock(struct index_mailbox *ibox, unsigned int lock_id)
{
	struct mbox_lock_context ctx;
	int i;

	i_assert(ibox->mbox_lock_id == (lock_id & ~1));

	if (lock_id & 1) {
		/* dropping exclusive lock */
		i_assert(ibox->mbox_excl_locks > 0);
		if (--ibox->mbox_excl_locks > 0)
			return 0;
		if (ibox->mbox_shared_locks > 0) {
			/* drop to shared lock */
			if (mbox_update_locking(ibox, F_RDLCK) < 0)
				return -1;
			return 0;
		}
	} else {
		/* dropping shared lock */
		i_assert(ibox->mbox_shared_locks > 0);
		if (--ibox->mbox_shared_locks > 0)
			return 0;
		if (ibox->mbox_excl_locks > 0)
			return 0;
	}
	/* all locks gone */

	memset(&ctx, 0, sizeof(ctx));
	ctx.ibox = ibox;

	for (i = 0; i < MBOX_LOCK_COUNT; i++)
		ctx.lock_status[i] = 1;

	return mbox_unlock_files(&ctx);
}
