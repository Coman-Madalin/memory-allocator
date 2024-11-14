// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define METADATA_SIZE        (sizeof(struct block_meta))
#define MMAP_THRESHOLD        (128 * 1024)
#define MULT_KB            1024

static struct block_meta *used_blocks;

static struct block_meta *free_blocks;

size_t calculate_padding(size_t size)
{
	size_t padding = 8 - size % 8;

	if (padding == 8)
		padding = 0;

	return padding;
}

void prealloc_heap(void)
{
	free_blocks = sbrk(128 * MULT_KB);
	DIE(free_blocks == (void *)-1, "brk failed!");

	free_blocks->size = 128 * MULT_KB - METADATA_SIZE;
	free_blocks->status = STATUS_FREE;
	free_blocks->prev = NULL;
	free_blocks->next = NULL;
}

void remove_block(struct block_meta *block)
{
	if (block == NULL)
		return;

	if (block == used_blocks)
		used_blocks = block->next;

	if (block == free_blocks)
		free_blocks = block->next;

	if (block->prev != NULL)
		block->prev->next = block->next;

	if (block->next != NULL)
		block->next->prev = block->prev;

	block->prev = block->next = NULL;
}

void *get_next_block(struct block_meta *block)
{
	struct block_meta *last_used_block = used_blocks;

	if (last_used_block != NULL)
		while (last_used_block->next != NULL && last_used_block <= block)
			last_used_block = last_used_block->next;

	if (free_blocks == NULL)
		return last_used_block;

	struct block_meta *last_free_block = free_blocks;

	while (last_free_block->next != NULL && last_free_block <= block)
		last_free_block = last_free_block->next;

	if (last_used_block == NULL)
		return last_free_block;

	if (last_used_block <= block) {
		if (last_free_block <= block)
			return last_free_block;

		return last_free_block;
	}

	if (last_free_block <= block)
		return last_used_block;

	if (last_used_block < last_free_block) {
		if (last_used_block <= block)
			return last_free_block;

		return last_used_block;
	}

	return last_free_block;
}

void coalesce_free_blocks(void)
{
	struct block_meta *curr = free_blocks;

	while (curr->next != NULL) {
		if ((size_t)curr + curr->size + METADATA_SIZE == (size_t)curr->next) {
			curr->size += curr->next->size + METADATA_SIZE;
			remove_block(curr->next);
			continue;
		}

		curr = curr->next;
	}
}

void *find_place_brk(size_t size)
{
	struct block_meta *curr = free_blocks;
	struct block_meta *most_efficient_block = curr;

	while (curr != NULL) {
		if (curr->size >= size) {
			if (curr->size < most_efficient_block->size ||
				most_efficient_block->size < size) {
				most_efficient_block = curr;
			}
		}
		curr = curr->next;
	}

	if (most_efficient_block != NULL && most_efficient_block->size < size)
		return NULL;

	remove_block(most_efficient_block);
	return most_efficient_block;
}

void add_used_block(struct block_meta *block)
{
	block->prev = block->next = NULL;
	block->status = STATUS_ALLOC;

	if (used_blocks == NULL) {
		used_blocks = block;
		return;
	}

	if (used_blocks > block) {
		block->next = used_blocks;
		used_blocks->prev = block;
		used_blocks = block;
		return;
	}

	struct block_meta *curr = used_blocks;

	while (curr->next != NULL) {
		if (curr->next > block)
			break;

		curr = curr->next;
	}

	block->next = curr->next;

	if (block->next != NULL)
		block->next->prev = block;

	curr->next = block;
	block->prev = curr;
}

void add_free_block(struct block_meta *block)
{
	block->status = STATUS_FREE;
	block->prev = block->next = NULL;

	if (free_blocks == NULL) {
		free_blocks = block;
		return;
	}

	if (free_blocks > block) {
		block->next = free_blocks;
		free_blocks->prev = block;
		free_blocks = block;
		return;
	}

	struct block_meta *curr = free_blocks;

	while (curr->next != NULL) {
		if (curr->next > block)
			break;

		curr = curr->next;
	}

	if (curr < block) {
		block->next = curr->next;

		if (block->next != NULL)
			block->next->prev = block;

		curr->next = block;
		block->prev = curr;
	} else {
		block->next = curr;
		block->prev = curr->prev;

		if (block->prev != NULL)
			block->prev->next = block;

		curr->prev = block;

		if (curr == free_blocks)
			free_blocks = block;
	}
}

void *reuse_block_brk(size_t size)
{
	struct block_meta *curr = free_blocks;

	while (curr->next != NULL)
		curr = curr->next;

	struct block_meta *last_used_block = used_blocks;

	while (last_used_block->next != NULL && last_used_block < curr)
		last_used_block = last_used_block->next;

	size_t size_to_add = size - curr->size;
	size_t payload_padding = calculate_padding((size_t)curr + size);

	if (curr < last_used_block) {
		long free_memory = last_used_block - curr - METADATA_SIZE;
		if ((long)(size_to_add + payload_padding) < free_memory) {
			remove_block(curr);
			curr->size = size + payload_padding;
			add_used_block(curr);
			return curr;
		}
		// this means that we can't expand the last free block, and we want to
		// make a new one with the method already present in increase_brk
		return (void *)-1;
	}

	// curr block is the last allocated block with brk
	void *result = sbrk((long)(size_to_add + payload_padding));
	DIE(result == (void *)-1, "brk failed!");

	remove_block(curr);
	curr->size = size + payload_padding;
	add_used_block(curr);
	return curr;
}

void *increase_brk(size_t size)
{
	if (free_blocks != NULL) {
		void *return_value = reuse_block_brk(size);

		if ((long)return_value != -1)
			return return_value;
	}

	size_t payload_padding = calculate_padding(size);

	struct block_meta *used_block = sbrk(METADATA_SIZE + size + payload_padding);
	DIE(used_block == (void *)-1, "brk failed!");

	used_block->size = size + payload_padding;
	used_block->status = STATUS_ALLOC;

	add_used_block(used_block);

	return used_block;
}

void *allocate_memory(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;

	struct block_meta *new_used_block = NULL;

	if (size + METADATA_SIZE < threshold) {
		if (used_blocks == NULL && free_blocks == NULL)
			prealloc_heap();

		struct block_meta *free_block = find_place_brk(size);

		if (free_block == NULL) {
			struct block_meta *used_block = increase_brk(size);

			return (void *)used_block + METADATA_SIZE;
		}

		new_used_block = free_block;

		struct block_meta *new_free_block = (void *)free_block + METADATA_SIZE + size;
		size_t payload_padding = calculate_padding((size_t)new_free_block);

		new_free_block = (void *)new_free_block + payload_padding;

		long new_free_block_size = free_block->size - size - payload_padding;

		new_used_block->status = STATUS_ALLOC;

		// enough space for another free_block
		if (new_free_block_size >= (long)METADATA_SIZE + 1) {
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
		DIE(new_used_block == (void *) -1, "mmap failed!");

		new_used_block->size = size;
		new_used_block->status = STATUS_MAPPED;
	}

	return (void *)new_used_block + METADATA_SIZE;
}

void *os_malloc(size_t size)
{
	return allocate_memory(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	if (ptr == 0)
		return;

	struct block_meta *used_block = (ptr - METADATA_SIZE);

	if (used_block->status == STATUS_ALLOC) {
		remove_block(used_block);
		add_free_block(used_block);
		coalesce_free_blocks();
	} else {
		size_t payload_padding = calculate_padding(used_block->size);

		int result = munmap(used_block, METADATA_SIZE + used_block->size + payload_padding);
		DIE(result == -1, "munmap failed!");
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total_size = nmemb * size;
	void *payload = allocate_memory(total_size, getpagesize());
	u_char *bytes_new_used_block = (u_char *)payload;

	if (payload == NULL)
		return NULL;

	for (size_t i = 0; i < total_size; i++)
		bytes_new_used_block[i] = 0;

	return payload;
}

void *os_realloc(void *ptr, size_t size)
{
	if (ptr == NULL)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *used_block = ptr - METADATA_SIZE;

	if (used_block->status == STATUS_FREE)
		return NULL;

	size_t payload_padding = calculate_padding(size);

	if (used_block->size == size + payload_padding)
		return ptr;

	if (used_block->status == STATUS_MAPPED) {
		struct block_meta *new_used_block = os_malloc(size) - METADATA_SIZE;

		u_char *bytes_new_used_block = (u_char *)new_used_block;
		u_char *bytes_used_block = (u_char *)used_block;

		size_t size_to_copy = size;

		if (size_to_copy > used_block->size)
			size_to_copy = used_block->size;

		for (size_t i = METADATA_SIZE; i < size_to_copy + METADATA_SIZE; i++)
			bytes_new_used_block[i] = bytes_used_block[i];

		os_free(ptr);

		return (void *)new_used_block + METADATA_SIZE;
	}

	long remaining_size = used_block->size - size - payload_padding;

	if (size < used_block->size) {
		if (remaining_size > (long)(METADATA_SIZE + 1)) {
			struct block_meta *new_free_block = (void *)used_block + METADATA_SIZE + size + payload_padding;

			new_free_block->size = remaining_size - METADATA_SIZE;
			add_free_block(new_free_block);
			used_block->size = size + payload_padding;
		}
		return ptr;
	}

	struct block_meta *next_block = get_next_block(used_block);

	// If we have enough space until the next block, we expand the used block
	if ((void *)used_block + METADATA_SIZE + size + payload_padding < (void *)next_block) {
		used_block->size = size + payload_padding;
		return ptr;
	}

	// the current block is the last allocated block
	if (used_block >= next_block) {
		long inverse_of_remaining_size = -remaining_size;

		void *result = sbrk(inverse_of_remaining_size);
		DIE(result == (void *)-1, "brk failed!");
		used_block->size = size + payload_padding;
		return ptr;
	}

	if (next_block->status == STATUS_FREE &&
		(void *)used_block + METADATA_SIZE + size + payload_padding <=
		(void *)next_block + METADATA_SIZE + next_block->size) {
		long free_size = (void *)next_block + next_block->size -
						 (void *)used_block - size - payload_padding;

		used_block->size = size + payload_padding;
		remove_block(next_block);

		if (free_size > (long)METADATA_SIZE + 1) {
			struct block_meta *new_free_block = (void *)used_block + METADATA_SIZE + size + payload_padding;

			new_free_block->size = free_size - METADATA_SIZE;

			add_free_block(new_free_block);
		} else {
			used_block->size += free_size;
		}

		return ptr;
	}

	struct block_meta *new_used_block = os_malloc(size) - METADATA_SIZE;
	u_char *bytes_new_used_block = (u_char *)new_used_block;
	u_char *bytes_used_block = (u_char *)used_block;
	size_t size_to_copy = size;

	if (size_to_copy > used_block->size)
		size_to_copy = used_block->size;

	for (size_t i = METADATA_SIZE; i < size_to_copy + METADATA_SIZE; i++)
		bytes_new_used_block[i] = bytes_used_block[i];

	os_free(ptr);

	return (void *)new_used_block + METADATA_SIZE;
}
