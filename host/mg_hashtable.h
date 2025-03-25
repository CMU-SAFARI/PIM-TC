#ifndef __MG_HASHTABLE_H__
#define __MG_HASHTABLE_H__

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

// Hash table with linear probing to avoid pointer chasing with linked lists and make use of cache as much as possible
typedef struct {
	node_frequency_t* table;

	uint32_t k;
	uint32_t
	    size; // Size is the first prime number after 2*K -> Total number of cells, of which a maximum of k will be used

	uint32_t nr_elements;          // Number of elements currently saved. No more than k
	uint32_t max_probing_distance; // Saved the maximum linear probing distance to not search for a non-present value in
	                               // all the table
} node_freq_hashtable_t;

// Returns an initialized hash table that can contain k
node_freq_hashtable_t create_hashtable(uint32_t k);

// Frees the memory occupied by the hashtable
void delete_hashtable(node_freq_hashtable_t* table);

// Returns if a value is a prime number
bool is_prime(uint32_t value);

// Returns the first prime value over the given value
uint32_t first_prime_over(uint32_t value);

// Apply Misra-Gries
// If the node id is present, increment its frequency
// If the node id is not present, if there is still enough space, add the node id
// If the node id is not present and there is not enough space, decrement all frequencies
void update_top_frequency(node_freq_hashtable_t* table, uint32_t node_id);

// Take t most frequent nodes from the threads
void update_global_top_frequency(node_freq_hashtable_t* table, uint32_t node_id, uint32_t node_frequency);

#endif //__MG_HASHTABLE_H__
