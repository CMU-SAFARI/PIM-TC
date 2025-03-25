#ifndef __UTIL_H__
#define __UTIL_H__

#include <mram.h>   // Transfer data between WRAM and MRAM
#include <stdint.h> // Fixed size integers

#include "../common/common.h"

// Size of the heap in the WRAM used as buffer for the MRAM
#ifndef WRAM_BUFFER_SIZE
#define WRAM_BUFFER_SIZE 2048
#endif

// dpu cannot use the standard library random
void     srand(uint32_t seed);
uint32_t rand();
uint32_t rand_range(uint32_t from, uint32_t to); // from and to are included

// Maps the most frequent nodes to new values to reduce the number of comparisons for high degree nodes
void frequent_nodes_remapping(__mram_ptr edge_t* sample, uint32_t from_edge, uint32_t to_edge, edge_t* sample_buffer,
                              uint32_t nr_top_nodes, node_frequency_t* top_frequent_nodes, uint32_t max_node_id);

// Reverse the previous mapping in order to use the most up-to-date version of the list of most frequent nodes
void reverse_frequent_nodes_remapping(__mram_ptr edge_t* sample, uint32_t from_edge, uint32_t to_edge,
                                      edge_t* sample_buffer, uint32_t nr_top_nodes,
                                      node_frequency_t* top_frequent_nodes, uint32_t max_node_id);

// Merge the sorted sample and update
void merge_sample_update(__mram_ptr edge_t* old_sample, uint32_t edges_in_old_sample, __mram_ptr edge_t* update,
                         uint32_t edges_in_update, __mram_ptr edge_t* new_sample, void** edge_buffers);

// Debug function for printing the content of the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample);

#endif /* __UTIL_H__ */
