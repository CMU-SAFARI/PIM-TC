#include <assert.h>  // Assert
#include <limits.h>  // Max values
#include <math.h>    // Round
#include <pthread.h> // Mutexes
#include <stdbool.h> // Booleans
#include <stdint.h>  // Fixed size integers
#include <stdio.h>   // Standard output for debug functions
#include <stdlib.h>  // Various things
#include <stdlib.h>  // Random

#include "../common/common.h"
#include "handle_edges_parallel.h"
#include "host_util.h"
#include "mg_hashtable.h"

extern const hash_parameters_t coloring_params; // Set by the main thread

edge_colors_t get_edge_colors(edge_t edge, uint32_t colors) {

	// Color hashing formula: ((a * id + b) % p ) % colors
	uint32_t color_u = ((coloring_params.a * edge.u + coloring_params.b) % coloring_params.p) % colors;
	uint32_t color_v = ((coloring_params.a * edge.v + coloring_params.b) % coloring_params.p) % colors;

	// The colors must be ordered
	if (color_u < color_v) {
		return (edge_colors_t){color_u, color_v};
	}
	return (edge_colors_t){color_v, color_u};
}

void* handle_edges_file(void* args_thread) {

	create_batches_args_t* args = (create_batches_args_t*)args_thread;

	// Buffer to read each line. Each node can use 10 chars each at max (unsigned integers of 4 bytes)
	char     char_buffer[32];
	uint32_t node1, node2;

	char* mmaped_file = args->mmaped_file;

	uint64_t file_char_counter = args->from_char;

	// If the division makes the thread start in the middle of an edge, skip to the first full edge
	// The edge skipped will be handled by the thread with previous id
	if (file_char_counter != 0 && mmaped_file[file_char_counter - 1] != '\n') {
		for (; file_char_counter < args->to_char; file_char_counter++) {
			if (mmaped_file[file_char_counter] == '\n') {
				file_char_counter++;
				break;
			}
		}
	}

	// Seed used for the local random number generator
	uint32_t local_seed = args->seed + args->th_id;

	node_freq_hashtable_t mg_table;
	if (args->update_idx == 0 && args->k > 0) {
		mg_table = create_hashtable(args->k);
	}

	edge_t current_edge;
	while (file_char_counter < args->to_char) {

		// Read the file char by char until EOL
		uint32_t c = 0;
		for (; c < sizeof(char_buffer) && file_char_counter < args->file_size; c++) {
			if (mmaped_file[file_char_counter] == '\n') {
				file_char_counter++;
				break;
			}

			char_buffer[c] = mmaped_file[file_char_counter];
			file_char_counter++;
		}
		char_buffer[c] = 0; // Without this, some remains of previous edges may be considered

		args->total_edges_thread++;

		if (fabs(args->p - 1.0) > EPSILON) { // p != 1  //If uniform sampling is used
			float random = (float)rand_r(&local_seed) / ((float)INT_MAX + 1.0);
			if (random > args->p) {
				continue;
			}

			args->edges_kept++; // Count the number of edges considered
		}

		// Each edge is formed by two unsigned integers separated by a space
		sscanf(char_buffer, "%d %d", &node1, &node2);

		// Edges are considered valid from the file (no duplicates, node1 != node2). No additional checks are performed
		if (node1 < node2) { // Nodes in edge need to be ordered
			current_edge = (edge_t){node1, node2};

			if (node2 > args->max_node_id) {
				args->max_node_id = node2;
			}
		} else {
			current_edge = (edge_t){node2, node1};

			if (node1 > args->max_node_id) {
				args->max_node_id = node1;
			}
		}

		if (args->update_idx == 0 && args->k > 0) {
			update_top_frequency(&mg_table, node1);
			update_top_frequency(&mg_table, node2);
		}

		insert_edge_into_batches(current_edge, args->dpu_info_array, args->batch_size, args->colors, args->th_id,
		                         args->send_to_dpus_mutex, args->dpu_set, args->update_idx);
	}

	send_batches(args->th_id, args->dpu_info_array, args->send_to_dpus_mutex, args->dpu_set, args->update_idx);

	if (args->update_idx == 0 && args->k > 0) {
		// Select the top 2*t edges to return to the main thread
		// No need to return all top k if only a few are used
		for (uint32_t i = 0; i < 2 * args->t; i++) {
			// Find the maximum element in unsorted array
			uint32_t max_idx = i;
			// Consider only valid entries
			for (uint32_t j = i + 1; j < mg_table.size; j++) {
				if (mg_table.table[j].frequency > mg_table.table[max_idx].frequency) {
					max_idx = j;
				}
			}

			args->top_freq[i] = mg_table.table[max_idx];

			if (max_idx != i) {
				node_frequency_t temp   = mg_table.table[max_idx];
				mg_table.table[max_idx] = mg_table.table[i];
				mg_table.table[i]       = temp;
			}
		}

		delete_hashtable(&mg_table);
	}

	pthread_exit(NULL);
}

void insert_edge_into_batches(edge_t current_edge, dpu_info_t* dpu_info_array, uint32_t batch_size, uint32_t colors,
                              uint32_t th_id, pthread_mutex_t* mutex, struct dpu_set_t* dpu_set, uint32_t update_idx) {

	// Given that the current edge has colors (a,b), with a <= b
	edge_colors_t current_edge_colors = get_edge_colors(current_edge, colors);
	uint32_t      a                   = current_edge_colors.color_u;
	uint32_t      b                   = current_edge_colors.color_v;

	// Considering the current way triplets are assigned to a DPU, to find the ids of the DPUs that will
	// handle the current edge, it is necessary to consider the cases (a, b, c3), (a, c2, b) and (c1, a, b) (not
	// allowing for duplicates)

	// For (a, b, c3), the first occurrence has id:
	// [sum from x = 0 to (a-1) 0.5*(colors - x) * (colors - x + 1)]
	// + 0.5 * (colors - a) * (colors - a + 1)
	// - 0.5 * (colors - b) * (colors - b + 1)

	// This formula uses different Gauss sums that show up considering the creation of the triplets.
	// After the first occurrence, the other ids are the following (+1) until c3 reaches the number of total colors
	uint32_t current_dpu_id = round(
	    (1.0 / 6) * (a * a * a - 3 * a * a * colors + a * (3 * colors * colors - 1) - 3 * b * (b - 2 * colors - 1)));

	for (uint32_t c3 = b; c3 < colors; c3++) { // Varying the third color

		dpu_info_t* current_dpu_info = &dpu_info_array[th_id * NR_DPUS + current_dpu_id];

		(current_dpu_info->batch)[(current_dpu_info->edge_count_batch)++] = current_edge;

		if (current_dpu_info->edge_count_batch == batch_size) {
			send_batches(th_id, dpu_info_array, mutex, dpu_set, update_idx);
		}

		current_dpu_id++;
	}

	// For (a, c2, b), the first occurrence has id:
	// [sum from x = 0 to (a-1) 0.5*(colors - x) * (colors - x + 1)] + b - a
	// This formula uses different Gauss sums that show up considering the creation of the triplets
	// After the first occurrence, the other ids are determined adding (colors - 1 - c2)

	current_dpu_id =
	    round((1.0 / 6) * a * (a * a - 3 * a * (colors + 1) + 3 * colors * colors + 6 * colors + 2) - a + b);

	for (uint32_t c2 = a; c2 <= b; c2++) { // Varying the third color
		if (c2 != a && c2 != b) {          // Avoid duplicate insertion in triplets (y, y, y)

			dpu_info_t* current_dpu_info = &dpu_info_array[th_id * NR_DPUS + current_dpu_id];

			(current_dpu_info->batch)[(current_dpu_info->edge_count_batch)++] = current_edge;

			if (current_dpu_info->edge_count_batch == batch_size) {
				send_batches(th_id, dpu_info_array, mutex, dpu_set, update_idx);
			}
		}

		current_dpu_id += colors - 1 - c2;
	}

	for (uint32_t c1 = 0; c1 <= a; c1++) {
		if (c1 != b || c1 != a) { // Avoid duplicate insertions in the cases already considered

			// Similar idea as before. This time, (c1, a, b)
			//[sum from x=0 to c1-1 0.5*(C-x)*(C-x+1)] + 0.5*(C-c1)*(C-c1+1) - 0.5*(C-a)(C-a+1) + b - a
			current_dpu_id = round(-0.5 * a * a + a * colors - 0.5 * a + b + 0.5 * colors * colors * c1 -
			                       0.5 * colors * c1 * c1 + (1.0 / 6) * c1 * c1 * c1 - (1.0 / 6) * c1);

			dpu_info_t* current_dpu_info = &dpu_info_array[th_id * NR_DPUS + current_dpu_id];

			(current_dpu_info->batch)[(current_dpu_info->edge_count_batch)++] = current_edge;

			if (current_dpu_info->edge_count_batch == batch_size) {
				send_batches(th_id, dpu_info_array, mutex, dpu_set, update_idx);
			}

			current_dpu_id += (1.0 / 2) * (colors - c1) * (colors - c1 + 1);
		}
	}
}

void send_batches(uint32_t th_id, dpu_info_t* dpu_info_array, pthread_mutex_t* mutex, struct dpu_set_t* dpu_set,
                  uint32_t update_idx) {

	// Limit transfers
	uint64_t max_edges_per_transfer = MAX_BATCH_TRANSFER_SIZE_BYTES;

	// Determine the max amount of edges per batch that this thread needs to send
	uint32_t max_edges_to_send = 0;
	for (int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++) {
		if (max_edges_to_send < dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch) {
			max_edges_to_send = dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch;
		}
	}

	for (uint32_t batch_offset = 0; batch_offset < max_edges_to_send; batch_offset += max_edges_per_transfer) {

		// Size of the biggest remaining batch
		uint32_t max_remaining_edges_to_send = 0;
		for (int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++) {
			if (max_remaining_edges_to_send < dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch) {
				max_remaining_edges_to_send = dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch;
			}
		}

		// Send data to the DPUs
		pthread_mutex_lock(mutex);

		// Wait for all the DPUs to finish the previous task.
		DPU_ASSERT(dpu_sync(*dpu_set));

		uint32_t         dpu_id;
		struct dpu_set_t dpu;
		DPU_FOREACH(*dpu_set, dpu, dpu_id) {
			DPU_ASSERT(dpu_prepare_xfer(dpu, &dpu_info_array[th_id * NR_DPUS + dpu_id].batch[batch_offset]));
		}

		// If the amount of edges to send is too big, send the most amount of edges possible
		// If the amount of edges is not too big, send only the remaining edges
		bool     batch_too_big = max_remaining_edges_to_send > max_edges_to_send;
		uint32_t edges_to_send = batch_too_big ? max_edges_to_send : max_remaining_edges_to_send;

		// Depending on the update index, the free space where to send the batch may be at the beginning or in the
		// middle of the heap
		uint32_t heap_offset = (update_idx % 2 == 0) ? 0 : MIDDLE_HEAP_OFFSET;
		DPU_ASSERT(dpu_push_xfer(*dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, heap_offset,
		                         edges_to_send * sizeof(edge_t), DPU_XFER_DEFAULT));

		// Parallel transfer also for the current batch sizes
		DPU_FOREACH(*dpu_set, dpu, dpu_id) {

			// If less data than the full remaining batch is sent (so the maximum allowed amount of data per transfer)
			if (dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch > max_edges_to_send) {
				DPU_ASSERT(dpu_prepare_xfer(dpu, &max_edges_to_send));

			} else {
				DPU_ASSERT(dpu_prepare_xfer(dpu, &dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch));
			}
		}

		DPU_ASSERT(dpu_push_xfer(*dpu_set, DPU_XFER_TO_DPU, "edges_in_batch", 0,
		                         sizeof(dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch), DPU_XFER_DEFAULT));

		DPU_ASSERT(dpu_launch(*dpu_set, DPU_ASYNCHRONOUS));

		pthread_mutex_unlock(mutex);

		// Update the count for the remaining edges to send
		for (dpu_id = 0; dpu_id < NR_DPUS; dpu_id++) {
			uint32_t last_batch_size = dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch;

			dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch =
			    last_batch_size >= max_edges_to_send ? last_batch_size - max_edges_to_send : 0;
		}
	}
}
