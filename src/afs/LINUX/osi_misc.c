/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 * 
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/*
 * Linux support routines.
 *
 */
#include <afsconfig.h>
#include "afs/param.h"


#include <linux/module.h> /* early to avoid printf->printk mapping */
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/kthread.h>
#include "afs/sysincludes.h"
#include "afsincludes.h"
#include "afs/afs_stats.h"

#include "osi_compat.h"

int afs_osicred_initialized = 0;
afs_ucred_t afs_osi_cred;

void
osi_linux_mask(void)
{
    SIG_LOCK(current);
    sigfillset(&current->blocked);
    RECALC_SIGPENDING(current);
    SIG_UNLOCK(current);
}

void
osi_linux_unmaskrxk(void)
{
    extern struct task_struct *rxk_ListenerTask;
    /* Note this unblocks signals on the rxk_Listener
     * thread from a thread that is stopping rxk */
    SIG_LOCK(rxk_ListenerTask);
    sigdelset(&rxk_ListenerTask->blocked, SIGKILL);
    SIG_UNLOCK(rxk_ListenerTask);
}

/* LOOKUP_POSITIVE is becoming the default */
#ifndef LOOKUP_POSITIVE
#define LOOKUP_POSITIVE 0
#endif
/* Lookup name and return vnode for same. */
int
osi_lookupname_internal(char *aname, int followlink, struct vfsmount **mnt,
			struct dentry **dpp)
{
    int code;
#if defined(HAVE_LINUX_PATH_LOOKUP)
    struct nameidata path_data;
#else
    afs_linux_path_t path_data;
#endif
    int flags = LOOKUP_POSITIVE;

    if (followlink)
       flags |= LOOKUP_FOLLOW;
    code = afs_kern_path(aname, flags, &path_data);

    if (!code)
	afs_get_dentry_ref(&path_data, mnt, dpp);

    return code;
}

static char *
afs_getname(char *aname)
{
    int len;
    char *name = kmem_cache_alloc(names_cachep, GFP_KERNEL);

    if (!name)
	return ERR_PTR(-ENOMEM);

    len = strncpy_from_user(name, aname, PATH_MAX);
    if (len < 0)
	goto error;
    if (len >= PATH_MAX) {
	len = -ENAMETOOLONG;
	goto error;
    }
    return name;

error:
    kmem_cache_free(names_cachep, name);
    return ERR_PTR(len);
}

static void
afs_putname(char *name)
{
    kmem_cache_free(names_cachep, name);
}

int
osi_lookupname(char *aname, uio_seg_t seg, int followlink,
	       struct dentry **dpp)
{
    int code;
    char *name;

    if (seg == AFS_UIOUSER) {
	name = afs_getname(aname);
	if (IS_ERR(name))
	    return -PTR_ERR(name);
    } else {
	name = aname;
    }
    code = osi_lookupname_internal(name, followlink, NULL, dpp);
    if (seg == AFS_UIOUSER) {
	afs_putname(name);
    }
    return code;
}

int osi_abspath(char *aname, char *buf, int buflen,
		int followlink, char **pathp)
{
    struct dentry *dp = NULL;
    struct vfsmount *mnt = NULL;
    char *name, *path;
    int code;

    name = afs_getname(aname);
    if (IS_ERR(name))
	return -PTR_ERR(name);
    code = osi_lookupname_internal(name, followlink, &mnt, &dp);
    if (!code) {
	path = afs_d_path(dp, mnt, buf, buflen);
	if (IS_ERR(path)) {
	    code = -PTR_ERR(path);
	} else {
	    *pathp = path;
	}

	dput(dp);
	mntput(mnt);
    }

    afs_putname(name);
    return code;
}


/* This could use some work, and support on more platforms. */
static int
afs_thread_wrapper(void *rock)
{
    void (*proc)(void) = rock;
    __module_get(THIS_MODULE);
    (*proc)();
    module_put(THIS_MODULE);
    return 0;
}

static int
afs_thread_wrapper_glock(void *rock)
{
    void (*proc)(void) = rock;
    __module_get(THIS_MODULE);
    AFS_GLOCK();
    (*proc)();
    AFS_GUNLOCK();
    module_put(THIS_MODULE);
    return 0;
}

void
afs_start_thread(void (*proc)(void), char *name, int needs_glock)
{
    if (needs_glock) {
	kthread_run(afs_thread_wrapper_glock, proc, "%s", name);
    } else {
	kthread_run(afs_thread_wrapper, proc, "%s", name);
    }
}
