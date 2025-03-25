#include <alloc.h> // Alloc heap in WRAM
#include <defs.h>
#include <mram.h>   // Transfer data between WRAM and MRAM. Access MRAM
#include <mutex.h>  // Mutex for tasklets
#include <stdio.h>  // Mainly debug messages
#include <stdlib.h> // Various things

#include "../common/common.h"
#include "dpu_util.h"
#include "locate_nodes.h"
#include "triangle_counter.h"

uint32_t global_sample_read_offset = 0;
MUTEX_INIT(read_from_sample);

uint32_t count_triangles(__mram_ptr edge_t* sample, uint32_t edges_in_sample, uint32_t num_locations,
                         __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr) {
	uint32_t triangle_count = 0;

	// Create a buffer in the WRAM to read more than one edge from the sample
	// Better to read more single edges to consider in order to acquire the mutex less often
	uint32_t max_edges_in_sample_buffer = (WRAM_BUFFER_SIZE - (WRAM_BUFFER_SIZE >> 2)) / sizeof(edge_t);
	edge_t*  sample_buffer              = (edge_t*)wram_buffer_ptr;
	uint32_t edges_to_read              = 0;

	// Create a buffer in the WRAM of the sample with edges starting with u and v to speed up the research
	uint32_t max_edges_in_counting_sample_buffer = (WRAM_BUFFER_SIZE >> 3) / sizeof(edge_t);
	edge_t*  u_counting_sample_buffer            = (edge_t*)wram_buffer_ptr + max_edges_in_sample_buffer;
	edge_t*  v_counting_sample_buffer =
	    (edge_t*)wram_buffer_ptr + max_edges_in_sample_buffer + max_edges_in_counting_sample_buffer;

	// After decreasing the size of the stack, there is more space for dynamic allocation
	// Given a buffer of size N bytes, the cycles needed for the binary search without a buffer are log2(N/8) * (77 +
	// 0.5 * 8), with a buffer (77 + 0.5 * N) A buffer gives better results with N between 24 and 960
	uint32_t    max_node_locs_in_bin_search_buffer = 768 / sizeof(node_loc_t);
	node_loc_t* bin_search_buffer                  = mem_alloc(max_node_locs_in_bin_search_buffer * sizeof(node_loc_t));
	uint32_t    node_locs_in_bin_search_buffer     = 0; // How many locations are actually loaded in the cache

	uint32_t local_sample_read_index;
	uint32_t sample_buffer_index = max_edges_in_sample_buffer;

	// Keep track of the index where the buffer is taken from
	uint32_t start_index_u_counting_sample_buffer = 0;
	uint32_t start_index_v_counting_sample_buffer = 0;

	while (sample_buffer_index < edges_to_read || global_sample_read_offset < edges_in_sample) {

		if (sample_buffer_index == max_edges_in_sample_buffer) {

			mutex_lock(read_from_sample);

			// The tasklets consider a few edges at a instant
			if (edges_in_sample - global_sample_read_offset >= max_edges_in_sample_buffer) {
				edges_to_read = max_edges_in_sample_buffer;
			} else {
				edges_to_read = edges_in_sample - global_sample_read_offset;
			}

			// It may be possible that all the edges become read while waiting for the mutex
			if (edges_to_read == 0) {
				mutex_unlock(read_from_sample);
				break;
			}

			local_sample_read_index = global_sample_read_offset;
			global_sample_read_offset += edges_to_read;
			mutex_unlock(read_from_sample);

			mram_read(&sample[local_sample_read_index], sample_buffer, edges_to_read * sizeof(edge_t));
			sample_buffer_index = 0;
		}

		edge_t current_edge = sample_buffer[sample_buffer_index];
		sample_buffer_index++;

		uint32_t u = current_edge.u;
		uint32_t v = current_edge.v;

		// No need to find the u_info because the starting location is given by the address of the current edge
		node_loc_t v_info = get_location_info(num_locations, v, AFTER_SAMPLE_HEAP_POINTER, bin_search_buffer,
		                                      max_node_locs_in_bin_search_buffer, &node_locs_in_bin_search_buffer);

		if (v_info.index_in_sample == -1) { // There is no other edge with v as first node
			continue;
		}

		uint32_t u_sample_index = local_sample_read_index + sample_buffer_index - 1;
		uint32_t v_sample_index =
		    v_info.index_in_sample; // Location in sample of the first occurrences of v as first nodes of an edge

		uint32_t u_sample_offset = 0; // Offsets in the from the indexes in the sample
		uint32_t v_sample_offset = 0;

		uint32_t u_neighbor_id;
		uint32_t v_neighbor_id;

		uint32_t u_counting_sample_buffer_index = 0;
		uint32_t v_counting_sample_buffer_index = 0;

		// Use the u edges that are already present in the buffer (wait for first load by looking at v)
		if (start_index_v_counting_sample_buffer != 0 && u_sample_index >= start_index_u_counting_sample_buffer &&
		    u_sample_index < start_index_u_counting_sample_buffer + max_edges_in_counting_sample_buffer) {
			u_counting_sample_buffer_index = u_sample_index - start_index_u_counting_sample_buffer;
		} else {

			// Use the edges that are already present in the WRAM
			uint32_t u_edges_to_copy_from_sample_buffer;

			// If in the edges buffer there are more edges than there can be in the u_buffer, copy everything that fits
			if (edges_to_read - (sample_buffer_index - 1) > max_edges_in_counting_sample_buffer) {
				u_edges_to_copy_from_sample_buffer   = max_edges_in_counting_sample_buffer;
				u_counting_sample_buffer_index       = 0;
				start_index_u_counting_sample_buffer = u_sample_index;
			} else {
				// Copy only the last edges in the buffer
				u_edges_to_copy_from_sample_buffer = edges_to_read - (sample_buffer_index - 1);
				u_counting_sample_buffer_index =
				    max_edges_in_counting_sample_buffer - u_edges_to_copy_from_sample_buffer;

				// Make it seems like all the buffer was filled with important data, even though only the last part is
				start_index_u_counting_sample_buffer = u_sample_index - u_counting_sample_buffer_index;
			}

			for (uint32_t i = 0; i < u_edges_to_copy_from_sample_buffer; i++) {
				u_counting_sample_buffer[u_counting_sample_buffer_index + i] =
				    sample_buffer[sample_buffer_index - 1 + i];
			}
		}

		// If the data loaded in v is still usable (wait for first load)
		if (start_index_v_counting_sample_buffer != 0 && v_sample_index >= start_index_v_counting_sample_buffer &&
		    v_sample_index < start_index_v_counting_sample_buffer + max_edges_in_counting_sample_buffer) {
			v_counting_sample_buffer_index = v_sample_index - start_index_v_counting_sample_buffer;
		} else {
			mram_read(&sample[v_sample_index], v_counting_sample_buffer,
			          max_edges_in_counting_sample_buffer * sizeof(edge_t));
			start_index_v_counting_sample_buffer = v_sample_index;
		}

		while (u_counting_sample_buffer[u_counting_sample_buffer_index].u == u &&
		       v_counting_sample_buffer[v_counting_sample_buffer_index].u == v) {

			// Do not start reading outside the sample region
			// v_sample_offset is continuously updated
			if (v_sample_index + v_sample_offset > edges_in_sample) {
				break;
			}

			u_neighbor_id = u_counting_sample_buffer[u_counting_sample_buffer_index].v;
			v_neighbor_id = v_counting_sample_buffer[v_counting_sample_buffer_index].v;

			// Because the edges are ordered, it is possible to efficiently traverse the sample
			if (u_neighbor_id == v_neighbor_id) {
				// It does not matter if a triangle is counted in multiple DPUs. The results is adjusted considering
				// this
				triangle_count++;

				u_sample_offset++;
				v_sample_offset++;

				u_counting_sample_buffer_index++;
				v_counting_sample_buffer_index++;
			}

			if (u_neighbor_id < v_neighbor_id) {
				u_sample_offset++;
				u_counting_sample_buffer_index++;
			}

			if (u_neighbor_id > v_neighbor_id) {
				v_sample_offset++;
				v_counting_sample_buffer_index++;
			}

			// Retrieve new edges starting with u
			if (u_counting_sample_buffer_index == max_edges_in_counting_sample_buffer) {
				mram_read(&sample[u_sample_index + u_sample_offset], u_counting_sample_buffer,
				          max_edges_in_counting_sample_buffer * sizeof(edge_t));
				start_index_u_counting_sample_buffer = u_sample_index + u_sample_offset;
				u_counting_sample_buffer_index       = 0;
			}

			// Retrieve new edges starting with v
			if (v_counting_sample_buffer_index == max_edges_in_counting_sample_buffer) {
				mram_read(&sample[v_sample_index + v_sample_offset], v_counting_sample_buffer,
				          max_edges_in_counting_sample_buffer * sizeof(edge_t));
				start_index_v_counting_sample_buffer = v_sample_index + v_sample_offset;
				v_counting_sample_buffer_index       = 0;
			}
		}
	}

	return triangle_count;
}

node_loc_t get_location_info(uint32_t unique_nodes, uint32_t node_id, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER,
                             node_loc_t* node_loc_buffer_ptr, uint32_t max_node_loc_in_buffer,
                             uint32_t* node_locs_in_bin_search_buffer) {

	int low = 0, high = unique_nodes - 1;

	// Use directly the WRAM buffer if the data is already loaded
	if (*node_locs_in_bin_search_buffer != 0 && node_loc_buffer_ptr[0].id <= node_id &&
	    node_loc_buffer_ptr[*node_locs_in_bin_search_buffer - 1].id >= node_id) {
		return get_location_info_WRAM(node_id, node_loc_buffer_ptr, *node_locs_in_bin_search_buffer);
	}

	while (low <= high) {

		// If there are more elements than the maximum that can fit in the WRAM buffer, do normal binary search
		if ((uint32_t)(high - low + 1) > max_node_loc_in_buffer) {
			node_loc_t current_node;

			int mid = (low + high) >> 1; // Divide by 2 with right shift
			mram_read((__mram_ptr void*)(AFTER_SAMPLE_HEAP_POINTER + mid * sizeof(node_loc_t)), &current_node,
			          sizeof(node_loc_t)); // Read the current node data from the MRAM

			if (current_node.id == node_id) {
				return current_node;
			} else if (current_node.id > node_id) {
				high = mid - 1;
			} else {
				low = mid + 1;
			}
		} else {
			// Do not load more elements than necessary
			*node_locs_in_bin_search_buffer = (high - low + 1);

			// Search in the remaining elements
			mram_read((__mram_ptr void*)(AFTER_SAMPLE_HEAP_POINTER + low * sizeof(node_loc_t)), node_loc_buffer_ptr,
			          (*node_locs_in_bin_search_buffer) *
			              sizeof(node_loc_t)); // Read the current node data from the MRAM

			return get_location_info_WRAM(node_id, node_loc_buffer_ptr, *node_locs_in_bin_search_buffer);
		}
	}
	// Dummy node informations when the node is not present. The only thing that makes this not valid is the number of
	// neighbors at 0
	return (node_loc_t){0, -1};
}

node_loc_t get_location_info_WRAM(uint32_t node_id, node_loc_t* node_loc_buffer_ptr, uint32_t num_elements) {
	int low = 0, high = num_elements - 1;

	while (low <= high) {

		int        mid          = (low + high) >> 1; // Divide by 2 with right shift
		node_loc_t current_node = node_loc_buffer_ptr[mid];

		if (current_node.id == node_id) {
			return current_node;
		} else if (current_node.id > node_id) {
			high = mid - 1;
		} else {
			low = mid + 1;
		}
	}
	// Dummy node informations when the node is not present. The only thing that makes this not valid is the number of
	// neighbors at 0
	return (node_loc_t){0, -1};
}
