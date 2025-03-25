#include <assert.h>  // Assert
#include <barrier.h> // Barrier for tasklets
#include <defs.h>    // Get tasklet id
#include <mram.h>    // Transfer data between WRAM and MRAM. Access MRAM
#include <mutex.h>   // Mutex for tasklets
#include <stdint.h>  // Fixed size integers
#include <stdio.h>   // Mainly debug messages

#include "../common/common.h"
#include "dpu_util.h"
#include "quicksort.h"

// Considering the WRAM buffer divided into two sections
#ifndef EDGES_IN_BLOCK
#define EDGES_IN_BLOCK (WRAM_BUFFER_SIZE / (sizeof(edge_t) << 1)) // Divide into two buffer for edges
#endif

BARRIER_INIT(sync_tasklets_quicksort, NR_TASKLETS);

// Store the offsets for each block split through quicksort (8 bytes for MRAM alignment)
__mram uint64_t indices_loc[NR_SPLITS][NR_TASKLETS];

// Store prefix sum for offsets of a particular split over all blocks (8 bytes for MRAM alignment)
__mram uint64_t indices_off[NR_SPLITS][NR_TASKLETS];

// Determine which split needs to be ordered next by a tasklet
uint32_t current_split = 0;
MUTEX_INIT(splits_mutex);

/*This is the main quicksort function. The sample will be moved from the current location to the top of the heap*/
void sort_sample(uint32_t edges_in_sample, __mram_ptr edge_t* sample_from, edge_t* wram_buffer_ptr,
                 uint32_t max_node_id) {

	// Number of edges per tasklet
	uint32_t nr_edges_tasklets =
	    me() < edges_in_sample % NR_TASKLETS ? edges_in_sample / NR_TASKLETS + 1 : edges_in_sample / NR_TASKLETS;

	// First edge inside the sample that is handled the current tasklet
	uint32_t base_tasklet;
	if (me() < edges_in_sample % NR_TASKLETS) {
		base_tasklet = me() * (edges_in_sample / NR_TASKLETS + 1);
	} else {
		base_tasklet = me() * (edges_in_sample / NR_TASKLETS) + edges_in_sample % NR_TASKLETS;
	}

	// Determine the number of edges in each partition of quicksort

	// Best performance with starting pivot at the highest node id present
	edge_t pivot_prev = (edge_t){max_node_id, max_node_id};
	for (uint32_t i = NR_SPLITS / 2; i > 0; i >>= 1) {
		edge_t pivot =
		    (edge_t){pivot_prev.u >> 1, pivot_prev.v >> 1}; // Determine the pivot depending on the previous one

		for (uint32_t split = i; split < NR_SPLITS; split += i * 2) {

			uint64_t start = 0;
			if (split > i) {
				mram_read(&indices_loc[split - 1 - i][me()], &start,
				          sizeof(uint64_t)); // Direct MRAM access leads better results
			}

			uint64_t nr_edges_split;
			if (split + i < NR_SPLITS) {
				mram_read(&indices_loc[split - 1 + i][me()], &nr_edges_split,
				          sizeof(uint64_t)); // Direct MRAM access leads better results
				nr_edges_split -= start;
			} else {
				nr_edges_split = nr_edges_tasklets - start;
			}

			uint64_t index_tmp =
			    mram_partitioning(sample_from + base_tasklet + start, sample_from + base_tasklet + start,
			                      nr_edges_split, wram_buffer_ptr, wram_buffer_ptr + EDGES_IN_BLOCK, pivot);

			index_tmp += start;
			mram_write(&index_tmp, (__mram_ptr void*)&indices_loc[split - 1][me()], sizeof(uint64_t));

			pivot = (edge_t){pivot.u + pivot_prev.u, pivot.v + pivot_prev.v};
		}
		pivot_prev = (edge_t){pivot_prev.u >> 1, pivot_prev.v >> 1};
	}

	indices_loc[NR_SPLITS - 1][me()] = nr_edges_tasklets - indices_loc[NR_SPLITS - 2][me()];

	for (int32_t i = NR_SPLITS - 3; i >= 0; i--) {
		indices_loc[i + 1][me()] = indices_loc[i + 1][me()] - indices_loc[i][me()];
	}

	barrier_wait(&sync_tasklets_quicksort);

	for (uint32_t split = me(); split < NR_SPLITS; split += NR_TASKLETS) {
		indices_off[split][0] = indices_loc[split][0];
		for (uint32_t i = 1; i < NR_TASKLETS; i++) {
			indices_off[split][i] = indices_off[split][i - 1] + indices_loc[split][i];
		}
	}

	barrier_wait(&sync_tasklets_quicksort);

	// Merge partitions into MRAM in the right order. The new sample will be placed at the start of the MRAM heap
	reorder(sample_from + base_tasklet, DPU_MRAM_HEAP_POINTER, wram_buffer_ptr, (uint64_t(*)[NR_TASKLETS])indices_loc,
	        (uint64_t(*)[NR_TASKLETS])indices_off);

	barrier_wait(&sync_tasklets_quicksort);

	// Do the final sort on the partitions
	uint32_t split_task;

	mutex_lock(splits_mutex);
	split_task = current_split;
	current_split++;
	mutex_unlock(splits_mutex);

	while (split_task < NR_SPLITS) {
		uint32_t byte_offset = 0;
		for (uint32_t i = 0; i < split_task; i++) {
			byte_offset += indices_off[i][NR_TASKLETS - 1] * sizeof(edge_t);
		}
		nr_edges_tasklets = indices_off[split_task][NR_TASKLETS - 1];

		if (nr_edges_tasklets > 0) {
			sort_full(DPU_MRAM_HEAP_POINTER + byte_offset, DPU_MRAM_HEAP_POINTER + byte_offset, nr_edges_tasklets,
			          wram_buffer_ptr);
		}

		mutex_lock(splits_mutex);
		split_task = current_split;
		current_split++;
		mutex_unlock(splits_mutex);
	}
}

/*Performs a full step of quicksort using two caches that iterate from left and right*/
uint32_t mram_partitioning(__mram_ptr edge_t* in, __mram_ptr edge_t* out, uint32_t num_edges, edge_t* left_wram_cache,
                           edge_t* right_wram_cache, edge_t pivot) {

	// All the indexes are relative to the subsection beeing considered, not all the data in the sample
	uint32_t left_i  = 0;
	uint32_t right_i = num_edges - EDGES_IN_BLOCK;
	uint32_t status  = 0;

	int64_t i = 0;
	int64_t j = num_edges;

	mram_read(in, left_wram_cache, EDGES_IN_BLOCK * sizeof(edge_t));
	mram_read((__mram_ptr void*)(in + right_i), right_wram_cache, EDGES_IN_BLOCK * sizeof(edge_t));

	do {
		// Using caches all the data in this quick_sort_blocks call is partitioned.
		// Having a buffer with the data on the left and a buffer with the data on the right
		// Partitioning is done comparing the data in the buffers.
		// When one of the buffer has been completely observed, the partitioned buffer is copied
		// and a new unpartitioned buffer is loaded
		status = mram_partition_step(left_wram_cache, right_wram_cache, num_edges, &i, &j, pivot);

		if (status == 1 || status == 2) {
			mram_write(left_wram_cache, (__mram_ptr void*)(out + left_i), EDGES_IN_BLOCK * sizeof(edge_t));
			left_i += EDGES_IN_BLOCK;
			mram_read((__mram_ptr void*)(in + left_i), left_wram_cache, EDGES_IN_BLOCK * sizeof(edge_t));
		}

		if (status == 1 || status == 3) {
			mram_write(right_wram_cache, (__mram_ptr void*)(out + right_i), EDGES_IN_BLOCK * sizeof(edge_t));
			right_i -= EDGES_IN_BLOCK;
			mram_read((__mram_ptr void*)(in + right_i), right_wram_cache, EDGES_IN_BLOCK * sizeof(edge_t));
		}

	} while (status != 0);

	// All the elements of the current section have been partitioned
	// Write the remaining elements in cache to MRAM
	uint32_t nr_left  = i % EDGES_IN_BLOCK;
	uint32_t nr_right = (num_edges - j) % EDGES_IN_BLOCK;

	if (nr_left > 0) {
		mram_write(left_wram_cache, (__mram_ptr void*)(out + left_i), nr_left * sizeof(edge_t));
	}
	if (nr_right > 0) {
		mram_write(right_wram_cache + EDGES_IN_BLOCK - nr_right,
		           (__mram_ptr void*)(out + right_i + EDGES_IN_BLOCK - nr_right), nr_right * sizeof(edge_t));
	}
	return i;
}

/*Performs a sub-step of the MRAM buffer partitioning using WRAM caches.
  Returns 1 if both caches are full, 2 if the left is full and 3 if the right is full.*/
uint32_t mram_partition_step(edge_t* left_wram_cache, edge_t* right_wram_cache, uint64_t n_edges, int64_t* i,
                             int64_t* j, edge_t pivot) {

	// Convert absolute position to position inside WRAM buffer
	int64_t left  = *i % EDGES_IN_BLOCK;
	int64_t right = EDGES_IN_BLOCK - 1 - (n_edges - *j) % EDGES_IN_BLOCK;

	while (*i < *j) {
		// Swap of left and right elements according to the value compared to pivot
		if ((left_wram_cache[left].u > pivot.u) ||
		    (left_wram_cache[left].u == pivot.u && left_wram_cache[left].v > pivot.v)) {
			if ((right_wram_cache[right].u < pivot.u) ||
			    (right_wram_cache[right].u == pivot.u && right_wram_cache[right].v < pivot.v)) {

				edge_t tmp              = left_wram_cache[left];
				left_wram_cache[left]   = right_wram_cache[right];
				right_wram_cache[right] = tmp;

				(*i)++;
				(*j)--;
				left++;
				right--;
			} else {
				(*j)--;
				right--;
			}
		} else {
			(*i)++;
			left++;
		}

		// Return a code determining which buffer is full
		if (left == EDGES_IN_BLOCK && right == -1) {
			return 1;
		} else if (left == EDGES_IN_BLOCK) {
			return 2;
		}
		if (right == -1) {
			return 3;
		}
	}
	return 0;
}

/*Write the input partitions specified by indices_loc to the output locations specified by indices_off.*/
void reorder(__mram_ptr edge_t* input, __mram_ptr edge_t* output, edge_t* wram_buffer_ptr,
             uint64_t indices_loc[NR_SPLITS][NR_TASKLETS], uint64_t indices_off[NR_SPLITS][NR_TASKLETS]) {

	// The offset to load the elements
	uint64_t offset_in = 0;
	// The offset to store the elements
	uint64_t offset_glob = 0;
	for (uint32_t i = 0; i < NR_SPLITS; i++) {

		uint64_t offset = 0;
		uint64_t length = 0;

		if (me() > 0) {
			mram_read((__mram_ptr void*)&indices_off[i][me() - 1], &offset, sizeof(uint64_t));
		}
		mram_read((__mram_ptr void*)&indices_loc[i][me()], &length, sizeof(uint64_t));

		uint64_t offset_tot = offset + offset_glob;

		// Read and write edges from and to the MRAM using the WRAM buffer
		for (uint32_t base = 0; base + 2 * EDGES_IN_BLOCK <= length; base += 2 * EDGES_IN_BLOCK) {
			mram_read((__mram_ptr void*)(input + offset_in), wram_buffer_ptr, 2 * EDGES_IN_BLOCK * sizeof(edge_t));
			mram_write(wram_buffer_ptr, (__mram_ptr void*)(output + offset_tot), 2 * EDGES_IN_BLOCK * sizeof(edge_t));

			offset_in += 2 * EDGES_IN_BLOCK;
			offset_tot += 2 * EDGES_IN_BLOCK;
		}
		uint64_t rem_size = length % (2 * EDGES_IN_BLOCK);
		if (rem_size > 0) {
			mram_read((__mram_ptr void*)(input + offset_in), wram_buffer_ptr, rem_size * sizeof(edge_t));
			mram_write(wram_buffer_ptr, (__mram_ptr void*)(output + offset_tot), rem_size * sizeof(edge_t));
		}

		offset_in += rem_size;

		uint64_t offset_glob_tmp;
		mram_read((__mram_ptr void*)&indices_off[i][NR_TASKLETS - 1], &offset_glob_tmp, sizeof(uint64_t));
		offset_glob += offset_glob_tmp;
	}
}

/*Fully sort an array in MRAM using quicksort with random pivot selection.*/
void sort_full(__mram_ptr edge_t* in, __mram_ptr edge_t* out, uint32_t n_edges, edge_t* wram_buffer_ptr) {

	// Contain the boundaries of the simulated recursion levels
	// With 32 indexes, 2^32 elements can be ordered
	int32_t  max_levels = 32;
	uint32_t level_start[max_levels];
	uint32_t level_end[max_levels];

	uint32_t local_start, local_end; // Indexes of the elements inside each simulated recursion
	int32_t  i = 0;                  // Simulated recursion level (can be negative)

	level_start[0] = 0; // First simulated recursion spans across all assigned section of the sample
	level_end[0]   = n_edges;

	__dma_aligned edge_t pivots[5];

	while (i >= 0) { /// While there are still some levels to handle

		// If last available level is reached, the array cannot be ordered
		assert(i < max_levels);

		local_start = level_start[i]; // local start at level i
		local_end   = level_end[i];   // local end at level i

		if (local_start < local_end) {
			// Partition the the array
			uint32_t size = local_end - local_start;

			// If the remaining edges fit in the WRAM buffer, use it for faster quicksort and copy back the result
			if (size <= 2 * EDGES_IN_BLOCK) {

				mram_read((__mram_ptr void*)(in + local_start), wram_buffer_ptr, size * sizeof(edge_t));
				quicksort_wram(wram_buffer_ptr, size);
				mram_write(wram_buffer_ptr, (__mram_ptr void*)(out + local_start), size * sizeof(edge_t));

				// Current level has been sorted
				level_start[i] = local_end;
				i--;
			} else {

				// Take 5 values and choose the middle one as a pivot (still random choice, but not so random)
				uint32_t rand = rand_range(0, EDGES_IN_BLOCK - 1);

				mram_read((__mram_ptr void*)(in + local_start + rand), pivots, 5 * sizeof(edge_t));
				wram_selection_sort(pivots, 5);

				uint32_t p = mram_partitioning(in + local_start, out + local_start, size, wram_buffer_ptr,
				                               wram_buffer_ptr + EDGES_IN_BLOCK, pivots[2]);

				// The next level will sort the left section
				level_start[i + 1] = local_start;
				level_end[i + 1]   = local_start + p;

				// Overwritten the current level. It will sort the right section
				level_start[i] = local_start + p;

				i++; // Increase one level

				// If this level (after i++) is bigger than the previous one, swap them (execute before the smaller one.
				// Tail "recursion") It is guaranteed that 2^(max_levels) elements can be ordered
				if (level_end[i] - level_start[i] > level_end[i - 1] - level_start[i - 1]) {
					uint32_t temp      = level_start[i];
					level_start[i]     = level_start[i - 1];
					level_start[i - 1] = temp;

					temp             = level_end[i];
					level_end[i]     = level_end[i - 1];
					level_end[i - 1] = temp;
				}
			}
		} else {
			// If the level does not need to be ordered, order the level i-1
			i--;
		}
	}
}

/*Iterative quicksort on WRAM edges buffer*/
void quicksort_wram(edge_t* wram_edges_array, uint64_t num_edges) {
	// Contain the boundaries of the simulated recursion levels
	// With 16 indexes, 2^16 elements can be ordered
	const int32_t max_levels = 16;
	uint32_t      level_start[max_levels];
	uint32_t      level_end[max_levels];

	uint32_t local_start, local_end; // Indexes of the elements inside each simulated recursion
	int32_t  i = 0;                  // Simulated recursion level (can be negative)

	level_start[0] = 0; // First simulated recursion spans across all assigned section of the sample
	level_end[0]   = num_edges;

	while (i >= 0) { /// While there are still some levels to handle

		// If last available level is reached, the array cannot be ordered
		assert(i < max_levels);

		local_start = level_start[i]; // local start at level i
		local_end   = level_end[i];   // local end at level i

		if (local_start < local_end) {

			uint32_t size = local_end - local_start;
			// With less than 10 elements, selection sort is faster than quicksort
			if (size < 10) {
				wram_selection_sort(wram_edges_array + local_start, size);
				i--;
			} else {
				// Partition the the array
				uint32_t p = wram_buffer_partitioning(wram_edges_array + local_start, size);

				// The next level will sort the left section
				level_start[i + 1] = local_start;
				level_end[i + 1]   = local_start + p;

				// Overwritten the current level. It will sort the right section
				level_start[i] = local_start + p;

				i++; // Increase one level

				// If this level (after i++) is bigger than the previous one, swap them (execute before the smaller one.
				// Tail "recursion") It is guaranteed that 2^(max_levels) elements can be ordered
				if (level_end[i] - level_start[i] > level_end[i - 1] - level_start[i - 1]) {
					uint32_t temp      = level_start[i];
					level_start[i]     = level_start[i - 1];
					level_start[i - 1] = temp;

					temp             = level_end[i];
					level_end[i]     = level_end[i - 1];
					level_end[i - 1] = temp;
				}
			}
		} else {
			// If the level does not need to be ordered, order the level i-1
			i--;
		}
	}
}

void wram_selection_sort(edge_t* edges_array, uint32_t num_edges) {
	for (uint32_t i = 0; i < num_edges; i++) {
		edge_t   min   = edges_array[i];
		uint32_t min_i = i;

		for (uint32_t j = i + 1; j < num_edges; j++) {
			if ((edges_array[j].u < min.u) || (edges_array[j].u == min.u && edges_array[j].v < min.v)) {
				min   = edges_array[j];
				min_i = j;
			}
		}
		edges_array[min_i] = edges_array[i];
		edges_array[i]     = min;
	}
}

// Partitioning the WRAM buffer
uint32_t wram_buffer_partitioning(edge_t* edges_array, int64_t num_edges) {

	edge_t pivot = edges_array[num_edges / 2];

	int32_t i = 0;
	int32_t j = num_edges - 1;

	// Hoare partitioning
	while (i <= j) {
		while ((edges_array[i].u < pivot.u) || (edges_array[i].u == pivot.u && edges_array[i].v < pivot.v)) {
			i++;
		}

		while ((edges_array[j].u > pivot.u) || (edges_array[j].u == pivot.u && edges_array[j].v > pivot.v)) {
			j--;
		}

		if (i <= j) {
			edge_t tmp     = edges_array[i];
			edges_array[i] = edges_array[j];
			edges_array[j] = tmp;
			i++;
			j--;
		}
	}
	return i;
}
