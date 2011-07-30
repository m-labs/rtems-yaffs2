/*
 * YAFFS port to RTEMS
 *
 * Copyright (C) 2010, 2011 Sebastien Bourdeauducq
 * Copyright (C) 2011 Stephan Hoffmann <sho@reLinux.de>
 * Copyright (C) 2011 embedded brains GmbH <rtems@embedded-brains.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * As a special exception, linking other files with the object code from
 * this one to produce an executable application does not by itself cause
 * the resulting executable application to be covered by the GNU General
 * Public License.
 * This exception does not however invalidate any other reasons why the
 * executable file might be covered by the GNU Public License. In particular,
 * the other YAFFS files are not covered by this exception, and using them
 * in a proprietary application requires a paid license from Aleph One.
 */

#include <rtems.h>
#include <rtems/libio.h>
#include <rtems/seterr.h>
#include <rtems/userenv.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <dirent.h>

#include "yportenv.h"

#include "yaffs_guts.h"
#include "yaffs_trace.h"
#include "yaffs_packedtags2.h"

#include "rtems_yaffs.h"
/* locking */

static void ylock(struct yaffs_dev *dev)
{
	rtems_yaffs_os_context *os_context = dev->os_context;
	(*os_context->lock)(dev, os_context);
}

static void yunlock(struct yaffs_dev *dev)
{
	rtems_yaffs_os_context *os_context = dev->os_context;
	(*os_context->unlock)(dev, os_context);
}

static void rtems_yaffs_os_unmount(struct yaffs_dev *dev)
{
	rtems_yaffs_os_context *os_context = dev->os_context;
	(*os_context->unmount)(dev, os_context);
}

/* Helper functions */

static int is_path_divider(YCHAR ch)
{
	const YCHAR *str = YAFFS_PATH_DIVIDERS;

	while(*str){
		if(*str == ch)
			return 1;
		str++;
	}

	return 0;
}

static struct yaffs_obj *h_find_object(struct yaffs_dev *dev, struct yaffs_obj *dir, const char *pathname, const char **out_of_fs);

static struct yaffs_obj *h_follow_link(struct yaffs_obj *obj, const char **out_of_fs)
{
	if(obj)
		obj = yaffs_get_equivalent_obj(obj);

	while(obj && obj->variant_type == YAFFS_OBJECT_TYPE_SYMLINK) {
		YCHAR *alias = obj->variant.symlink_variant.alias;

		if(is_path_divider(*alias))
			/* Starts with a /, need to scan from root up */
			obj = h_find_object(obj->my_dev, NULL, alias, out_of_fs);
		else
			/* Relative to here, so use the parent of the symlink as a start */
			obj = h_find_object(obj->my_dev, obj->parent, alias, out_of_fs);
	}
	return obj;
}


static struct yaffs_obj *h_find_object(struct yaffs_dev *dev, struct yaffs_obj *dir, const char *pathname, const char **out_of_fs)
{
	YCHAR str[YAFFS_MAX_NAME_LENGTH+1];
	int i;
	char sux[NAME_MAX];

	/* RTEMS sometimes calls eval_path with pathloc already pointing to the file to look for
	 * and name being the name of the file. Deal with this case.
	 * And no, I have no idea either.
	 */
	if(strchr(pathname, '/') == NULL) {
		yaffs_get_obj_name(dir, sux, NAME_MAX);
		if(strcmp(sux, pathname) == 0)
			return dir;
	}
	
	*out_of_fs = NULL;
	
	if(!dev->is_mounted)
		return NULL;
	
	if(dir == NULL)
		dir = dev->root_dir;
	
	while(dir) {
		/*
		 * parse off /.
		 * curve ball: also throw away surplus '/'
		 * eg. "/ram/x////ff" gets treated the same as "/ram/x/ff"
		 */
		while(is_path_divider(*pathname))
			pathname++; /* get rid of '/' */

		i = 0;
		str[0] = 0;

		while(*pathname && !is_path_divider(*pathname)) {
			if (i < YAFFS_MAX_NAME_LENGTH) {
				str[i] = *pathname;
				str[i+1] = '\0';
				i++;
			}
			pathname++;
		}

		if(strcmp(str, _Y(".")) == 0) {
			/* Do nothing */
		} else if(strcmp(str, _Y("..")) == 0) {
			if(dir->parent != NULL) 
				dir = dir->parent;
			else {
				while(is_path_divider(*pathname))
					pathname++; /* get rid of '/' */
				*out_of_fs = pathname;
				return NULL;
			}
		}
		else {
			if(str[0] != 0) {
				if(dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY)
					return NULL;
				dir = yaffs_find_by_name(dir, str);
			}
			dir = h_follow_link(dir, out_of_fs);
		}

		if(!*pathname)
			/* got to the end of the string */
			return dir;
	}
	
	return NULL;
}

/* RTEMS interface */

static const rtems_filesystem_file_handlers_r yaffs_directory_handlers;
static const rtems_filesystem_file_handlers_r yaffs_file_handlers;
static const rtems_filesystem_operations_table yaffs_ops;

//#define VERBOSE_DEBUG

static int ycb_eval_path(const char *pathname, size_t pathnamelen, int flags, rtems_filesystem_location_info_t *pathloc)
{
	const char *out_of_fs;
	struct yaffs_dev *dev;
	struct yaffs_obj *obj;

	dev = pathloc->mt_entry->fs_info;

	/* The RTEMS eval path system is not a good idea. */
	ylock(dev);
	obj = h_find_object(dev, pathloc->node_access, pathname, &out_of_fs);
	yunlock(dev);
	if(obj == NULL) {
		/* Really. It is not. */
		int extra;
		
		if(out_of_fs == NULL) {
#ifdef VERBOSE_DEBUG
			printf("newnode: ENOENT (%s)\n", pathname);
#endif
			rtems_set_errno_and_return_minus_one(ENOENT);
		}
#ifdef VERBOSE_DEBUG
		printf("newnode: out of fs (%s -> %s) we@%p callback@%p\n", pathname, out_of_fs, ycb_eval_path, pathloc->mt_entry->mt_point_node.ops->evalpath_h);
#endif
		*pathloc = pathloc->mt_entry->mt_point_node;
		extra = 0;
		while(is_path_divider(*(out_of_fs-1))) {
			out_of_fs--;
			extra++;
		}
		return pathloc->ops->evalpath_h(out_of_fs-2, pathnamelen - (out_of_fs - pathname) + 2 + extra, flags, pathloc);
	}
	pathloc->node_access = obj;
	pathloc->ops = &yaffs_ops;
	if(obj->variant_type == YAFFS_OBJECT_TYPE_DIRECTORY)
		pathloc->handlers = &yaffs_directory_handlers;
	else if(obj->variant_type == YAFFS_OBJECT_TYPE_FILE)
		pathloc->handlers = &yaffs_file_handlers;
	else {
		errno = ENOSYS;
		return -1;
	}
#ifdef VERBOSE_DEBUG
	printf("newnode: %p (path: %s, flags: %x)\n", pathloc->node_access, pathname, flags);
#endif
	return 0;
}

static int ycb_eval_path_for_make(const char *path, rtems_filesystem_location_info_t *pathloc, const char **name)
{
	char *s;
	char *path1, *path2;
	int r, i;

	path1 = strdup(path);
	if(path1 == NULL) {
		errno = ENOMEM;
		return -1;
	}
	i = strlen(path1) - 1;
	while((i >= 0) && (path1[i] == '/')) {
		path1[i] = '\0';
		i--;
	}

	s = strrchr(path1, '/');
	if(s == NULL) {
		*name = path;
		free(path1);
		return ycb_eval_path(".", strlen(path), 0, pathloc);
	}

	*name = path + (s - path1) + 1;

	path2 = strdup(path);
	if(path2 == NULL) {
		errno = ENOMEM;
		free(path1);
		return -1;
	}
	s = path2 + (s - path1);
	*s = 0;
	r = ycb_eval_path(path2, strlen(path2), 0, pathloc);
	free(path1);
	free(path2);

	if(r == 0) {
		if(pathloc->handlers != &yaffs_directory_handlers) {
			pathloc->ops->freenod_h(pathloc);
			errno = EINVAL;
			return -1;
		}
	}
	
	return r;
}

static int ycb_link(rtems_filesystem_location_info_t *to_loc, rtems_filesystem_location_info_t *parent_loc, const char *name)
{
	/* TODO */
	errno = ENOSYS;
	return -1;
}

static int ycb_dir_rmnod(rtems_filesystem_location_info_t *parent_loc, rtems_filesystem_location_info_t *pathloc);

static int ycb_unlink(rtems_filesystem_location_info_t *parent_pathloc, rtems_filesystem_location_info_t *pathloc)
{
	return ycb_dir_rmnod(parent_pathloc, pathloc);
}

static rtems_filesystem_node_types_t ycb_node_type(rtems_filesystem_location_info_t *pathloc)
{
	struct yaffs_obj *obj;
	
	obj = pathloc->node_access;
	switch(obj->variant_type) {
		case YAFFS_OBJECT_TYPE_FILE:
			return RTEMS_FILESYSTEM_MEMORY_FILE;
		case YAFFS_OBJECT_TYPE_SYMLINK:
			return RTEMS_FILESYSTEM_SYM_LINK;
		case YAFFS_OBJECT_TYPE_DIRECTORY:
			return RTEMS_FILESYSTEM_DIRECTORY;
		case YAFFS_OBJECT_TYPE_HARDLINK:
			return RTEMS_FILESYSTEM_HARD_LINK;
		case YAFFS_OBJECT_TYPE_SPECIAL:
			return RTEMS_FILESYSTEM_DEVICE;
		default: /* YAFFS_OBJECT_TYPE_UNKNOWN */
			errno = EINVAL;
			return -1;
	}
}

static int ycb_mknod(const char *path, mode_t mode, dev_t the_dev, rtems_filesystem_location_info_t *pathloc)
{
	char *name, *s;
	int ret = 0;

	struct yaffs_obj *parent = pathloc->node_access;
	struct yaffs_dev *dev = parent->my_dev;

	name = strdup(path);
	if(name == NULL) {
		errno = ENOMEM;
		return -1;
	}
	s = strchr(name, '/');
	if(s != NULL)
		*s = '\0';

	if(parent->my_dev->read_only) {
		errno = EROFS;
		free(name);
		return -1;
	}

	ylock(dev);

	if(yaffs_find_by_name(parent, name)) {
		errno = EEXIST;
		ret = -1;
		goto free;
	}

	if(S_ISDIR(mode)) {
		struct yaffs_obj *dir;
		
		dir = yaffs_create_dir(parent, name, mode, 0, 0);
		if(dir == NULL) {
			errno = ENOSPC; /* just assume no space */
			ret = -1;
			goto free;
		}
	} else if(S_ISREG(mode)) {
		struct yaffs_obj *file;
		
		file = yaffs_create_file(parent, name, mode, 0, 0);
		if(file == NULL) {
			errno = ENOSPC; /* just assume no space */
			ret = -1;
			goto free;
		}
	} else {
		printf("mknod of unsupported type\n");
		errno = ENOSYS;
		ret = -1;
		goto free;
	}
free:
	yunlock(dev);
	free(name);
	return ret;
}

static int ycb_chown(rtems_filesystem_location_info_t *pathloc, uid_t owner, gid_t group)
{
	/* not implemented */
	errno = 0;
	return 0;
}

static int ycb_freenod(rtems_filesystem_location_info_t *pathloc)
{
#ifdef VERBOSE_DEBUG
	printf("freenode: %p\n", pathloc->node_access);
#endif
	return 0;
}

static int ycb_mount(rtems_filesystem_mount_table_entry_t *mt_entry)
{
	/* not implemented */
	errno = ENOSYS;
	return -1;
}

static int ycb_unmount(rtems_filesystem_mount_table_entry_t *mt_entry)
{
	/* not implemented */
	errno = ENOSYS;
	return -1;
}

static int ycb_fsunmount(rtems_filesystem_mount_table_entry_t *mt_entry)
{
	struct yaffs_dev *dev = mt_entry->fs_info;

	ylock(dev);
	yaffs_flush_whole_cache(dev);
	yaffs_deinitialise(dev);
	yunlock(dev);
	rtems_yaffs_os_unmount(dev);

	return 0;
}

static int ycb_utime(rtems_filesystem_location_info_t *pathloc, time_t actime, time_t modtime)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;

	obj = pathloc->node_access;
	dev = obj->my_dev;
	
	ylock(dev);
	obj = yaffs_get_equivalent_obj(obj);
	if(obj != NULL) {
		obj->dirty = 1;
		obj->yst_atime = obj->yst_ctime = (u32)actime;
		obj->yst_mtime = (u32)modtime;
	}
	yunlock(dev);
	return 0;
}

static int ycb_evaluate_link(rtems_filesystem_location_info_t *pathloc, int flags)
{
	/* TODO */
	errno = ENOSYS;
	return -1;
}

static int ycb_symlink(rtems_filesystem_location_info_t *loc, const char *link_name, const char *node_name)
{
	/* TODO */
	errno = ENOSYS;
	return -1;
}

static ssize_t ycb_readlink(rtems_filesystem_location_info_t *loc, char *buf, size_t bufsize)
{
	/* TODO */
	errno = ENOSYS;
	return -1;
}

static int ycb_rename(rtems_filesystem_location_info_t *old_parent_loc, rtems_filesystem_location_info_t *old_loc, rtems_filesystem_location_info_t *new_parent_loc, const char *name)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	int r;
	char oldname[NAME_MAX];

	obj = old_loc->node_access;
	dev = obj->my_dev;

	if(obj->my_dev->read_only) {
		errno = EROFS;
		return -1;
	}

	ylock(dev);
	yaffs_get_obj_name(obj, oldname, NAME_MAX);
	r = yaffs_rename_obj(obj->parent, oldname, new_parent_loc->node_access, name);
	if(r == YAFFS_FAIL) {
		yunlock(dev);
		errno = EIO;
		return -1;
	}
	yunlock(dev);
	return 0;
}

static int ycb_statvfs(rtems_filesystem_location_info_t *loc, struct statvfs *buf)
{
	/* TODO */
	printf("%s\n", __func__);
	errno = ENOSYS;
	return -1;
}

static int ycb_dir_open(rtems_libio_t *iop, const char *pathname, uint32_t flag, uint32_t mode)
{
	/* nothing to do */
	return 0;
}

static int ycb_dir_close(rtems_libio_t *iop)
{
	/* nothing to do */
	return 0;
}

static ssize_t ycb_dir_read(rtems_libio_t *iop, void *buffer, size_t count)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	struct dirent *de = (struct dirent *)buffer;
	size_t i;
	size_t maxcount;
	struct list_head *next;
	ssize_t readlen;
	
	obj = (struct yaffs_obj *)iop->pathinfo.node_access;
	dev = obj->my_dev;
	maxcount = count / sizeof(struct dirent);

	ylock(dev);
	
	if(iop->offset == 0) {
		if(list_empty(&obj->variant.dir_variant.children))
			iop->data1 = NULL;
		else
			iop->data1 = list_entry(obj->variant.dir_variant.children.next, struct yaffs_obj, siblings);
	}
	
	i = 0;
	while((i < maxcount) && (iop->data1 != NULL)) {
		de[i].d_ino = (long)yaffs_get_equivalent_obj((struct yaffs_obj *)iop->data1)->obj_id;
		de[i].d_off = 0;
		yaffs_get_obj_name((struct yaffs_obj *)iop->data1, de[i].d_name, NAME_MAX);
		de[i].d_reclen = sizeof(struct dirent);
		de[i].d_namlen = (unsigned short)strnlen(de[i].d_name, NAME_MAX);
		
		i++;
		next = ((struct yaffs_obj *)iop->data1)->siblings.next;
		if(next == &obj->variant.dir_variant.children)
			iop->data1 = NULL; /* end of list */
		else
			iop->data1 = list_entry(next, struct yaffs_obj, siblings);
	}
	
	readlen = (ssize_t)(i * sizeof(struct dirent));
	iop->offset = iop->offset + readlen;

	yunlock(dev);
	
	return readlen;
}

static rtems_off64_t ycb_dir_lseek(rtems_libio_t *iop, rtems_off64_t length, int whence)
{
	/* we don't support anything else than rewinding */
	if((whence != SEEK_SET) || (length != 0))
		rtems_set_errno_and_return_minus_one(ENOTSUP);
	iop->offset = 0;
	return 0;
}

static int ycb_fstat(rtems_filesystem_location_info_t *loc, struct stat *buf)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	
	obj = (struct yaffs_obj *)loc->node_access;
	dev = (struct yaffs_dev *)obj->my_dev;

	ylock(dev);
	
	obj = yaffs_get_equivalent_obj(obj);

	buf->st_ino = obj->obj_id;
	buf->st_mode = obj->yst_mode & ~(unsigned)S_IFMT; /* clear out file type bits */

	if(obj->variant_type == YAFFS_OBJECT_TYPE_DIRECTORY)
		buf->st_mode |= S_IFDIR;
	else if(obj->variant_type == YAFFS_OBJECT_TYPE_SYMLINK)
		buf->st_mode |= S_IFLNK;
	else if(obj->variant_type == YAFFS_OBJECT_TYPE_FILE)
		buf->st_mode |= S_IFREG;

	buf->st_rdev = 0ll;
	buf->st_nlink = (nlink_t)yaffs_get_obj_link_count(obj);
	buf->st_uid = 0;
	buf->st_gid = 0;
	buf->st_rdev = obj->yst_rdev;
	buf->st_size = yaffs_get_obj_length(obj);
	buf->st_blksize = obj->my_dev->data_bytes_per_chunk;
	buf->st_blocks = (blkcnt_t)((buf->st_size + buf->st_blksize -1)/buf->st_blksize);
	buf->st_atime = (time_t)obj->yst_atime;
	buf->st_ctime = (time_t)obj->yst_ctime;
	buf->st_mtime = (time_t)obj->yst_mtime;

	yunlock(dev);
	
	return 0;
}

static int ycb_fchmod(rtems_filesystem_location_info_t *loc, mode_t mode)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	int result;

	if(mode & ~(0777u)){
		errno = EINVAL;
		return -1;
	}
	
	obj = loc->node_access;
	if(obj->my_dev->read_only) {
		errno = EROFS;
		return -1;
	}

	dev = obj->my_dev;

	ylock(dev);

	obj = yaffs_get_equivalent_obj(obj);

	result = YAFFS_FAIL;
	if(obj) {
		obj->yst_mode = (obj->yst_mode & ~0777u) | mode;
		obj->dirty = 1;
		result = yaffs_flush_file(obj, 0, 0);
	}
	if(result != YAFFS_OK) {
		yunlock(dev);
		errno = EIO;
		return -1;
	}
	yunlock(dev);
	return 0;
}

static int ycb_fdatasync(rtems_libio_t *iop)
{
	return 0;
}

static int ycb_dir_rmnod(rtems_filesystem_location_info_t *parent_loc, rtems_filesystem_location_info_t *pathloc)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	int r;

	obj = pathloc->node_access;
	if(obj->my_dev->read_only) {
		errno = EROFS;
		return -1;
	}
	dev = obj->my_dev;
	ylock(dev);
	r = yaffs_del_obj(obj);
	yunlock(dev);
	if(r == YAFFS_FAIL) {
		errno = ENOTEMPTY;
		return -1;
	}
	return 0;
}

static int ycb_file_open(rtems_libio_t *iop, const char *pathname, uint32_t flag, uint32_t mode)
{
	/* nothing to do */
	return 0;
}

static int ycb_file_close(rtems_libio_t *iop)
{
	/* nothing to do */
	return 0;
}

static ssize_t ycb_file_read(rtems_libio_t *iop, void *buffer, size_t count)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	ssize_t nr;
	int ol;
	size_t maxread;

	obj = iop->pathinfo.node_access;
	dev = obj->my_dev;

	ylock(dev);
	
	ol = yaffs_get_obj_length(obj);
	if(iop->offset >= ol)
		maxread = 0;
	else
		maxread = (size_t)(ol - (int)iop->offset);
	if(count > maxread)
		count = maxread;
	
	nr = yaffs_file_rd(obj, buffer, iop->offset, (int)count);

	yunlock(dev);
	
	if(nr < 0) {
		errno = ENOSPC;
		return -1;
	}
	return nr;
}

static ssize_t ycb_file_write(rtems_libio_t *iop, const void *buffer, size_t count)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	ssize_t nw;

	obj = iop->pathinfo.node_access;
	if(obj->my_dev->read_only) {
		errno = EROFS;
		return -1;
	}
	dev = obj->my_dev;
	ylock(dev);
	nw = yaffs_wr_file(obj, buffer, iop->offset, (int)count, 0);
	yunlock(dev);
	if(nw < 0) {
		errno = ENOSPC;
		return -1;
	}
	return nw;
}

static rtems_off64_t ycb_file_lseek(rtems_libio_t *iop, rtems_off64_t length, int whence)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;

	obj = iop->pathinfo.node_access;
	switch(whence) {
		case SEEK_SET:
			iop->offset = length;
			break;
		case SEEK_CUR:
			iop->offset += length;
			break;
		case SEEK_END:
			dev = obj->my_dev;
			ylock(dev);
			iop->offset = yaffs_get_obj_length(obj) + length;
			yunlock(dev);
			break;
		default:
			errno = EINVAL;
			return -1;
	}
	return iop->offset;
}

int ycb_file_ftruncate(rtems_libio_t *iop, rtems_off64_t length)
{
	struct yaffs_obj *obj;
	struct yaffs_dev *dev;
	int r;

	obj = iop->pathinfo.node_access;
	dev = obj->my_dev;
	ylock(dev);
	r = yaffs_resize_file(obj, length);
	yunlock(dev);
	if(r == YAFFS_FAIL) {
		errno = EIO;
		return -1;
	}
	iop->size = length;
	return 0;
}

int rtems_yaffs_mount_handler(rtems_filesystem_mount_table_entry_t *mt_entry, const void *data)
{
	const rtems_yaffs_mount_data *mount_data = data;
	struct yaffs_dev *dev = mount_data->dev;

	ylock(dev);
	if (yaffs_guts_initialise(dev) == YAFFS_FAIL) {
		yunlock(dev);
		errno = ENOMEM;
		return -1;
	}

	mt_entry->mt_fs_root.node_access = dev->root_dir;
	mt_entry->mt_fs_root.handlers = &yaffs_directory_handlers;
	mt_entry->mt_fs_root.ops = &yaffs_ops;
	mt_entry->fs_info = dev;

	yaffs_flush_whole_cache(dev);
	yunlock(dev);

	return 0;
}

static const rtems_filesystem_file_handlers_r yaffs_directory_handlers = {
	.open_h = ycb_dir_open,
	.close_h = ycb_dir_close,
	.read_h = ycb_dir_read,
	.write_h = rtems_filesystem_default_write,	     /* write */
	.ioctl_h = rtems_filesystem_default_ioctl,	     /* ioctl */
	.lseek_h = ycb_dir_lseek,
	.fstat_h = ycb_fstat,
	.fchmod_h = ycb_fchmod,
	.ftruncate_h = rtems_filesystem_default_ftruncate,	     /* ftruncate */
	.fpathconf_h = rtems_filesystem_default_fpathconf,	     /* fpathconf */
	.fsync_h = ycb_fdatasync,    /* fsync */
	.fdatasync_h = ycb_fdatasync,
	.fcntl_h = rtems_filesystem_default_fcntl,	     /* fcntl */
	.rmnod_h = ycb_dir_rmnod
};

static const rtems_filesystem_file_handlers_r yaffs_file_handlers = {
	.open_h = ycb_file_open,
	.close_h = ycb_file_close,
	.read_h = ycb_file_read,
	.write_h = ycb_file_write,
	.ioctl_h = rtems_filesystem_default_ioctl,	     /* ioctl */
	.lseek_h = ycb_file_lseek,
	.fstat_h = ycb_fstat,
	.fchmod_h = rtems_filesystem_default_fchmod,	     /* fchmod */
	.ftruncate_h = ycb_file_ftruncate,
	.fpathconf_h = rtems_filesystem_default_fpathconf,	     /* fpathconf */
	.fsync_h = ycb_fdatasync,    /* fsync */
	.fdatasync_h = ycb_fdatasync,
	.fcntl_h = rtems_filesystem_default_fcntl,	     /* fcntl */
	.rmnod_h = rtems_filesystem_default_rmnod	      /* rmnod */
};

static const rtems_filesystem_operations_table yaffs_ops = {
	.evalpath_h = ycb_eval_path,
	.evalformake_h = ycb_eval_path_for_make,
	.link_h = ycb_link,
	.unlink_h = ycb_unlink,
	.node_type_h = ycb_node_type,
	.mknod_h = ycb_mknod,
	.chown_h = ycb_chown,
	.freenod_h = ycb_freenod,
	.mount_h = ycb_mount,
	.fsmount_me_h = rtems_yaffs_mount_handler,
	.unmount_h = ycb_unmount,
	.fsunmount_me_h = ycb_fsunmount,
	.utime_h = ycb_utime,
	.eval_link_h = ycb_evaluate_link,
	.symlink_h = ycb_symlink,
	.readlink_h = ycb_readlink,
	.rename_h = ycb_rename,
	.statvfs_h = NULL
};

/* Yeah, who thought writing filesystem glue code could ever be so complicated? */
