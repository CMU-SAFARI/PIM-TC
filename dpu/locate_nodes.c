#include <attributes.h> // For __mram_ptr
#include <defs.h>       // Get tasklet id
#include <handshake.h>  // Handshake for tasklets
#include <mram.h>       // Transfer data between WRAM and MRAM. Access MRAM
#include <stdbool.h>    // Booleans
#include <stdint.h>     // Fixed size integers
#include <stdio.h>      // Mainly debug messages
#include <stdlib.h>     // Various things

#include "../common/common.h"
#include "dpu_util.h"
#include "locate_nodes.h"

// Offset where to write and read in the MRAM common for all tasklets
uint32_t global_read_offset  = 0;
uint32_t global_write_offset = 0;

uint32_t remaining_tasklets = NR_TASKLETS;

uint32_t node_locations(__mram_ptr edge_t* sample, uint32_t edges_in_sample, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER,
                        void* wram_buffer_ptr) {
	// Create a buffer in the WRAM of the sample to speed up research
	uint32_t max_edges_in_sample_buffer = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1; // Divide by 2 with right shift
	// Use half the WRAM buffer for buffering the sample
	edge_t* sample_buffer = (edge_t*)wram_buffer_ptr;

	// Use the other half of the WRAM buffer to store node locations that will be transferred to the MRAM
	// Because the size of an edge variable is the same as the size of a node location variable,
	// There is no need to check if there is enough space for more node locations (written when new edges are read)
	node_loc_t* nodes_loc_buffer = wram_buffer_ptr + max_edges_in_sample_buffer * sizeof(edge_t);

	// Indexes to traverse the WRAM buffers
	uint32_t nodes_loc_buffer_index = 0;
	uint32_t sample_buffer_index    = max_edges_in_sample_buffer; // At max at first to have a read at first

	uint32_t previous_node_id;

	uint32_t local_read_offset; // Where the currently considered edges were read from the sample
	uint32_t edges_read    = 0;
	bool     is_first_read = true;

	// Used only the first node of each edge
	uint32_t current_node_id;
	uint32_t index_in_sample; // Index of the first occurrence of the unique node inside tha sample

	uint32_t local_unique_nodes = 0; // Number of unique nodes found

	// Search all the unique nodes considering only the first nodes of the edges
	// Read until there are edges in the local buffer or there are edges in the sample
	while (true) {

		if (sample_buffer_index == max_edges_in_sample_buffer) {

			// The tasklets read the sample in order, after the previous one has read its section
			// The first tasklets starts, then all the other can proceed in order
			if (NR_TASKLETS > 1) {
				if (me() == 0) {
					if (!is_first_read) { // The first tasklet does not wait the first time
						handshake_wait_for(NR_TASKLETS - 1);
					}
				} else {
					handshake_wait_for(me() - 1);
				}
			}

			/*WRITE TO THE MRAM*/
			if (!is_first_read) { // Nothing to write at first
				write_nodes_loc(&nodes_loc_buffer_index, nodes_loc_buffer, AFTER_SAMPLE_HEAP_POINTER);
			}

			// Making the tasklet write outside of the handshake lock is painfully difficult

			if (global_read_offset < edges_in_sample) {
				// Transfer some edges of the sample to the WRAM

				/*READ FROM THE MRAM*/
				local_read_offset   = virtually_read_from_sample(edges_in_sample, &edges_read);
				sample_buffer_index = 0;

				if (NR_TASKLETS > 1) {
					handshake_notify();
				}
				is_first_read = false;

				// Read, if present, the node id of the previous edge
				// Do not create a new node location for a node id already considered
				if (local_read_offset != 0) {
					edge_t previous_edge;
					mram_read(&sample[local_read_offset - 1], &previous_edge, sizeof(edge_t));
					previous_node_id = previous_edge.u;
				} // If local_read_offset == 0, the previous_node_id will not be considered

				mram_read(&sample[local_read_offset], sample_buffer, edges_read * sizeof(edge_t));
			} else {
				if (NR_TASKLETS > 1) {
					handshake_notify();
				}
				break;
			}
		} else {

			if (sample_buffer_index == edges_read) {
				break;
			}

			// Skip all edges already considered in a previous node location
			// Skip until there are edges to consider and, if there is a previous node id, the current node id is the
			// same
			for (; sample_buffer_index < edges_read; sample_buffer_index++) {
				current_node_id = sample_buffer[sample_buffer_index].u;

				// If it is the first edge or if it is a new node, it must be considered
				if (local_read_offset == 0 || current_node_id != previous_node_id) {
					break;
				}
			}

			// All the nodes in the buffer have been considered already
			if (sample_buffer_index == edges_read) {
				continue;
			}

			// Position inside the MRAM sample
			index_in_sample = local_read_offset + sample_buffer_index;
			local_unique_nodes++;

			// Count the number of nodes that have the current_node as value u in the edge
			// The result is the number of neighbors of that node
			// This is possible because the sample is ordered, there are no duplicates and u<v in each edge
			while (sample_buffer_index < edges_read && sample_buffer[sample_buffer_index].u == current_node_id) {
				sample_buffer_index++;
			}

			// Transfer the data to the MRAM
			node_loc_t current_node_info             = (node_loc_t){current_node_id, index_in_sample};
			nodes_loc_buffer[nodes_loc_buffer_index] = current_node_info;
			nodes_loc_buffer_index++;

			// There will be no need to transfer node locations while considering the current edge buffer
		}
	}

	// The tasklets read the sample in order, after the previous one has read its section
	// The first tasklets starts, then all the other can proceed in order

	if (NR_TASKLETS > 1) {
		if (me() == 0) {
			handshake_wait_for(NR_TASKLETS - 1);
		} else {
			handshake_wait_for(me() - 1);
		}
	}

	// Write the last node locations
	write_nodes_loc(&nodes_loc_buffer_index, nodes_loc_buffer, AFTER_SAMPLE_HEAP_POINTER);
	remaining_tasklets--;

	if (NR_TASKLETS > 1 && remaining_tasklets > 0) {
		handshake_notify();
	}

	return local_unique_nodes;
}

void write_nodes_loc(uint32_t* nodes_loc_buffer_index, node_loc_t* nodes_loc_buffer,
                     __mram_ptr node_loc_t* AFTER_SAMPLE_HEAP_POINTER) {

	// Copy back the node locations (if any). The handshake will grant the correct order
	if (*nodes_loc_buffer_index > 0) {
		uint32_t local_write_offset = global_write_offset;
		global_write_offset += *nodes_loc_buffer_index;

		mram_write(nodes_loc_buffer, (__mram_ptr void*)(AFTER_SAMPLE_HEAP_POINTER + local_write_offset),
		           (*nodes_loc_buffer_index) * sizeof(node_loc_t));
		*nodes_loc_buffer_index = 0;
	}
}

uint32_t virtually_read_from_sample(uint32_t edges_in_sample, uint32_t* edges_read) {

	// Read more edges
	uint32_t max_edges_in_sample_buffer = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1;

	if (edges_in_sample - global_read_offset >= max_edges_in_sample_buffer) {
		*edges_read = max_edges_in_sample_buffer;
	} else {
		*edges_read = edges_in_sample - global_read_offset;
	}

	uint32_t local_read_offset = global_read_offset;
	global_read_offset += *edges_read;

	return local_read_offset;
}

// No need to use WRAM buffer because it is only for debugging
void print_node_locations(uint32_t number_of_nodes, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER) {
	printf("Printing the node location informations. There are %d unique nodes:\n", number_of_nodes);

	for (uint32_t i = 0; i < number_of_nodes; i++) {
		node_loc_t current_node;
		mram_read((__mram_ptr void*)(AFTER_SAMPLE_HEAP_POINTER + i * sizeof(node_loc_t)), &current_node,
		          sizeof(node_loc_t)); // Read the informations of one node from MRAM
		printf("Id: %d Index in sample: %d\n", current_node.id, current_node.index_in_sample);
	}
}
