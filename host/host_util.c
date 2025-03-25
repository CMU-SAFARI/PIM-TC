#include <assert.h> // Assert
#include <dpu.h>    // Create dpu set
#include <stdint.h>
#include <stdio.h>    // Print
#include <stdlib.h>   // Exit
#include <sys/time.h> // Measure execution time

#include "host_util.h"
#include "mg_hashtable.h"

void usage() {
	printf("Triangle Counting on the UPMEM architecture\n\n");
	printf("Usage:\n\n");
	printf(" -s #                 [Seed # is used for the random number generator. Random if not given]\n");
	printf(" -M #                 [The sample size inside the DPUs is #. Maximum allowed value if not given]\n");
	printf(" -p #                 [Edges are kept with probability #. No edges are ignored (p = 1) if not given]\n");

	printf(" -k #                 [The dictionary for Misra-Gries for each thread has a maximum of # entries. "
	       "Misra-Gries is not used if not given]\n");
	printf(" -t #                 [Send a maximum of # top frequent nodes to the DPUs. Ignored if Misra-Gries is not "
	       "used. Default value is 5]\n");

	printf(" -c #                 [Use # colors to color the nodes of the graph. Required]\n");
	printf(" -f # <filenames ...> [Number of graph update files and their filenames. COO format. Required]\n");
	exit(1);
}

void* allocate_dpus(void* dpu_set) {
	DPU_ASSERT(dpu_alloc(NR_DPUS, NULL, (struct dpu_set_t*)dpu_set));
	DPU_ASSERT(dpu_load(*(struct dpu_set_t*)dpu_set, DPU_BINARY, NULL));

	return NULL;
}

float timedifference_msec(struct timeval t0, struct timeval t1) {
	return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}

uint64_t get_free_memory() {
	FILE* meminfo = fopen("/proc/meminfo", "r");
	if (meminfo == NULL) {
		printf("Cannot read the amount of available memory.\n");
		exit(1);
	}

	char     line[256];
	uint64_t ramKB = 0;
	while (fgets(line, sizeof(line), meminfo)) {
		if (sscanf(line, "MemAvailable: %ld kB", &ramKB) == 1) {
			break;
		}
	}

	if (fclose(meminfo) != 0) {
		printf("Cannot close file containing the memory informations.\n");
		exit(1);
	}

	return ramKB * 1024; // Return number of bytes
}

hash_parameters_t get_hash_parameters() {
	// Determine the parameters used to color the nodes
	uint32_t p = 8191;                 // Prime number
	uint32_t a = rand() % (p - 1) + 1; // a in [1, p-1]
	uint32_t b = rand() % p;           // b in [0, p-1]

	return (hash_parameters_t){p, a, b};
}

uint32_t global_top_freq(node_frequency_t** top_freq_th, node_frequency_t* result_top_f, uint32_t t) {

	// Can hold all the top frequencies from the threads
	node_freq_hashtable_t top_freq = create_hashtable(NR_THREADS * 2 * t);

	uint32_t valid_nodes = 0;
	// For every top frequent node id in each thread
	for (uint32_t th_id = 0; th_id < NR_THREADS; th_id++) {
		for (uint32_t i = 0; i < 2 * t; i++) {

			// The cell is invalid
			if (top_freq_th[th_id][i].frequency <= 0) {
				continue;
			}

			update_global_top_frequency(&top_freq, top_freq_th[th_id][i].node_id, top_freq_th[th_id][i].frequency);
			valid_nodes++;
		}
	}

	// Select the top t edges to return to the main thread
	// No need to return all top k if only a few are used
	for (uint32_t i = 0; i < t; i++) {
		// Find the maximum element in unsorted array
		uint32_t max_idx = i;
		for (uint32_t j = i + 1; j < top_freq.size; j++) {
			if (top_freq.table[j].frequency > top_freq.table[max_idx].frequency) {
				max_idx = j;
			}
		}

		result_top_f[i] = top_freq.table[max_idx];

		if (max_idx != i) {
			node_frequency_t temp   = top_freq.table[max_idx];
			top_freq.table[max_idx] = top_freq.table[i];
			top_freq.table[i]       = temp;
		}
	}

	delete_hashtable(&top_freq);

	return (valid_nodes > t ? t : valid_nodes);
}
