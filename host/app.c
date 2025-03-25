#include <assert.h>  // Assert
#include <dpu.h>     // Create DPU set
#include <dpu_log.h> // Get logs from dpus (used for debug)
#include <stdio.h>   // Print

// Handle file
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <math.h>     // floor and ceil
#include <pthread.h>  // Threads
#include <stdbool.h>  // Booleans
#include <stdint.h>   // Known size integers
#include <stdlib.h>   // Various
#include <sys/time.h> // Measure execution time
#include <time.h>     // Random seed

#include "../common/common.h"
#include "handle_edges_parallel.h"
#include "host_util.h"
#include "mg_hashtable.h"

static int32_t  seed;        // Seed for random numbers
static uint32_t sample_size; // Sample size in DPUs
static float    p;           // Probability of ignoring edges
static uint32_t k;           // Size of Misra-Gries dictionary for each thread
static uint32_t t;           // Max number of top frequent nodes to send to the DPUs
static uint32_t colors;      // Number of colors to use
static char*    filename;    // Name of the file in COO format

hash_parameters_t coloring_params; // Set by the main thread, used by all threads

int main(int argc, char* argv[]) {

	// Display the usage
	if (argc < 2) usage();

	////Initialise values before reading input
	srand(time(NULL));
	seed        = rand();
	sample_size = MAX_SAMPLE_SIZE;
	p           = 1;
	k           = 0;
	t           = 5;
	colors      = 0;
	filename    = "";

	////Read input
	while ((argc > 1) && (argv[1][0] == '-')) {

		// Wrong number of arguments remaining
		if (argc < 3) {
			usage();
		}

		switch (argv[1][1]) {
			case 's':
			case 'S':
				seed = atoi(argv[2]);
				argv += 2;
				argc -= 2;
				break;

			case 'M':
			case 'm':
				sample_size = atoi(argv[2]);
				argv += 2;
				argc -= 2;
				break;

			case 'p':
			case 'P':
				p = atof(argv[2]);
				argv += 2;
				argc -= 2;
				break;

			case 'k':
			case 'K':
				k = atoi(argv[2]);
				argv += 2;
				argc -= 2;
				break;

			case 't':
			case 'T':
				t = atoi(argv[2]);
				argv += 2;
				argc -= 2;
				break;

			case 'c':
			case 'C':
				colors = atoi(argv[2]);
				argv += 2;
				argc -= 2;
				break;

			case 'f':
			case 'F':
				filename = argv[2];
				argv += 2;
				argc -= 2;
				break;

			default:
				printf("Wrong argument: %s\n", argv[1]);
				usage();
				break;
		}
	}

	////Checking input
	srand(seed);

	if (sample_size > MAX_SAMPLE_SIZE) {
		printf("Sample size is too big. Max possible value is %d.\n", MAX_SAMPLE_SIZE);
		exit(1);
	}

	if (p < 0 || p > 1) {
		printf("Invalid percentage of kept edges.\n");
		exit(1);
	}

	if (k != 0 && t > k) {
		printf("Invalid parameters for Misra-Gries.\n");
		exit(1);
	}

	if (k == 0) {
		t = 0;
	}

	if (colors == 0) {
		printf("Invalid number of colors.\n");
		exit(1);
	}

	// Number of triplets created given the colors. binom(c+2, 3)
	uint32_t triplets_created = round((1.0 / 6) * colors * (colors + 1) * (colors + 2));
	if (triplets_created > NR_DPUS) {
		printf("More triplets than DPUs. Use more DPUs or less colors. "
		       "Given %d colors, no less than %d DPUs can be used.\n",
		       colors, triplets_created);
		exit(1);
	}

	if (access(filename, F_OK) != 0) {
		printf("File does not exist.\n");
		exit(1);
	}

	////Start counting the time
	struct timeval start;
	gettimeofday(&start, 0);

	////Allocate DPUs
	struct dpu_set_t dpu_set, dpu;

	// If it's possible to use multiple threads, allocate the DPUs using another thread.
	// Otherwise, the main thread does it
	pthread_t dpu_allocation_thread;
	if (NR_THREADS > 1) {
		pthread_create(&dpu_allocation_thread, NULL, allocate_dpus, (void*)&dpu_set);
	} else {
		allocate_dpus((void*)&dpu_set);
	}

	////Load the file into memory. Faster access from threads when reading edges
	struct stat file_stat;
	stat(filename, &file_stat);

	int   file_fd     = open(filename, O_RDONLY);
	char* mmaped_file = (char*)mmap(0, file_stat.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, file_fd, 0);
	close(file_fd);

	////Allocate the memory used to store the batches to send to the DPUs

	// Each thread has its own data, so no mutexes are needed while inserting edges into batches
	dpu_info_t* dpu_info_array = malloc(sizeof(dpu_info_t) * NR_THREADS * NR_DPUS);

	// Limit the size of the batches to fit in memory (occupy a maximum of 90% of free memory)
	uint32_t max_batch_size = (0.9 * get_free_memory() / sizeof(edge_t)) / (NR_THREADS * NR_DPUS);

	// Allocate batches of the maximum size from the beginning, even if it takes more time
	for (int th_id = 0; th_id < NR_THREADS; th_id++) {
		for (int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++) {
			dpu_info_array[th_id * NR_DPUS + dpu_id].edge_count_batch = 0;
			dpu_info_array[th_id * NR_DPUS + dpu_id].batch = (edge_t*)malloc(max_batch_size * sizeof(edge_t));
		}
	}

	////Initializing DPUs
	if (NR_THREADS > 1) {
		// If multiple threads were used, wait for the DPUs allocation to finish
		pthread_join(dpu_allocation_thread, NULL);
	}

	// Sending the input arguments to the DPUs
	dpu_arguments_t input_arguments = {.seed = seed, .sample_size = sample_size, .t = t, .padding = 0};

	DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_INPUT_ARGUMENTS", 0, &input_arguments, sizeof(dpu_arguments_t),
	                            DPU_XFER_DEFAULT));

	// Launch DPUs for setup
	DPU_ASSERT(dpu_launch(dpu_set, DPU_ASYNCHRONOUS));

	struct timeval now;
	gettimeofday(&now, 0);

	float setup_time = timedifference_msec(start, now);
	printf("Time for the setup: %f\n", setup_time);

	gettimeofday(&start, 0);

	coloring_params = get_hash_parameters(); // Global, shared with other source code file

	////Prepare variables for threads that will create the sample
	pthread_mutex_t send_to_dpus_mutex; // Mutex used to prevent from copying data to the DPUs before the previous batch
	                                    // has been processed
	if (pthread_mutex_init(&send_to_dpus_mutex, NULL) != 0) {
		printf("Mutex not initialised. Exiting.\n");
		return 1;
	}

	// Handle edges in different threads
	pthread_t             threads[NR_THREADS];
	create_batches_args_t create_batches_args[NR_THREADS];

	// Contains the most frequent nodes in the section of edges analysed by a single thread
	// Only top 2*t are kept considering that t are sent to the DPUs
	node_frequency_t* top_freq[NR_THREADS];
	for (uint32_t th_id = 0; th_id < NR_THREADS; th_id++) {
		if (k > 0) {
			top_freq[th_id] = (node_frequency_t*)malloc(2 * t * sizeof(node_frequency_t));
		} else {
			top_freq[th_id] = NULL; // Do not waste space if Misra-Gries is not used
		}
	}

	////Start edge creation
	uint64_t char_per_thread = file_stat.st_size / NR_THREADS; // Number of chars that each thread will handle
	for (uint32_t th_id = 0; th_id < NR_THREADS; th_id++) {

		// Determine the sections of chars that each thread will consider
		uint64_t from_char_in_file = char_per_thread * th_id;
		uint64_t to_char_in_file; // Not included
		if (th_id != NR_THREADS - 1) {
			to_char_in_file = char_per_thread * (th_id + 1);
		} else {
			to_char_in_file = file_stat.st_size; // If last thread, handle all remaining chars
		}

		create_batches_args[th_id] = (create_batches_args_t){
		    .th_id              = th_id,
		    .max_node_id        = 0,
		    .mmaped_file        = mmaped_file,
		    .file_size          = file_stat.st_size,
		    .from_char          = from_char_in_file,
		    .to_char            = to_char_in_file,
		    .seed               = seed,
		    .p                  = p,
		    .edges_kept         = 0,
		    .total_edges_thread = 0,
		    .k                  = k,
		    .t                  = t,
		    .top_freq           = top_freq[th_id],
		    .batch_size         = max_batch_size,
		    .colors             = colors,
		    .dpu_info_array     = dpu_info_array,
		    .dpu_set            = &dpu_set,
		    .send_to_dpus_mutex = &send_to_dpus_mutex,
		};

		pthread_create(&threads[th_id], NULL, handle_edges_file, (void*)&create_batches_args[th_id]);
	}

	// Wait for all threads to finish
	for (uint32_t th_id = 0; th_id < NR_THREADS; th_id++) {
		pthread_join(threads[th_id], NULL);
	}

	// Find the max node id. Necessary because the performance of quicksort highly depends on the accuracy of this value
	uint32_t max_node_id = 0;
	for (uint32_t th_id = 0; th_id < NR_THREADS; th_id++) {
		max_node_id = (max_node_id < create_batches_args[th_id].max_node_id) ? create_batches_args[th_id].max_node_id
		                                                                     : max_node_id;
	}

	// The threads sent the last batches, need to wait for them to be processed
	DPU_ASSERT(dpu_sync(dpu_set));

	if (k > 0) {
		node_frequency_t top_frequent_nodes[t];
		uint64_t         nr_top_nodes = global_top_freq(top_freq, top_frequent_nodes, t);

		DPU_ASSERT(dpu_broadcast_to(dpu_set, DPU_MRAM_HEAP_POINTER_NAME, 0, &top_frequent_nodes,
		                            sizeof(top_frequent_nodes), DPU_XFER_DEFAULT));
		DPU_ASSERT(dpu_broadcast_to(dpu_set, "nr_top_nodes", 0, &nr_top_nodes, sizeof(nr_top_nodes), DPU_XFER_DEFAULT));
	}

	gettimeofday(&now, 0);
	float sample_creation_time = timedifference_msec(start, now);
	printf("Time for the sample creation: %f\n", sample_creation_time);

	/*READING THE ESTIMATION FROM EVERY DPU*/
	gettimeofday(&start, 0);

	// Signal the DPUs to start counting
	execution_config_t execution_config = {1, max_node_id};
	DPU_ASSERT(dpu_broadcast_to(dpu_set, "execution_config", 0, &execution_config, sizeof(execution_config),
	                            DPU_XFER_DEFAULT));

	// Launch the DPUs program one last time
	DPU_ASSERT(dpu_launch(dpu_set, DPU_ASYNCHRONOUS));

	////Free memory while DPUs are counting the triangles
	for (int th_id = 0; th_id < NR_THREADS; th_id++) {
		for (int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++) {
			free(dpu_info_array[th_id * NR_DPUS + dpu_id].batch);
		}
	}
	free(dpu_info_array);
	pthread_mutex_destroy(&send_to_dpus_mutex);
	munmap(mmaped_file, file_stat.st_size); // Free mmapped memory (graph file)

	if (k > 0) {
		for (uint32_t th_id = 0; th_id < NR_THREADS; th_id++) {
			free(top_freq[th_id]);
		}
	}

	uint32_t edges_in_graph = 0;
	for (int th_id = 0; th_id < NR_THREADS; th_id++) {
		edges_in_graph += create_batches_args[th_id].total_edges_thread;
	}

	DPU_ASSERT(dpu_sync(dpu_set));

	uint64_t single_dpu_triangle_estimation[NR_DPUS];

	uint32_t dpu_id;
	DPU_FOREACH(dpu_set, dpu, dpu_id) {
		DPU_ASSERT(dpu_prepare_xfer(dpu, &single_dpu_triangle_estimation[dpu_id]));
	}
	DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "triangle_estimation", 0,
	                         sizeof(single_dpu_triangle_estimation[0]), DPU_XFER_DEFAULT));

	uint64_t total_triangle_estimation = 0;

	////Adjust the result knowing that some triangles may have been counted multiple times
	// First color in the triplet section considered
	int first_color_in_triplet = 0;
	// id of the  next triplet (and DPU) that counts the triangle whose nodes are all colored with the same color
	uint32_t next_same_color_triplet_id = 0;

	for (uint32_t dpu_id = 0; dpu_id < NR_DPUS; dpu_id++) {

		int32_t addition_multiplier = 1;

		// It may be possible that there are more DPUs allocated than DPUs actually used
		if (dpu_id < triplets_created) {

			if (dpu_id == next_same_color_triplet_id) {
				addition_multiplier = 2 - colors;

				// Add binom(C + 1 - first_color_in_triplet, 2) to the previous id to find what is the id of the next
				// DPU that counted triangles with nodes of the same color
				next_same_color_triplet_id +=
				    0.5 * (colors - first_color_in_triplet) * (colors - first_color_in_triplet + 1);
				first_color_in_triplet++;
			}
		}

		total_triangle_estimation += single_dpu_triangle_estimation[dpu_id] * addition_multiplier;
	}

	////Adjust the result due to lost triangles caused by uniform sampling
	if (fabs(p - 1.0) > EPSILON) { // p != 1
		double edges_kept = 0;
		for (int i = 0; i < NR_THREADS; i++) {
			edges_kept += create_batches_args[i].edges_kept;
		}

		// Not using p directly to be more precise
		total_triangle_estimation /= pow((edges_kept / edges_in_graph), 3);
	}

	// For debug purpose, get standard output from the DPUs
	/*DPU_FOREACH(dpu_set, dpu) {
	    DPU_ASSERT(dpu_log_read(dpu, stdout));
	}*/

	// Free the DPUs
	DPU_ASSERT(dpu_free(dpu_set));

	gettimeofday(&now, 0);

	float triangle_counting_time = timedifference_msec(start, now);
	printf("Time to count the triangles: %f\n", triangle_counting_time);

	printf("Triangles: %ld\n", total_triangle_estimation);
}
