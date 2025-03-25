#include <mram.h> // Transfer data between WRAM and MRAM
#include <stdbool.h>
#include <stdint.h> // Fixed size integers
#include <stdio.h>  // Standard output for debug functions

#include "../common/common.h"
#include "dpu_util.h"

// Pseudo-random number generator
uint32_t random_previous = 0;
void     srand(uint32_t seed) { // Set seed
    random_previous = seed;
}
uint32_t rand() {
	uint32_t r      = 214013 * random_previous + 2531011; // MS rand
	random_previous = r;
	return r;
}
// From and to are included
uint32_t rand_range(uint32_t from, uint32_t to) {
	return (rand() % (to - from + 1) + from);
}

void frequent_nodes_remapping(__mram_ptr edge_t* sample, uint32_t from_edge, uint32_t to_edge, edge_t* sample_buffer,
                              uint32_t nr_top_nodes, node_frequency_t* top_frequent_nodes, uint32_t max_node_id) {

	uint32_t max_edges_in_sample_buffer = WRAM_BUFFER_SIZE / sizeof(edge_t);
	uint32_t edges_in_sample_buffer     = 0;

	while (from_edge < to_edge) {

		if (to_edge - from_edge >= max_edges_in_sample_buffer) {
			edges_in_sample_buffer = max_edges_in_sample_buffer;
		} else {
			edges_in_sample_buffer = to_edge - from_edge;
		}

		// In case the the number of edges to handle is a multiple of max_edges_in_sample_buffer
		if (edges_in_sample_buffer == 0) {
			break;
		}

		mram_read(&sample[from_edge], sample_buffer, edges_in_sample_buffer * sizeof(edge_t));

		for (uint32_t i = 0; i < edges_in_sample_buffer; i++) {

			// Replace the most frequent nodes to make their node ids the highest
			for (uint32_t k = 0; k < nr_top_nodes; k++) {

				if (sample_buffer[i].u == top_frequent_nodes[k].node_id) {
					sample_buffer[i].u = max_node_id + nr_top_nodes - k;
				}

				if (sample_buffer[i].v == top_frequent_nodes[k].node_id) {
					sample_buffer[i].v = max_node_id + nr_top_nodes - k;
				}
			}

			// Invert the nodes to make them ordered
			if (sample_buffer[i].u > sample_buffer[i].v) {
				sample_buffer[i] = (edge_t){sample_buffer[i].v, sample_buffer[i].u};
			}
		}
		mram_write(sample_buffer, &sample[from_edge], edges_in_sample_buffer * sizeof(edge_t));
		from_edge += edges_in_sample_buffer;
	}
}

// Debug function for printing the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample) {
	printf("Printing the sample with %d edges:\n", edges_in_sample);

	edge_t current_edge;
	for (uint32_t i = 0; i < edges_in_sample; i++) {
		current_edge = sample[i];
		printf("%d %d\n", current_edge.u, current_edge.v);
	}
}
