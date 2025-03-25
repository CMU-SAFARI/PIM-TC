#ifndef __QUICKSORT_H__
#define __QUICKSORT_H__

#include <mram.h>   // Transfer data between WRAM and MRAM. Access MRAM
#include <stdint.h> // Fixed size integers

#include "../common/common.h"

// Number (must be power of two multiple of tasklets) of sample splits to create.
// More splits means more tasklet balance
#ifndef NR_SPLITS
#define NR_SPLITS 256
#endif

/*This is the main quicksort function. The sample will be moved from the current location to the top of the heap*/
void sort_sample(uint32_t edges_in_sample, __mram_ptr edge_t* sample_from, edge_t* wram_buffer_ptr,
                 uint32_t max_node_id);

/*Performs a full step of quicksort using two caches that iterate from left and right*/
uint32_t mram_partitioning(__mram_ptr edge_t* in, __mram_ptr edge_t* out, uint32_t num_edges, edge_t* left_wram_cache,
                           edge_t* right_wram_cache, edge_t pivot);

/*Performs a sub-step of the MRAM buffer partitioning using WRAM caches.
  Returns 1 if both caches are full, 2 if the left is full and 3 if the right is full.*/
uint32_t mram_partition_step(edge_t* left_wram_cache, edge_t* right_wram_cache, uint64_t n_edges, int64_t* i,
                             int64_t* j, edge_t pivot);

/*Write the input partitions specified by indices_loc to the output locations specified by indices_off.*/
void reorder(__mram_ptr edge_t* input, __mram_ptr edge_t* output, edge_t* wram_buffer_ptr,
             uint64_t indices_loc[NR_SPLITS][NR_TASKLETS], uint64_t indices_off[NR_SPLITS][NR_TASKLETS]);

/*Fully sort an array in MRAM using quicksort with random pivot selection.*/
void sort_full(__mram_ptr edge_t* in, __mram_ptr edge_t* out, uint32_t n, edge_t* wram_buffer_ptr);

/*Iterative quicksort on WRAM edges buffer*/
void quicksort_wram(edge_t* wram_edges_array, uint64_t num_edges);

void wram_selection_sort(edge_t* edges_array, uint32_t num_edges);

// Partitioning the WRAM buffer
uint32_t wram_buffer_partitioning(edge_t* edges_array, int64_t num_edges);

#endif /* __QUICKSORT_H__ */
