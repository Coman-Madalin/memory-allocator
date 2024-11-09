// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include <errno.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static struct block_meta *used_blocks = NULL;

static struct block_meta *free_blocks = NULL;

void prealloc_heap() {
	free_blocks = sbrk(128 * MULT_KB);

	if ((long)free_blocks == -1) {
		printf("An error has ocurred while preallocating the heap");
		exit(-1);
	}

	free_blocks->size = 128 * MULT_KB;
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
		if (curr->size >= size + METADATA_SIZE) {
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
			printf("Not enough free memory in malloc while using brk");
			exit(-1);
		}

		new_used_block = free_block;
		struct block_meta *new_free_block = free_block + METADATA_SIZE + size;

		new_free_block->size = free_block->size - 2 * METADATA_SIZE - size;
		new_free_block->status = STATUS_FREE;
		new_free_block->next = new_free_block->prev = NULL;

		new_used_block->size = size;
		new_used_block->status = STATUS_ALLOC;

		add_free_block(new_free_block);
	} else {
		new_used_block = mmap(NULL, size + METADATA_SIZE,
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

	munmap(used_block, used_block->size + METADATA_SIZE);
}

void *os_calloc(size_t nmemb, size_t size) {
	/* TODO: Implement os_calloc */
	return NULL;
}

void *os_realloc(void *ptr, size_t size) {
	/* TODO: Implement os_realloc */
	return NULL;
}
