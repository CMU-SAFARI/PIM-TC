#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "mg_hashtable.h"

node_freq_hashtable_t create_hashtable(uint32_t k) { // k is the maximum amount of values inside the hashtable
	uint32_t size = first_prime_over(2 * k);

	node_freq_hashtable_t new_table = (node_freq_hashtable_t){NULL, k, size, 0, 0};

	new_table.table = (node_frequency_t*)malloc(size * sizeof(node_frequency_t));

	for (uint32_t i = 0; i < size; i++) {
		new_table.table[i] = (node_frequency_t){-1, -1};
	}
	return new_table;
}

void delete_hashtable(node_freq_hashtable_t* table) {
	free(table->table);
}

uint32_t first_prime_over(uint32_t value) {
	uint32_t current_value = value + 1;

	while (!is_prime(current_value)) {
		current_value++;
	}

	return current_value;
}

bool is_prime(uint32_t value) {
	if (value <= 1) {
		return false;
	}

	if (value == 2 || value == 3) {
		return true;
	}

	if (value % 2 == 0 || value % 3 == 0) {
		return false;
	}

	for (uint32_t x = 5; x * x <= value; x += 6) {
		if (value % x == 0 || value % (x + 2) == 0) {
			return false;
		}
	}

	return true;
}

void update_top_frequency(node_freq_hashtable_t* table, uint32_t node_id) {

	uint32_t base_index = node_id % (table->size);

	// Search for the node id in the hash table
	// Limit the search using the maximum probing distance used before,
	// in order to avoid search in all the table if not present
	for (uint32_t i = 0; i <= table->max_probing_distance; i++) {
		uint32_t current_index = (base_index + i) % (table->size);

		if ((table->table[current_index]).node_id == node_id) {
			(table->table[current_index]).frequency++;
			return;
		}
	}

	// Table is full, decrease all frequencies by one
	if (table->nr_elements == table->k) {

		for (uint32_t i = 0; i < (table->size); i++) {

			(table->table[i]).frequency--; // Decrease frequency. Does not matter if lower than 0

			if ((table->table[i]).frequency == 0) {
				table->nr_elements--;
				(table->table[i]).node_id = -1; // Put invalid node_id
			}
		}
		return;
	}

	// Node id not found inside the table, but there is still space
	uint32_t probing_distance = 0;

	// Search a place with an empty slot with linear probing
	uint32_t current_index = base_index % (table->size);
	while ((table->table[current_index]).frequency > 0) {
		probing_distance++;
		current_index = (base_index + probing_distance) % (table->size);
	}

	(table->table[current_index]).node_id   = node_id;
	(table->table[current_index]).frequency = 1;
	table->nr_elements++;

	if (table->max_probing_distance < probing_distance) {
		table->max_probing_distance = probing_distance;
	}
}

void update_global_top_frequency(node_freq_hashtable_t* table, uint32_t node_id, uint32_t node_frequency) {
	uint32_t base_index = node_id % (table->size);

	// Search for the node id in the hash table
	// Limit the search using the maximum probing distance used before,
	// in order to avoid search in all the table if not present
	for (uint32_t i = 0; i <= table->max_probing_distance; i++) {
		uint32_t current_index = (base_index + i) % (table->size);

		if ((table->table[current_index]).node_id == node_id) {
			(table->table[current_index]).frequency += node_frequency;
			return;
		}
	}

	// Node id not found, but for sure there is still space
	uint32_t probing_distance = 0;

	// Search a place with an empty slot with linear probing
	uint32_t current_index = base_index % (table->size);
	while ((table->table[current_index]).frequency > 0) {
		probing_distance++;
		current_index = (base_index + probing_distance) % (table->size);
	}

	(table->table[current_index]).node_id   = node_id;
	(table->table[current_index]).frequency = node_frequency;
	table->nr_elements++;

	if (table->max_probing_distance < probing_distance) {
		table->max_probing_distance = probing_distance;
	}
}
