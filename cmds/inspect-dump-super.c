/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/zoned.h"
#include "common/help.h"
#include "common/string-utils.h"
#include "common/messages.h"
#include "cmds/commands.h"

static int load_and_dump_sb(char *filename, int fd, u64 sb_bytenr, int full,
		int force)
{
	struct btrfs_super_block sb;
	struct stat st;
	u64 ret;

	if (fstat(fd, &st) < 0) {
		error("unable to stat %s to when loading superblock: %m", filename);
		return 1;
	}

	if (S_ISBLK(st.st_mode) || S_ISREG(st.st_mode)) {
		off_t last_byte;

		last_byte = lseek(fd, 0, SEEK_END);
		if (last_byte == -1) {
			error("cannot read end of file %s: %m", filename);
			return 1;
		}

		if (sb_bytenr > last_byte)
			return 0;
	}

	ret = sbread(fd, &sb, sb_bytenr);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		/* check if the disk if too short for further superblock */
		if (ret == 0 && errno == 0)
			return 0;

		error("failed to read the superblock on %s at %llu read %llu/%d bytes",
		       filename, sb_bytenr, ret, BTRFS_SUPER_INFO_SIZE);
		error("error = '%m', errno = %d", errno);
		return 1;
	}
	pr_verbose(LOG_DEFAULT, "superblock: bytenr=%llu, device=%s\n", sb_bytenr, filename);
	pr_verbose(LOG_DEFAULT, "---------------------------------------------------------\n");
	if (btrfs_super_magic(&sb) != BTRFS_MAGIC && !force) {
		error("bad magic on superblock on %s at %llu (use --force to dump it anyway)",
				filename, sb_bytenr);
		return 1;
	}
	btrfs_print_superblock(&sb, full);
	putchar('\n');
	return 0;
}

static const char * const cmd_inspect_dump_super_usage[] = {
	"btrfs inspect-internal dump-super [options] device [device...]",
	"Dump superblock from a device in a textual form",
	"",
	OPTLINE("-f|--full", "print full superblock information, backup roots etc."),
	OPTLINE("-a|--all", "print information about all superblocks"),
	OPTLINE("-s|--super <super>", "specify which copy to print out (values: 0, 1, 2)"),
	OPTLINE("-F|--force", "attempt to dump superblocks with bad magic"),
	OPTLINE("--bytenr <offset>", "specify alternate superblock offset"),
	"",
	"Deprecated syntax:",
	OPTLINE("-s <bytenr>", "specify alternate superblock offset, values other than 0, 1, 2 "
		"will be interpreted as --bytenr for backward compatibility, "
		"option renamed for consistency with other tools (eg. check)"),
	OPTLINE("-i <super>", "specify which copy to print out (values: 0, 1, 2), now moved to --super"),
	NULL
};

static int cmd_inspect_dump_super(const struct cmd_struct *cmd,
				  int argc, char **argv)
{
	bool all = false;
	bool full = false;
	bool force = false;
	char *filename;
	int fd = -1;
	int i;
	u64 arg;
	u64 sb_bytenr = btrfs_sb_offset(0);

	while (1) {
		int c;
		enum { GETOPT_VAL_BYTENR = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{"all", no_argument, NULL, 'a'},
			{"bytenr", required_argument, NULL, GETOPT_VAL_BYTENR },
			{"full", no_argument, NULL, 'f'},
			{"force", no_argument, NULL, 'F'},
			{"super", required_argument, NULL, 's' },
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "fFai:s:", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'i':
			warning(
			    "option -i is deprecated, please use -s or --super");
			arg = arg_strtou64(optarg);
			if (arg >= BTRFS_SUPER_MIRROR_MAX) {
				error("super mirror too big: %llu >= %d",
					arg, BTRFS_SUPER_MIRROR_MAX);
				return 1;
			}
			sb_bytenr = btrfs_sb_offset(arg);
			break;

		case 'a':
			all = true;
			break;
		case 'f':
			full = true;
			break;
		case 'F':
			force = true;
			break;
		case 's':
			arg = arg_strtou64(optarg);
			if (BTRFS_SUPER_MIRROR_MAX <= arg) {
				warning(
		"deprecated use of -s <bytenr> with %llu, assuming --bytenr",
						arg);
				sb_bytenr = arg;
			} else {
				sb_bytenr = btrfs_sb_offset(arg);
			}
			all = false;
			break;
		case GETOPT_VAL_BYTENR:
			arg = arg_strtou64(optarg);
			sb_bytenr = arg;
			all = false;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	for (i = optind; i < argc; i++) {
		filename = argv[i];
		fd = open(filename, O_RDONLY);
		if (fd < 0) {
			error("cannot open %s: %m", filename);
			return 1;
		}

		if (all) {
			int idx;

			for (idx = 0; idx < BTRFS_SUPER_MIRROR_MAX; idx++) {
				sb_bytenr = btrfs_sb_offset(idx);
				if (load_and_dump_sb(filename, fd,
						sb_bytenr, full, force)) {
					close(fd);
					return 1;
				}
			}
		} else {
			if (load_and_dump_sb(filename, fd, sb_bytenr, full, force)) {
				close(fd);
				return 1;
			}
		}
		close(fd);
	}

	return 0;
}
DEFINE_SIMPLE_COMMAND(inspect_dump_super, "dump-super");
