#ifndef __HOST_UTIL_H__
#define __HOST_UTIL_H__

#include <dpu.h>
#include <sys/time.h>

#include "../common/common.h"

// Allow for files bigger than 4GB
#define _FILE_OFFSET_BITS 64

typedef struct {
	edge_t*  batch;            // Pointer to the array containing the edges in the current batch for the DPU
	uint64_t edge_count_batch; // Current number of edges in the batch for the DPU
} dpu_info_t;

typedef struct {
	uint32_t th_id;
	uint32_t max_node_id;
	uint32_t update_idx; // Index of the update file

	// Handle the file
	char*    mmaped_file; // Information about file
	uint64_t file_size;
	uint64_t from_char;
	uint64_t to_char;

	// Uniform sampling
	int32_t  seed;
	double   p;
	uint32_t edges_kept;
	uint32_t total_edges_thread;

	// Misra-Gries
	uint32_t          k;
	uint32_t          t;
	node_frequency_t* top_freq;

	/// Create batches
	uint32_t    batch_size;
	uint32_t    colors;
	dpu_info_t* dpu_info_array;

	// Send the batches
	struct dpu_set_t* dpu_set;
	pthread_mutex_t*  send_to_dpus_mutex;
} create_batches_args_t;

// Get ordered colors of the edge
edge_colors_t get_edge_colors(edge_t edge, uint32_t colors);

// Function executed by each thread handling the edges. The file is read and the edges are inserted in the correct batch
void* handle_edges_file(void* args_thread);

// Insert the current edge into the correct batches considering how triplets are assigned to the DPUs
void insert_edge_into_batches(edge_t current_edge, dpu_info_t* dpu_info_array, uint32_t batch_size, uint32_t colors,
                              uint32_t th_id, pthread_mutex_t* mutex, struct dpu_set_t* dpu_set, uint32_t update_idx);

// Send the full batch to the specific DPU. th_id_to is not included
void send_batches(uint32_t th_id, dpu_info_t* dpu_info_array, pthread_mutex_t* mutex, struct dpu_set_t* dpu_set,
                  uint32_t update_idx);

#endif /* __HOST_UTIL_H_ */
