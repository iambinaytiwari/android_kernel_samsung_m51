// SPDX-License-Identifier: GPL-2.0
/*
 * f2fs sysfs interface
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 * Copyright (c) 2017 Chao Yu <chao@kernel.org>
 */
#include <linux/compiler.h>
#include <linux/proc_fs.h>
#include <linux/f2fs_fs.h>
#include <linux/seq_file.h>
#include <linux/unicode.h>
#include <linux/statfs.h>
#include <linux/nls.h>

#include "f2fs.h"
#include "segment.h"
#include "gc.h"
#include <trace/events/f2fs.h>

static struct proc_dir_entry *f2fs_proc_root;

/* Sysfs support for f2fs */
enum {
	GC_THREAD,	/* struct f2fs_gc_thread */
	SM_INFO,	/* struct f2fs_sm_info */
	DCC_INFO,	/* struct discard_cmd_control */
	NM_INFO,	/* struct f2fs_nm_info */
	F2FS_SBI,	/* struct f2fs_sb_info */
#ifdef CONFIG_F2FS_STAT_FS
	STAT_INFO,      /* struct f2fs_stat_info */
#endif
#ifdef CONFIG_F2FS_FAULT_INJECTION
	FAULT_INFO_RATE,	/* struct f2fs_fault_info */
	FAULT_INFO_TYPE,	/* struct f2fs_fault_info */
#endif
	RESERVED_BLOCKS,	/* struct f2fs_sb_info */
};

const char *sec_fua_mode_names[NR_F2FS_SEC_FUA_MODE] = {
	"NONE",
	"ROOT",
	"ALL",
};

struct f2fs_attr {
	struct attribute attr;
	ssize_t (*show)(struct f2fs_attr *, struct f2fs_sb_info *, char *);
	ssize_t (*store)(struct f2fs_attr *, struct f2fs_sb_info *,
			 const char *, size_t);
	int struct_type;
	int offset;
	int id;
};

static ssize_t f2fs_sbi_show(struct f2fs_attr *a,
			     struct f2fs_sb_info *sbi, char *buf);

static unsigned char *__struct_ptr(struct f2fs_sb_info *sbi, int struct_type)
{
	if (struct_type == GC_THREAD)
		return (unsigned char *)sbi->gc_thread;
	else if (struct_type == SM_INFO)
		return (unsigned char *)SM_I(sbi);
	else if (struct_type == DCC_INFO)
		return (unsigned char *)SM_I(sbi)->dcc_info;
	else if (struct_type == NM_INFO)
		return (unsigned char *)NM_I(sbi);
	else if (struct_type == F2FS_SBI || struct_type == RESERVED_BLOCKS)
		return (unsigned char *)sbi;
#ifdef CONFIG_F2FS_FAULT_INJECTION
	else if (struct_type == FAULT_INFO_RATE ||
					struct_type == FAULT_INFO_TYPE)
		return (unsigned char *)&F2FS_OPTION(sbi).fault_info;
#endif
#ifdef CONFIG_F2FS_STAT_FS
	else if (struct_type == STAT_INFO)
		return (unsigned char *)F2FS_STAT(sbi);
#endif
	return NULL;
}

static ssize_t dirty_segments_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu\n",
			(unsigned long long)(dirty_segments(sbi)));
}

static ssize_t free_segments_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu\n",
			(unsigned long long)(free_segments(sbi)));
}

static ssize_t lifetime_write_kbytes_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	struct super_block *sb = sbi->sb;

	if (!sb->s_bdev->bd_part)
		return sprintf(buf, "0\n");

	return sprintf(buf, "%llu\n",
			(unsigned long long)(sbi->kbytes_written +
			BD_PART_WRITTEN(sbi)));
}

static ssize_t sec_fs_stat_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	struct dentry *root = sbi->sb->s_root;
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct kstatfs statbuf;
	int ret;

	if (!root->d_sb->s_op->statfs)
		goto errout;

	ret = root->d_sb->s_op->statfs(root, &statbuf);
	if (ret)
		goto errout;

	return snprintf(buf, PAGE_SIZE, "\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%u\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%u\",\"%s\":\"%u\"\n",
		"F_BLOCKS", statbuf.f_blocks,
		"F_BFREE", statbuf.f_bfree,
		"F_SFREE", free_sections(sbi),
		"F_FILES", statbuf.f_files,
		"F_FFREE", statbuf.f_ffree,
		"F_FUSED", ckpt->valid_inode_count,
		"F_NUSED", ckpt->valid_node_count);

errout:
	return snprintf(buf, PAGE_SIZE, "\"%s\":\"%d\",\"%s\":\"%d\",\"%s\":\"%d\","
		"\"%s\":\"%d\",\"%s\":\"%d\",\"%s\":\"%d\",\"%s\":\"%d\"\n",
		"F_BLOCKS", 0, "F_BFREE", 0, "F_SFREE", 0, "F_FILES", 0,
		"F_FFREE", 0, "F_FUSED", 0, "F_NUSED", 0);
}

static ssize_t features_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	struct super_block *sb = sbi->sb;
	int len = 0;

	if (!sb->s_bdev->bd_part)
		return sprintf(buf, "0\n");

	if (f2fs_sb_has_encrypt(sbi))
		len += scnprintf(buf, PAGE_SIZE - len, "%s",
						"encryption");
	if (f2fs_sb_has_blkzoned(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "blkzoned");
	if (f2fs_sb_has_extra_attr(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "extra_attr");
	if (f2fs_sb_has_project_quota(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "projquota");
	if (f2fs_sb_has_inode_chksum(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "inode_checksum");
	if (f2fs_sb_has_flexible_inline_xattr(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "flexible_inline_xattr");
	if (f2fs_sb_has_quota_ino(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "quota_ino");
	if (f2fs_sb_has_inode_crtime(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "inode_crtime");
	if (f2fs_sb_has_lost_found(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "lost_found");
	if (f2fs_sb_has_verity(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "verity");
	if (f2fs_sb_has_sb_chksum(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "sb_checksum");
	if (f2fs_sb_has_casefold(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "casefold");
	if (f2fs_sb_has_compression(sbi))
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "compression");
	len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				len ? ", " : "", "pin_file");
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t current_reserved_blocks_show(struct f2fs_attr *a,
					struct f2fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%u\n", sbi->current_reserved_blocks);
}

static ssize_t unusable_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	block_t unusable;

	if (test_opt(sbi, DISABLE_CHECKPOINT))
		unusable = sbi->unusable_block_count;
	else
		unusable = f2fs_get_unusable_blocks(sbi);
	return sprintf(buf, "%llu\n", (unsigned long long)unusable);
}

static ssize_t encoding_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
#ifdef CONFIG_UNICODE
	struct super_block *sb = sbi->sb;

	if (f2fs_sb_has_casefold(sbi))
		return snprintf(buf, PAGE_SIZE, "%s (%d.%d.%d)\n",
			sb->s_encoding->charset,
			(sb->s_encoding->version >> 16) & 0xff,
			(sb->s_encoding->version >> 8) & 0xff,
			sb->s_encoding->version & 0xff);
#endif
	return sprintf(buf, "(none)");
}

static ssize_t mounted_time_sec_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	return sprintf(buf, "%llu", SIT_I(sbi)->mounted_time);
}

#ifdef CONFIG_F2FS_STAT_FS
static ssize_t moved_blocks_foreground_show(struct f2fs_attr *a,
				struct f2fs_sb_info *sbi, char *buf)
{
	struct f2fs_stat_info *si = F2FS_STAT(sbi);

	return sprintf(buf, "%llu\n",
		(unsigned long long)(si->tot_blks -
			(si->bg_data_blks + si->bg_node_blks)));
}

static ssize_t moved_blocks_background_show(struct f2fs_attr *a,
				struct f2fs_sb_info *sbi, char *buf)
{
	struct f2fs_stat_info *si = F2FS_STAT(sbi);

	return sprintf(buf, "%llu\n",
		(unsigned long long)(si->bg_data_blks + si->bg_node_blks));
}

static ssize_t avg_vblocks_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	struct f2fs_stat_info *si = F2FS_STAT(sbi);

	si->dirty_count = dirty_segments(sbi);
	f2fs_update_sit_info(sbi);
	return sprintf(buf, "%llu\n", (unsigned long long)(si->avg_vblocks));
}
#endif

static ssize_t f2fs_sbi_show(struct f2fs_attr *a,
			struct f2fs_sb_info *sbi, char *buf)
{
	unsigned char *ptr = NULL;
	unsigned int *ui;

	ptr = __struct_ptr(sbi, a->struct_type);
	if (!ptr)
		return -EINVAL;

	if (!strcmp(a->attr.name, "extension_list")) {
		__u8 (*extlist)[F2FS_EXTENSION_LEN] =
					sbi->raw_super->extension_list;
		int cold_count = le32_to_cpu(sbi->raw_super->extension_count);
		int hot_count = sbi->raw_super->hot_ext_count;
		int len = 0, i;

		len += scnprintf(buf + len, PAGE_SIZE - len,
						"cold file extension:\n");
		for (i = 0; i < cold_count; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n",
								extlist[i]);

		len += scnprintf(buf + len, PAGE_SIZE - len,
						"hot file extension:\n");
		for (i = cold_count; i < cold_count + hot_count; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n",
								extlist[i]);
		return len;
	} else if (!strcmp(a->attr.name, "sec_gc_stat")) {
		return snprintf(buf, PAGE_SIZE, "\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\"\n",
			"FGGC", sbi->sec_stat.gc_count[FG_GC],
			"FGGC_NSEG", sbi->sec_stat.gc_node_seg_count[FG_GC],
			"FGGC_NBLK", sbi->sec_stat.gc_node_blk_count[FG_GC],
			"FGGC_DSEG", sbi->sec_stat.gc_data_seg_count[FG_GC],
			"FGGC_DBLK", sbi->sec_stat.gc_data_blk_count[FG_GC],
			"FGGC_TTIME", sbi->sec_stat.gc_ttime[FG_GC],
			"BGGC", sbi->sec_stat.gc_count[BG_GC],
			"BGGC_NSEG", sbi->sec_stat.gc_node_seg_count[BG_GC],
			"BGGC_NBLK", sbi->sec_stat.gc_node_blk_count[BG_GC],
			"BGGC_DSEG", sbi->sec_stat.gc_data_seg_count[BG_GC],
			"BGGC_DBLK", sbi->sec_stat.gc_data_blk_count[BG_GC],
			"BGGC_TTIME", sbi->sec_stat.gc_ttime[BG_GC]);
	} else if (!strcmp(a->attr.name, "sec_io_stat")) {
		u64 kbytes_written = 0;

		if (sbi->sb->s_bdev->bd_part)
			kbytes_written = BD_PART_WRITTEN(sbi) -
					 sbi->sec_stat.kwritten_byte;

		return snprintf(buf, PAGE_SIZE, "\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\","
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%u\","
		"\"%s\":\"%u\",\"%s\":\"%u\"\n",
			"CP",		sbi->sec_stat.cp_cnt[STAT_CP_ALL],
			"CPBG",		sbi->sec_stat.cp_cnt[STAT_CP_BG],
			"CPSYNC",	sbi->sec_stat.cp_cnt[STAT_CP_FSYNC],
			"CPNONRE",	sbi->sec_stat.cpr_cnt[CP_NON_REGULAR],
			"CPSBNEED",	sbi->sec_stat.cpr_cnt[CP_SB_NEED_CP],
			"CPWPINO",	sbi->sec_stat.cpr_cnt[CP_WRONG_PINO],
			"CP_MAX_INT",	sbi->sec_stat.cp_max_interval,
			"LFSSEG",	sbi->sec_stat.alloc_seg_type[LFS],
			"SSRSEG",	sbi->sec_stat.alloc_seg_type[SSR],
			"LFSBLK",	sbi->sec_stat.alloc_blk_count[LFS],
			"SSRBLK",	sbi->sec_stat.alloc_blk_count[SSR],
			"IPU",		(u64)atomic64_read(&sbi->sec_stat.inplace_count),
			"FSYNC",	sbi->sec_stat.fsync_count,
			"FSYNC_MB",	sbi->sec_stat.fsync_dirty_pages >> 8,
			"HOT_DATA",	sbi->sec_stat.hot_file_written_blocks >> 8,
			"COLD_DATA",	sbi->sec_stat.cold_file_written_blocks >> 8,
			"WARM_DATA",	sbi->sec_stat.warm_file_written_blocks >> 8,
			"MAX_INMEM",	sbi->sec_stat.max_inmem_pages,
			"DROP_INMEM",	sbi->sec_stat.drop_inmem_all,
			"DROP_INMEMF",	sbi->sec_stat.drop_inmem_files,
			"WRITE_MB",	(u64)(kbytes_written >> 10),
			"FS_PERROR",	sbi->sec_stat.fs_por_error,
			"FS_ERROR",	sbi->sec_stat.fs_error,
			"MAX_UNDSCD",	sbi->sec_stat.max_undiscard_blks);
	} else if (!strcmp(a->attr.name, "sec_fsck_stat")) {
		return snprintf(buf, PAGE_SIZE,
		"\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%llu\",\"%s\":\"%u\","
		"\"%s\":\"%u\",\"%s\":\"%u\"\n",
			"FSCK_RBYTES",	sbi->sec_fsck_stat.fsck_read_bytes,
			"FSCK_WBYTES",	sbi->sec_fsck_stat.fsck_written_bytes,
			"FSCK_TIME_MS",	sbi->sec_fsck_stat.fsck_elapsed_time,
			"FSCK_EXIT",	sbi->sec_fsck_stat.fsck_exit_code,
			"FSCK_VNODES",	sbi->sec_fsck_stat.valid_node_count,
			"FSCK_VINODES",	sbi->sec_fsck_stat.valid_inode_count);
	} else if (!strcmp(a->attr.name, "sec_heimdallfs_stat")) {
		return snprintf(buf, PAGE_SIZE,
		"\"%s\":\"%u\",\"%s\":\"%llu\",\"%s\":\"%u\",\"%s\":\"%llu\",\"%s\":\"%llu\"\n",
			"NR_PKGS", sbi->sec_heimdallfs_stat.nr_pkgs,
			"NR_PKG_BLKS", sbi->sec_heimdallfs_stat.nr_pkg_blks,
			"NR_COMP_PKGS", sbi->sec_heimdallfs_stat.nr_comp_pkgs,
			"NR_COMP_PKG_BLKS", sbi->sec_heimdallfs_stat.nr_comp_pkg_blks,
			"NR_COMP_PKG_SAVED_BLKS", sbi->sec_heimdallfs_stat.nr_comp_saved_blks);
	} else if (!strcmp(a->attr.name, "sec_fua_mode")) {
		int len = 0, i;
		for (i = 0; i < NR_F2FS_SEC_FUA_MODE; i++) {
			if (i == sbi->s_sec_cond_fua_mode)
				len += snprintf(buf, PAGE_SIZE, "[%s] ",
						sec_fua_mode_names[i]);
			else
				len += snprintf(buf, PAGE_SIZE, "%s ",
						sec_fua_mode_names[i]);
		}
		return len;
 	}

	ui = (unsigned int *)(ptr + a->offset);

	return sprintf(buf, "%u\n", *ui);
}

static ssize_t __sbi_store(struct f2fs_attr *a,
			struct f2fs_sb_info *sbi,
			const char *buf, size_t count)
{
	unsigned char *ptr;
	unsigned long t;
	unsigned int *ui;
	ssize_t ret;
	unsigned int i = 0;

	ptr = __struct_ptr(sbi, a->struct_type);
	if (!ptr)
		return -EINVAL;

	if (!strcmp(a->attr.name, "extension_list")) {
		const char *name = strim((char *)buf);
		bool set = true, hot;

		if (!strncmp(name, "[h]", 3))
			hot = true;
		else if (!strncmp(name, "[c]", 3))
			hot = false;
		else
			return -EINVAL;

		name += 3;

		if (*name == '!') {
			name++;
			set = false;
		}

		if (strlen(name) >= F2FS_EXTENSION_LEN)
			return -EINVAL;

		down_write(&sbi->sb_lock);

		ret = f2fs_update_extension_list(sbi, name, hot, set);
		if (ret)
			goto out;

		ret = f2fs_commit_super(sbi, false);
		if (ret)
			f2fs_update_extension_list(sbi, name, hot, !set);
out:
		up_write(&sbi->sb_lock);
		return ret ? ret : count;
	} else if(!strcmp(a->attr.name, "sec_gc_stat")) {
			sbi->sec_stat.gc_count[BG_GC] = 0;
			sbi->sec_stat.gc_count[FG_GC] = 0;
			sbi->sec_stat.gc_node_seg_count[BG_GC] = 0;
			sbi->sec_stat.gc_node_seg_count[FG_GC] = 0;
			sbi->sec_stat.gc_data_seg_count[BG_GC] = 0;
			sbi->sec_stat.gc_data_seg_count[FG_GC] = 0;
			sbi->sec_stat.gc_node_blk_count[BG_GC] = 0;
			sbi->sec_stat.gc_node_blk_count[FG_GC] = 0;
			sbi->sec_stat.gc_data_blk_count[BG_GC] = 0;
			sbi->sec_stat.gc_data_blk_count[FG_GC] = 0;
			sbi->sec_stat.gc_ttime[BG_GC] = 0;
			sbi->sec_stat.gc_ttime[FG_GC] = 0;
		return count;
	} else if (!strcmp(a->attr.name, "sec_io_stat")) {
		sbi->sec_stat.cp_cnt[STAT_CP_ALL] = 0;
		sbi->sec_stat.cp_cnt[STAT_CP_BG] = 0;
		sbi->sec_stat.cp_cnt[STAT_CP_FSYNC] = 0;
		for (i = 0; i < NR_CP_REASON; i++)
			sbi->sec_stat.cpr_cnt[i] = 0;
		sbi->sec_stat.cp_max_interval= 0;
		sbi->sec_stat.alloc_seg_type[LFS] = 0;
		sbi->sec_stat.alloc_seg_type[SSR] = 0;
		sbi->sec_stat.alloc_blk_count[LFS] = 0;
		sbi->sec_stat.alloc_blk_count[SSR] = 0;
		atomic64_set(&sbi->sec_stat.inplace_count, 0);
		sbi->sec_stat.fsync_count = 0;
		sbi->sec_stat.fsync_dirty_pages = 0;
		sbi->sec_stat.hot_file_written_blocks = 0;
		sbi->sec_stat.cold_file_written_blocks = 0;
		sbi->sec_stat.warm_file_written_blocks = 0;
		sbi->sec_stat.max_inmem_pages = 0;
		sbi->sec_stat.drop_inmem_all = 0;
		sbi->sec_stat.drop_inmem_files = 0;
		if (sbi->sb->s_bdev->bd_part)
			sbi->sec_stat.kwritten_byte = BD_PART_WRITTEN(sbi);
		sbi->sec_stat.fs_por_error = 0;
		sbi->sec_stat.fs_error = 0;
		sbi->sec_stat.max_undiscard_blks = 0;
		return count;
	} else if (!strcmp(a->attr.name, "sec_fsck_stat")) {
		sbi->sec_fsck_stat.fsck_read_bytes = 0;
		sbi->sec_fsck_stat.fsck_written_bytes = 0;
		sbi->sec_fsck_stat.fsck_elapsed_time = 0;
		sbi->sec_fsck_stat.fsck_exit_code = 0;
		sbi->sec_fsck_stat.valid_node_count = 0;
		sbi->sec_fsck_stat.valid_inode_count = 0;
		return count;
	}

	ui = (unsigned int *)(ptr + a->offset);

	ret = kstrtoul(skip_spaces(buf), 0, &t);
	if (ret < 0)
		return ret;
#ifdef CONFIG_F2FS_FAULT_INJECTION
	if (a->struct_type == FAULT_INFO_TYPE && t >= (1 << FAULT_MAX))
		return -EINVAL;
	if (a->struct_type == FAULT_INFO_RATE && t >= UINT_MAX)
		return -EINVAL;
#endif
	if (a->struct_type == RESERVED_BLOCKS) {
		spin_lock(&sbi->stat_lock);
		if (t > (unsigned long)(sbi->user_block_count -
				F2FS_OPTION(sbi).root_reserved_blocks)) {
			spin_unlock(&sbi->stat_lock);
			return -EINVAL;
		}
		*ui = t;
		sbi->current_reserved_blocks = min(sbi->reserved_blocks,
				sbi->user_block_count - valid_user_blocks(sbi));
		spin_unlock(&sbi->stat_lock);
		return count;
	}

	if (!strcmp(a->attr.name, "discard_granularity")) {
		if (t == 0 || t > MAX_PLIST_NUM)
			return -EINVAL;
		if (t == *ui)
			return count;
		*ui = t;
		return count;
	}

	if (!strcmp(a->attr.name, "migration_granularity")) {
		if (t == 0 || t > sbi->segs_per_sec)
			return -EINVAL;
	}

	if (!strcmp(a->attr.name, "trim_sections"))
		return -EINVAL;

	if (!strcmp(a->attr.name, "gc_urgent")) {
		if (t >= 1) {
			sbi->gc_mode = GC_URGENT;
			if (sbi->gc_thread) {
				sbi->gc_thread->gc_wake = 1;
				wake_up_interruptible_all(
					&sbi->gc_thread->gc_wait_queue_head);
				wake_up_discard_thread(sbi, true);
			}
		} else {
			sbi->gc_mode = GC_NORMAL;
		}
		return count;
	}
	if (!strcmp(a->attr.name, "gc_idle")) {
		if (t == GC_IDLE_CB)
			sbi->gc_mode = GC_IDLE_CB;
		else if (t == GC_IDLE_GREEDY)
			sbi->gc_mode = GC_IDLE_GREEDY;
		else
			sbi->gc_mode = GC_NORMAL;
		return count;
	}

	if (!strcmp(a->attr.name, "iostat_enable")) {
		sbi->iostat_enable = !!t;
		if (!sbi->iostat_enable)
			f2fs_reset_iostat(sbi);
		return count;
	} else if (!strcmp(a->attr.name, "sec_fua_mode")) {
		const char *mode= strim((char *)buf);
		int idx;

		for (idx = 0; idx < NR_F2FS_SEC_FUA_MODE; idx++) {
			if(!strcmp(mode, sec_fua_mode_names[idx]))
				sbi->s_sec_cond_fua_mode = idx;
		}

		return count;
	}

	if (!strcmp(a->attr.name, "iostat_period_ms")) {
		if (t < MIN_IOSTAT_PERIOD_MS || t > MAX_IOSTAT_PERIOD_MS)
			return -EINVAL;
		spin_lock(&sbi->iostat_lock);
		sbi->iostat_period_ms = (unsigned int)t;
		spin_unlock(&sbi->iostat_lock);
		return count;
	}

	*ui = (unsigned int)t;

	return count;
}

static ssize_t f2fs_sbi_store(struct f2fs_attr *a,
			struct f2fs_sb_info *sbi,
			const char *buf, size_t count)
{
	ssize_t ret;
	bool gc_entry = (!strcmp(a->attr.name, "gc_urgent") ||
					a->struct_type == GC_THREAD);

	if (gc_entry) {
		if (!down_read_trylock(&sbi->sb->s_umount))
			return -EAGAIN;
	}
	ret = __sbi_store(a, sbi, buf, count);
	if (gc_entry)
		up_read(&sbi->sb->s_umount);

	return ret;
}

static ssize_t f2fs_attr_show(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct f2fs_sb_info *sbi = container_of(kobj, struct f2fs_sb_info,
								s_kobj);
	struct f2fs_attr *a = container_of(attr, struct f2fs_attr, attr);

	return a->show ? a->show(a, sbi, buf) : 0;
}

static ssize_t f2fs_attr_store(struct kobject *kobj, struct attribute *attr,
						const char *buf, size_t len)
{
	struct f2fs_sb_info *sbi = container_of(kobj, struct f2fs_sb_info,
									s_kobj);
	struct f2fs_attr *a = container_of(attr, struct f2fs_attr, attr);

	return a->store ? a->store(a, sbi, buf, len) : 0;
}

static void f2fs_sb_release(struct kobject *kobj)
{
	struct f2fs_sb_info *sbi = container_of(kobj, struct f2fs_sb_info,
								s_kobj);
	complete(&sbi->s_kobj_unregister);
}

enum feat_id {
	FEAT_CRYPTO = 0,
	FEAT_BLKZONED,
	FEAT_ATOMIC_WRITE,
	FEAT_EXTRA_ATTR,
	FEAT_PROJECT_QUOTA,
	FEAT_INODE_CHECKSUM,
	FEAT_FLEXIBLE_INLINE_XATTR,
	FEAT_QUOTA_INO,
	FEAT_INODE_CRTIME,
	FEAT_LOST_FOUND,
	FEAT_VERITY,
	FEAT_SB_CHECKSUM,
	FEAT_CASEFOLD,
	FEAT_COMPRESSION,
	FEAT_TEST_DUMMY_ENCRYPTION_V2,
};

static ssize_t f2fs_feature_show(struct f2fs_attr *a,
		struct f2fs_sb_info *sbi, char *buf)
{
	switch (a->id) {
	case FEAT_CRYPTO:
	case FEAT_BLKZONED:
	case FEAT_ATOMIC_WRITE:
	case FEAT_EXTRA_ATTR:
	case FEAT_PROJECT_QUOTA:
	case FEAT_INODE_CHECKSUM:
	case FEAT_FLEXIBLE_INLINE_XATTR:
	case FEAT_QUOTA_INO:
	case FEAT_INODE_CRTIME:
	case FEAT_LOST_FOUND:
	case FEAT_VERITY:
	case FEAT_SB_CHECKSUM:
	case FEAT_CASEFOLD:
	case FEAT_COMPRESSION:
	case FEAT_TEST_DUMMY_ENCRYPTION_V2:
		return sprintf(buf, "supported\n");
	}
	return 0;
}

#define F2FS_ATTR_OFFSET(_struct_type, _name, _mode, _show, _store, _offset) \
static struct f2fs_attr f2fs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.show	= _show,					\
	.store	= _store,					\
	.struct_type = _struct_type,				\
	.offset = _offset					\
}

#define F2FS_RW_ATTR(struct_type, struct_name, name, elname)	\
	F2FS_ATTR_OFFSET(struct_type, name, 0644,		\
		f2fs_sbi_show, f2fs_sbi_store,			\
		offsetof(struct struct_name, elname))

#define F2FS_RO_ATTR(struct_type, struct_name, name, elname)	\
	F2FS_ATTR_OFFSET(struct_type, name, 0444,		\
		f2fs_sbi_show, f2fs_sbi_store,			\
		offsetof(struct struct_name, elname))

#define F2FS_GENERAL_RO_ATTR(name) \
static struct f2fs_attr f2fs_attr_##name = __ATTR(name, 0444, name##_show, NULL)

#define F2FS_FEATURE_RO_ATTR(_name, _id)			\
static struct f2fs_attr f2fs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = 0444 },	\
	.show	= f2fs_feature_show,				\
	.id	= _id,						\
}

#define F2FS_STAT_ATTR(_struct_type, _struct_name, _name, _elname)	\
static struct f2fs_attr f2fs_attr_##_name = {			\
	.attr = {.name = __stringify(_name), .mode = 0444 },	\
	.show = f2fs_sbi_show,					\
	.struct_type = _struct_type,				\
	.offset = offsetof(struct _struct_name, _elname),       \
}

F2FS_RO_ATTR(GC_THREAD, f2fs_gc_kthread, gc_urgent_sleep_time,
							urgent_sleep_time);
F2FS_RW_ATTR(GC_THREAD, f2fs_gc_kthread, gc_min_sleep_time, min_sleep_time);
F2FS_RW_ATTR(GC_THREAD, f2fs_gc_kthread, gc_max_sleep_time, max_sleep_time);
F2FS_RW_ATTR(GC_THREAD, f2fs_gc_kthread, gc_no_gc_sleep_time, no_gc_sleep_time);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, gc_idle, gc_mode);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, gc_urgent, gc_mode);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, reclaim_segments, rec_prefree_segments);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, main_blkaddr, main_blkaddr);
F2FS_RW_ATTR(DCC_INFO, discard_cmd_control, max_small_discards, max_discards);
F2FS_RW_ATTR(DCC_INFO, discard_cmd_control, discard_granularity, discard_granularity);
F2FS_RW_ATTR(RESERVED_BLOCKS, f2fs_sb_info, reserved_blocks, reserved_blocks);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, batched_trim_sections, trim_sections);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, ipu_policy, ipu_policy);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, min_ipu_util, min_ipu_util);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, min_fsync_blocks, min_fsync_blocks);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, min_seq_blocks, min_seq_blocks);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, min_hot_blocks, min_hot_blocks);
F2FS_RW_ATTR(SM_INFO, f2fs_sm_info, min_ssr_sections, min_ssr_sections);
F2FS_RW_ATTR(NM_INFO, f2fs_nm_info, ram_thresh, ram_thresh);
F2FS_RW_ATTR(NM_INFO, f2fs_nm_info, ra_nid_pages, ra_nid_pages);
F2FS_RW_ATTR(NM_INFO, f2fs_nm_info, dirty_nats_ratio, dirty_nats_ratio);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, max_victim_search, max_victim_search);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, migration_granularity, migration_granularity);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, dir_level, dir_level);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, cp_interval, interval_time[CP_TIME]);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, idle_interval, interval_time[REQ_TIME]);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, discard_idle_interval,
					interval_time[DISCARD_TIME]);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, gc_idle_interval, interval_time[GC_TIME]);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info,
		umount_discard_timeout, interval_time[UMOUNT_DISCARD_TIMEOUT]);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, iostat_enable, iostat_enable);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, iostat_period_ms, iostat_period_ms);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, readdir_ra, readdir_ra);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, gc_pin_file_thresh, gc_pin_file_threshold);
F2FS_RW_ATTR(F2FS_SBI, f2fs_super_block, extension_list, extension_list);
#ifdef CONFIG_F2FS_FAULT_INJECTION
F2FS_RW_ATTR(FAULT_INFO_RATE, f2fs_fault_info, inject_rate, inject_rate);
F2FS_RW_ATTR(FAULT_INFO_TYPE, f2fs_fault_info, inject_type, inject_type);
#endif
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, data_io_flag, data_io_flag);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, node_io_flag, node_io_flag);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, sec_gc_stat, sec_stat);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, sec_io_stat, sec_stat);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, sec_fsck_stat, sec_fsck_stat);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, sec_heimdallfs_stat, sec_heimdallfs_stat);
F2FS_RW_ATTR(F2FS_SBI, f2fs_sb_info, sec_fua_mode, s_sec_cond_fua_mode);
F2FS_GENERAL_RO_ATTR(dirty_segments);
F2FS_GENERAL_RO_ATTR(free_segments);
F2FS_GENERAL_RO_ATTR(lifetime_write_kbytes);
F2FS_GENERAL_RO_ATTR(sec_fs_stat);
F2FS_GENERAL_RO_ATTR(features);
F2FS_GENERAL_RO_ATTR(current_reserved_blocks);
F2FS_GENERAL_RO_ATTR(unusable);
F2FS_GENERAL_RO_ATTR(encoding);
F2FS_GENERAL_RO_ATTR(mounted_time_sec);
#ifdef CONFIG_F2FS_STAT_FS
F2FS_STAT_ATTR(STAT_INFO, f2fs_stat_info, cp_foreground_calls, cp_count);
F2FS_STAT_ATTR(STAT_INFO, f2fs_stat_info, cp_background_calls, bg_cp_count);
F2FS_STAT_ATTR(STAT_INFO, f2fs_stat_info, gc_foreground_calls, call_count);
F2FS_STAT_ATTR(STAT_INFO, f2fs_stat_info, gc_background_calls, bg_gc);
F2FS_GENERAL_RO_ATTR(moved_blocks_background);
F2FS_GENERAL_RO_ATTR(moved_blocks_foreground);
F2FS_GENERAL_RO_ATTR(avg_vblocks);
#endif

#ifdef CONFIG_FS_ENCRYPTION
F2FS_FEATURE_RO_ATTR(encryption, FEAT_CRYPTO);
F2FS_FEATURE_RO_ATTR(test_dummy_encryption_v2, FEAT_TEST_DUMMY_ENCRYPTION_V2);
#endif
#ifdef CONFIG_BLK_DEV_ZONED
F2FS_FEATURE_RO_ATTR(block_zoned, FEAT_BLKZONED);
#endif
F2FS_FEATURE_RO_ATTR(atomic_write, FEAT_ATOMIC_WRITE);
F2FS_FEATURE_RO_ATTR(extra_attr, FEAT_EXTRA_ATTR);
F2FS_FEATURE_RO_ATTR(project_quota, FEAT_PROJECT_QUOTA);
F2FS_FEATURE_RO_ATTR(inode_checksum, FEAT_INODE_CHECKSUM);
F2FS_FEATURE_RO_ATTR(flexible_inline_xattr, FEAT_FLEXIBLE_INLINE_XATTR);
F2FS_FEATURE_RO_ATTR(quota_ino, FEAT_QUOTA_INO);
F2FS_FEATURE_RO_ATTR(inode_crtime, FEAT_INODE_CRTIME);
F2FS_FEATURE_RO_ATTR(lost_found, FEAT_LOST_FOUND);
#ifdef CONFIG_FS_VERITY
F2FS_FEATURE_RO_ATTR(verity, FEAT_VERITY);
#endif
F2FS_FEATURE_RO_ATTR(sb_checksum, FEAT_SB_CHECKSUM);
F2FS_FEATURE_RO_ATTR(casefold, FEAT_CASEFOLD);
#ifdef CONFIG_F2FS_FS_COMPRESSION
F2FS_FEATURE_RO_ATTR(compression, FEAT_COMPRESSION);
#endif

#define ATTR_LIST(name) (&f2fs_attr_##name.attr)
static struct attribute *f2fs_attrs[] = {
	ATTR_LIST(gc_urgent_sleep_time),
	ATTR_LIST(gc_min_sleep_time),
	ATTR_LIST(gc_max_sleep_time),
	ATTR_LIST(gc_no_gc_sleep_time),
	ATTR_LIST(gc_idle),
	ATTR_LIST(gc_urgent),
	ATTR_LIST(reclaim_segments),
	ATTR_LIST(main_blkaddr),
	ATTR_LIST(max_small_discards),
	ATTR_LIST(discard_granularity),
	ATTR_LIST(batched_trim_sections),
	ATTR_LIST(ipu_policy),
	ATTR_LIST(min_ipu_util),
	ATTR_LIST(min_fsync_blocks),
	ATTR_LIST(min_seq_blocks),
	ATTR_LIST(min_hot_blocks),
	ATTR_LIST(min_ssr_sections),
	ATTR_LIST(max_victim_search),
	ATTR_LIST(migration_granularity),
	ATTR_LIST(dir_level),
	ATTR_LIST(ram_thresh),
	ATTR_LIST(ra_nid_pages),
	ATTR_LIST(dirty_nats_ratio),
	ATTR_LIST(cp_interval),
	ATTR_LIST(idle_interval),
	ATTR_LIST(discard_idle_interval),
	ATTR_LIST(gc_idle_interval),
	ATTR_LIST(umount_discard_timeout),
	ATTR_LIST(iostat_enable),
	ATTR_LIST(iostat_period_ms),
	ATTR_LIST(readdir_ra),
	ATTR_LIST(gc_pin_file_thresh),
	ATTR_LIST(extension_list),
	ATTR_LIST(sec_gc_stat),
	ATTR_LIST(sec_io_stat),
	ATTR_LIST(sec_fsck_stat),
	ATTR_LIST(sec_heimdallfs_stat),
	ATTR_LIST(sec_fua_mode),
#ifdef CONFIG_F2FS_FAULT_INJECTION
	ATTR_LIST(inject_rate),
	ATTR_LIST(inject_type),
#endif
	ATTR_LIST(data_io_flag),
	ATTR_LIST(node_io_flag),
	ATTR_LIST(dirty_segments),
	ATTR_LIST(free_segments),
	ATTR_LIST(unusable),
	ATTR_LIST(lifetime_write_kbytes),
	ATTR_LIST(sec_fs_stat),
	ATTR_LIST(features),
	ATTR_LIST(reserved_blocks),
	ATTR_LIST(current_reserved_blocks),
	ATTR_LIST(encoding),
	ATTR_LIST(mounted_time_sec),
#ifdef CONFIG_F2FS_STAT_FS
	ATTR_LIST(cp_foreground_calls),
	ATTR_LIST(cp_background_calls),
	ATTR_LIST(gc_foreground_calls),
	ATTR_LIST(gc_background_calls),
	ATTR_LIST(moved_blocks_foreground),
	ATTR_LIST(moved_blocks_background),
	ATTR_LIST(avg_vblocks),
#endif
	NULL,
};

static struct attribute *f2fs_feat_attrs[] = {
#ifdef CONFIG_FS_ENCRYPTION
	ATTR_LIST(encryption),
	ATTR_LIST(test_dummy_encryption_v2),
#endif
#ifdef CONFIG_BLK_DEV_ZONED
	ATTR_LIST(block_zoned),
#endif
	ATTR_LIST(atomic_write),
	ATTR_LIST(extra_attr),
	ATTR_LIST(project_quota),
	ATTR_LIST(inode_checksum),
	ATTR_LIST(flexible_inline_xattr),
	ATTR_LIST(quota_ino),
	ATTR_LIST(inode_crtime),
	ATTR_LIST(lost_found),
#ifdef CONFIG_FS_VERITY
	ATTR_LIST(verity),
#endif
	ATTR_LIST(sb_checksum),
	ATTR_LIST(casefold),
#ifdef CONFIG_F2FS_FS_COMPRESSION
	ATTR_LIST(compression),
#endif
	NULL,
};

static const struct sysfs_ops f2fs_attr_ops = {
	.show	= f2fs_attr_show,
	.store	= f2fs_attr_store,
};

static struct kobj_type f2fs_sb_ktype = {
	.default_attrs	= f2fs_attrs,
	.sysfs_ops	= &f2fs_attr_ops,
	.release	= f2fs_sb_release,
};

static struct kobj_type f2fs_ktype = {
	.sysfs_ops	= &f2fs_attr_ops,
};

static struct kset f2fs_kset = {
	.kobj   = {.ktype = &f2fs_ktype},
};

static struct kobj_type f2fs_feat_ktype = {
	.default_attrs	= f2fs_feat_attrs,
	.sysfs_ops	= &f2fs_attr_ops,
};

static struct kobject f2fs_feat = {
	.kset	= &f2fs_kset,
};

static int __maybe_unused segment_info_seq_show(struct seq_file *seq,
						void *offset)
{
	struct super_block *sb = seq->private;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	unsigned int total_segs =
			le32_to_cpu(sbi->raw_super->segment_count_main);
	int i;

	seq_puts(seq, "format: segment_type|valid_blocks\n"
		"segment_type(0:HD, 1:WD, 2:CD, 3:HN, 4:WN, 5:CN)\n");

	for (i = 0; i < total_segs; i++) {
		struct seg_entry *se = get_seg_entry(sbi, i);

		if ((i % 10) == 0)
			seq_printf(seq, "%-10d", i);
		seq_printf(seq, "%d|%-3u", se->type, se->valid_blocks);
		if ((i % 10) == 9 || i == (total_segs - 1))
			seq_putc(seq, '\n');
		else
			seq_putc(seq, ' ');
	}

	return 0;
}

static int __maybe_unused segment_bits_seq_show(struct seq_file *seq,
						void *offset)
{
	struct super_block *sb = seq->private;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	unsigned int total_segs =
			le32_to_cpu(sbi->raw_super->segment_count_main);
	int i, j;

	seq_puts(seq, "format: segment_type|valid_blocks|bitmaps\n"
		"segment_type(0:HD, 1:WD, 2:CD, 3:HN, 4:WN, 5:CN)\n");

	for (i = 0; i < total_segs; i++) {
		struct seg_entry *se = get_seg_entry(sbi, i);

		seq_printf(seq, "%-10d", i);
		seq_printf(seq, "%d|%-3u|", se->type, se->valid_blocks);
		for (j = 0; j < SIT_VBLOCK_MAP_SIZE; j++)
			seq_printf(seq, " %.2x", se->cur_valid_map[j]);
		seq_putc(seq, '\n');
	}
	return 0;
}

void f2fs_record_iostat(struct f2fs_sb_info *sbi)
{
	unsigned long long iostat_diff[NR_IO_TYPE];
	int i;

	if (time_is_after_jiffies(sbi->iostat_next_period))
		return;

	/* Need double check under the lock */
	spin_lock(&sbi->iostat_lock);
	if (time_is_after_jiffies(sbi->iostat_next_period)) {
		spin_unlock(&sbi->iostat_lock);
		return;
	}
	sbi->iostat_next_period = jiffies +
				msecs_to_jiffies(sbi->iostat_period_ms);

	for (i = 0; i < NR_IO_TYPE; i++) {
		iostat_diff[i] = sbi->rw_iostat[i] -
				sbi->prev_rw_iostat[i];
		sbi->prev_rw_iostat[i] = sbi->rw_iostat[i];
	}
	spin_unlock(&sbi->iostat_lock);

	trace_f2fs_iostat(sbi, iostat_diff);
}

static int __maybe_unused iostat_info_seq_show(struct seq_file *seq,
					       void *offset)
{
	struct super_block *sb = seq->private;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	time64_t now = ktime_get_real_seconds();

	if (!sbi->iostat_enable)
		return 0;

	seq_printf(seq, "time:		%-16llu\n", now);

	/* print app write IOs */
	seq_puts(seq, "[WRITE]\n");
	seq_printf(seq, "app buffered:	%-16llu\n",
				sbi->rw_iostat[APP_BUFFERED_IO]);
	seq_printf(seq, "app direct:	%-16llu\n",
				sbi->rw_iostat[APP_DIRECT_IO]);
	seq_printf(seq, "app mapped:	%-16llu\n",
				sbi->rw_iostat[APP_MAPPED_IO]);

	/* print fs write IOs */
	seq_printf(seq, "fs data:	%-16llu\n",
				sbi->rw_iostat[FS_DATA_IO]);
	seq_printf(seq, "fs node:	%-16llu\n",
				sbi->rw_iostat[FS_NODE_IO]);
	seq_printf(seq, "fs meta:	%-16llu\n",
				sbi->rw_iostat[FS_META_IO]);
	seq_printf(seq, "fs gc data:	%-16llu\n",
				sbi->rw_iostat[FS_GC_DATA_IO]);
	seq_printf(seq, "fs gc node:	%-16llu\n",
				sbi->rw_iostat[FS_GC_NODE_IO]);
	seq_printf(seq, "fs cp data:	%-16llu\n",
				sbi->rw_iostat[FS_CP_DATA_IO]);
	seq_printf(seq, "fs cp node:	%-16llu\n",
				sbi->rw_iostat[FS_CP_NODE_IO]);
	seq_printf(seq, "fs cp meta:	%-16llu\n",
				sbi->rw_iostat[FS_CP_META_IO]);

	/* print app read IOs */
	seq_puts(seq, "[READ]\n");
	seq_printf(seq, "app buffered:	%-16llu\n",
				sbi->rw_iostat[APP_BUFFERED_READ_IO]);
	seq_printf(seq, "app direct:	%-16llu\n",
				sbi->rw_iostat[APP_DIRECT_READ_IO]);
	seq_printf(seq, "app mapped:	%-16llu\n",
				sbi->rw_iostat[APP_MAPPED_READ_IO]);

	/* print fs read IOs */
	seq_printf(seq, "fs data:	%-16llu\n",
				sbi->rw_iostat[FS_DATA_READ_IO]);
	seq_printf(seq, "fs gc data:	%-16llu\n",
				sbi->rw_iostat[FS_GDATA_READ_IO]);
	seq_printf(seq, "fs compr_data:	%-16llu\n",
				sbi->rw_iostat[FS_CDATA_READ_IO]);
	seq_printf(seq, "fs node:	%-16llu\n",
				sbi->rw_iostat[FS_NODE_READ_IO]);
	seq_printf(seq, "fs meta:	%-16llu\n",
				sbi->rw_iostat[FS_META_READ_IO]);

	/* print other IOs */
	seq_puts(seq, "[OTHER]\n");
	seq_printf(seq, "fs discard:	%-16llu\n",
				sbi->rw_iostat[FS_DISCARD]);

	return 0;
}

static int __maybe_unused victim_bits_seq_show(struct seq_file *seq,
						void *offset)
{
	struct super_block *sb = seq->private;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	int i;

	seq_puts(seq, "format: victim_secmap bitmaps\n");

	for (i = 0; i < MAIN_SECS(sbi); i++) {
		if ((i % 10) == 0)
			seq_printf(seq, "%-10d", i);
		seq_printf(seq, "%d", test_bit(i, dirty_i->victim_secmap) ? 1 : 0);
		if ((i % 10) == 9 || i == (MAIN_SECS(sbi) - 1))
			seq_putc(seq, '\n');
		else
			seq_putc(seq, ' ');
	}
	return 0;
}

#define F2FS_PROC_FILE_DEF(_name)					\
static int _name##_open_fs(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, _name##_seq_show, PDE_DATA(inode));	\
}									\
									\
static const struct file_operations f2fs_seq_##_name##_fops = {		\
	.open = _name##_open_fs,					\
	.read = seq_read,						\
	.llseek = seq_lseek,						\
	.release = single_release,					\
};

F2FS_PROC_FILE_DEF(segment_info);
F2FS_PROC_FILE_DEF(segment_bits);
F2FS_PROC_FILE_DEF(iostat_info);
F2FS_PROC_FILE_DEF(victim_bits);

int __init f2fs_init_sysfs(void)
{
	int ret;

	kobject_set_name(&f2fs_kset.kobj, "f2fs");
	f2fs_kset.kobj.parent = fs_kobj;
	ret = kset_register(&f2fs_kset);
	if (ret)
		return ret;

	ret = kobject_init_and_add(&f2fs_feat, &f2fs_feat_ktype,
				   NULL, "features");
	if (ret) {
		kobject_put(&f2fs_feat);
		kset_unregister(&f2fs_kset);
	} else {
		f2fs_proc_root = proc_mkdir("fs/f2fs", NULL);
	}
	return ret;
}

void f2fs_exit_sysfs(void)
{
	kobject_put(&f2fs_feat);
	kset_unregister(&f2fs_kset);
	remove_proc_entry("fs/f2fs", NULL);
	f2fs_proc_root = NULL;
}

#define SEC_MAX_VOLUME_NAME	16
static bool __volume_is_userdata(struct f2fs_sb_info *sbi)
{
	char volume_name[SEC_MAX_VOLUME_NAME] = {0, };

	utf16s_to_utf8s(sbi->raw_super->volume_name, SEC_MAX_VOLUME_NAME,
			UTF16_LITTLE_ENDIAN, volume_name, SEC_MAX_VOLUME_NAME);
	volume_name[SEC_MAX_VOLUME_NAME - 1] = '\0';

	if (!strcmp(volume_name, "data"))
		return true;

	return false;
}

int f2fs_register_sysfs(struct f2fs_sb_info *sbi)
{
	struct super_block *sb = sbi->sb;
	int err;

	sbi->s_kobj.kset = &f2fs_kset;
	init_completion(&sbi->s_kobj_unregister);
	err = kobject_init_and_add(&sbi->s_kobj, &f2fs_sb_ktype, NULL,
				"%s", sb->s_id);
	if (err) {
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
		return err;
	}

	if (__volume_is_userdata(sbi)) {
		err = sysfs_create_link(&f2fs_kset.kobj, &sbi->s_kobj,
				"userdata");
		if (err)
			pr_err("Can not create sysfs link for userdata(%d)\n",
					err);
	}

	if (f2fs_proc_root)
		sbi->s_proc = proc_mkdir(sb->s_id, f2fs_proc_root);

	if (sbi->s_proc) {
		proc_create_data("segment_info", S_IRUGO, sbi->s_proc,
				 &f2fs_seq_segment_info_fops, sb);
		proc_create_data("segment_bits", S_IRUGO, sbi->s_proc,
				 &f2fs_seq_segment_bits_fops, sb);
		proc_create_data("iostat_info", S_IRUGO, sbi->s_proc,
				&f2fs_seq_iostat_info_fops, sb);
		proc_create_data("victim_bits", S_IRUGO, sbi->s_proc,
				&f2fs_seq_victim_bits_fops, sb);
	}
	return 0;
}

void f2fs_unregister_sysfs(struct f2fs_sb_info *sbi)
{
	if (sbi->s_proc) {
		remove_proc_entry("iostat_info", sbi->s_proc);
		remove_proc_entry("segment_info", sbi->s_proc);
		remove_proc_entry("segment_bits", sbi->s_proc);
		remove_proc_entry("victim_bits", sbi->s_proc);
		remove_proc_entry(sbi->sb->s_id, f2fs_proc_root);
	}

	if (__volume_is_userdata(sbi))
		sysfs_delete_link(&f2fs_kset.kobj, &sbi->s_kobj, "userdata");

	kobject_del(&sbi->s_kobj);
	kobject_put(&sbi->s_kobj);
	wait_for_completion(&sbi->s_kobj_unregister);
}
