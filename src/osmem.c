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
		used_blocks = block->next;
	}

	if (block == free_blocks) {
		free_blocks = block->next;
	}

	if (block->prev != NULL) {
		block->prev->next = block->next;
	}

	if (block->next != NULL) {
		block->next->prev = block->prev;
	}

	block->prev = block->next = NULL;
}

void print_free_blocks() {
	printf("FREE BLOCKS: \n");
	struct block_meta *curr = free_blocks;
	while (curr != NULL) {
		printf("%u with size: %u\n", curr, curr->size);
		curr = curr->next;
	}
}

void print_used_blocks() {
	printf("USED BLOCKS: \n");
	struct block_meta *curr = used_blocks;
	while (curr != NULL) {
		printf("%u with size: %u\n", curr, curr->size);
		curr = curr->next;
	}
}

void coalesce_free_blocks() {
	struct block_meta *curr = free_blocks;

	while (curr->next != NULL) {
		if ((size_t)curr + curr->size + METADATA_SIZE ==
			(size_t)curr->next) {
			curr->size += curr->next->size + METADATA_SIZE;
			remove_block(curr->next);
			continue;
		}

		curr = curr->next;
	}
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
	block->status = STATUS_ALLOC;

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
	block->status = STATUS_FREE;
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

	if (curr < block) {
		block->next = curr->next;

		if (block->next != NULL) {
			block->next->prev = block;
		}

		curr->next = block;
		block->prev = curr;
	} else {
		block->next = curr;
		block->prev = curr->prev;

		if (block->prev != NULL) {
			block->prev->next = block;
		}

		curr->prev = block;

		if (curr == free_blocks)
			free_blocks = block;
	}
}

// TODO: make free_blocks circular
// TODO: use DIE for sbrk and similar function calls
void *reuse_block_brk(size_t size) {
	struct block_meta *curr = free_blocks;

	while (curr->next != NULL) {
		curr = curr->next;
	}

	struct block_meta *last_used_block = used_blocks;
	while (last_used_block->next != NULL && last_used_block < curr) {
		last_used_block = last_used_block->next;
	}

	size_t size_to_add = size - curr->size;
	size_t payload_padding = calculate_padding((size_t)curr + size);

	if (curr < last_used_block) {
		long free_memory = last_used_block - curr - METADATA_SIZE;
		// TODO: maybe rewrite this
		if ((long)(size_to_add + payload_padding) < free_memory) {
			remove_block(curr);
			// added padding
			curr->size = size + payload_padding;
			add_used_block(curr);
			return curr;
		}
		// this means that we can't expand the last free block, and we want to
		// make a new one with the method already present in increase_brk
		return (void *)-1;
	}

	// curr block is the last allocated block with brk
	sbrk(size_to_add + payload_padding);

	remove_block(curr);
	curr->size = size + payload_padding;
	add_used_block(curr);
	return curr;
}

void *increase_brk(size_t size) {
	if (free_blocks != NULL) {
		void *return_value = reuse_block_brk(size);
		if ((long)return_value != -1)
			return return_value;
	}

	size_t payload_padding = calculate_padding(size);

	// TODO: might need to get current brk position with sbrk(0) to pad the
	//  metadata too

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
	if (size + METADATA_SIZE < MMAP_THRESHOLD) {
		if (used_blocks == NULL && free_blocks == NULL) {
			prealloc_heap();
		}

		struct block_meta *free_block = find_place_brk(size);

		if (free_block == NULL) {
			printf("Not enough free memory in malloc while using brk!\n");
			printf("Trying to increase brk...\n");
			new_used_block = increase_brk(size);

			return ((void *)new_used_block + METADATA_SIZE);
		}

		new_used_block = free_block;

		struct block_meta *new_free_block = (void *)free_block +
											METADATA_SIZE + size;

		size_t payload_padding = calculate_padding((size_t)new_free_block);

		new_free_block = (void *)new_free_block + payload_padding;

		long new_free_block_size =
				free_block->size - size - payload_padding;

		new_used_block->status = STATUS_ALLOC;

		// enough space for another free_block
		if (new_free_block_size >= METADATA_SIZE + 1) {
			new_free_block->size = new_free_block_size - METADATA_SIZE;

			new_free_block->status = STATUS_FREE;
			new_free_block->next = new_free_block->prev = NULL;

			new_used_block->size = size + payload_padding;
			add_free_block(new_free_block);
		}
		add_used_block(new_used_block);
	} else {
		size_t payload_padding = calculate_padding(size);
		new_used_block = mmap(NULL, size + METADATA_SIZE + payload_padding,
							  PROT_READ | PROT_WRITE,
							  MAP_PRIVATE | MAP_ANON,
							  -1, 0);

		new_used_block->size = size;
		new_used_block->status = STATUS_MAPPED;
	}

	return ((void *)new_used_block + METADATA_SIZE);
}

void os_free(void *ptr) {
	if (ptr == 0)
		return;

	struct block_meta *used_block = (ptr - METADATA_SIZE);
	if (used_block->status == STATUS_ALLOC) {
		remove_block(used_block);
		add_free_block(used_block);
		coalesce_free_blocks();
	} else {
		size_t payload_padding = calculate_padding(used_block->size);
		munmap(used_block,
			   METADATA_SIZE + used_block->size + payload_padding);
	}
}

void *os_calloc(size_t nmemb, size_t size) {
	int total_size = nmemb * size;
	if (total_size == 0) {
		return NULL;
	}

	struct block_meta *new_used_block = NULL;
	if (total_size + METADATA_SIZE < getpagesize()) {
		if (used_blocks == NULL && free_blocks == NULL) {
			prealloc_heap();
		}

		struct block_meta *free_block = find_place_brk(total_size);
		if (free_block == NULL) {
			printf("Not enough free memory in malloc while using brk!\n");
			printf("Trying to increase brk...\n");
			new_used_block = increase_brk(total_size);
			u_char *bytes_new_used_block = (u_char *)new_used_block;

			for (size_t i = 0; i < new_used_block->size; i++) {
				bytes_new_used_block[i + METADATA_SIZE] = 0;
			}

			return ((void *)new_used_block + METADATA_SIZE);
		}

		new_used_block = free_block;

		struct block_meta *new_free_block = (void *)free_block +
											METADATA_SIZE + total_size;

		size_t payload_padding = calculate_padding((size_t)new_free_block);

		new_free_block = (void *)new_free_block + payload_padding;

		long new_free_block_size =
				free_block->size - total_size - payload_padding;

		new_used_block->status = STATUS_ALLOC;

		// enough space for another free_block
		if (new_free_block_size >= METADATA_SIZE + 1) {
			new_free_block->size = new_free_block_size - METADATA_SIZE;

			new_free_block->status = STATUS_FREE;
			new_free_block->next = new_free_block->prev = NULL;

			new_used_block->size = total_size + payload_padding;
			add_free_block(new_free_block);
		}
		add_used_block(new_used_block);
	} else {
		size_t payload_padding = calculate_padding(total_size);
		new_used_block = mmap(NULL,
							  total_size + METADATA_SIZE + payload_padding,
							  PROT_READ | PROT_WRITE,
							  MAP_PRIVATE | MAP_ANON,
							  -1, 0);

		new_used_block->size = total_size;
		new_used_block->status = STATUS_MAPPED;
	}

	u_char *bytes_new_used_block = (u_char *)new_used_block;

	for (size_t i = 0; i < new_used_block->size; i++) {
		bytes_new_used_block[i + METADATA_SIZE] = 0;
	}

	return ((void *)new_used_block + METADATA_SIZE);
}

void *os_realloc(void *ptr, size_t size) {
	/* TODO: Implement os_realloc */
	return NULL;
}
