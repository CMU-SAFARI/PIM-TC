#include <alloc.h> //Alloc heap in WRAM
#include <defs.h>  //Get tasklet id
#include <mram.h>  //Transfer data between WRAM and MRAM
#include <stdbool.h>
#include <stdint.h> //Fixed size integers
#include <stdio.h>  //Standard output for debug functions
#include <stdlib.h>

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

// Reverse the previous mapping in order to use the most up-to-date version of the list of most frequent nodes
void reverse_frequent_nodes_remapping(__mram_ptr edge_t* sample, uint32_t from_edge, uint32_t to_edge,
                                      edge_t* sample_buffer, uint32_t nr_top_nodes,
                                      node_frequency_t* top_frequent_nodes, uint32_t max_node_id) {

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

				if (sample_buffer[i].u == max_node_id + nr_top_nodes - k) {
					sample_buffer[i].u = top_frequent_nodes[k].node_id;
				}

				if (sample_buffer[i].v == max_node_id + nr_top_nodes - k) {
					sample_buffer[i].v = top_frequent_nodes[k].node_id;
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

void *temp_buffer_1 = NULL, *temp_buffer_2 = NULL;
void  merge_sample_update(__mram_ptr edge_t* old_sample, uint32_t edges_in_old_sample, __mram_ptr edge_t* update,
                          uint32_t edges_in_update, __mram_ptr edge_t* new_sample, void** edge_buffers) {
    if (me() != 0) {
        return;
    }

    uint32_t max_edges_buffer = WRAM_BUFFER_SIZE / sizeof(edge_t);

    edge_t* old_sample_buffer;
    edge_t* new_sample_buffer;
    edge_t* update_buffer;
    if (NR_TASKLETS >= 3) {
        old_sample_buffer = edge_buffers[0];
        new_sample_buffer =
            edge_buffers[1]; // This function is called by only one tasklet, so the other buffers can be freely used
        update_buffer = edge_buffers[2];
    } else {
        old_sample_buffer = edge_buffers[0];

        // There is enough space
        if (temp_buffer_1 == NULL && temp_buffer_2 == NULL) {
            temp_buffer_1 = mem_alloc(WRAM_BUFFER_SIZE); // Allocate these only once
            temp_buffer_2 = mem_alloc(WRAM_BUFFER_SIZE);
        }
        new_sample_buffer = temp_buffer_1;
        update_buffer     = temp_buffer_2;
    }

    uint32_t old_sample_edge_offset = 0;
    uint32_t new_sample_edge_offset = 0;
    uint32_t update_edge_offset     = 0;

    uint32_t old_sample_buffer_offset = max_edges_buffer;
    uint32_t new_sample_buffer_offset = 0;
    uint32_t update_buffer_offset     = max_edges_buffer;

    while (old_sample_edge_offset < edges_in_old_sample && update_edge_offset < edges_in_update) {

        if (old_sample_buffer_offset == max_edges_buffer) {
            uint32_t remaining_edges = edges_in_old_sample - old_sample_edge_offset;
            uint32_t edges_to_copy   = remaining_edges > max_edges_buffer ? max_edges_buffer : remaining_edges;

            mram_read(&old_sample[old_sample_edge_offset], old_sample_buffer, edges_to_copy * sizeof(edge_t));
            old_sample_buffer_offset = 0;
        }

        if (update_buffer_offset == max_edges_buffer) {
            uint32_t remaining_edges = edges_in_update - update_edge_offset;
            uint32_t edges_to_copy   = remaining_edges > max_edges_buffer ? max_edges_buffer : remaining_edges;

            mram_read(&update[update_edge_offset], update_buffer, edges_to_copy * sizeof(edge_t));
            update_buffer_offset = 0;
        }

        edge_t current_old_sample_edge = old_sample_buffer[old_sample_buffer_offset];
        edge_t current_update_edge     = update_buffer[update_buffer_offset];

        if ((current_old_sample_edge.u < current_update_edge.u) ||
            (current_old_sample_edge.u == current_update_edge.u && current_old_sample_edge.v < current_update_edge.v)) {

            new_sample_buffer[new_sample_buffer_offset++] = current_old_sample_edge;
            old_sample_buffer_offset++;
            old_sample_edge_offset++;
        } else {

            new_sample_buffer[new_sample_buffer_offset++] = current_update_edge;
            update_buffer_offset++;
            update_edge_offset++;
        }

        if (new_sample_buffer_offset == max_edges_buffer) {
            mram_write(new_sample_buffer, &new_sample[new_sample_edge_offset], max_edges_buffer * sizeof(edge_t));
            new_sample_edge_offset += max_edges_buffer;
            new_sample_buffer_offset = 0;
        }
    }

    // Finish the last edges in the old sample
    while (old_sample_edge_offset < edges_in_old_sample) {

        if (old_sample_buffer_offset == max_edges_buffer) {
            uint32_t remaining_edges = edges_in_old_sample - old_sample_edge_offset;
            uint32_t edges_to_copy   = remaining_edges > max_edges_buffer ? max_edges_buffer : remaining_edges;

            mram_read(&old_sample[old_sample_edge_offset], old_sample_buffer, edges_to_copy * sizeof(edge_t));
            old_sample_buffer_offset = 0;
        }

        new_sample_buffer[new_sample_buffer_offset++] = old_sample_buffer[old_sample_buffer_offset];
        old_sample_buffer_offset++;
        old_sample_edge_offset++;

        if (new_sample_buffer_offset == max_edges_buffer) {
            mram_write(new_sample_buffer, &new_sample[new_sample_edge_offset], max_edges_buffer * sizeof(edge_t));
            new_sample_edge_offset += max_edges_buffer;
            new_sample_buffer_offset = 0;
        }
    }

    while (update_edge_offset < edges_in_update) {
        if (update_buffer_offset == max_edges_buffer) {
            uint32_t remaining_edges = edges_in_update - update_edge_offset;
            uint32_t edges_to_copy   = remaining_edges > max_edges_buffer ? max_edges_buffer : remaining_edges;

            mram_read(&update[update_edge_offset], update_buffer, edges_to_copy * sizeof(edge_t));
            update_buffer_offset = 0;
        }

        new_sample_buffer[new_sample_buffer_offset++] = update_buffer[update_buffer_offset];
        update_buffer_offset++;
        update_edge_offset++;

        if (new_sample_buffer_offset == max_edges_buffer) {
            mram_write(new_sample_buffer, &new_sample[new_sample_edge_offset], max_edges_buffer * sizeof(edge_t));
            new_sample_edge_offset += max_edges_buffer;
            new_sample_buffer_offset = 0;
        }
    }
    // Write the last edges
    mram_write(new_sample_buffer, &new_sample[new_sample_edge_offset], new_sample_buffer_offset * sizeof(edge_t));
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
