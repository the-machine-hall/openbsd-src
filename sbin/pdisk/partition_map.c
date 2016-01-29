/*	$OpenBSD: partition_map.c,v 1.86 2016/01/29 17:34:08 krw Exp $	*/

/*
 * partition_map.c - partition map routines
 *
 * Written by Eryk Vershen
 */

/*
 * Copyright 1996,1997,1998 by Apple Computer, Inc.
 *              All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appears in all copies and
 * that both the copyright notice and this permission notice appear in
 * supporting documentation.
 *
 * APPLE COMPUTER DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL APPLE COMPUTER BE LIABLE FOR ANY SPECIAL, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT,
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/queue.h>
#include <sys/stdint.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dpme.h"
#include "partition_map.h"
#include "io.h"
#include "file_media.h"

#define APPLE_HFS_FLAGS_VALUE	0x4000037f

const char     *kFreeType = "Apple_Free";
const char     *kMapType = "Apple_partition_map";
const char     *kUnixType = "OpenBSD";
const char     *kHFSType = "Apple_HFS";

enum add_action {
	kReplace = 0,
	kAdd = 1,
	kSplit = 2
};

int		add_data_to_map(struct dpme *, long, struct partition_map *);
int		coerce_block0(struct partition_map *);
int		contains_driver(struct entry *);
void		combine_entry(struct entry *);
struct dpme    *create_dpme(const char *, const char *, uint32_t, uint32_t);
void		delete_entry(struct entry *);
void		insert_in_base_order(struct entry *);
void		insert_in_disk_order(struct entry *);
int		read_partition_map(struct partition_map *);
void		remove_driver(struct entry *);
void		renumber_disk_addresses(struct partition_map *);

struct partition_map *
open_partition_map(int fd, char *name, uint64_t mediasz, uint32_t sectorsz)
{
	struct partition_map *map;
	int ok;

	map = malloc(sizeof(struct partition_map));
	if (map == NULL) {
		warn("can't allocate memory for open partition map");
		return NULL;
	}

	map->fd = fd;
	map->name = name;

	map->changed = 0;
	LIST_INIT(&map->disk_order);
	LIST_INIT(&map->base_order);
	map->physical_block = sectorsz;
	map->blocks_in_map = 0;
	map->maximum_in_map = -1;

	if (mediasz > UINT32_MAX)
		map->media_size = UINT32_MAX;
	else
		map->media_size = mediasz;

	map->block0 = malloc(sizeof(struct block0));
	if (map->block0 == NULL) {
		warn("can't allocate memory for block zero buffer");
		free(map);
		return NULL;
	}
	if (read_block0(map->fd, map->block0) == 0) {
		warnx("Can't read block 0 from '%s'", name);
		free_partition_map(map);
		return NULL;
	}
	if (map->block0->sbSig == BLOCK0_SIGNATURE &&
	    map->block0->sbBlkSize == sectorsz &&
	    map->block0->sbBlkCount == mediasz) {
		if (read_partition_map(map) == 0)
			return map;
	} else {
		if (map->block0->sbSig != BLOCK0_SIGNATURE)
			warnx("Block 0 signature: Expected 0x%04x, "
			    "got 0x%04x", BLOCK0_SIGNATURE,
			    map->block0->sbSig);
		else if (map->block0->sbBlkSize != sectorsz)
			warnx("Block 0 sbBlkSize (%u) != sector size (%u)",
			    map->block0->sbBlkSize, sectorsz);
		else if (map->block0->sbBlkCount != mediasz)
			warnx("Block 0 sbBlkCount (%u) != media size (%llu)",
			    map->block0->sbBlkCount,
			    (unsigned long long)mediasz);
	}

	if (!lflag) {
		my_ungetch('\n');
		printf("No valid partition map found on '%s'.\n", name);
		ok = get_okay("Create default map? [n/y]: ", 0);
		flush_to_newline(0);
		if (ok == 1) {
			free_partition_map(map);
			map = create_partition_map(fd, name, mediasz, sectorsz);
			if (map)
				return map;
		}
	}

	free_partition_map(map);
	return NULL;
}


void
free_partition_map(struct partition_map *map)
{
	struct entry *entry;

	if (map) {
		free(map->block0);
		while (!LIST_EMPTY(&map->disk_order)) {
			entry = LIST_FIRST(&map->disk_order);
			LIST_REMOVE(entry, disk_entry);
			free(entry->dpme);
			free(entry);
		}
		free(map);
	}
}

int
read_partition_map(struct partition_map *map)
{
	struct entry *cur, *nextcur;
	struct dpme *dpme;
	int ix;
	uint32_t limit, base, next, nextbase;

	limit = 1; /* There has to be at least one, which has actual value. */
	for (ix = 1; ix <= limit; ix++) {
		dpme = malloc(sizeof(struct dpme));
		if (dpme == NULL) {
			warn("can't allocate memory for partition entry");
			return 1;
		}
		if (read_dpme(map->fd, ix, dpme) == 0) {
			warnx("Can't read block %u from '%s'", ix, map->name);
			free(dpme);
			return 1;
		}
		if (dpme->dpme_signature != DPME_SIGNATURE) {
			warnx("Invalid signature on block %d. Expected %x, "
			    "got %x", ix, DPME_SIGNATURE,
			    dpme->dpme_signature);
			free(dpme);
			return 1;
		}
		if (ix == 1)
			limit = dpme->dpme_map_entries;
		if (limit != dpme->dpme_map_entries) {
			warnx("Invalid entry count on block %d. "
			    "Expected %d, got %d", ix, limit,
			    dpme->dpme_map_entries);
			free(dpme);
			return 1;
		}
		if (dpme->dpme_lblock_start >= dpme->dpme_pblocks) {
			warnx("\tlogical start (%u) >= block count"
			    "count (%u).", dpme->dpme_lblock_start,
			    dpme->dpme_pblocks);
			free(dpme);
			return 1;
		}
		if (dpme->dpme_lblocks > dpme->dpme_pblocks -
			dpme->dpme_lblock_start) {
			warnx("\tlogical blocks (%u) > available blocks (%u).",
			    dpme->dpme_lblocks,
			    dpme->dpme_pblocks - dpme->dpme_lblock_start);
			free(dpme);
			return 1;
		}

		if (add_data_to_map(dpme, ix, map) == 0) {
			free(dpme);
			return 1;
		}
	}

	/* Traverse base_order looking for
	 *
	 * 1) Overlapping partitions
	 * 2) Unmapped space
	 */
	LIST_FOREACH(cur, &map->base_order, base_entry) {
		base = cur->dpme->dpme_pblock_start;
		next = base + cur->dpme->dpme_pblocks;
		if (base >= map->media_size ||
		    next < base ||
		    next > map->media_size) {
			warnx("Partition extends past end of disk: %u -> %u",
			    base, next);
		}
		nextcur = LIST_NEXT(cur, base_entry);
		if (nextcur)
			nextbase = nextcur->dpme->dpme_pblock_start;
		else
			nextbase = map->media_size;
		if (next != nextbase)
			warnx("Unmapped pblocks: %u -> %u", next, nextbase);
		if (next > nextbase)
			warnx("Partition %ld overlaps next partition",
			    cur->disk_address);
	}

	return 0;
}


void
write_partition_map(struct partition_map *map)
{
	struct entry *entry;
	int result;

	result = write_block0(map->fd, map->block0);
	if (result == 0)
		warn("Unable to write block zero");

	LIST_FOREACH(entry, &map->disk_order, disk_entry) {
		result = write_dpme(map->fd, entry->disk_address, entry->dpme);
		if (result == 0)
			warn("Unable to write block %ld", entry->disk_address);
	}
}


int
add_data_to_map(struct dpme *dpme, long ix, struct partition_map *map)
{
	struct entry *entry;

	entry = malloc(sizeof(struct entry));
	if (entry == NULL) {
		warn("can't allocate memory for map entries");
		return 0;
	}
	entry->disk_address = ix;
	entry->the_map = map;
	entry->dpme = dpme;
	entry->contains_driver = contains_driver(entry);

	insert_in_disk_order(entry);
	insert_in_base_order(entry);

	map->blocks_in_map++;
	if (map->maximum_in_map < 0) {
		if (strncasecmp(dpme->dpme_type, kMapType, DPISTRLEN) == 0)
			map->maximum_in_map = dpme->dpme_pblocks;
	}
	return 1;
}

struct partition_map *
create_partition_map(int fd, char *name, u_int64_t mediasz, uint32_t sectorsz)
{
	struct partition_map *map;
	struct dpme *dpme;

	map = malloc(sizeof(struct partition_map));
	if (map == NULL) {
		warn("can't allocate memory for open partition map");
		return NULL;
	}
	map->name = name;
	map->fd = fd;
	map->changed = 1;
	LIST_INIT(&map->disk_order);
	LIST_INIT(&map->base_order);

	map->physical_block = sectorsz;

	map->blocks_in_map = 0;
	map->maximum_in_map = -1;
	map->media_size = mediasz;

	map->block0 = calloc(1, sizeof(struct block0));
	if (map->block0 == NULL) {
		warn("can't allocate memory for block zero buffer");
	} else {
		coerce_block0(map);

		dpme = calloc(1, sizeof(struct dpme));
		if (dpme == NULL) {
			warn("can't allocate memory for disk buffers");
		} else {
			dpme->dpme_signature = DPME_SIGNATURE;
			dpme->dpme_map_entries = 1;
			dpme->dpme_pblock_start = 1;
			dpme->dpme_pblocks = map->media_size - 1;
			strlcpy(dpme->dpme_type, kFreeType,
			    sizeof(dpme->dpme_type));
			dpme_init_flags(dpme);

			if (add_data_to_map(dpme, 1, map) == 0) {
				free(dpme);
			} else {
				add_partition_to_map("Apple", kMapType,
				    1, (map->media_size <= 128 ? 2 : 63), map);
				return map;
			}
		}
	}

	free_partition_map(map);
	return NULL;
}


int
coerce_block0(struct partition_map *map)
{
	struct block0 *p;

	p = map->block0;
	if (p->sbSig != BLOCK0_SIGNATURE) {
		p->sbSig = BLOCK0_SIGNATURE;
		p->sbBlkSize = map->physical_block;
		p->sbBlkCount = map->media_size;
		p->sbDevType = 0;
		p->sbDevId = 0;
		p->sbData = 0;
		p->sbDrvrCount = 0;
	}
	return 0;
}


int
add_partition_to_map(const char *name, const char *dptype, uint32_t base,
    uint32_t length, struct partition_map *map)
{
	struct entry *cur;
	struct dpme *dpme;
	enum add_action act;
	int limit;
	uint32_t adjusted_base = 0;
	uint32_t adjusted_length = 0;
	uint32_t new_base = 0;
	uint32_t new_length = 0;

	/* find a block that starts includes base and length */
	LIST_FOREACH(cur, &map->base_order, base_entry) {
		if (cur->dpme->dpme_pblock_start <= base &&
		    (base + length) <=
		    (cur->dpme->dpme_pblock_start + cur->dpme->dpme_pblocks))
			break;
	}
	/* if it is not Extra then punt */
	if (cur == NULL ||
	    strncasecmp(cur->dpme->dpme_type, kFreeType, DPISTRLEN) != 0) {
		printf("requested base and length is not "
		       "within an existing free partition\n");
		return 0;
	}
	/* figure out what to do and sizes */
	dpme = cur->dpme;
	if (dpme->dpme_pblock_start == base) {
		/* replace or add */
		if (dpme->dpme_pblocks == length) {
			act = kReplace;
		} else {
			act = kAdd;
			adjusted_base = base + length;
			adjusted_length = dpme->dpme_pblocks - length;
		}
	} else {
		/* split or add */
		if (dpme->dpme_pblock_start + dpme->dpme_pblocks == base +
		    length) {
			act = kAdd;
			adjusted_base = dpme->dpme_pblock_start;
			adjusted_length = base - adjusted_base;
		} else {
			act = kSplit;
			new_base = dpme->dpme_pblock_start;
			new_length = base - new_base;
			adjusted_base = base + length;
			adjusted_length = dpme->dpme_pblocks - (length +
			    new_length);
		}
	}
	/* if the map will overflow then punt */
	if (map->maximum_in_map < 0)
		limit = map->media_size;
	else
		limit = map->maximum_in_map;
	if (map->blocks_in_map + act > limit) {
		printf("the map is not big enough\n");
		return 0;
	}
	dpme = create_dpme(name, dptype, base, length);
	if (dpme == NULL)
		return 0;

	if (act == kReplace) {
		free(cur->dpme);
		cur->dpme = dpme;
	} else {
		/* adjust this block's size */
		cur->dpme->dpme_pblock_start = adjusted_base;
		cur->dpme->dpme_pblocks = adjusted_length;
		cur->dpme->dpme_lblocks = adjusted_length;
		/* insert new with block address equal to this one */
		if (add_data_to_map(dpme, cur->disk_address, map) == 0) {
			free(dpme);
		} else if (act == kSplit) {
			dpme = create_dpme("", kFreeType, new_base, new_length);
			if (dpme != NULL) {
				/*
				 * insert new with block address equal to
				 * this one
				 */
				if (add_data_to_map(dpme, cur->disk_address,
				    map) == 0)
					free(dpme);
			}
		}
	}
	renumber_disk_addresses(map);
	map->changed = 1;
	return 1;
}


struct dpme*
create_dpme(const char *name, const char *dptype, uint32_t base,
    uint32_t length)
{
	struct dpme *dpme;

	dpme = calloc(1, sizeof(struct dpme));
	if (dpme == NULL) {
		warn("can't allocate memory for disk buffers");
	} else {
		dpme->dpme_signature = DPME_SIGNATURE;
		dpme->dpme_map_entries = 1;
		dpme->dpme_pblock_start = base;
		dpme->dpme_pblocks = length;
		strlcpy(dpme->dpme_name, name, sizeof(dpme->dpme_name));
		strlcpy(dpme->dpme_type, dptype, sizeof(dpme->dpme_type));
		dpme->dpme_lblock_start = 0;
		dpme->dpme_lblocks = dpme->dpme_pblocks;
		dpme_init_flags(dpme);
	}
	return dpme;
}

void
dpme_init_flags(struct dpme *dpme)
{
	if (strncasecmp(dpme->dpme_type, kFreeType, DPISTRLEN) == 0)
		dpme->dpme_flags = 0;
	else if (strncasecmp(dpme->dpme_type, kMapType, DPISTRLEN) == 0)
		dpme->dpme_flags = DPME_VALID | DPME_ALLOCATED;
	else if (strncasecmp(dpme->dpme_type, kHFSType, DPISTRLEN) == 0)
		dpme->dpme_flags = APPLE_HFS_FLAGS_VALUE;
	else
		dpme->dpme_flags = DPME_VALID | DPME_ALLOCATED |
		    DPME_READABLE | DPME_WRITABLE;
}

void
renumber_disk_addresses(struct partition_map *map)
{
	struct entry *cur;
	long ix;

	/* reset disk addresses */
	ix = 1;
	LIST_FOREACH(cur, &map->disk_order, disk_entry) {
		cur->disk_address = ix++;
		cur->dpme->dpme_map_entries = map->blocks_in_map;
	}
}

void
delete_partition_from_map(struct entry *entry)
{
	struct dpme *dpme;

	if (strncasecmp(entry->dpme->dpme_type, kMapType, DPISTRLEN) == 0) {
		printf("Can't delete entry for the map itself\n");
		return;
	}
	if (strncasecmp(entry->dpme->dpme_type, kFreeType, DPISTRLEN) == 0) {
		printf("Can't delete entry for free space\n");
		return;
	}
	if (entry->contains_driver) {
		printf("This program can't install drivers\n");
		if (get_okay("are you sure you want to delete this driver? "
		    "[n/y]: ", 0) != 1) {
			return;
		}
		remove_driver(entry);	/* update block0 if necessary */
	}

	dpme = entry->dpme;
	memset(dpme->dpme_name, 0, sizeof(dpme->dpme_name));
	memset(dpme->dpme_type, 0, sizeof(dpme->dpme_type));
	strlcpy(dpme->dpme_type, kFreeType, sizeof(dpme->dpme_type));
	dpme_init_flags(dpme);

	combine_entry(entry);
	renumber_disk_addresses(entry->the_map);
	entry->the_map->changed = 1;
}


int
contains_driver(struct entry *entry)
{
	struct partition_map *map;
	struct block0  *p;
	struct ddmap   *m;
	int i;
	uint32_t start;

	map = entry->the_map;
	p = map->block0;

	if (p->sbDrvrCount > 0) {
		m = p->sbDDMap;
		for (i = 0; i < p->sbDrvrCount; i++) {
			start = m[i].ddBlock;
			if (entry->dpme->dpme_pblock_start <= start &&
			    (start + m[i].ddSize) <=
			    (entry->dpme->dpme_pblock_start +
			    entry->dpme->dpme_pblocks))
				return 1;
		}
	}
	return 0;
}


void
combine_entry(struct entry *entry)
{
	struct entry *p;
	uint32_t end;

	if (entry == NULL ||
	    strncasecmp(entry->dpme->dpme_type, kFreeType, DPISTRLEN) != 0)
		return;

	p = LIST_NEXT(entry, base_entry);
	if (p != NULL) {
		if (strncasecmp(p->dpme->dpme_type, kFreeType, DPISTRLEN) !=
		    0) {
			/* next is not free */
		} else if (entry->dpme->dpme_pblock_start +
		    entry->dpme->dpme_pblocks != p->dpme->dpme_pblock_start) {
			/* next is not contiguous (XXX this is bad) */
			printf("next entry is not contiguous\n");
			/* start is already minimum */
			/* new end is maximum of two ends */
			end = p->dpme->dpme_pblock_start +
			    p->dpme->dpme_pblocks;
			if (end > entry->dpme->dpme_pblock_start +
			    entry->dpme->dpme_pblocks) {
				entry->dpme->dpme_pblocks = end -
				    entry->dpme->dpme_pblock_start;
			}
			entry->dpme->dpme_lblocks = entry->dpme->dpme_pblocks;
			delete_entry(p);
		} else {
			entry->dpme->dpme_pblocks += p->dpme->dpme_pblocks;
			entry->dpme->dpme_lblocks = entry->dpme->dpme_pblocks;
			delete_entry(p);
		}
	}

	LIST_FOREACH(p, &entry->the_map->base_order, base_entry) {
		if (LIST_NEXT(p, base_entry) == entry)
			break;
	}
	if (p != NULL) {
		if (strncasecmp(p->dpme->dpme_type, kFreeType, DPISTRLEN) !=
		    0) {
			/* previous is not free */
		} else if (p->dpme->dpme_pblock_start + p->dpme->dpme_pblocks !=
		    entry->dpme->dpme_pblock_start) {
			/* previous is not contiguous (XXX this is bad) */
			printf("previous entry is not contiguous\n");
			/* new end is maximum of two ends */
			end = p->dpme->dpme_pblock_start +
			    p->dpme->dpme_pblocks;
			if (end < entry->dpme->dpme_pblock_start +
			    entry->dpme->dpme_pblocks) {
				end = entry->dpme->dpme_pblock_start +
				    entry->dpme->dpme_pblocks;
			}
			entry->dpme->dpme_pblocks = end -
			    p->dpme->dpme_pblock_start;
			/* new start is previous entry's start */
			entry->dpme->dpme_pblock_start =
			    p->dpme->dpme_pblock_start;
			entry->dpme->dpme_lblocks = entry->dpme->dpme_pblocks;
			delete_entry(p);
		} else {
			entry->dpme->dpme_pblock_start =
			    p->dpme->dpme_pblock_start;
			entry->dpme->dpme_pblocks += p->dpme->dpme_pblocks;
			entry->dpme->dpme_lblocks = entry->dpme->dpme_pblocks;
			delete_entry(p);
		}
	}
	entry->contains_driver = contains_driver(entry);
}


void
delete_entry(struct entry *entry)
{
	struct partition_map *map;

	map = entry->the_map;
	map->blocks_in_map--;

	LIST_REMOVE(entry, disk_entry);
	LIST_REMOVE(entry, base_entry);

	free(entry->dpme);
	free(entry);
}


struct entry *
find_entry_by_disk_address(long ix, struct partition_map *map)
{
	struct entry *cur;

	LIST_FOREACH(cur, &map->disk_order, disk_entry) {
		if (cur->disk_address == ix)
			break;
	}
	return cur;
}


struct entry *
find_entry_by_type(const char *type_name, struct partition_map *map)
{
	struct entry *cur;

	LIST_FOREACH(cur, &map->base_order, base_entry) {
		if (strncasecmp(cur->dpme->dpme_type, type_name, DPISTRLEN) ==
		    0)
			break;
	}
	return cur;
}

struct entry *
find_entry_by_base(uint32_t base, struct partition_map *map)
{
	struct entry *cur;

	LIST_FOREACH(cur, &map->base_order, base_entry) {
		if (cur->dpme->dpme_pblock_start == base)
			break;
	}
	return cur;
}


void
move_entry_in_map(long index1, long index2, struct partition_map *map)
{
	struct entry *p1, *p2;

	if (index1 == index2)
		return;

	if (index1 == 1 || index2 == 1) {
		printf("Partition #1 cannot be moved\n");
		return;
	}
	p1 = find_entry_by_disk_address(index1, map);
	if (p1 == NULL) {
		printf("Partition #%ld not found\n", index1);
		return;
	}
	p2 = find_entry_by_disk_address(index2, map);
	if (p2 == NULL) {
		printf("Partition #%ld not found\n", index2);
		return;
	}

	LIST_REMOVE(p1, disk_entry);
	LIST_REMOVE(p2, disk_entry);

	p1->disk_address = index2;
	p2->disk_address = index1;

	insert_in_disk_order(p1);
	insert_in_disk_order(p2);

	renumber_disk_addresses(map);
	map->changed = 1;
}


void
insert_in_disk_order(struct entry *entry)
{
	struct partition_map *map;
	struct entry *cur;

	/* find position in disk list & insert */
	map = entry->the_map;
	if (LIST_EMPTY(&map->disk_order)) {
		LIST_INSERT_HEAD(&map->disk_order, entry, disk_entry);
		return;
	}

	LIST_FOREACH(cur, &map->disk_order, disk_entry) {
		if (cur->disk_address >= entry->disk_address) {
			LIST_INSERT_BEFORE(cur, entry, disk_entry);
			return;
		}
		if (LIST_NEXT(cur, disk_entry) == NULL) {
			LIST_INSERT_AFTER(cur, entry, disk_entry);
			return;
		}
	}
}


void
insert_in_base_order(struct entry *entry)
{
	struct partition_map *map;
	struct entry *cur;
	uint32_t start;

	/* find position in base list & insert */
	map = entry->the_map;
	if (LIST_EMPTY(&map->base_order)) {
		LIST_INSERT_HEAD(&map->base_order, entry, base_entry);
		return;
	}

	start = entry->dpme->dpme_pblock_start;
	LIST_FOREACH(cur, &map->base_order, base_entry) {
		if (start <= cur->dpme->dpme_pblock_start) {
			LIST_INSERT_BEFORE(cur, entry, base_entry);
			return;
		}
		if (LIST_NEXT(cur, base_entry) == NULL) {
			LIST_INSERT_AFTER(cur, entry, base_entry);
			return;
		}
	}
}


void
resize_map(long new_size, struct partition_map *map)
{
	struct entry *entry;
	struct entry *next;
	int incr;

	entry = find_entry_by_type(kMapType, map);

	if (entry == NULL) {
		printf("Couldn't find entry for map!\n");
		return;
	}
	if (new_size == entry->dpme->dpme_pblocks)
		return;

	next = LIST_NEXT(entry, base_entry);

	if (new_size < entry->dpme->dpme_pblocks) {
		/* make it smaller */
		if (next == NULL ||
		    strncasecmp(next->dpme->dpme_type, kFreeType, DPISTRLEN) !=
		    0)
			incr = 1;
		else
			incr = 0;
		if (new_size < map->blocks_in_map + incr) {
			printf("New size would be too small\n");
			return;
		}
		goto doit;
	}
	/* make it larger */
	if (next == NULL ||
	    strncasecmp(next->dpme->dpme_type, kFreeType, DPISTRLEN) != 0) {
		printf("No free space to expand into\n");
		return;
	}
	if (entry->dpme->dpme_pblock_start + entry->dpme->dpme_pblocks
	    != next->dpme->dpme_pblock_start) {
		printf("No contiguous free space to expand into\n");
		return;
	}
	if (new_size > entry->dpme->dpme_pblocks + next->dpme->dpme_pblocks) {
		printf("No enough free space\n");
		return;
	}
doit:
	entry->dpme->dpme_type[0] = 0;
	delete_partition_from_map(entry);
	add_partition_to_map("Apple", kMapType, 1, new_size, map);
	map->maximum_in_map = new_size;
}


void
remove_driver(struct entry *entry)
{
	struct block0 *p;
	struct ddmap *m;
	int i, j;
	uint32_t start;

	p = entry->the_map->block0;

	/*
	 * compute the factor to convert the block numbers in block0
	 * into partition map block numbers.
	 */
	if (p->sbDrvrCount > 0) {
		m = p->sbDDMap;
		for (i = 0; i < p->sbDrvrCount; i++) {
			start = m[i].ddBlock;

			/*
			 * zap the driver if it is wholly contained in the
			 * partition
			 */
			if (entry->dpme->dpme_pblock_start <= start &&
			    (start + m[i].ddSize) <=
			    (entry->dpme->dpme_pblock_start
				+ entry->dpme->dpme_pblocks)) {
				/* delete this driver */
				/*
				 * by copying down later ones and zapping the
				 * last
				 */
				for (j = i + 1; j < p->sbDrvrCount; j++, i++) {
					m[i].ddBlock = m[i].ddBlock;
					m[i].ddSize = m[j].ddSize;
					m[i].ddType = m[j].ddType;
				}
				m[i].ddBlock = 0;
				m[i].ddSize = 0;
				m[i].ddType = 0;
				p->sbDrvrCount -= 1;
				return;	/* XXX if we continue we will delete
					 * other drivers? */
			}
		}
	}
}
