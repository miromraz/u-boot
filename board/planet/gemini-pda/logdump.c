// SPDX-License-Identifier: GPL-2.0+
/*
 * DEBUG: dump U-Boot's recorded console to the eMMC so it can be read back
 * with mtkclient. The Planet Gemini PDA has no UART, the video console shows
 * no text and USB fastboot does not enumerate, so this is the only log channel.
 *
 * The dump lands in the LAST 1 MiB of the GPT partition named "cache" on
 * mmc 0 (user area, hwpart 0). The partition is looked up by name at runtime,
 * never by a hardcoded offset, so a wrong offset can never hit another
 * partition. Format: a 64-byte header (magic, length, seq, timer) followed by
 * the raw console-record bytes, zero-padded to whole 512-byte blocks.
 *
 * Temporary bring-up aid; revert after the console log has been recovered.
 */

#include <blk.h>
#include <command.h>
#include <dm.h>
#include <membuf.h>
#include <part.h>
#include <time.h>
#include <video.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <linux/sizes.h>
#include <linux/string.h>

DECLARE_GLOBAL_DATA_PTR;

#define GEMINI_LOG_MAGIC	"GEMINI-UBOOT-LOG"
#define GEMINI_LOG_WINDOW	SZ_1M		/* dump into the last 1 MiB */

struct gemini_log_hdr {
	char magic[16];
	u32 length;
	u32 seq;
	u64 timer;
	u8 reserved[32];
} __packed;

/* header + the whole console record, rounded up to a 512-byte block */
#define GEMINI_LOG_MAX	ALIGN(sizeof(struct gemini_log_hdr) + \
			      CONFIG_CONSOLE_RECORD_OUT_SIZE, 512)
static u8 gemini_log_buf[GEMINI_LOG_MAX];

/* The "no text on screen" clue: where the framebuffer actually is. */
static void gemini_log_video_info(void)
{
	struct udevice *dev;
	struct uclass *uc;

	uclass_id_foreach_dev(UCLASS_VIDEO, dev, uc) {
		struct video_priv *pr = dev_get_uclass_priv(dev);

		printf("logdump: video %s fb=%p size=%d line=%d bpix=%d rot=%d %ux%u\n",
		       dev->name, pr->fb, pr->fb_size, pr->line_length,
		       pr->bpix, pr->rot, pr->xsize, pr->ysize);
	}
	uclass_id_foreach_dev(UCLASS_VIDEO_CONSOLE, dev, uc)
		printf("logdump: vidconsole %s\n", dev->name);
}

int gemini_logdump_write(void)
{
	struct gemini_log_hdr *hdr = (struct gemini_log_hdr *)gemini_log_buf;
	struct disk_partition info;
	struct blk_desc *desc;
	struct membuf mb;
	lbaint_t start, blks, win;
	u32 len = 0;
	char *data;
	int i;

	gemini_log_video_info();

	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (!desc) {
		printf("logdump: mmc 0 not found\n");
		return -1;
	}
	if (part_get_info_by_name(desc, "cache", &info) < 0) {
		printf("logdump: 'cache' partition not found\n");
		return -1;
	}
	win = GEMINI_LOG_WINDOW / info.blksz;
	if (info.size < 2 * win) {
		printf("logdump: 'cache' smaller than 2 MiB, refusing\n");
		return -1;
	}

	/* Read the console record without consuming it (work on a copy). */
	mb = gd->console_out;
	for (i = 0; i < 2; i++) {
		int n = membuf_getraw(&mb, GEMINI_LOG_MAX - sizeof(*hdr) - len,
				      true, &data);
		if (n <= 0)
			break;
		memcpy(gemini_log_buf + sizeof(*hdr) + len, data, n);
		len += n;
	}

	memcpy(hdr->magic, GEMINI_LOG_MAGIC, sizeof(hdr->magic));
	hdr->length = len;
	hdr->seq = 0;
	hdr->timer = get_timer(0);
	memset(hdr->reserved, 0, sizeof(hdr->reserved));

	blks = DIV_ROUND_UP(sizeof(*hdr) + len, info.blksz);
	memset(gemini_log_buf + sizeof(*hdr) + len, 0,
	       blks * info.blksz - sizeof(*hdr) - len);

	start = info.start + info.size - win;	/* start of the last 1 MiB */
	if (blk_dwrite(desc, start, blks, gemini_log_buf) != blks) {
		printf("logdump: blk_dwrite failed\n");
		return -1;
	}

	printf("logdump: wrote %u bytes (" LBAFU " blocks) to 'cache' at LBA " LBAF "\n",
	       (u32)(sizeof(*hdr) + len), blks, start);
	return 0;
}

static int do_gemini_logdump(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	return gemini_logdump_write() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(gemini_logdump, 1, 0, do_gemini_logdump,
	   "dump recorded console to the eMMC 'cache' partition", "");

/* Also dump early, before the console/menu, in case the later commands hang. */
int board_late_init(void)
{
	gemini_logdump_write();
	return 0;
}
