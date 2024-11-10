// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include <errno.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static struct block_meta *used_blocks = NULL;

static struct block_meta *free_blocks = NULL;

size_t calculate_padding(size_t size) {
	size_t padding = 8 - size % 8;
	if (padding == 8)
		padding = 0;

	return padding;
}

void prealloc_heap() {
	free_blocks = sbrk(128 * MULT_KB);

	if ((long)free_blocks == -1) {
		printf("An error has ocurred while preallocating the heap");
		exit(-1);
	}

	free_blocks->size = 128 * MULT_KB - METADATA_SIZE;
	free_blocks->status = STATUS_FREE;
	free_blocks->prev = NULL;
	free_blocks->next = NULL;
}

void remove_block(struct block_meta *block) {
	if (block == NULL) {
		return;
	}

	if (block == used_blocks) {
		used_blocks = NULL;
	}

	if (block == free_blocks) {
		free_blocks = NULL;
	}

	if (block->prev != NULL) {
		block->prev->next = block->next;
	}

	if (block->next != NULL) {
		block->next->prev = block->prev;
	}

	block->prev = block->next = NULL;
}

void *find_place_brk(size_t size) {
	struct block_meta *curr = free_blocks;

	while (curr != NULL) {
		if (curr->size >= size) {
			break;
		}

		curr = curr->next;
	}

	remove_block(curr);
	return curr;
}

void add_used_block(struct block_meta *block) {
	block->prev = block->next = NULL;

	if (used_blocks == NULL) {
		used_blocks = block;
		return;
	}

	struct block_meta *curr = used_blocks;

	while (curr->next != NULL) {
		if (curr->next > block) {
			break;
		}
		curr = curr->next;
	}

	block->next = curr->next;

	if (block->next != NULL) {
		block->next->prev = block;
	}

	curr->next = block;
	block->prev = curr;
}

void add_free_block(struct block_meta *block) {
	block->prev = block->next = NULL;

	if (free_blocks == NULL) {
		free_blocks = block;
		return;
	}

	struct block_meta *curr = free_blocks;

	while (curr->next != NULL) {
		if (curr->next > block) {
			break;
		}
		curr = curr->next;
	}

	block->next = curr->next;

	if (block->next != NULL) {
		block->next->prev = block;
	}

	curr->next = block;
	block->prev = curr;
}

// TODO: make free_blocks circular
// TODO: use DIE for sbrk and similar function calls

void *reuse_block_brk(size_t size) {
	struct block_meta *curr = free_blocks;

	while (curr->next != NULL) {
		curr = curr->next;
	}

	size_t size_to_add = size - curr->size;
	size_t payload_padding = calculate_padding((size_t)curr + size);

	printf("BRK TRY TO ALLOCATE: %d\n\n", size_to_add + payload_padding);
	sbrk(size_to_add + payload_padding + 1);

	return curr;
}

void *increase_brk(size_t size) {
	if (free_blocks != NULL)
		return reuse_block_brk(size);

	size_t payload_padding = calculate_padding(size);

	// TODO: might need to get current brk position with sbrk(0) to pad the
	//  metadata too

	printf("BRK TRY TO ALLOCATE: %d\n\n", size + payload_padding);
	struct block_meta *used_block = sbrk(METADATA_SIZE
										 + size + payload_padding);
	used_block->size = size + payload_padding;
	used_block->status = STATUS_ALLOC;

	add_used_block(used_block);

	return used_block;
}

// TODO: rewrite this at some point more efficiently
void *os_malloc(size_t size) {
	if (size == 0) {
		return NULL;
	}

	struct block_meta *new_used_block = NULL;
	if (size < MMAP_THRESHOLD) {
		if (used_blocks == NULL && free_blocks == NULL) {
			prealloc_heap();
		}

		struct block_meta *free_block = find_place_brk(size);

		if (free_block == NULL) {
			printf("Not enough free memory in malloc while using brk!\n");
			printf("Trying to increase brk...\n");
			struct block_meta *used_block = increase_brk(size);
			return ((void *)used_block + METADATA_SIZE);
		}

		new_used_block = free_block;

		struct block_meta *new_free_block = (void *)free_block +
											METADATA_SIZE + size;

		size_t payload_padding = calculate_padding((size_t)new_free_block);

		new_free_block = (void *)new_free_block + payload_padding;

		long new_free_block_size = free_block + free_block->size +
								   METADATA_SIZE - new_used_block -
								   METADATA_SIZE - size - payload_padding;

		new_used_block->status = STATUS_ALLOC;

		// enough space for another free_block
		if (new_free_block_size >= METADATA_SIZE + 1) {
			new_free_block->size = free_block->size - METADATA_SIZE - size;
			new_free_block->status = STATUS_FREE;
			new_free_block->next = new_free_block->prev = NULL;

			new_used_block->size = size;
			add_free_block(new_free_block);
		}
	} else {
		size_t payload_padding = calculate_padding(size);
		new_used_block = mmap(NULL, size + METADATA_SIZE + payload_padding,
							  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
							  -1, 0);

		new_used_block->size = size;
		new_used_block->status = STATUS_MAPPED;
	}

	add_used_block(new_used_block);
	return ((void *)new_used_block + METADATA_SIZE);
}

void os_free(void *ptr) {
	if (ptr == 0)
		return;

	struct block_meta *used_block = (ptr - METADATA_SIZE);
	if (used_block->status == STATUS_ALLOC)
		return;

	size_t payload_padding = calculate_padding(used_block->size);
	munmap(used_block, METADATA_SIZE + used_block->size + payload_padding);
}

void *os_calloc(size_t nmemb, size_t size) {
	/* TODO: Implement os_calloc */
	return NULL;
}

void *os_realloc(void *ptr, size_t size) {
	/* TODO: Implement os_realloc */
	return NULL;
}
