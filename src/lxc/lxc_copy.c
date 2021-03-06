/*
 *
 * Copyright © 2015 Christian Brauner <christian.brauner@mailbox.org>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include "config.h"

#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>

#include <lxc/lxccontainer.h>

#include "attach.h"
#include "log.h"
#include "confile.h"
#include "arguments.h"
#include "lxc.h"
#include "conf.h"
#include "state.h"
#include "utils.h"
#include "bdev/bdev.h"

#ifndef HAVE_GETSUBOPT
#include <../include/getsubopt.h>
#endif

lxc_log_define(lxc_copy_ui, lxc);

enum mnttype {
	LXC_MNT_BIND,
	LXC_MNT_AUFS,
	LXC_MNT_OVL,
};

struct mnts {
	enum mnttype mnt_type;
	char *src;
	char *dest;
	char *options;
	char *upper;
	char *workdir;
	char *lower;
};

static unsigned int mnt_table_size = 0;
static struct mnts *mnt_table = NULL;

static int my_parser(struct lxc_arguments *args, int c, char *arg);

static const struct option my_longopts[] = {
	{ "newname", required_argument, 0, 'N'},
	{ "newpath", required_argument, 0, 'p'},
	{ "rename", no_argument, 0, 'R'},
	{ "snapshot", no_argument, 0, 's'},
	{ "foreground", no_argument, 0, 'F'},
	{ "daemon", no_argument, 0, 'd'},
	{ "ephemeral", no_argument, 0, 'e'},
	{ "mount", required_argument, 0, 'm'},
	{ "backingstore", required_argument, 0, 'B'},
	{ "fssize", required_argument, 0, 'L'},
	{ "keepdata", no_argument, 0, 'D'},
	{ "keepname", no_argument, 0, 'K'},
	{ "keepmac", no_argument, 0, 'M'},
	LXC_COMMON_OPTIONS
};

/* mount keys */
static char *const keys[] = {
	[LXC_MNT_BIND] = "bind",
	[LXC_MNT_AUFS] = "aufs",
	[LXC_MNT_OVL] = "overlay",
	NULL
};

static struct lxc_arguments my_args = {
	.progname = "lxc-copy",
	.help = "\n\
--name=NAME [-P lxcpath] -N newname [-p newpath] [-B backingstorage] [-s] [-K] [-M] [-L size [unit]]\n\
--name=NAME [-P lxcpath] [-N newname] [-p newpath] [-B backingstorage] -e [-d] [-D] [-K] [-M] [-m {bind,aufs,overlay}=/src:/dest]\n\
--name=NAME [-P lxcpath] -N newname -R\n\
\n\
lxc-copy clone a container\n\
\n\
Options :\n\
  -n, --name=NAME           NAME of the container\n\
  -N, --newname=NEWNAME     NEWNAME for the restored container\n\
  -p, --newpath=NEWPATH     NEWPATH for the container to be stored\n\
  -R, --rename		    rename container\n\
  -s, --snapshot	    create snapshot instead of clone\n\
  -F, --foreground	    start with current tty attached to /dev/console\n\
  -d, --daemon		    daemonize the container (default)\n\
  -e, --ephemeral	    start ephemeral container\n\
  -m, --mount	            directory to mount into container, either \n\
			    {bind,aufs,overlay}=/src-path or {bind,aufs,overlay}=/src-path:/dst-path\n\
  -B, --backingstorage=TYPE backingstorage type for the container\n\
  -L, --fssize		    size of the new block device for block device containers\n\
  -D, --keedata	            pass together with -e start a persistent snapshot \n\
  -K, --keepname	    keep the hostname of the original container\n\
  -M, --keepmac		    keep the MAC address of the original container\n",
	.options = my_longopts,
	.parser = my_parser,
	.task = CLONE,
	.daemonize = 1,
};

static struct mnts *add_mnt(struct mnts **mnts, unsigned int *num,
			    enum mnttype type);
static int mk_rand_ovl_dirs(struct mnts *mnts, unsigned int num,
			    struct lxc_arguments *arg);
static char *construct_path(char *path, bool as_prefix);
static char *set_mnt_entry(struct mnts *m);
static int do_clone(struct lxc_container *c, char *newname, char *newpath,
		    int flags, char *bdevtype, uint64_t fssize, enum task task,
		    char **args);
static int do_clone_ephemeral(struct lxc_container *c,
			      struct lxc_arguments *arg, char **args,
			      int flags);
static int do_clone_rename(struct lxc_container *c, char *newname);
static int do_clone_task(struct lxc_container *c, enum task task, int flags,
			 char **args);
static void free_mnts(struct mnts *m, unsigned int num);
static uint64_t get_fssize(char *s);
static int parse_mntsubopts(char *subopts, char *const *keys,
			    char *mntparameters);
static int parse_aufs_mnt(char *mntstring, enum mnttype type);
static int parse_bind_mnt(char *mntstring, enum mnttype type);
static int parse_ovl_mnt(char *mntstring, enum mnttype type);

int main(int argc, char *argv[])
{
	struct lxc_container *c;
	int flags = 0;
	int ret;

	if (lxc_arguments_parse(&my_args, argc, argv))
		exit(EXIT_FAILURE);

	if (!my_args.log_file)
		my_args.log_file = "none";

	if (lxc_log_init(my_args.name, my_args.log_file, my_args.log_priority,
			 my_args.progname, my_args.quiet, my_args.lxcpath[0]))
		exit(EXIT_FAILURE);
	lxc_log_options_no_override();

	if (geteuid()) {
		if (access(my_args.lxcpath[0], O_RDWR) < 0) {
			fprintf(stderr, "You lack access to %s\n", my_args.lxcpath[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (!my_args.newname && !(my_args.task == DESTROY)) {
		printf("Error: You must provide a NEWNAME for the clone.\n");
		exit(EXIT_FAILURE);
	}

	if (my_args.task == SNAP || my_args.task == DESTROY)
		flags |= LXC_CLONE_SNAPSHOT;
	if (my_args.keepname)
		flags |= LXC_CLONE_KEEPNAME;
	if (my_args.keepmac)
		flags |= LXC_CLONE_KEEPMACADDR;

	if (!my_args.newpath)
		my_args.newpath = (char *)my_args.lxcpath[0];

	c = lxc_container_new(my_args.name, my_args.lxcpath[0]);
	if (!c)
		exit(EXIT_FAILURE);

	if (!c->may_control(c)) {
		fprintf(stderr, "Insufficent privileges to control %s\n",
			c->name);
		lxc_container_put(c);
		exit(EXIT_FAILURE);
	}

	if (!c->is_defined(c)) {
		fprintf(stderr, "Error: container %s is not defined\n",
			c->name);
		lxc_container_put(c);
		exit(EXIT_FAILURE);
	}

	ret = do_clone_task(c, my_args.task, flags, &argv[optind]);

	lxc_container_put(c);

	if (ret == 0)
		exit(EXIT_SUCCESS);
	exit(EXIT_FAILURE);
}

static struct mnts *add_mnt(struct mnts **mnts, unsigned int *num, enum mnttype type)
{
	struct mnts *m, *n;

	n = realloc(*mnts, (*num + 1) * sizeof(struct mnts));
	if (!n)
		return NULL;

	*mnts = n;
	m = *mnts + *num;
	(*num)++;

	*m = (struct mnts) {.mnt_type = type};

	return m;
}

static int mk_rand_ovl_dirs(struct mnts *mnts, unsigned int num, struct lxc_arguments *arg)
{
	char upperdir[MAXPATHLEN];
	char workdir[MAXPATHLEN];
	unsigned int i;
	int ret = 0;
	struct mnts *m;

	for (i = 0; i < num; i++) {
		m = mnts + i;
		if ((m->mnt_type == LXC_MNT_OVL) || (m->mnt_type == LXC_MNT_AUFS)) {
			ret = snprintf(upperdir, MAXPATHLEN, "%s/%s/delta#XXXXXX",
					arg->newpath, arg->newname);
			if (ret < 0 || ret >= MAXPATHLEN)
				goto err;
			if (!mkdtemp(upperdir))
				goto err;
			m->upper = strdup(upperdir);
			if (!m->upper)
				goto err;
		}

		if (m->mnt_type == LXC_MNT_OVL) {
			ret = snprintf(workdir, MAXPATHLEN, "%s/%s/work#XXXXXX",
					arg->newpath, arg->newname);
			if (ret < 0 || ret >= MAXPATHLEN)
				goto err;
			if (!mkdtemp(workdir))
				goto err;
			m->workdir = strdup(workdir);
			if (!m->workdir)
				goto err;
		}
	}

	return 0;

err:
	free_mnts(mnt_table, mnt_table_size);
	return -1;
}

static char *construct_path(char *path, bool as_prefix)
{
	char **components = NULL;
	char *cleanpath = NULL;

	components = lxc_normalize_path(path);
	if (!components)
		return NULL;

	cleanpath = lxc_string_join("/", (const char **)components, as_prefix);
	lxc_free_array((void **)components, free);

	return cleanpath;
}

static char *set_mnt_entry(struct mnts *m)
{
	char *mntentry = NULL;
	int ret = 0;
	size_t len = 0;

	if (m->mnt_type == LXC_MNT_AUFS) {
		len = strlen("  aufs br==rw:=ro,xino=,create=dir") +
		      2 * strlen(m->src) + strlen(m->dest) + strlen(m->upper) +
		      strlen(m->workdir) + 1;

		mntentry = malloc(len);
		if (!mntentry)
			goto err;

		ret = snprintf(mntentry, len, "%s %s aufs br=%s=rw:%s=ro,xino=%s,create=dir",
			       m->src, m->dest, m->upper, m->src, m->workdir);
		if (ret < 0 || (size_t)ret >= len)
			goto err;
	} else if (m->mnt_type == LXC_MNT_OVL) {
		len = strlen("  overlay lowerdir=,upperdir=,workdir=,create=dir") +
		      2 * strlen(m->src) + strlen(m->dest) + strlen(m->upper) +
		      strlen(m->workdir) + 1;

		mntentry = malloc(len);
		if (!mntentry)
			goto err;

		ret = snprintf(mntentry, len, "%s %s overlay lowerdir=%s,upperdir=%s,workdir=%s,create=dir",
				m->src, m->dest, m->src, m->upper, m->workdir);
		if (ret < 0 || (size_t)ret >= len)
			goto err;
	} else if (m->mnt_type == LXC_MNT_BIND) {
		len = strlen("  none bind,optional,, 0 0") +
		      strlen(is_dir(m->src) ? "create=dir" : "create=file") +
		      strlen(m->src) + strlen(m->dest) + strlen(m->options) + 1;

		mntentry = malloc(len);
		if (!mntentry)
			goto err;

		ret = snprintf(mntentry, len, "%s %s none bind,optional,%s,%s 0 0",
				m->src,	m->dest, m->options,
				is_dir(m->src) ? "create=dir" : "create=file");
		if (ret < 0 || (size_t)ret >= len)
			goto err;
	}

	return mntentry;

err:
	free_mnts(mnt_table, mnt_table_size);
	free(mntentry);
	return NULL;
}

static int do_clone(struct lxc_container *c, char *newname, char *newpath,
		    int flags, char *bdevtype, uint64_t fssize, enum task task,
		    char **args)
{
	struct lxc_container *clone;

	clone = c->clone(c, newname, newpath, flags, bdevtype, NULL, fssize,
			 args);
	if (!clone) {
		fprintf(stderr, "clone failed\n");
		return -1;
	}

	INFO("Created container %s as %s of %s\n", newname,
	     task ? "snapshot" : "copy", c->name);

	lxc_container_put(clone);

	return 0;
}

static int do_clone_ephemeral(struct lxc_container *c,
			      struct lxc_arguments *arg, char **args,
			      int flags)
{
	char randname[MAXPATHLEN];
	unsigned int i;
	int ret = 0;
	bool bret = true;
	struct lxc_container *clone;
	struct mnts *n;

	lxc_attach_options_t attach_options = LXC_ATTACH_OPTIONS_DEFAULT;
	attach_options.env_policy = LXC_ATTACH_CLEAR_ENV;

	if (!arg->newname) {
		ret = snprintf(randname, MAXPATHLEN, "%s/%s_XXXXXX", arg->newpath, arg->name);
		if (ret < 0 || ret >= MAXPATHLEN)
			return -1;
		if (!mkdtemp(randname))
			return -1;
		if (chmod(randname, 0770) < 0)
			return -1;
		arg->newname = randname + strlen(arg->newpath) + 1;
	}

	clone = c->clone(c, arg->newname, arg->newpath, flags,
			 arg->bdevtype, NULL, arg->fssize, args);
	if (!clone)
		return -1;

	if (!arg->keepdata)
		if (!clone->set_config_item(clone, "lxc.ephemeral", "1"))
			goto err;

	/* allocate and create random upper- and workdirs for overlay mounts */
	if (mk_rand_ovl_dirs(mnt_table, mnt_table_size, arg) < 0)
		goto err;

	/* allocate and set mount entries */
	for (i = 0; i < mnt_table_size; i++) {
		char *mntentry = NULL;
		n = mnt_table + i;
		if ((mntentry = set_mnt_entry(n))) {
			bret = clone->set_config_item(clone, "lxc.mount.entry", mntentry);
			free(mntentry);
		}
		if (!mntentry || !bret)
			goto err;
	}

	if (!clone->save_config(clone, NULL))
		goto err;

	printf("Created %s as %s of %s\n", arg->name, "clone", arg->newname);

	if (!arg->daemonize && arg->argc) {
		clone->want_daemonize(clone, true);
		arg->daemonize = 1;
	} else if (!arg->daemonize) {
		clone->want_daemonize(clone, false);
	}

	if (!clone->start(clone, 0, NULL)) {
		if (!(clone->lxc_conf->ephemeral == 1))
			goto err;
		goto put;
	}

	if (arg->daemonize && arg->argc) {
		ret = clone->attach_run_wait(clone, &attach_options, arg->argv[0], (const char *const *)arg->argv);
		if (ret < 0)
			goto put;
		clone->shutdown(clone, true);
	}

	lxc_container_put(clone);
	return 0;

err:
	clone->destroy(clone);
put:
	free_mnts(mnt_table, mnt_table_size);
	lxc_container_put(clone);
	return -1;
}

static int do_clone_rename(struct lxc_container *c, char *newname)
{
	if (!c->rename(c, newname)) {
		ERROR("Error: Renaming container %s to %s failed\n", c->name, newname);
		return -1;
	}

	INFO("Renamed container %s to %s\n", c->name, newname);

	return 0;
}

static int do_clone_task(struct lxc_container *c, enum task task, int flags,
			 char **args)
{
	int ret = 0;

	switch (task) {
	case DESTROY:
		ret = do_clone_ephemeral(c, &my_args, args, flags);
		break;
	case RENAME:
		ret = do_clone_rename(c, my_args.newname);
		break;
	default:
		ret = do_clone(c, my_args.newname, my_args.newpath, flags,
			       my_args.bdevtype, my_args.fssize, my_args.task,
			       args);
		break;
	}

	return ret;
}

static void free_mnts(struct mnts *m, unsigned int num)
{
	unsigned int i;
	struct mnts *n;

	for (i = 0; i < num; i++) {
		n = m + i;
		free(n->src);
		free(n->dest);
		free(n->options);
		free(n->upper);
		free(n->workdir);
	}
	free(m);
}

/* we pass fssize in bytes */
static uint64_t get_fssize(char *s)
{
	uint64_t ret;
	char *end;

	ret = strtoull(s, &end, 0);
	if (end == s) {
		fprintf(stderr, "Invalid blockdev size '%s', using default size\n", s);
		return 0;
	}
	while (isblank(*end))
		end++;
	if (*end == '\0') {
		ret *= 1024ULL * 1024ULL; // MB by default
	} else if (*end == 'b' || *end == 'B') {
		ret *= 1ULL;
	} else if (*end == 'k' || *end == 'K') {
		ret *= 1024ULL;
	} else if (*end == 'm' || *end == 'M') {
		ret *= 1024ULL * 1024ULL;
	} else if (*end == 'g' || *end == 'G') {
		ret *= 1024ULL * 1024ULL * 1024ULL;
	} else if (*end == 't' || *end == 'T') {
		ret *= 1024ULL * 1024ULL * 1024ULL * 1024ULL;
	} else {
		fprintf(stderr, "Invalid blockdev unit size '%c' in '%s', " "using default size\n", *end, s);
		return 0;
	}

	return ret;
}

static int my_parser(struct lxc_arguments *args, int c, char *arg)
{
	char *subopts = NULL;
	char *mntparameters = NULL;
	switch (c) {
	case 'N':
		args->newname = arg;
		break;
	case 'p':
		args->newpath = arg;
		break;
	case 'R':
		args->task = RENAME;
		break;
	case 's':
		args->task = SNAP;
		break;
	case 'F':
		args->daemonize = 0;
		break;
	case 'd':
		args->daemonize = 1;
		break;
	case 'e':
		args->task = DESTROY;
		break;
	case 'm':
		subopts = optarg;
		if (parse_mntsubopts(subopts, keys, mntparameters) < 0)
			return -1;
		break;
	case 'B':
		args->bdevtype = arg;
		break;
	case 'L':
		args->fssize = get_fssize(optarg);
		break;
	case 'D':
		args->keepdata = 1;
		break;
	case 'K':
		args->keepname = 1;
		break;
	case 'M':
		args->keepmac = 1;
		break;
	}

	return 0;
}

static int parse_aufs_mnt(char *mntstring, enum mnttype type)
{
	int len = 0;
	const char *xinopath = "/dev/shm/aufs.xino";
	char **mntarray = NULL;
	struct mnts *m = NULL;

	m = add_mnt(&mnt_table, &mnt_table_size, type);
	if (!m)
		goto err;

	mntarray = lxc_string_split(mntstring, ':');
	if (!mntarray)
		goto err;

	m->src = construct_path(mntarray[0], true);
	if (!m->src)
		goto err;

	len = lxc_array_len((void **)mntarray);
	if (len == 1) /* aufs=src */
		m->dest = construct_path(mntarray[0], false);
	else if (len == 2) /* aufs=src:dest */
		m->dest = construct_path(mntarray[1], false);
	else
		INFO("Excess elements in mount specification");

	if (!m->dest)
		goto err;

	m->workdir = strdup(xinopath);
	if (!m->workdir)
		goto err;

	lxc_free_array((void **)mntarray, free);
	return 0;

err:
	free_mnts(mnt_table, mnt_table_size);
	lxc_free_array((void **)mntarray, free);
	return -1;
}

static int parse_bind_mnt(char *mntstring, enum mnttype type)
{
	int len = 0;
	char **mntarray = NULL;
	struct mnts *m = NULL;

	m = add_mnt(&mnt_table, &mnt_table_size, type);
	if (!m)
		goto err;

	mntarray = lxc_string_split(mntstring, ':');
	if (!mntarray)
		goto err;

	m->src = construct_path(mntarray[0], true);
	if (!m->src)
		goto err;

	len = lxc_array_len((void **)mntarray);
	if (len == 1) { /* bind=src */
		m->dest = construct_path(mntarray[0], false);
	} else if (len == 2) { /* bind=src:option or bind=src:dest */
		if (strncmp(mntarray[1], "rw", strlen(mntarray[1])) == 0)
			m->options = strdup("rw");

		if (strncmp(mntarray[1], "ro", strlen(mntarray[1])) == 0)
			m->options = strdup("ro");

		if (m->options)
			m->dest = construct_path(mntarray[0], false);
		else
			m->dest = construct_path(mntarray[1], false);
	} else if (len == 3) { /* bind=src:dest:option */
			m->dest = construct_path(mntarray[1], false);
			m->options = strdup(mntarray[2]);
	} else {
		INFO("Excess elements in mount specification");
	}

	if (!m->dest)
		goto err;

	if (!m->options)
		m->options = strdup("rw");

	if (!m->options || (strncmp(m->options, "rw", strlen(m->options)) &&
			    strncmp(m->options, "ro", strlen(m->options))))
		goto err;

	lxc_free_array((void **)mntarray, free);
	return 0;

err:
	free_mnts(mnt_table, mnt_table_size);
	lxc_free_array((void **)mntarray, free);
	return -1;
}

static int parse_mntsubopts(char *subopts, char *const *keys, char *mntparameters)
{
	while (*subopts != '\0') {
		switch (getsubopt(&subopts, keys, &mntparameters)) {
		case LXC_MNT_BIND:
			if (parse_bind_mnt(mntparameters, LXC_MNT_BIND) < 0)
				return -1;
			break;
		case LXC_MNT_OVL:
			if (parse_ovl_mnt(mntparameters, LXC_MNT_OVL) < 0)
				return -1;
			break;
		case LXC_MNT_AUFS:
			if (parse_aufs_mnt(mntparameters, LXC_MNT_AUFS) < 0)
				return -1;
			break;
		default:
			break;
		}
	}
	return 0;
}

static int parse_ovl_mnt(char *mntstring, enum mnttype type)
{
	int len = 0;
	char **mntarray = NULL;
	struct mnts *m;

	m = add_mnt(&mnt_table, &mnt_table_size, type);
	if (!m)
		goto err;

	mntarray = lxc_string_split(mntstring, ':');
	if (!mntarray)
		goto err;

	m->src = construct_path(mntarray[0], true);
	if (!m->src)
		goto err;

	len = lxc_array_len((void **)mntarray);
	if (len == 1) /* overlay=src */
		m->dest = construct_path(mntarray[0], false);
	else if (len == 2) /* overlay=src:dest */
		m->dest = construct_path(mntarray[1], false);
	else
		INFO("Excess elements in mount specification");

	if (!m->dest)
		goto err;

	lxc_free_array((void **)mntarray, free);
	return 0;

err:
	free_mnts(mnt_table, mnt_table_size);
	lxc_free_array((void **)mntarray, free);
	return -1;
}

